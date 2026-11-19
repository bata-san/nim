#pragma once
// CRI HCA audio decoder (the audio track of some USM movies). Versions 1.3-3.0, unencrypted
// (ciph 0) or the static-key cipher (ciph 1). Keyed encryption (ciph 56) is not supported.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nim {

class HcaDecoder {
public:
    // Parses the "HCA" header. Throws on unsupported streams.
    void parse_header(std::span<const uint8_t> header);
    // Feeds data after the header (any split); decodes complete frames into interleaved int16 PCM.
    void feed(std::span<const uint8_t> data, std::vector<int16_t>& out);
    bool ready() const { return channels_ != 0; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }
    uint32_t frame_count() const { return frame_count_; }
    uint32_t header_size() const { return header_size_; }
    size_t frame_errors() const { return frame_errors_; }

    static constexpr int kSubframes = 8, kSamplesPerSubframe = 128;

private:
    struct Channel {
        int type = 0;  // 0 discrete, 1 stereo primary, 2 stereo secondary
        int coded_count = 0;
        std::array<uint8_t, 128> scalefactors{}, resolution{}, intensity{}, noises{};
        int noise_count = 0, valid_count = 0;
        std::array<float, 128> gain{};
        std::array<std::array<float, 128>, kSubframes> spectra{};
        std::array<float, 128> previous{};  // IMDCT overlap
        std::array<std::array<float, 128>, kSubframes> wave{};
    };
    void decode_frame(std::span<const uint8_t> frame, std::vector<int16_t>& out);

    uint32_t version_ = 0, header_size_ = 0, channels_ = 0, sample_rate_ = 0, frame_count_ = 0;
    uint32_t encoder_delay_ = 0, frame_size_ = 0, min_res_ = 0, max_res_ = 15;
    uint32_t track_count_ = 1, channel_config_ = 0, total_bands_ = 0, base_bands_ = 0, stereo_bands_ = 0;
    uint32_t bands_per_hfr_group_ = 0, hfr_group_count_ = 0, ath_type_ = 0, ciph_type_ = 0;
    bool ms_stereo_ = false;
    uint32_t random_ = 1;
    std::array<uint8_t, 128> ath_curve_{};
    std::array<uint8_t, 256> cipher_{};
    std::vector<Channel> ch_;
    std::vector<uint8_t> pending_;
    uint64_t samples_out_ = 0;
    size_t frame_errors_ = 0;
};

} // namespace nim
