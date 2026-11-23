#include "nim/content/audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <format>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <unordered_map>

namespace nim {

PcmLevel measure_pcm_level(const PcmAudio& pcm) {
    PcmLevel level;
    if (!pcm.sample_rate || !pcm.frames()) return level;
    const size_t window = std::max<size_t>(pcm.sample_rate / 10, 1);
    float largest_square_mean = 0;
    for (size_t channel = 0; channel < pcm.channels; ++channel) {
        for (size_t first = 0; first < pcm.frames(); first += window) {
            const size_t last = std::min(first + window, pcm.frames());
            double sum = 0;
            for (size_t f = first; f < last; ++f) {
                const float sample = std::abs(float(pcm.samples[f * pcm.channels + channel])) / 32768.0f;
                level.peak = std::max(level.peak, sample);
                sum += sample * sample;
            }
            largest_square_mean = std::max(largest_square_mean, float(sum / double(last - first)));
        }
    }
    level.short_rms = std::sqrt(largest_square_mean);
    return level;
}

float limit_ambient_gain(float gain, PcmLevel level) {
    constexpr float rms_limit = 0.0631f;  // -24 dBFS
    constexpr float peak_limit = 0.2512f; // -12 dBFS
    if (level.short_rms > 0) gain = std::min(gain, rms_limit / level.short_rms);
    if (level.peak > 0) gain = std::min(gain, peak_limit / level.peak);
    return gain;
}

namespace {

#include "wwise_codebooks.inc"

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("WEM: " + what); }

int ilog(uint32_t v) {
    int r = 0;
    while (v) ++r, v >>= 1;
    return r;
}

// Vorbis packs fields least-significant bit first.
struct BitReader {
    const uint8_t* data = nullptr;
    size_t bits = 0, pos = 0;
    bool overrun = false;
    BitReader(std::span<const uint8_t> s) : data(s.data()), bits(s.size() * 8) {}
    uint32_t read(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i, ++pos) {
            if (pos >= bits) { overrun = true; return v; }
            v |= uint32_t((data[pos >> 3] >> (pos & 7)) & 1) << i;
        }
        return v;
    }
    int bit() {
        if (pos >= bits) { overrun = true; ++pos; return 0; }
        const int b = (data[pos >> 3] >> (pos & 7)) & 1;
        ++pos;
        return b;
    }
};

float float32_unpack(uint32_t x) {
    const double mantissa = double(x & 0x1FFFFF);
    const int exponent = int((x & 0x7FE00000) >> 21);
    return float(std::ldexp((x & 0x80000000) ? -mantissa : mantissa, exponent - 788));
}

uint32_t lookup1_values(uint32_t entries, uint32_t dims) {
    uint32_t r = uint32_t(std::floor(std::pow(double(entries), 1.0 / dims)));
    while (std::pow(double(r + 1), dims) <= entries) ++r;
    while (r && std::pow(double(r), dims) > entries) --r;
    return r;
}

// ---- Codebooks ------------------------------------------------------------------------------------
struct Codebook {
    uint32_t dimensions = 0, entries = 0;
    std::vector<uint8_t> lengths;               // 0 = unused
    std::vector<std::array<int32_t, 2>> tree;   // >= 0 node, < 0 leaf -(entry+1), kNone missing
    std::vector<float> vectors;                 // entries x dimensions (lookup type 1)
    static constexpr int32_t kNone = INT32_MIN;

    void build() {
        // Codeword assignment of the Vorbis spec (libvorbis _make_words).
        std::array<uint32_t, 33> marker{};
        tree.assign(1, {kNone, kNone});
        uint32_t used = 0, last = 0;
        for (uint32_t i = 0; i < entries; ++i) {
            const int len = lengths[i];
            if (!len) continue;
            ++used, last = i;
            uint32_t entry = marker[len];
            if (len < 32 && (entry >> len)) fail("overspecified codebook");
            // insert the codeword, most significant bit first
            int32_t node = 0;
            for (int b = len - 1; b >= 0; --b) {
                const int bit = (entry >> b) & 1;
                if (b == 0) {
                    tree[node][bit] = -int32_t(i) - 1;
                } else {
                    if (tree[node][bit] == kNone) {
                        tree[node][bit] = int32_t(tree.size());
                        tree.push_back({kNone, kNone});
                    }
                    if (tree[node][bit] < 0) fail("codebook prefix collision");
                    node = tree[node][bit];
                }
            }
            for (int j = len; j > 0; --j) {
                if (marker[j] & 1) {
                    marker[j] = j == 1 ? marker[1] + 1 : marker[j - 1] << 1;
                    break;
                }
                marker[j]++;
            }
            for (int j = len + 1; j <= 32; ++j) {
                if ((marker[j] >> 1) == entry) {
                    entry = marker[j];
                    marker[j] = marker[j - 1] << 1;
                } else {
                    break;
                }
            }
        }
        if (used == 1) {  // a single used entry: either bit value selects it
            for (auto& c : tree[0])
                if (c == kNone) c = -int32_t(last) - 1;
        }
    }

