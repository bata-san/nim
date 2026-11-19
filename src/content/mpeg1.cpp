// MPEG-1 video (ISO/IEC 11172-2) decoder with CRI's intra DC precision extension. See mpeg1.hpp.
#include "nim/content/mpeg1.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <execution>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace nim {
namespace {

// ---- bit reader -----------------------------------------------------------------------------------
struct Bits {
    const uint8_t* data;
    size_t size;  // bytes
    size_t pos = 0;  // bits
    uint32_t peek(int n) const {
        const size_t byte = pos >> 3;
        uint64_t v = 0;
        for (int i = 0; i < 5; ++i) v = v << 8 | (byte + i < size ? data[byte + i] : 0);
        return uint32_t((v << (24 + (pos & 7))) >> (64 - n)) & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
    }
    uint32_t read(int n) {
        const uint32_t v = peek(n);
        pos += n;
        return v;
    }
    void skip(int n) { pos += n; }
    bool exhausted() const { return pos >= size * 8; }
};

// ---- VLC tables -----------------------------------------------------------------------------------
struct VlcCode {
    const char* bits;
    int16_t a, b;
};
struct VlcEntry {
    int16_t a = 0, b = 0;
    uint8_t len = 0;  // 0 = invalid
};
struct Vlc {
    int bits = 0;
    std::vector<VlcEntry> table;
    Vlc(std::initializer_list<VlcCode> codes) {
        for (const auto& c : codes) bits = std::max(bits, int(std::strlen(c.bits)));
        table.resize(size_t(1) << bits);
        for (const auto& c : codes) {
            const int len = int(std::strlen(c.bits));
            uint32_t v = 0;
            for (int i = 0; i < len; ++i) v = v << 1 | uint32_t(c.bits[i] == '1');
            const uint32_t lo = v << (bits - len), hi = (v + 1) << (bits - len);
            for (uint32_t i = lo; i < hi; ++i) table[i] = {c.a, c.b, uint8_t(len)};
        }
    }
    const VlcEntry& decode(Bits& br) const {
        const VlcEntry& e = table[br.peek(bits)];
        if (!e.len) throw std::runtime_error("bad VLC");
        br.skip(e.len);
        return e;
    }
};

constexpr int16_t kEob = -1, kEscape = -2, kStuffing = -3, kAddrEscape = -4;

const Vlc& addr_vlc() {
    static const Vlc v{{"1", 1, 0}, {"011", 2, 0}, {"010", 3, 0}, {"0011", 4, 0}, {"0010", 5, 0}, {"00011", 6, 0},
                       {"00010", 7, 0}, {"0000111", 8, 0}, {"0000110", 9, 0}, {"00001011", 10, 0}, {"00001010", 11, 0},
                       {"00001001", 12, 0}, {"00001000", 13, 0}, {"00000111", 14, 0}, {"00000110", 15, 0},
                       {"0000010111", 16, 0}, {"0000010110", 17, 0}, {"0000010101", 18, 0}, {"0000010100", 19, 0},
                       {"0000010011", 20, 0}, {"0000010010", 21, 0}, {"00000100011", 22, 0}, {"00000100010", 23, 0},
                       {"00000100001", 24, 0}, {"00000100000", 25, 0}, {"00000011111", 26, 0}, {"00000011110", 27, 0},
                       {"00000011101", 28, 0}, {"00000011100", 29, 0}, {"00000011011", 30, 0}, {"00000011010", 31, 0},
                       {"00000011001", 32, 0}, {"00000011000", 33, 0}, {"00000001111", kStuffing, 0},
                       {"00000001000", kAddrEscape, 0}};
    return v;
}

enum : int16_t { MbQuant = 1, MbFwd = 2, MbBwd = 4, MbPattern = 8, MbIntra = 16 };
const Vlc& mbtype_vlc(char type) {
    static const Vlc i{{"1", MbIntra, 0}, {"01", MbIntra | MbQuant, 0}};
    static const Vlc p{{"1", MbFwd | MbPattern, 0}, {"01", MbPattern, 0}, {"001", MbFwd, 0}, {"00011", MbIntra, 0},
                       {"00010", MbFwd | MbPattern | MbQuant, 0}, {"00001", MbPattern | MbQuant, 0},
                       {"000001", MbIntra | MbQuant, 0}};
    static const Vlc b{{"10", MbFwd | MbBwd, 0}, {"11", MbFwd | MbBwd | MbPattern, 0}, {"010", MbBwd, 0},
                       {"011", MbBwd | MbPattern, 0}, {"0010", MbFwd, 0}, {"0011", MbFwd | MbPattern, 0},
                       {"00011", MbIntra, 0}, {"00010", MbFwd | MbBwd | MbPattern | MbQuant, 0},
                       {"000011", MbFwd | MbPattern | MbQuant, 0}, {"000010", MbBwd | MbPattern | MbQuant, 0},
                       {"000001", MbIntra | MbQuant, 0}};
    return type == 'I' ? i : type == 'P' ? p : b;
}

const Vlc& cbp_vlc() {
    static const Vlc v{{"111", 60, 0},       {"1101", 4, 0},      {"1100", 8, 0},       {"1011", 16, 0},
                       {"1010", 32, 0},      {"10011", 12, 0},    {"10010", 48, 0},     {"10001", 20, 0},
                       {"10000", 40, 0},     {"01111", 28, 0},    {"01110", 44, 0},     {"01101", 52, 0},
                       {"01100", 56, 0},     {"01011", 1, 0},     {"01010", 61, 0},     {"01001", 2, 0},
                       {"01000", 62, 0},     {"001111", 24, 0},   {"001110", 36, 0},    {"001101", 3, 0},
                       {"001100", 63, 0},    {"0010111", 5, 0},   {"0010110", 9, 0},    {"0010101", 17, 0},
                       {"0010100", 33, 0},   {"0010011", 6, 0},   {"0010010", 10, 0},   {"0010001", 18, 0},
                       {"0010000", 34, 0},   {"00011111", 7, 0},  {"00011110", 11, 0},  {"00011101", 19, 0},
                       {"00011100", 35, 0},  {"00011011", 13, 0}, {"00011010", 49, 0},  {"00011001", 21, 0},
                       {"00011000", 41, 0},  {"00010111", 14, 0}, {"00010110", 50, 0},  {"00010101", 22, 0},
                       {"00010100", 42, 0},  {"00010011", 15, 0}, {"00010010", 51, 0},  {"00010001", 23, 0},
                       {"00010000", 43, 0},  {"00001111", 25, 0}, {"00001110", 37, 0},  {"00001101", 26, 0},
                       {"00001100", 38, 0},  {"00001011", 29, 0}, {"00001010", 45, 0},  {"00001001", 53, 0},
                       {"00001000", 57, 0},  {"00000111", 30, 0}, {"00000110", 46, 0},  {"00000101", 54, 0},
                       {"00000100", 58, 0},  {"000000111", 31, 0}, {"000000110", 47, 0}, {"000000101", 55, 0},
                       {"000000100", 59, 0}, {"000000011", 27, 0}, {"000000010", 39, 0}, {"000000001", 0, 0}};
    return v;
}

const Vlc& motion_vlc() {
    static const Vlc v = [] {
        static const char* mag[17] = {"1",          "01",         "001",        "0001",       "000011",    "0000101",
                                      "0000100",    "0000011",    "000001011",  "000001010",  "000001001", "0000010001",
                                      "0000010000", "0000001111", "0000001110", "0000001101", "0000001100"};
        static std::vector<std::string> keep;
        std::vector<VlcCode> codes;
        keep.reserve(40);
        codes.push_back({mag[0], 0, 0});
        for (int m = 1; m <= 16; ++m) {
            keep.push_back(std::string(mag[m]) + "0");
            codes.push_back({keep.back().c_str(), int16_t(m), 0});
            keep.push_back(std::string(mag[m]) + "1");
            codes.push_back({keep.back().c_str(), int16_t(-m), 0});
        }
        Vlc out{};
        out.bits = 0;
        for (const auto& c : codes) out.bits = std::max(out.bits, int(std::strlen(c.bits)));
        out.table.resize(size_t(1) << out.bits);
        for (const auto& c : codes) {
            const int len = int(std::strlen(c.bits));
            uint32_t x = 0;
            for (int i = 0; i < len; ++i) x = x << 1 | uint32_t(c.bits[i] == '1');
            for (uint32_t i = x << (out.bits - len); i < (x + 1) << (out.bits - len); ++i) out.table[i] = {c.a, 0, uint8_t(len)};
        }
        return out;
    }();
    return v;
}

// Intra DC size, MPEG-2 tables (sizes up to 11 for 11-bit precision; MPEG-1's are a prefix of these).
const Vlc& dc_luma_vlc() {
    static const Vlc v{{"100", 0, 0},      {"00", 1, 0},        {"01", 2, 0},         {"101", 3, 0},
                       {"110", 4, 0},      {"1110", 5, 0},      {"11110", 6, 0},      {"111110", 7, 0},
                       {"1111110", 8, 0},  {"11111110", 9, 0},  {"111111110", 10, 0}, {"111111111", 11, 0}};
    return v;
}
const Vlc& dc_chroma_vlc() {
    static const Vlc v{{"00", 0, 0},        {"01", 1, 0},         {"10", 2, 0},          {"110", 3, 0},
                       {"1110", 4, 0},      {"11110", 5, 0},      {"111110", 6, 0},      {"1111110", 7, 0},
                       {"11111110", 8, 0},  {"111111110", 9, 0},  {"1111111110", 10, 0}, {"1111111111", 11, 0}};
    return v;
}

// DCT coefficients (run, level), sign bit not included. "10" is end of block, "000001" escape.
const Vlc& dct_vlc() {
    static const Vlc v{
        {"10", kEob, 0}, {"11", 0, 1}, {"011", 1, 1}, {"0100", 0, 2}, {"0101", 2, 1}, {"00101", 0, 3}, {"00111", 3, 1},
        {"00110", 4, 1}, {"000110", 1, 2}, {"000111", 5, 1}, {"000101", 6, 1}, {"000100", 7, 1}, {"0000110", 0, 4},
        {"0000100", 2, 2}, {"0000111", 8, 1}, {"0000101", 9, 1}, {"000001", kEscape, 0}, {"00100110", 0, 5},
        {"00100001", 0, 6}, {"00100101", 1, 3}, {"00100100", 3, 2}, {"00100111", 10, 1}, {"00100011", 11, 1},
        {"00100010", 12, 1}, {"00100000", 13, 1}, {"0000001010", 0, 7}, {"0000001100", 1, 4}, {"0000001011", 2, 3},
        {"0000001111", 4, 2}, {"0000001001", 5, 2}, {"0000001110", 14, 1}, {"0000001101", 15, 1},
        {"0000001000", 16, 1}, {"000000011101", 0, 8}, {"000000011000", 0, 9}, {"000000010011", 0, 10},
        {"000000010000", 0, 11}, {"000000011011", 1, 5}, {"000000010100", 2, 4}, {"000000011100", 3, 3},
        {"000000010010", 4, 3}, {"000000011110", 6, 2}, {"000000010101", 7, 2}, {"000000010001", 8, 2},
        {"000000011111", 17, 1}, {"000000011010", 18, 1}, {"000000011001", 19, 1}, {"000000010111", 20, 1},
        {"000000010110", 21, 1}, {"0000000011010", 0, 12}, {"0000000011001", 0, 13}, {"0000000011000", 0, 14},
        {"0000000010111", 0, 15}, {"0000000010110", 1, 6}, {"0000000010101", 1, 7}, {"0000000010100", 2, 5},
        {"0000000010011", 3, 4}, {"0000000010010", 5, 3}, {"0000000010001", 9, 2}, {"0000000010000", 10, 2},
        {"0000000011111", 22, 1}, {"0000000011110", 23, 1}, {"0000000011101", 24, 1}, {"0000000011100", 25, 1},
        {"0000000011011", 26, 1}, {"00000000011111", 0, 16}, {"00000000011110", 0, 17}, {"00000000011101", 0, 18},
        {"00000000011100", 0, 19}, {"00000000011011", 0, 20}, {"00000000011010", 0, 21}, {"00000000011001", 0, 22},
        {"00000000011000", 0, 23}, {"00000000010111", 0, 24}, {"00000000010110", 0, 25}, {"00000000010101", 0, 26},
        {"00000000010100", 0, 27}, {"00000000010011", 0, 28}, {"00000000010010", 0, 29}, {"00000000010001", 0, 30},
        {"00000000010000", 0, 31}, {"000000000011000", 0, 32}, {"000000000010111", 0, 33},
        {"000000000010110", 0, 34}, {"000000000010101", 0, 35}, {"000000000010100", 0, 36},
        {"000000000010011", 0, 37}, {"000000000010010", 0, 38}, {"000000000010001", 0, 39},
        {"000000000010000", 0, 40}, {"000000000011111", 1, 8}, {"000000000011110", 1, 9}, {"000000000011101", 1, 10},
        {"000000000011100", 1, 11}, {"000000000011011", 1, 12}, {"000000000011010", 1, 13},
        {"000000000011001", 1, 14}, {"0000000000010011", 1, 15}, {"0000000000010010", 1, 16},
        {"0000000000010001", 1, 17}, {"0000000000010000", 1, 18}, {"0000000000010100", 6, 3},
        {"0000000000011010", 11, 2}, {"0000000000011001", 12, 2}, {"0000000000011000", 13, 2},
        {"0000000000010111", 14, 2}, {"0000000000010110", 15, 2}, {"0000000000010101", 16, 2},
        {"0000000000011111", 27, 1}, {"0000000000011110", 28, 1}, {"0000000000011101", 29, 1},
        {"0000000000011100", 30, 1}, {"0000000000011011", 31, 1}};
    return v;
}

constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                                 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                                 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                                 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
constexpr uint8_t kDefaultIntra[64] = {8,  16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
                                       19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
                                       22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
                                       26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83};

// ---- IDCT (separable, double-precision basis; meets IEEE 1180 accuracy) ---------------------------------
struct IdctBasis {
    float m[8][8];  // m[x][u] = C(u)/2 cos((2x+1)u pi/16)
    IdctBasis() {
        for (int x = 0; x < 8; ++x)
            for (int u = 0; u < 8; ++u)
                m[x][u] = float((u ? 0.5 : 0.5 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16));
    }
};
const IdctBasis& basis() {
    static const IdctBasis b;
    return b;
}

// coefficients in raster order -> residual samples (rounded, not clamped)
void idct(const int* in, int* out) {
    const auto& m = basis().m;
    float tmp[64];
    for (int v = 0; v < 8; ++v) {  // rows of coefficients (vertical frequency v): horizontal transform
        const int* r = in + v * 8;
        float* t = tmp + v * 8;
        if (!(r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) {
            const float d = r[0] * m[0][0];
            for (int x = 0; x < 8; ++x) t[x] = d;
            continue;
        }
        for (int x = 0; x < 8; ++x) {
            float s = 0;
            for (int u = 0; u < 8; ++u) s += m[x][u] * float(r[u]);
            t[x] = s;
        }
    }
    for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y) {
            float s = 0;
            for (int v = 0; v < 8; ++v) s += m[y][v] * tmp[v * 8 + x];
            out[y * 8 + x] = int(std::lrint(s));
        }
}

uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

// ---- frames ---------------------------------------------------------------------------------------
struct Mpeg1Decoder::Frame {
    uint32_t stride_y = 0, stride_c = 0, rows_y = 0, rows_c = 0;
    std::vector<uint8_t> y, cb, cr;
    char type = 'I';
    void alloc(uint32_t mb_w, uint32_t mb_h) {
        stride_y = mb_w * 16, stride_c = mb_w * 8, rows_y = mb_h * 16, rows_c = mb_h * 8;
        y.assign(size_t(stride_y) * rows_y, 16);
        cb.assign(size_t(stride_c) * rows_c, 128);
        cr.assign(size_t(stride_c) * rows_c, 128);
    }
};

namespace {

// Motion-compensated prediction of an n x n block at (x, y) with a half-pel vector, clamped to the plane.
void predict(const uint8_t* src, uint32_t stride, uint32_t rows, int x, int y, int n, int dx, int dy, uint8_t* dst,
             uint32_t dst_stride, bool average) {
    const int sx = x + (dx >> 1), sy = y + (dy >> 1), hx = dx & 1, hy = dy & 1;
    const int maxx = int(stride) - 1, maxy = int(rows) - 1;
    const bool inside = sx >= 0 && sy >= 0 && sx + n + hx <= maxx + 1 && sy + n + hy <= maxy + 1;
    auto at = [&](int px, int py) -> int {
        if (!inside) px = std::clamp(px, 0, maxx), py = std::clamp(py, 0, maxy);
        return src[size_t(py) * stride + size_t(px)];
    };
    for (int j = 0; j < n; ++j) {
        uint8_t* d = dst + size_t(j) * dst_stride;
        for (int i = 0; i < n; ++i) {
            int v;
            if (!hx && !hy) v = at(sx + i, sy + j);
            else if (hx && !hy) v = (at(sx + i, sy + j) + at(sx + i + 1, sy + j) + 1) >> 1;
            else if (!hx) v = (at(sx + i, sy + j) + at(sx + i, sy + j + 1) + 1) >> 1;
            else v = (at(sx + i, sy + j) + at(sx + i + 1, sy + j) + at(sx + i, sy + j + 1) + at(sx + i + 1, sy + j + 1) + 2) >> 2;
            d[i] = average ? uint8_t((d[i] + v + 1) >> 1) : uint8_t(v);
        }
    }
}

struct MotionVector {
    int h = 0, v = 0;  // reconstructed, in full-pel units when full_pel, else half-pel
};

int decode_motion_component(Bits& br, int f_code, int pred) {
    const int r_size = f_code - 1, f = 1 << r_size;
    const int code = motion_vlc().decode(br).a;
    int d = code;
    if (code != 0 && f != 1) {
        const int r = int(br.read(r_size));
        d = ((std::abs(code) - 1) << r_size) + r + 1;
        if (code < 0) d = -d;
    }
    int m = pred + d;
    if (m > (f << 4) - 1) m -= f << 5;
    else if (m < -(f << 4)) m += f << 5;
    return m;
}

} // namespace

Mpeg1Decoder::Mpeg1Decoder(Output out) : out_(std::move(out)) {
    std::memcpy(intra_q_, kDefaultIntra, 64);
    std::fill(std::begin(non_intra_q_), std::end(non_intra_q_), uint8_t(16));
}
Mpeg1Decoder::~Mpeg1Decoder() = default;

void Mpeg1Decoder::reset() {
    buf_.clear();
    scan_ = 0;
    unit_ = SIZE_MAX;
    picture_open_ = false;
    slices_.clear();
    fwd_ = bwd_ = nullptr;
    bwd_pending_ = false;
}

void Mpeg1Decoder::feed(std::span<const uint8_t> es) {
    buf_.insert(buf_.end(), es.begin(), es.end());
    process(false);
}

void Mpeg1Decoder::flush() {
    process(true);
    if (bwd_ && bwd_pending_) emit(*bwd_);
    bwd_pending_ = false;
}

void Mpeg1Decoder::process(bool final) {
    for (;;) {
        size_t i = scan_;
        const size_t n = buf_.size();
        bool found = false;
        for (; i + 3 < n; ++i)
            if (buf_[i] == 0 && buf_[i + 1] == 0 && buf_[i + 2] == 1) {
                found = true;
                break;
            }
        if (!found) {
            scan_ = n >= 3 ? n - 3 : 0;
            if (final) {
                if (unit_ != SIZE_MAX) handle_unit(unit_, n);
                unit_ = SIZE_MAX;
                if (picture_open_) finish_picture();
            }
            break;
        }
        const uint8_t code = buf_[i + 3];
        if (unit_ != SIZE_MAX) handle_unit(unit_, i);
        if (picture_open_ && (code == 0x00 || code == 0xB3 || code == 0xB7 || code == 0xB8)) finish_picture();
        unit_ = i;
        scan_ = i + 4;
        // Drop consumed bytes when no picture holds references into the buffer.
        if (!picture_open_ && unit_ > 0x100000) {
            buf_.erase(buf_.begin(), buf_.begin() + std::ptrdiff_t(unit_));
            scan_ -= unit_;
            unit_ = 0;
        }
    }
}

void Mpeg1Decoder::handle_unit(size_t begin, size_t end) {
    if (end < begin + 4) return;
    const uint8_t code = buf_[begin + 3];
    std::span<const uint8_t> body(buf_.data() + begin + 4, end - begin - 4);
    if (code == 0xB3) {  // sequence header
        Bits br{body.data(), body.size()};
        const uint32_t w = br.read(12), h = br.read(12);
        br.skip(4 + 4 + 18 + 1 + 10 + 1);
        if (br.read(1)) {
            for (int k = 0; k < 64; ++k) intra_q_[kZigzag[k]] = uint8_t(br.read(8));
        } else {
            std::memcpy(intra_q_, kDefaultIntra, 64);
        }
        if (br.read(1)) {
            for (int k = 0; k < 64; ++k) non_intra_q_[kZigzag[k]] = uint8_t(br.read(8));
        } else {
            std::fill(std::begin(non_intra_q_), std::end(non_intra_q_), uint8_t(16));
        }
        if (w != width_ || h != height_ || !have_sequence_) {
            width_ = w, height_ = h, mb_w_ = (w + 15) / 16, mb_h_ = (h + 15) / 16;
            frames_.clear();
            fwd_ = bwd_ = nullptr;
            bwd_pending_ = false;
        }
        have_sequence_ = true;
    } else if (code == 0xB2) {  // user data: CRI writes "IDCPREC\0" + 8-digit length + 8-digit value
        static const char key[] = "IDCPREC";
        auto it = std::search(body.begin(), body.end(), key, key + 7);
        if (it != body.end() && size_t(body.end() - it) >= 8 + 16) {
            const char* v = reinterpret_cast<const char*>(&*it) + 16;
            int p = 0;
            for (int k = 0; k < 8; ++k) p = p * 10 + (v[k] >= '0' && v[k] <= '9' ? v[k] - '0' : 0);
            if (p >= 0 && p <= 3) dc_precision_ = p;
        }
    } else if (code == 0x00) {
        if (have_sequence_) begin_picture(body);
    } else if (code >= 0x01 && code <= 0xAF) {
        if (picture_open_) slices_.push_back({begin + 4, end - begin - 4, code});
    }
}

void Mpeg1Decoder::begin_picture(std::span<const uint8_t> header) {
    Bits br{header.data(), header.size()};
    br.skip(10);  // temporal reference
    const uint32_t t = br.read(3);
    br.skip(16);  // vbv delay
    if (t < 1 || t > 3) return;  // D pictures are not used
    pic_type_ = "?IPB"[t];
    if (t >= 2) full_pel_fwd_ = br.read(1), f_code_fwd_ = int(br.read(3));
    if (t == 3) full_pel_bwd_ = br.read(1), f_code_bwd_ = int(br.read(3));
    // A P picture needs a reference; B pictures can manage with the backward one (closed GOPs).
    if ((pic_type_ == 'P' && !bwd_) || (pic_type_ == 'B' && !bwd_)) return;
    picture_open_ = true;
    slices_.clear();
}

Mpeg1Decoder::Frame* Mpeg1Decoder::free_frame() {
    for (auto& f : frames_)
        if (f.get() != fwd_ && f.get() != bwd_) return f.get();
    frames_.push_back(std::make_unique<Frame>());
    frames_.back()->alloc(mb_w_, mb_h_);
    return frames_.back().get();
}

void Mpeg1Decoder::finish_picture() {
    picture_open_ = false;
    Frame* cur = free_frame();
    cur->type = pic_type_;
    // Macroblocks not covered by any slice keep the previous content of the buffer; start from the
    // reference (or grey) so damage stays local.
    if (pic_type_ != 'I' && bwd_) cur->y = bwd_->y, cur->cb = bwd_->cb, cur->cr = bwd_->cr;
    std::for_each(std::execution::par, slices_.begin(), slices_.end(), [&](const Slice& s) {
        try {
            decode_slice(s, *cur);
        } catch (...) {
            ++slice_errors_;
        }
    });
    slices_.clear();
    if (pic_type_ == 'B') {
        emit(*cur);
    } else {
        if (bwd_ && bwd_pending_) emit(*bwd_);
        fwd_ = bwd_;
        bwd_ = cur;
        bwd_pending_ = true;
    }
}

void Mpeg1Decoder::emit(const Frame& f) {
    Mpeg1Picture p;
    p.width = width_, p.height = height_;
    p.stride_y = f.stride_y, p.stride_c = f.stride_c;
    p.y = f.y.data(), p.cb = f.cb.data(), p.cr = f.cr.data();
    p.type = f.type;
    if (out_) out_(p);
}

void Mpeg1Decoder::decode_slice(const Slice& s, Frame& cur) {
    Bits br{buf_.data() + s.offset, s.size};
    if (s.row < 1 || uint32_t(s.row) > mb_h_) return;
    int qscale = int(br.read(5));
    while (br.read(1)) br.skip(8);

    const char type = pic_type_;
    const Frame* fref = type == 'P' ? bwd_ : (fwd_ ? fwd_ : bwd_);  // forward reference
    const Frame* bref = bwd_;                                      // backward reference (B only)
    const int dc_reset = 1 << (7 + dc_precision_);
    const int dc_mult = 8 >> dc_precision_;
    int dc_pred[3] = {dc_reset, dc_reset, dc_reset};
    MotionVector mv_fwd, mv_bwd;  // predictors (reconstructed vectors)
    int last_flags = 0;           // B: motion flags of the previous macroblock, for skipped ones
    const int mb_count = int(mb_w_ * mb_h_);
    int addr = (s.row - 1) * int(mb_w_) - 1;
    bool first = true;

    const Vlc& vt = mbtype_vlc(type);
    alignas(16) int coef[64];
    alignas(16) int res[64];
    alignas(16) uint8_t pred_y[256], pred_cb[64], pred_cr[64];

    auto vec = [&](const MotionVector& m, bool full_pel) { return MotionVector{full_pel ? m.h << 1 : m.h, full_pel ? m.v << 1 : m.v}; };

    // Builds the prediction for macroblock (mx, my) into pred_*.
    auto build_prediction = [&](int mx, int my, bool use_f, bool use_b, MotionVector f, MotionVector b) {
        bool have = false;
        auto one = [&](const Frame* ref, MotionVector v) {
            if (!ref) return;
            const int x = mx * 16, y = my * 16;
            predict(ref->y.data(), ref->stride_y, ref->rows_y, x, y, 16, v.h, v.v, pred_y, 16, have);
            const int ch = v.h / 2, cv = v.v / 2;
            predict(ref->cb.data(), ref->stride_c, ref->rows_c, x / 2, y / 2, 8, ch, cv, pred_cb, 8, have);
            predict(ref->cr.data(), ref->stride_c, ref->rows_c, x / 2, y / 2, 8, ch, cv, pred_cr, 8, have);
            have = true;
        };
        if (use_f) one(fref, f);
        if (use_b) one(bref, b);
        if (!have) {
            std::memset(pred_y, 128, sizeof pred_y);
            std::memset(pred_cb, 128, sizeof pred_cb);
            std::memset(pred_cr, 128, sizeof pred_cr);
        }
    };
    auto store_prediction = [&](int mx, int my) {
        for (int j = 0; j < 16; ++j) std::memcpy(&cur.y[size_t(my * 16 + j) * cur.stride_y + mx * 16], pred_y + j * 16, 16);
        for (int j = 0; j < 8; ++j) {
            std::memcpy(&cur.cb[size_t(my * 8 + j) * cur.stride_c + mx * 8], pred_cb + j * 8, 8);
            std::memcpy(&cur.cr[size_t(my * 8 + j) * cur.stride_c + mx * 8], pred_cr + j * 8, 8);
        }
    };
    auto skipped = [&](int a) {
        const int mx = a % int(mb_w_), my = a / int(mb_w_);
        if (type == 'P') {
            build_prediction(mx, my, true, false, {}, {});
            mv_fwd = {};
        } else {
            build_prediction(mx, my, last_flags & MbFwd, last_flags & MbBwd, vec(mv_fwd, full_pel_fwd_), vec(mv_bwd, full_pel_bwd_));
        }
        store_prediction(mx, my);
        dc_pred[0] = dc_pred[1] = dc_pred[2] = dc_reset;
    };

    // Reads one block's coefficients (raster order, dequantised). Returns false for an all-zero block.
    auto read_block = [&](int b, bool intra) {
        std::memset(coef, 0, sizeof coef);
        int n = 0;
        if (intra) {
            const int comp = b < 4 ? 0 : b - 3;
            const int size = (b < 4 ? dc_luma_vlc() : dc_chroma_vlc()).decode(br).a;
            int diff = 0;
            if (size) {
                const int v = int(br.read(size));
                diff = (v & (1 << (size - 1))) ? v : v - (1 << size) + 1;
            }
            dc_pred[comp] += diff;
            coef[0] = dc_pred[comp] * dc_mult;
            n = 1;
        } else if (br.peek(1)) {  // first coefficient of a non-intra block: "1s" = level 1
            br.skip(1);
            const int level = br.read(1) ? -1 : 1;
            int v = ((2 * std::abs(level) + 1) * qscale * non_intra_q_[0]) >> 4;
            if (!(v & 1)) v -= 1;
            coef[0] = level < 0 ? -v : v;
            n = 1;
        }
        const uint8_t* q = intra ? intra_q_ : non_intra_q_;
        for (;;) {
            const VlcEntry& e = dct_vlc().decode(br);
            int run, level;
            if (e.a == kEob) break;
            if (e.a == kEscape) {
                run = int(br.read(6));
                int l = int(br.read(8));
                if (l == 0) level = int(br.read(8));
                else if (l == 128) level = int(br.read(8)) - 256;
                else level = l > 128 ? l - 256 : l;
            } else {
                run = e.a;
                level = br.read(1) ? -e.b : e.b;
            }
            n += run;
            if (n > 63) throw std::runtime_error("coefficient index out of range");
            const int zz = kZigzag[n];
            const int a = std::abs(level);
            int v = intra ? (a * qscale * q[zz]) >> 3 : ((2 * a + 1) * qscale * q[zz]) >> 4;
            if (v && !(v & 1)) v -= 1;  // oddification (mismatch control)
            v = std::min(v, 2047);
            coef[zz] = level < 0 ? -std::min(v, 2048) : v;
            ++n;
        }
        return true;
    };
    auto put_block = [&](int b, bool add, const uint8_t* pred, int pred_stride, uint8_t* dst, uint32_t dst_stride) {
        bool dc_only = true;
        for (int k = 1; k < 64 && dc_only; ++k) dc_only = coef[k] == 0;
        if (dc_only) {
            const int d = int(std::lrint(coef[0] / 8.0));
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i) dst[j * dst_stride + i] = clamp8((add ? pred[j * pred_stride + i] : 0) + d);
        } else {
            idct(coef, res);
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                    dst[j * dst_stride + i] = clamp8((add ? pred[j * pred_stride + i] : 0) + res[j * 8 + i]);
        }
        (void)b;
    };

