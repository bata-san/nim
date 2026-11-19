// CRI HCA decoder. Bitstream layout and tables follow the public clHCA / VGAudio descriptions; the
// constant tables are generated from their closed forms (checked against the published values).
#include "nim/content/hca.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace nim {
namespace {

struct Bits {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t peek(int n) const {
        if (n == 0) return 0;
        const size_t byte = pos >> 3;
        uint64_t v = 0;
        for (int i = 0; i < 5; ++i) v = v << 8 | (byte + i < size ? data[byte + i] : 0);
        return uint32_t((v << (24 + (pos & 7))) >> (64 - n));
    }
    uint32_t read(int n) {
        const uint32_t v = peek(n);
        pos += size_t(n);
        return v;
    }
    void skip(int n) { pos = size_t(int64_t(pos) + n); }
};

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Tables {
    float scaling[64];          // dequantizer: sqrt(128) * 2^((i-63)*53/128)
    float range[16];            // quantizer step: 2/(2^k+1 ...) per resolution
    float scale_conversion[128];  // 2^((i-63)*53/128), 0 at both ends
    float intensity[16];        // (14-i)/7
    float window[256];          // full symmetric window, rising half = |published table|
    float cos_basis[256][128];  // IMDCT basis
    Tables() {
        for (int i = 0; i < 64; ++i) scaling[i] = float(std::sqrt(128.0) * std::pow(2.0, (i - 63) * 53.0 / 128.0));
        static const int denom[16] = {1, 3, 5, 7, 9, 11, 13, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095};
        range[0] = 1.0f;
        for (int i = 1; i < 16; ++i) range[i] = 2.0f / float(denom[i]);
        for (int i = 0; i < 128; ++i)
            scale_conversion[i] = (i == 0 || i >= 126) ? 0.0f : float(std::pow(2.0, (i - 63) * 53.0 / 128.0));
        for (int i = 0; i < 16; ++i) intensity[i] = i < 14 ? float((14 - i) / 7.0) : 0.0f;
        for (int n = 0; n < 128; ++n) {
            const float w = rising_window(n);
            window[n] = w;
            window[255 - n] = w;
        }
        for (int n = 0; n < 256; ++n)
            for (int k = 0; k < 128; ++k)
                cos_basis[n][k] = float(std::cos(3.14159265358979323846 / 128.0 * (n + 0.5 + 64.0) * (k + 0.5)));
    }
    // The published 128-entry window: first half rises to ~0.70, second half (stored negated) goes on to
    // 1; w[i]^2 + w[127-i]^2 = 1. Encoded here as the IEEE bit patterns of the magnitudes.
    static float rising_window(int n) {
        static const uint32_t hex[128] = {
            0x3A3504F0, 0x3B0183B8, 0x3B70C538, 0x3BBB9268, 0x3C04A809, 0x3C308200, 0x3C61284C, 0x3C8B3F17,
            0x3CA83992, 0x3CC77FBD, 0x3CE91110, 0x3D0677CD, 0x3D198FC4, 0x3D2DD35C, 0x3D434643, 0x3D59ECC1,
            0x3D71CBA8, 0x3D85741E, 0x3D92A413, 0x3DA078B4, 0x3DAEF522, 0x3DBE1C9E, 0x3DCDF27B, 0x3DDE7A1D,
            0x3DEFB6ED, 0x3E00D62B, 0x3E0A2EDA, 0x3E13E72A, 0x3E1E00B1, 0x3E287CF2, 0x3E335D55, 0x3E3EA321,
            0x3E4A4F75, 0x3E56633F, 0x3E62DF37, 0x3E6FC3D1, 0x3E7D1138, 0x3E8563A2, 0x3E8C72B7, 0x3E93B561,
            0x3E9B2AEF, 0x3EA2D26F, 0x3EAAAAAB, 0x3EB2B222, 0x3EBAE706, 0x3EC34737, 0x3ECBD03D, 0x3ED47F46,
            0x3EDD5128, 0x3EE6425C, 0x3EEF4EFF, 0x3EF872D7, 0x3F00D4A9, 0x3F0576CA, 0x3F0A1D3B, 0x3F0EC548,
            0x3F136C25, 0x3F180EF2, 0x3F1CAAC2, 0x3F213CA2, 0x3F25C1A5, 0x3F2A36E7, 0x3F2E9998, 0x3F32E705,
            0x3F371C9E, 0x3F3B37FE, 0x3F3F36F2, 0x3F431780, 0x3F46D7E6, 0x3F4A76A4, 0x3F4DF27C, 0x3F514A6F,
            0x3F547DC5, 0x3F578C03, 0x3F5A74EE, 0x3F5D3887, 0x3F5FD707, 0x3F6250DA, 0x3F64A699, 0x3F66D908,
            0x3F68E90E, 0x3F6AD7B1, 0x3F6CA611, 0x3F6E5562, 0x3F6FE6E7, 0x3F715BEF, 0x3F72B5D1, 0x3F73F5E6,
            0x3F751D89, 0x3F762E13, 0x3F7728D7, 0x3F780F20, 0x3F78E234, 0x3F79A34C, 0x3F7A5397, 0x3F7AF439,
            0x3F7B8648, 0x3F7C0ACE, 0x3F7C82C8, 0x3F7CEF26, 0x3F7D50CB, 0x3F7DA88E, 0x3F7DF737, 0x3F7E3D86,
            0x3F7E7C2A, 0x3F7EB3CC, 0x3F7EE507, 0x3F7F106C, 0x3F7F3683, 0x3F7F57CA, 0x3F7F74B6, 0x3F7F8DB6,
            0x3F7FA32E, 0x3F7FB57B, 0x3F7FC4F6, 0x3F7FD1ED, 0x3F7FDCAD, 0x3F7FE579, 0x3F7FEC90, 0x3F7FF22E,
            0x3F7FF688, 0x3F7FF9D0, 0x3F7FFC32, 0x3F7FFDDA, 0x3F7FFEED, 0x3F7FFF8F, 0x3F7FFFDF, 0x3F7FFFFC};
        float f;
        std::memcpy(&f, &hex[n], 4);
        return f;
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

// Orthonormal DCT-IV scale sqrt(2/128); the level matches the ADX tracks of comparable movies.
constexpr float kImdctScale = 0.125f;

constexpr uint8_t kMaxBits[16] = {0, 2, 3, 3, 4, 4, 4, 4, 5, 6, 7, 8, 9, 10, 11, 12};
// Prefix codes for resolutions 1..7, indexed [resolution][max-bit code]: bits used and value.
constexpr uint8_t kReadBits[8][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {2, 2, 2, 2, 2, 2, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0}, {2, 2, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0},
    {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4}, {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4},
    {3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}, {3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4}};
constexpr int8_t kReadValue[8][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},       {0, 0, 1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, -1, -1, 2, -2, 0, 0, 0, 0, 0, 0, 0, 0},    {0, 0, 1, -1, 2, -2, 3, -3, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 3, 3, -3, -3, 4, -4}, {0, 0, 1, 1, -1, -1, 2, 2, -2, -2, 3, -3, 4, -4, 5, -5},
    {0, 0, 1, 1, -1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6}, {0, 0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7}};
constexpr uint8_t kInvert[66] = {14, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 11, 11, 11, 11,
                                 11, 11, 10, 10, 10, 10, 10, 10, 10, 9,  9,  9,  9,  9,  9,  8,  8,  8,  8,  8,  8,  7,
                                 6,  6,  5,  4,  4,  4,  3,  3,  3,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1};

} // namespace

void HcaDecoder::parse_header(std::span<const uint8_t> h) {
    if (h.size() < 8 || (be32(h.data()) & 0x7F7F7F7F) != 0x48434100) throw std::runtime_error("HCA: no header");
    version_ = be16(h.data() + 4);
    header_size_ = be16(h.data() + 6);
    if (h.size() < header_size_) throw std::runtime_error("HCA: header truncated");
    size_t o = 8;
    bool have_ath = false;
    auto tag = [&](size_t at) { return be32(h.data() + at) & 0x7F7F7F7F; };
    while (o + 4 <= header_size_) {
        const uint32_t t = tag(o);
        if (t == 0x666D7400) {  // fmt
            channels_ = h[o + 4];
            sample_rate_ = be32(h.data() + o + 4) & 0xFFFFFF;
            frame_count_ = be32(h.data() + o + 8);
            encoder_delay_ = be16(h.data() + o + 12);
            o += 16;
        } else if (t == 0x636F6D70) {  // comp
            frame_size_ = be16(h.data() + o + 4);
            min_res_ = h[o + 6], max_res_ = h[o + 7];
            track_count_ = h[o + 8], channel_config_ = h[o + 9];
            total_bands_ = h[o + 10], base_bands_ = h[o + 11], stereo_bands_ = h[o + 12];
            bands_per_hfr_group_ = h[o + 13];
            ms_stereo_ = h[o + 14] != 0;
            o += 16;
        } else if (t == 0x64656300) {  // dec
            frame_size_ = be16(h.data() + o + 4);
            min_res_ = h[o + 6], max_res_ = h[o + 7];
            total_bands_ = h[o + 8] + 1u, base_bands_ = h[o + 9] + 1u;
            track_count_ = h[o + 10] >> 4, channel_config_ = h[o + 10] & 15;
            const uint8_t stereo_type = h[o + 11];
            if (stereo_type == 0) base_bands_ = total_bands_;
            stereo_bands_ = total_bands_ - base_bands_;
            bands_per_hfr_group_ = 0;
            o += 12;
        } else if (t == 0x76627200) {  // vbr
            o += 8;
        } else if (t == 0x61746800) {  // ath
            ath_type_ = be16(h.data() + o + 4);
            have_ath = true;
            o += 6;
        } else if (t == 0x6C6F6F70) {  // loop
            o += 16;
        } else if (t == 0x63697068) {  // ciph
            ciph_type_ = be16(h.data() + o + 4);
            o += 6;
        } else if (t == 0x72766100) {  // rva
            o += 8;
        } else if (t == 0x636F6D6D) {  // comm
            o += 5 + h[o + 4];
        } else {
            break;  // pad or unknown: the rest is padding up to header_size
        }
    }
    if (!have_ath) ath_type_ = version_ < 0x200 ? 1 : 0;
    if (!channels_ || !sample_rate_ || !frame_size_) throw std::runtime_error("HCA: incomplete header");
    if (ath_type_ != 0) throw std::runtime_error("HCA: ATH type 1 is not supported");
    if (ciph_type_ != 0 && ciph_type_ != 1) throw std::runtime_error("HCA: keyed encryption is not supported");
    if (ms_stereo_) throw std::runtime_error("HCA: M/S stereo is not supported");
    if (track_count_ == 0) track_count_ = 1;

    for (int i = 0; i < 256; ++i) cipher_[i] = uint8_t(i);
    if (ciph_type_ == 1) {
        uint32_t v = 0;
        for (int i = 1; i < 255; ++i) {
            v = (v * 13 + 11) & 0xFF;
            if (v == 0 || v == 0xFF) v = (v * 13 + 11) & 0xFF;
            cipher_[i] = uint8_t(v);
        }
        cipher_[0] = 0, cipher_[255] = 255;
    }

    if (bands_per_hfr_group_ > 0) {
        const int hfr_bands = int(total_bands_) - int(base_bands_) - int(stereo_bands_);
        hfr_group_count_ = hfr_bands > 0 ? uint32_t((hfr_bands + int(bands_per_hfr_group_) - 1) / int(bands_per_hfr_group_)) : 0;
    }
    ch_.assign(channels_, Channel{});
    std::vector<int> types(channels_, 0);
    const uint32_t per_track = channels_ / track_count_;
    if (stereo_bands_ && per_track > 1) {
        for (uint32_t t = 0; t < track_count_; ++t) {
            int* ct = types.data() + t * per_track;
            switch (per_track) {
            case 2: ct[0] = 1, ct[1] = 2; break;
            case 3: ct[0] = 1, ct[1] = 2; break;
            case 4: ct[0] = 1, ct[1] = 2; if (channel_config_ == 0) ct[2] = 1, ct[3] = 2; break;
            case 5: ct[0] = 1, ct[1] = 2; if (channel_config_ <= 2) ct[3] = 1, ct[4] = 2; break;
            case 6: ct[0] = 1, ct[1] = 2, ct[4] = 1, ct[5] = 2; break;
            case 7: ct[0] = 1, ct[1] = 2, ct[4] = 1, ct[5] = 2; break;
            case 8: ct[0] = 1, ct[1] = 2, ct[4] = 1, ct[5] = 2, ct[6] = 1, ct[7] = 2; break;
            default: break;
            }
        }
    }
    for (uint32_t c = 0; c < channels_; ++c) {
        ch_[c].type = types[c];
        ch_[c].coded_count = int(types[c] == 2 ? base_bands_ : base_bands_ + stereo_bands_);
    }
    random_ = 1;
    samples_out_ = 0;
    pending_.clear();
}

void HcaDecoder::feed(std::span<const uint8_t> data, std::vector<int16_t>& out) {
    if (!ready()) throw std::runtime_error("HCA: header not parsed");
    pending_.insert(pending_.end(), data.begin(), data.end());
    size_t o = 0;
    while (pending_.size() - o >= frame_size_) {
        const std::span<const uint8_t> frame(pending_.data() + o, frame_size_);
        o += frame_size_;
        try {
            decode_frame(frame, out);
        } catch (...) {
            ++frame_errors_;
            out.insert(out.end(), size_t(kSubframes) * kSamplesPerSubframe * channels_, int16_t(0));
        }
    }
    pending_.erase(pending_.begin(), pending_.begin() + std::ptrdiff_t(o));
}

void HcaDecoder::decode_frame(std::span<const uint8_t> raw, std::vector<int16_t>& out) {
    std::vector<uint8_t> frame(raw.begin(), raw.end());
    if (ciph_type_ == 1)
        for (auto& b : frame) b = cipher_[b];
    Bits br{frame.data(), frame.size()};
    if (br.read(16) != 0xFFFF) throw std::runtime_error("HCA: bad frame sync");
    const int noise_level = int(br.read(9)), boundary = int(br.read(7));
    const int packed_noise = (noise_level << 8) - boundary;
    const auto& T = tables();

    for (auto& c : ch_) {
        // scalefactors
        int cs = c.coded_count;
        if (version_ >= 0x300 && c.type != 2 && hfr_group_count_ > 0) cs += int(hfr_group_count_);
        const int delta_bits = int(br.read(3));
        if (delta_bits >= 6) {
            for (int i = 0; i < cs; ++i) c.scalefactors[i] = uint8_t(br.read(6));
        } else if (delta_bits > 0) {
            const int expected = (1 << delta_bits) - 1;
            int value = int(br.read(6));
            c.scalefactors[0] = uint8_t(value);
            for (int i = 1; i < cs; ++i) {
                const int d = int(br.read(delta_bits));
                if (d == expected) value = int(br.read(6));
                else value = value + d - (expected >> 1);
                if (value < 0 || value >= 64) throw std::runtime_error("HCA: bad scalefactor");
                c.scalefactors[i] = uint8_t(value);
            }
        } else {
            c.scalefactors.fill(0);
        }
        // intensity (stereo secondary) or HFR scales (v2 and earlier)
        if (c.type == 2) {
            if (version_ <= 0x200) {
                const uint32_t v = br.peek(4);
                c.intensity[0] = uint8_t(v);
                if (v < 15) {
                    br.skip(4);
                    for (int i = 1; i < kSubframes; ++i) c.intensity[i] = uint8_t(br.read(4));
                }
            } else {
                throw std::runtime_error("HCA: v3 intensity stereo is not supported");
            }
        } else if (version_ <= 0x200) {
            for (uint32_t i = 0; i < hfr_group_count_; ++i) c.scalefactors[128 - hfr_group_count_ + i] = uint8_t(br.read(6));
        }
        // resolution
        c.noise_count = c.valid_count = 0;
        for (int i = 0; i < c.coded_count; ++i) {
            int r = 0;
            const int sf = c.scalefactors[i];
            if (sf > 0) {
                const int level = ath_curve_[i] + ((packed_noise + i) >> 8);
                const int pos = level + 1 - ((5 * sf) >> 1);
                if (pos < 0) r = 15;
                else if (pos <= 65) r = kInvert[pos];
                else r = 0;
                r = std::clamp(r, int(min_res_), int(max_res_));
                if (r < 1) c.noises[c.noise_count++] = uint8_t(i);
                else c.noises[127 - c.valid_count++] = uint8_t(i);
            }
            c.resolution[i] = uint8_t(r);
        }
        for (int i = c.coded_count; i < 128; ++i) c.resolution[i] = 0;
        for (int i = 0; i < c.coded_count; ++i) c.gain[i] = T.scaling[c.scalefactors[i]] * T.range[c.resolution[i]];
    }

    for (int sub = 0; sub < kSubframes; ++sub) {
        for (auto& c : ch_) {
            auto& sp = c.spectra[sub];
            for (int i = 0; i < c.coded_count; ++i) {
                const int res = c.resolution[i];
                const int bits = kMaxBits[res];
                const int code = int(br.read(bits));
                float q;
                if (res > 7) {
                    const int s = (1 - ((code & 1) << 1)) * (code >> 1);
                    if (s == 0) br.skip(-1);
                    q = float(s);
                } else {
                    br.skip(int(kReadBits[res][code]) - bits);
                    q = float(kReadValue[res][code]);
                }
                sp[i] = c.gain[i] * q;
            }
            for (int i = c.coded_count; i < 128; ++i) sp[i] = 0;
        }
        // noise filling (only when the minimum resolution allows zero-resolution bands)
        for (auto& c : ch_) {
            if (min_res_ > 0 || c.valid_count <= 0 || c.noise_count <= 0) continue;
            if (ms_stereo_ && c.type != 1) continue;
            auto& sp = c.spectra[sub];
            for (int i = 0; i < c.noise_count; ++i) {
                const int ri = 128 - c.valid_count + int(((random_ & 0x7FFF) * uint32_t(c.valid_count)) >> 15);
                random_ = 0x343FD * random_ + 0x269EC3;
                const int ni = c.noises[i], vi = c.noises[ri];
                int sc = c.scalefactors[ni] - c.scalefactors[vi] + 62;
                sc = std::max(sc, 0);
                sp[ni] = T.scale_conversion[std::min(sc, 127)] * sp[vi];
            }
        }
        // high-frequency reconstruction
        if (bands_per_hfr_group_ > 0)
            for (auto& c : ch_) {
                if (c.type == 2) continue;
                auto& sp = c.spectra[sub];
                const int start = int(stereo_bands_ + base_bands_);
                int high = start, low = start - 1;
                const uint8_t* hfr = version_ <= 0x200 ? &c.scalefactors[128 - hfr_group_count_] : &c.scalefactors[start];
                const int limit = version_ <= 0x200 ? int(hfr_group_count_) : (int(hfr_group_count_) >> 1);
                for (int g = 0; g < int(hfr_group_count_); ++g) {
                    const int low_sub = g < limit ? 1 : 0;
                    for (uint32_t i = 0; i < bands_per_hfr_group_; ++i) {
                        if (high >= int(total_bands_) || low < 0) break;
                        int sc = hfr[g] - c.scalefactors[low] + 63;
                        sc = std::clamp(sc, 0, 127);
                        sp[high] = T.scale_conversion[sc] * sp[low];
                        ++high;
                        low -= low_sub;
                    }
                }
                if (high > 0) sp[high - 1] = 0.0f;
            }
        // intensity stereo
        if (stereo_bands_)
            for (uint32_t c = 0; c + 1 < channels_; ++c) {
                if (ch_[c].type != 1 || ch_[c + 1].type != 2) continue;
                auto& l = ch_[c].spectra[sub];
                auto& r = ch_[c + 1].spectra[sub];
                const float ratio_l = T.intensity[ch_[c + 1].intensity[sub]];
                const float ratio_r = 2.0f - ratio_l;
                for (uint32_t b = base_bands_; b < total_bands_; ++b) {
                    const float v = l[b];
                    l[b] = v * ratio_l;
                    r[b] = v * ratio_r;
                }
            }
        // IMDCT with overlap-add: y[n] = sum_k X[k] cos(pi/128 (n + 64.5)(k + 0.5)), 256 samples per 128 bins
        for (auto& c : ch_) {
            const auto& sp = c.spectra[sub];
            float y[256];
            for (int n = 0; n < 256; ++n) {
                const float* b = T.cos_basis[n];
                float s = 0;
                for (int k = 0; k < 128; ++k) s += b[k] * sp[k];
                y[n] = s;
            }
            auto& wv = c.wave[sub];
            for (int n = 0; n < 128; ++n) {
                wv[n] = T.window[n] * y[n] * kImdctScale + c.previous[n];
                c.previous[n] = T.window[128 + n] * y[128 + n] * kImdctScale;
            }
        }
    }

    // interleave, dropping the encoder delay at the start of the stream
    const size_t frame_samples = size_t(kSubframes) * kSamplesPerSubframe;
    const size_t skip = samples_out_ < encoder_delay_ ? size_t(std::min<uint64_t>(encoder_delay_ - samples_out_, frame_samples)) : 0;
    for (size_t i = skip; i < frame_samples; ++i) {
        const int sub = int(i / kSamplesPerSubframe), n = int(i % kSamplesPerSubframe);
        for (auto& c : ch_) {
            const float v = c.wave[sub][n] * 32768.0f;
            out.push_back(int16_t(std::clamp(std::lrint(v), -32768l, 32767l)));
        }
    }
    samples_out_ += frame_samples;
}

} // namespace nim