    int32_t decode(BitReader& r) const {
        int32_t node = 0;
        for (int depth = 0; depth < 33; ++depth) {
            const int32_t next = tree[node][r.bit()];
            if (next == kNone || r.overrun) return -1;
            if (next < 0) return -next - 1;
            node = next;
        }
        return -1;
    }
};

// ww2ogg's packed codebook format (narrower fields than a Vorbis setup).
Codebook unpack_codebook(uint32_t id) {
    static const auto offsets = [] {
        const size_t n = sizeof(kWwiseCodebooks);
        const uint32_t table = le32(kWwiseCodebooks + n - 4);
        std::vector<uint32_t> o;
        for (size_t p = table; p + 4 <= n; p += 4) o.push_back(le32(kWwiseCodebooks + p));
        return o;
    }();
    if (id + 1 >= offsets.size()) fail(std::format("codebook id {} not in the library", id));
    const uint32_t begin = offsets[id], end = offsets[id + 1];
    BitReader r(std::span<const uint8_t>(kWwiseCodebooks + begin, end - begin));
    Codebook c;
    c.dimensions = r.read(4);
    c.entries = r.read(14);
    if (!c.dimensions || !c.entries) fail("empty codebook");
    c.lengths.assign(c.entries, 0);
    if (r.read(1)) {  // ordered
        uint32_t length = r.read(5) + 1, current = 0;
        while (current < c.entries) {
            const uint32_t count = r.read(ilog(c.entries - current));
            if (current + count > c.entries || length > 32) fail("bad ordered codebook");
            std::fill_n(c.lengths.begin() + current, count, uint8_t(length));
            current += count;
            ++length;
        }
    } else {
        const int length_bits = int(r.read(3));
        const bool sparse = r.read(1);
        if (length_bits == 0 || length_bits > 5) fail("bad codeword length width");
        for (uint32_t i = 0; i < c.entries; ++i)
            if (!sparse || r.read(1)) c.lengths[i] = uint8_t(r.read(length_bits) + 1);
    }
    const uint32_t lookup = r.read(1);
    if (lookup == 1) {
        const float minimum = float32_unpack(r.read(32)), delta = float32_unpack(r.read(32));
        const int value_bits = int(r.read(4)) + 1;
        const bool sequence = r.read(1);
        const uint32_t quantvals = lookup1_values(c.entries, c.dimensions);
        std::vector<uint32_t> mult(quantvals);
        for (auto& m : mult) m = r.read(value_bits);
        c.vectors.resize(size_t(c.entries) * c.dimensions);
        for (uint32_t e = 0; e < c.entries; ++e) {
            float last = 0;
            uint32_t divisor = 1;
            for (uint32_t d = 0; d < c.dimensions; ++d) {
                const uint32_t off = (e / divisor) % quantvals;
                const float v = float(mult[off]) * delta + minimum + last;
                c.vectors[size_t(e) * c.dimensions + d] = v;
                if (sequence) last = v;
                divisor *= quantvals;
            }
        }
    }
    if (r.overrun) fail("codebook truncated");
    c.build();
    return c;
}

const Codebook& library_codebook(uint32_t id) {
    static std::mutex m;
    static std::unordered_map<uint32_t, std::unique_ptr<Codebook>> cache;
    std::lock_guard lock(m);
    auto& slot = cache[id];
    if (!slot) slot = std::make_unique<Codebook>(unpack_codebook(id));
    return *slot;
}

// ---- Setup ----------------------------------------------------------------------------------------
struct Floor1 {
    std::vector<uint32_t> partition_class;
    std::vector<uint32_t> class_dims, class_subclasses, class_master;
    std::vector<std::vector<int32_t>> subclass_books;
    uint32_t multiplier = 1;
    std::vector<uint32_t> x;  // X list (x[0] = 0, x[1] = 1 << rangebits)
    // derived
    std::vector<uint32_t> sorted, low, high;
};

struct Residue {
    uint32_t type = 0, begin = 0, end = 0, partition_size = 0, classifications = 0, classbook = 0;
    std::vector<std::array<int32_t, 8>> books;
};

struct Mapping {
    uint32_t submaps = 1;
    std::vector<std::pair<uint32_t, uint32_t>> coupling;  // magnitude, angle
    std::vector<uint32_t> mux;                            // channel -> submap
    std::vector<uint32_t> submap_floor, submap_residue;
};