    for (;;) {
        // macroblock address increment (with stuffing and escapes)
        int inc = 0;
        for (;;) {
            const uint32_t v = br.peek(11);
            if (v == 0x0F) { br.skip(11); continue; }
            if (v == 0x08) { br.skip(11); inc += 33; continue; }
            break;
        }
        if (br.peek(23) == 0) break;  // end of slice
        inc += addr_vlc().decode(br).a;
        if (first) {
            addr += inc;
            first = false;
        } else {
            for (int k = 1; k < inc; ++k) {
                if (addr + k >= mb_count) throw std::runtime_error("macroblock address out of range");
                skipped(addr + k);
            }
            if (inc > 1 && type == 'P') mv_fwd = {};
            addr += inc;
        }
        if (addr < 0 || addr >= mb_count) throw std::runtime_error("macroblock address out of range");
        const int mx = addr % int(mb_w_), my = addr / int(mb_w_);

        const int flags = vt.decode(br).a;
        if (flags & MbQuant) qscale = int(br.read(5));
        const bool intra = flags & MbIntra;
        if (flags & MbFwd) {
            mv_fwd.h = decode_motion_component(br, f_code_fwd_, mv_fwd.h);
            mv_fwd.v = decode_motion_component(br, f_code_fwd_, mv_fwd.v);
        } else if (type == 'P' && !intra) {
            mv_fwd = {};
        }
        if (flags & MbBwd) {
            mv_bwd.h = decode_motion_component(br, f_code_bwd_, mv_bwd.h);
            mv_bwd.v = decode_motion_component(br, f_code_bwd_, mv_bwd.v);
        }
        const int cbp = (flags & MbPattern) ? cbp_vlc().decode(br).a : (intra ? 63 : 0);

        uint8_t* y0 = &cur.y[size_t(my * 16) * cur.stride_y + mx * 16];
        uint8_t* cb0 = &cur.cb[size_t(my * 8) * cur.stride_c + mx * 8];
        uint8_t* cr0 = &cur.cr[size_t(my * 8) * cur.stride_c + mx * 8];
        if (intra) {
            mv_fwd = {}, mv_bwd = {};
            for (int b = 0; b < 6; ++b) {
                read_block(b, true);
                uint8_t* dst = b < 4 ? y0 + size_t((b >> 1) * 8) * cur.stride_y + (b & 1) * 8 : b == 4 ? cb0 : cr0;
                put_block(b, false, nullptr, 0, dst, b < 4 ? cur.stride_y : cur.stride_c);
            }
            last_flags = 0;
        } else {
            dc_pred[0] = dc_pred[1] = dc_pred[2] = dc_reset;
            const bool use_f = type == 'P' || (flags & MbFwd);
            const bool use_b = type == 'B' && (flags & MbBwd);
            build_prediction(mx, my, use_f, use_b, vec(mv_fwd, full_pel_fwd_), vec(mv_bwd, full_pel_bwd_));
            last_flags = flags;
            for (int b = 0; b < 6; ++b) {
                const uint8_t* pred = b < 4 ? pred_y + (b >> 1) * 128 + (b & 1) * 8 : b == 4 ? pred_cb : pred_cr;
                const int ps = b < 4 ? 16 : 8;
                uint8_t* dst = b < 4 ? y0 + size_t((b >> 1) * 8) * cur.stride_y + (b & 1) * 8 : b == 4 ? cb0 : cr0;
                const uint32_t ds = b < 4 ? cur.stride_y : cur.stride_c;
                if (cbp & (32 >> b)) {
                    read_block(b, false);
                    put_block(b, true, pred, ps, dst, ds);
                } else {
                    for (int j = 0; j < 8; ++j) std::memcpy(dst + j * ds, pred + j * ps, 8);
                }
            }
        }
        if (br.exhausted()) break;
    }
}

