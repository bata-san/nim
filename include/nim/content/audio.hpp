#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nim {

// Wwise streams (.wem, package entries of type 26/7) and PCM output (docs/ARCHITECTURE.md, audio).
//
// A WEM is RIFF/WAVE. Marathon's dialogue uses codec 0xFFFF, Wwise Vorbis: the 0x42-byte fmt
// chunk carries the "vorb" block at fmt+0x18:
//   +0x00 u32 sample count, +0x04 u32 mod signal (not 0x4A/0x4B/0x69/0x70: packets have no
//   packet-type bit and no window flags), +0x10 u32 setup packet offset, +0x14 u32 first audio
//   packet offset (both from the data chunk), +0x24 u32 uid, +0x28 u8 log2 blocksize 0, 1.
// Packets are u16 size + payload. The setup packet is a stripped Vorbis setup whose codebooks
// are 10-bit ids into ww2ogg's external library (third_party/ww2ogg).
struct PcmAudio {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    std::vector<int16_t> samples;  // interleaved
    size_t frames() const { return channels ? samples.size() / channels : 0; }
    double seconds() const { return sample_rate ? double(frames()) / sample_rate : 0; }
};

struct PcmLevel {
    float peak = 0;
    float short_rms = 0;  // loudest 100 ms window in any channel
};
PcmLevel measure_pcm_level(const PcmAudio& pcm);
float limit_ambient_gain(float gain, PcmLevel level);

struct WemInfo {
    uint16_t codec = 0;  // 0xFFFF Wwise Vorbis, 0x0001/0xFFFE PCM, 0x0002 Wwise IMA ADPCM, 0x3040 Opus
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t sample_count = 0;
};
std::optional<WemInfo> probe_wem(std::span<const uint8_t> bytes);
const char* wem_codec_name(uint16_t codec);

// Decodes Vorbis and PCM streams; throws std::runtime_error for other codecs or bad data.
PcmAudio decode_wem(std::span<const uint8_t> bytes);

// Exposed for tests: the fast inverse MDCT against the direct formula
// y[n] = sum_k X[k] cos(2pi/N (n + 1/2 + N/4)(k + 1/2)), N = 2 * in.size().
void inverse_mdct(std::span<const float> in, std::span<float> out);

} // namespace nim