struct Mode {
    bool blockflag = false;
    uint32_t mapping = 0;
};

struct Setup {
    std::vector<const Codebook*> books;
    std::vector<Floor1> floors;
    std::vector<Residue> residues;
    std::vector<Mapping> mappings;
    std::vector<Mode> modes;
    int mode_bits = 0;
};

Setup read_setup(BitReader& r, uint32_t channels) {
    Setup s;
    const uint32_t book_count = r.read(8) + 1;
    for (uint32_t i = 0; i < book_count; ++i) s.books.push_back(&library_codebook(r.read(10)));
    auto book = [&](uint32_t b) {
        if (b >= s.books.size()) fail("setup references a missing codebook");
        return b;
    };

    const uint32_t floor_count = r.read(6) + 1;
    for (uint32_t i = 0; i < floor_count; ++i) {
        Floor1 f;
        const uint32_t partitions = r.read(5);
        uint32_t max_class = 0;
        for (uint32_t p = 0; p < partitions; ++p) {
            f.partition_class.push_back(r.read(4));
            max_class = std::max(max_class, f.partition_class.back());
        }
        for (uint32_t c = 0; c <= max_class; ++c) {
            f.class_dims.push_back(r.read(3) + 1);
            f.class_subclasses.push_back(r.read(2));
            f.class_master.push_back(f.class_subclasses.back() ? book(r.read(8)) : 0);
            auto& sb = f.subclass_books.emplace_back();
            for (uint32_t k = 0; k < (1u << f.class_subclasses.back()); ++k) {
                const int32_t b = int32_t(r.read(8)) - 1;
                if (b >= 0) book(uint32_t(b));
                sb.push_back(b);
            }
        }
        f.multiplier = r.read(2) + 1;
        const int rangebits = int(r.read(4));
        f.x = {0, 1u << rangebits};
        for (uint32_t p = 0; p < partitions; ++p)
            for (uint32_t k = 0; k < f.class_dims[f.partition_class[p]]; ++k) f.x.push_back(r.read(rangebits));
        if (f.x.size() > 65) fail("floor1 has too many points");
        // neighbours and sort order
        const size_t n = f.x.size();
        f.low.assign(n, 0), f.high.assign(n, 0);
        for (size_t i = 2; i < n; ++i) {
            int lo = -1, hi = -1;
            for (size_t j = 0; j < i; ++j) {
                if (f.x[j] < f.x[i] && (lo < 0 || f.x[j] > f.x[size_t(lo)])) lo = int(j);
                if (f.x[j] > f.x[i] && (hi < 0 || f.x[j] < f.x[size_t(hi)])) hi = int(j);
            }
            if (lo < 0 || hi < 0) fail("floor1 X list has duplicates");
            f.low[i] = uint32_t(lo), f.high[i] = uint32_t(hi);
        }
        f.sorted.resize(n);
        for (size_t i = 0; i < n; ++i) f.sorted[i] = uint32_t(i);
        std::stable_sort(f.sorted.begin(), f.sorted.end(), [&](uint32_t a, uint32_t b) { return f.x[a] < f.x[b]; });
        s.floors.push_back(std::move(f));
    }

    const uint32_t residue_count = r.read(6) + 1;
    for (uint32_t i = 0; i < residue_count; ++i) {
        Residue res;
        res.type = r.read(2);
        if (res.type > 2) fail("bad residue type");
        res.begin = r.read(24);
        res.end = r.read(24);
        res.partition_size = r.read(24) + 1;
        res.classifications = r.read(6) + 1;
        res.classbook = book(r.read(8));
        std::vector<uint32_t> cascade(res.classifications);
        for (auto& c : cascade) {
            const uint32_t low_bits = r.read(3);
            const uint32_t high_bits = r.read(1) ? r.read(5) : 0;
            c = high_bits * 8 + low_bits;
        }
        res.books.resize(res.classifications);
        for (uint32_t c = 0; c < res.classifications; ++c)
            for (int k = 0; k < 8; ++k) res.books[c][k] = (cascade[c] >> k) & 1 ? int32_t(book(r.read(8))) : -1;
        s.residues.push_back(std::move(res));
    }

    const uint32_t mapping_count = r.read(6) + 1;
    for (uint32_t i = 0; i < mapping_count; ++i) {
        Mapping m;
        if (r.read(1)) m.submaps = r.read(4) + 1;
        if (r.read(1)) {
            const uint32_t steps = r.read(8) + 1;
            const int bits = ilog(channels - 1);
            for (uint32_t k = 0; k < steps; ++k) {
                const uint32_t mag = r.read(bits), ang = r.read(bits);
                if (mag == ang || mag >= channels || ang >= channels) fail("bad channel coupling");
                m.coupling.emplace_back(mag, ang);
            }
        }
        if (r.read(2)) fail("mapping reserved field set");
        m.mux.assign(channels, 0);
        if (m.submaps > 1)
            for (auto& x : m.mux) {
                x = r.read(4);
                if (x >= m.submaps) fail("bad mapping mux");
            }
        for (uint32_t k = 0; k < m.submaps; ++k) {
            r.read(8);  // time config (unused)
            m.submap_floor.push_back(r.read(8));
            m.submap_residue.push_back(r.read(8));
            if (m.submap_floor.back() >= s.floors.size() || m.submap_residue.back() >= s.residues.size())
                fail("bad submap");
        }
        s.mappings.push_back(std::move(m));
    }

    const uint32_t mode_count = r.read(6) + 1;
    for (uint32_t i = 0; i < mode_count; ++i) {
        Mode m;
        m.blockflag = r.read(1);
        m.mapping = r.read(8);
        if (m.mapping >= s.mappings.size()) fail("bad mode mapping");
        s.modes.push_back(m);
    }
    s.mode_bits = ilog(mode_count - 1);
    if (r.overrun) fail("setup packet truncated");
    return s;
}