void mpeg1_to_rgba(const Mpeg1Picture& p, uint32_t w, uint32_t h, uint8_t* out) {
    w = std::min(w, p.width), h = std::min(h, p.height);
    const int cw = int((p.width + 1) / 2), chh = int((p.height + 1) / 2);
    std::vector<uint32_t> rows(h);
    std::iota(rows.begin(), rows.end(), 0u);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](uint32_t row) {
        // Centred chroma: luma row r sits at chroma row r/2 - 0.25.
        const int c0 = int(row / 2), c1 = std::clamp((row & 1) ? c0 + 1 : c0 - 1, 0, chh - 1);
        const uint8_t* yr = p.y + size_t(row) * p.stride_y;
        const uint8_t* b0 = p.cb + size_t(c0) * p.stride_c;
        const uint8_t* b1 = p.cb + size_t(c1) * p.stride_c;
        const uint8_t* r0 = p.cr + size_t(c0) * p.stride_c;
        const uint8_t* r1 = p.cr + size_t(c1) * p.stride_c;
        uint8_t* o = out + size_t(row) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const int a0 = int(x / 2), a1 = std::clamp((x & 1) ? a0 + 1 : a0 - 1, 0, cw - 1);
            // weights 3/4 near, 1/4 far in each direction (sum 16)
            const int cbv = (9 * b0[a0] + 3 * b0[a1] + 3 * b1[a0] + b1[a1] + 8) >> 4;
            const int crv = (9 * r0[a0] + 3 * r0[a1] + 3 * r1[a0] + r1[a1] + 8) >> 4;
            const int C = 298 * (int(yr[x]) - 16), D = cbv - 128, E = crv - 128;
            o[0] = clamp8((C + 409 * E + 128) >> 8);
            o[1] = clamp8((C - 100 * D - 208 * E + 128) >> 8);
            o[2] = clamp8((C + 516 * D + 128) >> 8);
            o[3] = 255;
            o += 4;
        }
    });
}

} // namespace nim
