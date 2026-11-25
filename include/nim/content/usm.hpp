#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nim {

// CRI Sofdec2 movies (USM). Marathon ships them as package entries of type 27/1; the movie
// descriptor tag (class 80809E99) points at one (docs/ARCHITECTURE.md, movies).
//
// A USM is a sequence of big-endian chunks: u32 id ("CRID", "@SFV" video, "@SFA" audio,
// "@SBT" subtitles, "@ALP" alpha), u32 size, then a 0x18-byte chunk header
//   0x08 u16 header size (from 0x08), 0x0A u16 padding size, 0x0C u8 channel,
//   0x0F u8 data type (0 = stream data, 1 = header table, 2 = section end, 3 = seek table),
//   0x10 u32 frame time, 0x14 u32 frame rate (x100).
// Header tables are CRI @UTF tables.
constexpr uint32_t kClassMovie = 0x80809E99;

// One CRI @UTF table (big-endian): typed columns, constant or per-row.
struct UtfTable {
    using Value = std::variant<std::monostate, int64_t, uint64_t, float, std::string, std::vector<uint8_t>>;
    std::string name;
    std::vector<std::string> columns;
    std::vector<std::map<std::string, Value>> rows;
    int64_t integer(size_t row, const std::string& column, int64_t fallback = 0) const;
    std::string text(size_t row, const std::string& column) const;
};
// Throws on malformed input.
UtfTable parse_utf_table(std::span<const uint8_t> bytes);

enum class UsmStream : uint8_t { Video, Audio, Subtitle, Alpha, Other };

struct UsmChunk {
    UsmStream stream = UsmStream::Other;
    uint8_t channel = 0;
    uint8_t data_type = 0;
    uint32_t frame_time = 0, frame_rate = 0;  // seconds = frame_time / frame_rate
    std::span<const uint8_t> payload;
    double seconds() const { return frame_rate ? double(frame_time) / frame_rate : 0; }
};

struct UsmInfo {
    std::string filename;
    uint32_t width = 0, height = 0, display_width = 0, display_height = 0;
    uint32_t video_codec = 0;          // mpeg_codec: 1 = MPEG-1 (Sofdec), 5 = H.264, 9 = VP9
    uint32_t total_frames = 0;
    double frame_rate = 0;
    uint32_t frame_rate_numerator = 0, frame_rate_denominator = 0;
    struct Audio {
        uint8_t channel = 0;
        uint32_t codec = 0;            // audio_codec: 2 = ADX, 4 = HCA
        uint32_t sample_rate = 0, channels = 0, total_samples = 0;
    };
    std::vector<Audio> audio;
    std::vector<uint8_t> subtitle_channels;
    struct SeekPoint { uint64_t offset = 0; uint32_t frame = 0; };
    std::vector<SeekPoint> seek;                         // VIDEO_SEEKINFO: keyframe chunk offsets
    std::map<uint8_t, std::vector<uint8_t>> audio_headers;  // first stream payload of each audio channel
    std::vector<uint8_t> video_head;                      // first video payload (sequence header)
    double duration() const { return frame_rate > 0 ? total_frames / frame_rate : 0; }
};

// Calls `f` for every chunk in `bytes` (which may stop mid-chunk; the partial chunk is skipped
// and its offset returned). Returns the offset after the last complete chunk.
size_t for_each_usm_chunk(std::span<const uint8_t> bytes, const std::function<void(const UsmChunk&)>& f);
// Header tables at the start of the file (the first ~64 KB holds them).
UsmInfo parse_usm_info(std::span<const uint8_t> head);
const char* usm_video_codec_name(uint32_t codec);
const char* usm_audio_codec_name(uint32_t codec);

// @SBT subtitle payload: u32 language, u32 interval, u32 start, u32 duration, u32 length,
// then text (little-endian fields; the text is UTF-8 in recent files).
struct UsmSubtitle {
    uint32_t language = 0, start = 0, duration = 0, interval = 1000;
    std::string text;
};
std::optional<UsmSubtitle> parse_usm_subtitle(std::span<const uint8_t> payload);

// ---- ADX (CRI ADPCM, the audio of these movies) ---------------------------------------------------
// Big-endian header: u16 0x8000, u16 copyright offset (data starts at offset + 4),
// u8 encoding (3 = standard, 4 = exponential), u8 block size, u8 sample bits, u8 channels,
// u32 sample rate, u32 total samples, u16 highpass frequency, u8 version, u8 flags (encryption).
struct AdxDecoder {
    uint32_t sample_rate = 0, channels = 0, total_samples = 0;
    uint32_t block_size = 18;
    int32_t coef1 = 0, coef2 = 0;
    std::vector<std::array<int32_t, 2>> history;
    bool header_done = false, encrypted = false, ended = false;
    std::vector<uint8_t> pending;
    // Feeds one stream payload (the first carries the header). Appends interleaved PCM.
    void feed(std::span<const uint8_t> payload, std::vector<int16_t>& out);
};

} // namespace nim