// ---- Audio packets --------------------------------------------------------------------------------
const std::array<float, 256>& inverse_db_table() {
    static const auto t = [] {
        std::array<float, 256> a{};
        for (int i = 0; i < 256; ++i) a[i] = float(std::pow(10.0, -(255 - i) * 7.0 / 256.0));
        return a;
    }();
    return t;
}

int render_point(int x0, int y0, int x1, int y1, int x) {
    const int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
    const int off = adx ? ady * (x - x0) / adx : 0;
    return dy < 0 ? y0 - off : y0 + off;
}

void render_line(int x0, int y0, int x1, int y1, std::vector<int>& v) {
    const int n = int(v.size());
    const int dy = y1 - y0, adx = x1 - x0;
    if (adx <= 0) return;
    int ady = std::abs(dy);
    const int base = dy / adx;
    const int sy = dy < 0 ? base - 1 : base + 1;
    ady -= std::abs(base) * adx;
    int y = y0, err = 0;
    if (x0 < n) v[x0] = y;
    for (int x = x0 + 1; x < x1 && x < n; ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        v[x] = y;
    }
}

// Decodes a floor1 packet section; false = channel unused this packet.
bool decode_floor1(const Floor1& f, const Setup& s, BitReader& r, std::vector<int>& y) {
    if (!r.read(1)) return false;
    static constexpr int kRange[] = {256, 128, 86, 64};
    const int range = kRange[f.multiplier - 1];
    const int bits = ilog(uint32_t(range - 1));
    y.assign(f.x.size(), 0);
    y[0] = int(r.read(bits));
    y[1] = int(r.read(bits));
    size_t offset = 2;
    for (uint32_t cls : f.partition_class) {
        const uint32_t cdim = f.class_dims[cls], cbits = f.class_subclasses[cls];
        const uint32_t csub = (1u << cbits) - 1;
        uint32_t cval = 0;
        if (cbits) {
            const int32_t v = s.books[f.class_master[cls]]->decode(r);
            if (v < 0) return false;
            cval = uint32_t(v);
        }
        for (uint32_t j = 0; j < cdim; ++j) {
            const int32_t b = f.subclass_books[cls][cval & csub];
            cval >>= cbits;
            if (b >= 0) {
                const int32_t v = s.books[uint32_t(b)]->decode(r);
                if (v < 0) return false;
                y[offset + j] = v;
            } else {
                y[offset + j] = 0;
            }
        }
        offset += cdim;
    }
    return !r.overrun;
}

void synthesize_floor1(const Floor1& f, const std::vector<int>& y, size_t n2, std::vector<float>& out) {
    static constexpr int kRange[] = {256, 128, 86, 64};
    const int range = kRange[f.multiplier - 1];
    const size_t n = f.x.size();
    std::vector<int> fy(n);
    std::vector<uint8_t> step2(n, 0);
    step2[0] = step2[1] = 1;
    fy[0] = y[0], fy[1] = y[1];
    for (size_t i = 2; i < n; ++i) {
        const uint32_t lo = f.low[i], hi = f.high[i];
        const int predicted = render_point(int(f.x[lo]), fy[lo], int(f.x[hi]), fy[hi], int(f.x[i]));
        const int val = y[i];
        const int highroom = range - predicted, lowroom = predicted;
        const int room = (highroom < lowroom ? highroom : lowroom) * 2;
        if (val) {
            step2[lo] = step2[hi] = step2[i] = 1;
            if (val >= room)
                fy[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
            else
                fy[i] = (val & 1) ? predicted - (val + 1) / 2 : predicted + val / 2;
        } else {
            fy[i] = predicted;
        }
    }
    std::vector<int> curve(n2, 0);
    int lx = 0, ly = fy[f.sorted[0]] * int(f.multiplier), hx = 0, hy = 0;
    for (size_t k = 1; k < n; ++k) {
        const uint32_t i = f.sorted[k];
        if (!step2[i]) continue;
        hy = fy[i] * int(f.multiplier);
        hx = int(f.x[i]);
        render_line(lx, ly, hx, hy, curve);
        lx = hx, ly = hy;
    }
    if (size_t(hx) < n2) render_line(hx, hy, int(n2), hy, curve);
    const auto& db = inverse_db_table();
    out.resize(n2);
    for (size_t i = 0; i < n2; ++i) out[i] = db[std::clamp(curve[i], 0, 255)];
}

void decode_residue(const Residue& res, const Setup& s, BitReader& r, size_t n2, std::vector<std::vector<float>*>& vectors,
                    const std::vector<uint8_t>& skip) {
    const size_t ch = vectors.size();
    const Codebook& classbook = *s.books[res.classbook];
    auto decode_partitions = [&](std::vector<std::vector<float>*>& vecs, const std::vector<uint8_t>& dnd, size_t actual,
                                 uint32_t format) {
        const size_t begin = std::min<size_t>(res.begin, actual), end = std::min<size_t>(res.end, actual);
        const size_t psize = res.partition_size;
        if (end <= begin) return;
        const size_t partitions = (end - begin) / psize;
        const uint32_t per_word = classbook.dimensions;
        std::vector<std::vector<uint32_t>> classes(vecs.size(), std::vector<uint32_t>(partitions + per_word, 0));
        for (int pass = 0; pass < 8; ++pass) {
            size_t pc = 0;
            while (pc < partitions) {
                if (pass == 0)
                    for (size_t j = 0; j < vecs.size(); ++j) {
                        if (dnd[j]) continue;
                        int32_t temp = classbook.decode(r);
                        if (temp < 0) return;
                        for (int i = int(per_word) - 1; i >= 0; --i) {
                            classes[j][size_t(i) + pc] = uint32_t(temp) % res.classifications;
                            temp = int32_t(uint32_t(temp) / res.classifications);
                        }
                    }
                for (uint32_t i = 0; i < per_word && pc < partitions; ++i, ++pc)
                    for (size_t j = 0; j < vecs.size(); ++j) {
                        if (dnd[j]) continue;
                        const int32_t bi = res.books[classes[j][pc]][pass];
                        if (bi < 0) continue;
                        const Codebook& b = *s.books[uint32_t(bi)];
                        if (b.vectors.empty()) return;  // a scalar book cannot carry residue values
                        float* v = vecs[j]->data() + begin + pc * psize;
                        const uint32_t dims = b.dimensions;
                        if (format == 0) {
                            const size_t step = psize / dims;
                            for (size_t k = 0; k < step; ++k) {
                                const int32_t e = b.decode(r);
                                if (e < 0) return;
                                const float* vq = &b.vectors[size_t(e) * dims];
                                for (uint32_t d = 0; d < dims; ++d) v[k + d * step] += vq[d];
                            }
                        } else {
                            for (size_t k = 0; k < psize;) {
                                const int32_t e = b.decode(r);
                                if (e < 0) return;
                                const float* vq = &b.vectors[size_t(e) * dims];
                                for (uint32_t d = 0; d < dims && k < psize; ++d) v[k++] += vq[d];
                            }
                        }
                    }
            }
        }
    };

    if (res.type == 2) {
        bool any = false;
        for (auto d : skip) any |= !d;
        if (!any) return;
        std::vector<float> joint(n2 * ch, 0.0f);
        std::vector<std::vector<float>*> one{&joint};
        decode_partitions(one, {0}, n2 * ch, 1);
        for (size_t i = 0; i < n2; ++i)
            for (size_t c = 0; c < ch; ++c) (*vectors[c])[i] = joint[i * ch + c];
    } else {
        decode_partitions(vectors, skip, n2, res.type);
    }
}

// ---- Inverse MDCT ---------------------------------------------------------------------------------
struct MdctPlan {
    size_t n = 0;
    std::vector<std::complex<float>> pre, post, roots;
    std::vector<uint32_t> bitrev;
};

const MdctPlan& mdct_plan(size_t n) {
    static std::mutex m;
    static std::unordered_map<size_t, std::unique_ptr<MdctPlan>> plans;
    std::lock_guard lock(m);
    auto& p = plans[n];
    if (p) return *p;
    p = std::make_unique<MdctPlan>();
    p->n = n;
    const size_t M = n / 2, F = M / 2;  // DCT-IV of size M via an F-point complex FFT
    const double pi = std::numbers::pi;
    p->pre.resize(F), p->post.resize(F), p->roots.resize(F / 2 ? F / 2 : 1);
    for (size_t k = 0; k < F; ++k) {
        p->pre[k] = std::polar(1.0f, float(-pi * double(k) / double(M)));
        p->post[k] = std::polar(1.0f, float(-pi * (double(k) + 0.25) / double(M)));
    }
    for (size_t k = 0; k < F / 2; ++k) p->roots[k] = std::polar(1.0f, float(-2.0 * pi * double(k) / double(F)));
    p->bitrev.resize(F);
    const int bits = ilog(uint32_t(F)) - 1;
    for (size_t k = 0; k < F; ++k) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b) r |= uint32_t((k >> b) & 1) << (bits - 1 - b);
        p->bitrev[k] = r;
    }
    return *p;
}

} // namespace

void inverse_mdct(std::span<const float> in, std::span<float> out) {
    const size_t M = in.size(), n = M * 2, F = M / 2, N4 = n / 4;
    if (out.size() != n || F == 0 || (F & (F - 1))) throw std::invalid_argument("inverse_mdct size");
    const MdctPlan& p = mdct_plan(n);
    // DCT-IV u[k] = sum X[j] cos(pi/M (k + 1/2)(j + 1/2)):
    //   w[q] = (X[2q] + i X[M-1-2q]) e^{-i pi q / M}, W = FFT_F(w), T[q] = W[q] e^{-i pi (q + 1/4) / M},
    //   u[2q] = Re T[q], u[M-1-2q] = -Im T[q].
    std::vector<std::complex<float>> w(F);
    for (size_t q = 0; q < F; ++q) w[p.bitrev[q]] = std::complex<float>(in[2 * q], in[M - 1 - 2 * q]) * p.pre[q];
    for (size_t len = 2; len <= F; len <<= 1) {
        const size_t half = len / 2, stride = F / len;
        for (size_t i = 0; i < F; i += len)
            for (size_t j = 0; j < half; ++j) {
                const auto t = w[i + j + half] * p.roots[j * stride];
                w[i + j + half] = w[i + j] - t;
                w[i + j] += t;
            }
    }
    std::vector<float> u(M);
    for (size_t q = 0; q < F; ++q) {
        const auto t = w[q] * p.post[q];
        u[2 * q] = t.real();
        u[M - 1 - 2 * q] = -t.imag();
    }
    // Unfold: y[n] = u[n + N/4] (n < N/4), -u[3N/4 - 1 - n] (n < 3N/4), -u[n - 3N/4].
    for (size_t i = 0; i < N4; ++i) out[i] = u[i + N4];
    for (size_t i = N4; i < 3 * N4; ++i) out[i] = -u[3 * N4 - 1 - i];
    for (size_t i = 3 * N4; i < n; ++i) out[i] = -u[i - 3 * N4];
}

namespace {

float window_slope(size_t i, size_t len) {
    const double s = std::sin((double(i) + 0.5) / double(len) * std::numbers::pi / 2);
    return float(std::sin(std::numbers::pi / 2 * s * s));
}

struct Packet {
    size_t offset, size;
};

PcmAudio decode_vorbis(std::span<const uint8_t> bytes, size_t fmt, size_t fmt_size, size_t data, size_t data_size,
                       uint16_t channels, uint32_t sample_rate) {
    if (fmt_size != 0x42) fail(std::format("unsupported Vorbis header layout (fmt size {:#x})", fmt_size));
    const uint8_t* vorb = bytes.data() + fmt + 0x18;
    const uint32_t sample_count = le32(vorb);
    const uint32_t mod_signal = le32(vorb + 4);
    const bool mod_packets = mod_signal != 0x4A && mod_signal != 0x4B && mod_signal != 0x69 && mod_signal != 0x70;
    const uint32_t setup_offset = le32(vorb + 0x10), audio_offset = le32(vorb + 0x14);
    const size_t bs0 = size_t(1) << vorb[0x28], bs1 = size_t(1) << vorb[0x29];
    if (bs0 < 64 || bs1 < bs0 || bs1 > 8192) fail("bad block sizes");
    if (channels == 0 || channels > 32) fail("bad channel count");  // up to 16: ambisonic beds

    auto packet_at = [&](size_t off) -> std::optional<Packet> {
        if (off + 2 > data_size) return std::nullopt;
        const size_t size = le16(bytes.data() + data + off);
        if (off + 2 + size > data_size) return std::nullopt;
        return Packet{data + off + 2, size};
    };
    auto setup_packet = packet_at(setup_offset);
    if (!setup_packet) fail("setup packet out of range");
    BitReader sr(bytes.subspan(setup_packet->offset, setup_packet->size));
    const Setup setup = read_setup(sr, channels);

    std::vector<Packet> packets;
    for (size_t off = audio_offset; off < data_size;) {
        auto p = packet_at(off);
        if (!p) break;
        packets.push_back(*p);
        off += 2 + p->size;
    }

    // mode (and its blockflag) of each packet, read up front for the window shapes
    auto mode_of = [&](const Packet& p) -> int {
        if (!p.size) return -1;
        BitReader r(bytes.subspan(p.offset, p.size));
        if (!mod_packets && r.read(1)) return -1;
        const uint32_t m = r.read(setup.mode_bits);
        return m < setup.modes.size() ? int(m) : -1;
    };
    std::vector<int> modes(packets.size());
    for (size_t i = 0; i < packets.size(); ++i) modes[i] = mode_of(packets[i]);

    std::vector<std::vector<float>> planes(channels);
    std::vector<std::vector<float>> residue(channels), floor(channels);
    std::vector<float> block;
    std::vector<int> floor_y;
    int64_t start = int64_t(bs1), first_center = -1, last_center = 0;
    // Positions start one long block in: a short first block followed by a long one would otherwise
    // place the long block before sample 0. Output is taken from the first block centre either way.
    size_t prev_n = 0;
    bool prev_long = false;
    for (size_t pi = 0; pi < packets.size(); ++pi) {
        if (modes[pi] < 0) continue;
        const Mode& mode = setup.modes[size_t(modes[pi])];
        const Mapping& map = setup.mappings[mode.mapping];
        const size_t n = mode.blockflag ? bs1 : bs0, n2 = n / 2;
        BitReader r(bytes.subspan(packets[pi].offset, packets[pi].size));
        if (!mod_packets) r.read(1);
        r.read(setup.mode_bits);
        bool prev_flag = prev_long, next_flag = false;
        if (mode.blockflag) {
            if (mod_packets) {
                for (size_t k = pi + 1; k < packets.size(); ++k)
                    if (modes[k] >= 0) { next_flag = setup.modes[size_t(modes[k])].blockflag; break; }
            } else {
                prev_flag = r.read(1);
                next_flag = r.read(1);
            }
        }

        // floors
        std::vector<uint8_t> unused(channels, 0);
        for (uint32_t c = 0; c < channels; ++c) {
            const uint32_t sub = map.mux[c];
            if (!decode_floor1(setup.floors[map.submap_floor[sub]], setup, r, floor_y)) {
                unused[c] = 1;
                continue;
            }
            synthesize_floor1(setup.floors[map.submap_floor[sub]], floor_y, n2, floor[c]);
        }
        std::vector<uint8_t> no_residue = unused;
        for (auto [mag, ang] : map.coupling)
            if (!no_residue[mag] || !no_residue[ang]) no_residue[mag] = no_residue[ang] = 0;
        // residues
        for (uint32_t c = 0; c < channels; ++c) residue[c].assign(n2, 0.0f);
        for (uint32_t sub = 0; sub < map.submaps; ++sub) {
            std::vector<std::vector<float>*> vecs;
            std::vector<uint8_t> skip;
            for (uint32_t c = 0; c < channels; ++c)
                if (map.mux[c] == sub) vecs.push_back(&residue[c]), skip.push_back(no_residue[c]);
            decode_residue(setup.residues[map.submap_residue[sub]], setup, r, n2, vecs, skip);
        }
        // inverse coupling
        for (auto it = map.coupling.rbegin(); it != map.coupling.rend(); ++it) {
            auto& M = residue[it->first];
            auto& A = residue[it->second];
            for (size_t i = 0; i < n2; ++i) {
                const float m = M[i], a = A[i];
                float nm, na;
                if (m > 0) {
                    if (a > 0) nm = m, na = m - a;
                    else na = m, nm = m + a;
                } else {
                    if (a > 0) nm = m, na = m + a;
                    else na = m, nm = m - a;
                }
                M[i] = nm, A[i] = na;
            }
        }

        // window for this block
        size_t left_start, left_n, right_start, right_n;
        if (mode.blockflag && !prev_flag) left_start = n / 4 - bs0 / 4, left_n = bs0 / 2;
        else left_start = 0, left_n = n / 2;
        if (mode.blockflag && !next_flag) right_start = n * 3 / 4 - bs0 / 4, right_n = bs0 / 2;
        else right_start = n / 2, right_n = n / 2;

        if (prev_n) start += int64_t(prev_n * 3 / 4) - int64_t(n / 4);
        const int64_t center = start + int64_t(n / 2);
        block.resize(n);
        for (uint32_t c = 0; c < channels; ++c) {
            auto& plane = planes[c];
            if (plane.size() < size_t(start) + n) plane.resize(size_t(start) + n, 0.0f);
            if (unused[c]) continue;
            for (size_t i = 0; i < n2; ++i) residue[c][i] *= floor[c][i];
            inverse_mdct(residue[c], block);
            float* dst = plane.data() + start;
            for (size_t i = 0; i < n; ++i) {
                float w;
                if (i < left_start) w = 0;
                else if (i < left_start + left_n) w = window_slope(i - left_start, left_n);
                else if (i < right_start) w = 1;
                else if (i < right_start + right_n) w = window_slope(right_n - 1 - (i - right_start), right_n);
                else w = 0;
                dst[i] += block[i] * w;
            }
        }
        if (first_center < 0) first_center = center;  // the first block only primes the overlap
        last_center = center;
        prev_n = n;
        prev_long = mode.blockflag;
    }

    PcmAudio pcm;
    pcm.sample_rate = sample_rate;
    pcm.channels = channels;
    if (first_center < 0) return pcm;
    size_t frames = size_t(std::max<int64_t>(0, last_center - first_center));
    if (sample_count) frames = std::min<size_t>(frames, sample_count);
    pcm.samples.resize(frames * channels);
    for (size_t i = 0; i < frames; ++i)
        for (uint32_t c = 0; c < channels; ++c) {
            const size_t src = size_t(first_center) + i;
            const float v = src < planes[c].size() ? planes[c][src] : 0.0f;
            pcm.samples[i * channels + c] = int16_t(std::clamp(std::lrint(v * 32767.0f), -32768l, 32767l));
        }
    return pcm;
}

struct Chunks {
    size_t fmt = 0, fmt_size = 0, data = 0, data_size = 0;
};

std::optional<Chunks> riff_chunks(std::span<const uint8_t> b) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) return std::nullopt;
    Chunks c;
    for (size_t o = 12; o + 8 <= b.size();) {
        const uint32_t size = le32(b.data() + o + 4);
        if (std::memcmp(b.data() + o, "fmt ", 4) == 0) c.fmt = o + 8, c.fmt_size = size;
        if (std::memcmp(b.data() + o, "data", 4) == 0) c.data = o + 8, c.data_size = std::min<size_t>(size, b.size() - o - 8);
        o += 8 + size + (size & 1);
    }
    if (!c.fmt || c.fmt_size < 0x10 || c.fmt + c.fmt_size > b.size() || !c.data) return std::nullopt;
    return c;
}

} // namespace

std::optional<WemInfo> probe_wem(std::span<const uint8_t> bytes) {
    auto c = riff_chunks(bytes);
    if (!c) return std::nullopt;
    const uint8_t* f = bytes.data() + c->fmt;
    WemInfo i;
    i.codec = le16(f);
    i.channels = le16(f + 2);
    i.sample_rate = le32(f + 4);
    if (i.codec == 0xFFFF && c->fmt_size >= 0x1C) i.sample_count = le32(f + 0x18);
    else if ((i.codec == 1 || i.codec == 0xFFFE) && i.channels) i.sample_count = uint32_t(c->data_size / (2u * i.channels));
    return i;
}

const char* wem_codec_name(uint16_t codec) {
    switch (codec) {
    case 0xFFFF: return "Vorbis";
    case 0x0001: case 0xFFFE: return "PCM";
    case 0x0002: return "IMA ADPCM";
    case 0x3040: case 0x3041: return "Opus";
    default: return "?";
    }
}

PcmAudio decode_wem(std::span<const uint8_t> bytes) {
    if (bytes.size() < 12) fail("no recording in this language (placeholder entry)");
    auto c = riff_chunks(bytes);
    if (!c) fail("not a RIFF/WAVE stream");
    const uint8_t* f = bytes.data() + c->fmt;
    const uint16_t codec = le16(f), channels = le16(f + 2);
    const uint32_t rate = le32(f + 4);
    if (codec == 0xFFFF) return decode_vorbis(bytes, c->fmt, c->fmt_size, c->data, c->data_size, channels, rate);
    if ((codec == 1 || codec == 0xFFFE) && le16(f + 14) == 16 && channels) {
        PcmAudio pcm;
        pcm.sample_rate = rate;
        pcm.channels = channels;
        pcm.samples.resize(c->data_size / 2);
        std::memcpy(pcm.samples.data(), bytes.data() + c->data, pcm.samples.size() * 2);
        return pcm;
    }
    fail(std::format("codec {:#06x} ({}) is not supported", codec, wem_codec_name(codec)));
}

} // namespace nim
