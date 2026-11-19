#include "nim/content/usm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <optional>
#include <stdexcept>

namespace nim {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

UsmStream stream_of(uint32_t id) {
    switch (id) {
    case 0x40534656: return UsmStream::Video;     // @SFV
    case 0x40534641: return UsmStream::Audio;     // @SFA
    case 0x40534254: return UsmStream::Subtitle;  // @SBT
    case 0x40414C50: return UsmStream::Alpha;     // @ALP
    default: return UsmStream::Other;
    }
}

} // namespace

int64_t UtfTable::integer(size_t row, const std::string& column, int64_t fallback) const {
    if (row >= rows.size()) return fallback;
    auto it = rows[row].find(column);
    if (it == rows[row].end()) return fallback;
    if (auto v = std::get_if<int64_t>(&it->second)) return *v;
    if (auto v = std::get_if<uint64_t>(&it->second)) return int64_t(*v);
    if (auto v = std::get_if<float>(&it->second)) return int64_t(*v);
    return fallback;
}

std::string UtfTable::text(size_t row, const std::string& column) const {
    if (row >= rows.size()) return {};
    auto it = rows[row].find(column);
    if (it == rows[row].end()) return {};
    if (auto v = std::get_if<std::string>(&it->second)) return *v;
    return {};
}

UtfTable parse_utf_table(std::span<const uint8_t> bytes) {
    if (bytes.size() < 0x20 || std::memcmp(bytes.data(), "@UTF", 4) != 0) throw std::runtime_error("not a @UTF table");
    const size_t size = be32(bytes.data() + 4);
    if (size + 8 > bytes.size() || size < 0x18) throw std::runtime_error("@UTF table truncated");
    const uint8_t* t = bytes.data() + 8;
    const size_t rows_offset = be16(t + 2), strings_offset = be32(t + 4), data_offset = be32(t + 8);
    const uint32_t name_offset = be32(t + 12);
    const size_t columns = be16(t + 16), row_width = be16(t + 18), row_count = be32(t + 20);
    if (strings_offset > size || data_offset > size || rows_offset > size) throw std::runtime_error("@UTF offsets out of range");
    auto str = [&](uint32_t o) -> std::string {
        size_t p = strings_offset + o;
        if (p >= data_offset) return {};
        const char* s = reinterpret_cast<const char*>(t + p);
        return std::string(s, strnlen(s, data_offset - p));
    };
    auto value_size = [](uint8_t type) -> size_t {
        switch (type) {
        case 0: case 1: return 1;
        case 2: case 3: return 2;
        case 4: case 5: case 8: case 0xA: return 4;
        case 6: case 7: case 9: case 0xB: return 8;
        default: throw std::runtime_error("unknown @UTF column type");
        }
    };
    auto read_value = [&](uint8_t type, const uint8_t* p) -> UtfTable::Value {
        switch (type) {
        case 0: return uint64_t(p[0]);
        case 1: return int64_t(int8_t(p[0]));
        case 2: return uint64_t(be16(p));
        case 3: return int64_t(int16_t(be16(p)));
        case 4: return uint64_t(be32(p));
        case 5: return int64_t(int32_t(be32(p)));
        case 6: return be64(p);
        case 7: return int64_t(be64(p));
        case 8: return std::bit_cast<float>(be32(p));
        case 9: return float(std::bit_cast<double>(be64(p)));
        case 0xA: return str(be32(p));
        case 0xB: {
            const size_t o = data_offset + be32(p), n = be32(p + 4);
            if (o + n > size) return std::monostate{};
            return std::vector<uint8_t>(t + o, t + o + n);
        }
        }
        return std::monostate{};
    };

    UtfTable table;
    table.name = str(name_offset);
    struct Column { std::string name; uint8_t flags, type; UtfTable::Value constant; };
    std::vector<Column> cols;
    size_t p = 24;
    for (size_t c = 0; c < columns; ++c) {
        if (p + 5 > size) throw std::runtime_error("@UTF schema truncated");
        Column col;
        const uint8_t f = t[p++];
        col.flags = f >> 4;
        col.type = f & 0xF;
        if (col.flags & 1) col.name = str(be32(t + p)), p += 4;
        if (col.flags & 2) {
            const size_t n = value_size(col.type);
            if (p + n > size) throw std::runtime_error("@UTF schema truncated");
            col.constant = read_value(col.type, t + p);
            p += n;
        }
        table.columns.push_back(col.name);
        cols.push_back(std::move(col));
    }
    for (size_t r = 0; r < row_count; ++r) {
        size_t q = rows_offset + r * row_width;
        auto& row = table.rows.emplace_back();
        for (const auto& col : cols) {
            if (col.flags & 4) {
                const size_t n = value_size(col.type);
                if (q + n > size) throw std::runtime_error("@UTF row truncated");
                row[col.name] = read_value(col.type, t + q);
                q += n;
            } else if (col.flags & 2) {
                row[col.name] = col.constant;
            }
        }
    }
    return table;
}

size_t for_each_usm_chunk(std::span<const uint8_t> bytes, const std::function<void(const UsmChunk&)>& f) {
    size_t o = 0;
    while (o + 0x20 <= bytes.size()) {
        const uint8_t* c = bytes.data() + o;
        const uint32_t id = be32(c), size = be32(c + 4);
        if (size < 0x18) throw std::runtime_error("bad USM chunk size");
        if (o + 8 + size > bytes.size()) break;
        const uint16_t header = be16(c + 8), padding = be16(c + 10);
        if (header + padding > size) throw std::runtime_error("bad USM chunk header");
        UsmChunk chunk;
        chunk.stream = stream_of(id);
        chunk.channel = c[12];
        chunk.data_type = c[15] & 3;
        chunk.frame_time = be32(c + 16);
        chunk.frame_rate = be32(c + 20);
        chunk.payload = bytes.subspan(o + 8 + header, size - header - padding);
        if (id != 0x43524944) f(chunk);  // CRID is handled by parse_usm_info
        o += 8 + size;
    }
    return o;
}

UsmInfo parse_usm_info(std::span<const uint8_t> head) {
    if (head.size() < 0x20 || std::memcmp(head.data(), "CRID", 4) != 0) throw std::runtime_error("not a USM (no CRID)");
    UsmInfo info;
    const uint32_t crid_size = be32(head.data() + 4);
    const uint16_t crid_header = be16(head.data() + 8);
    if (8 + size_t(crid_size) <= head.size()) {
        auto dir = parse_utf_table(head.subspan(8 + crid_header));
        for (size_t r = 0; r < dir.rows.size(); ++r)
            if (dir.integer(r, "stmid") == 0) info.filename = dir.text(r, "filename");
    }
    bool section_end = false;
    std::span<const uint8_t> rest = head.subspan(std::min<size_t>(head.size(), 8 + size_t(crid_size)));
    for_each_usm_chunk(rest, [&](const UsmChunk& c) {
        if (c.data_type == 2) section_end = true;
        if (c.data_type == 0 && c.stream == UsmStream::Subtitle &&
            std::find(info.subtitle_channels.begin(), info.subtitle_channels.end(), c.channel) == info.subtitle_channels.end())
            info.subtitle_channels.push_back(c.channel);
        if (c.data_type == 0 && c.stream == UsmStream::Audio && !info.audio_headers.contains(c.channel))
            info.audio_headers[c.channel].assign(c.payload.begin(), c.payload.end());
        if (c.data_type == 0 && c.stream == UsmStream::Video && info.video_head.empty())
            info.video_head.assign(c.payload.begin(), c.payload.begin() + std::ptrdiff_t(std::min<size_t>(c.payload.size(), 4096)));
        if (c.data_type == 3 && c.stream == UsmStream::Video && c.payload.size() >= 8 &&
            std::memcmp(c.payload.data(), "@UTF", 4) == 0) {
            try {
                auto t = parse_utf_table(c.payload);
                for (size_t r = 0; r < t.rows.size(); ++r)
                    info.seek.push_back({uint64_t(t.integer(r, "ofs_byte")), uint32_t(t.integer(r, "ofs_frmid"))});
            } catch (...) {
            }
            return;
        }
        if (c.data_type != 1 || c.payload.size() < 8 || std::memcmp(c.payload.data(), "@UTF", 4) != 0) return;
        UtfTable t;
        try { t = parse_utf_table(c.payload); } catch (...) { return; }
        if (t.name == "VIDEO_HDRINFO" && !t.rows.empty()) {
            info.width = uint32_t(t.integer(0, "width"));
            info.height = uint32_t(t.integer(0, "height"));
            info.display_width = uint32_t(t.integer(0, "disp_width", info.width));
            info.display_height = uint32_t(t.integer(0, "disp_height", info.height));
            info.video_codec = uint32_t(t.integer(0, "mpeg_codec"));
            info.total_frames = uint32_t(t.integer(0, "total_frames"));
            const int64_t n = t.integer(0, "framerate_n"), d = t.integer(0, "framerate_d", 1);
            info.frame_rate = d ? double(n) / double(d) : 0;
        } else if (t.name == "AUDIO_HDRINFO" && !t.rows.empty()) {
            UsmInfo::Audio a;
            a.channel = c.channel;
            a.codec = uint32_t(t.integer(0, "audio_codec"));
            a.sample_rate = uint32_t(t.integer(0, "sampling_rate"));
            a.channels = uint32_t(t.integer(0, "num_channels"));
            a.total_samples = uint32_t(t.integer(0, "total_samples"));
            info.audio.push_back(a);
        }
    });
    (void)section_end;
    return info;
}

const char* usm_video_codec_name(uint32_t codec) {
    switch (codec) {
    case 1: return "MPEG-1";
    case 5: return "H.264";
    case 9: return "VP9";
    default: return "?";
    }
}

const char* usm_audio_codec_name(uint32_t codec) {
    switch (codec) {
    case 2: return "ADX";
    case 4: return "HCA";
    default: return "?";
    }
}

std::optional<UsmSubtitle> parse_usm_subtitle(std::span<const uint8_t> payload) {
    if (payload.size() < 20) return std::nullopt;
    UsmSubtitle s;
    s.language = le32(payload.data());
    s.interval = le32(payload.data() + 4);
    s.start = le32(payload.data() + 8);
    s.duration = le32(payload.data() + 12);
    const uint32_t n = le32(payload.data() + 16);
    if (20 + size_t(n) > payload.size()) return std::nullopt;
    s.text.assign(reinterpret_cast<const char*>(payload.data() + 20), n);
    while (!s.text.empty() && s.text.back() == '\0') s.text.pop_back();
    return s;
}

void AdxDecoder::feed(std::span<const uint8_t> payload, std::vector<int16_t>& out) {
    if (ended || encrypted) return;
    pending.insert(pending.end(), payload.begin(), payload.end());
    size_t o = 0;
    if (!header_done) {
        if (pending.size() < 0x14) return;
        if (be16(pending.data()) != 0x8000) { ended = true; return; }
        const uint32_t data_start = be16(pending.data() + 2) + 4u;
        if (pending.size() < data_start) return;
        const uint8_t encoding = pending[4];
        block_size = pending[5];
        channels = pending[7];
        sample_rate = be32(pending.data() + 8);
        total_samples = be32(pending.data() + 12);
        const uint16_t cutoff = be16(pending.data() + 16);
        const uint8_t flags = pending[19];
        if (encoding != 3 || pending[6] != 4 || channels == 0 || channels > 8 || block_size < 3 || sample_rate == 0) {
            ended = true;
            return;
        }
        encrypted = flags == 0x08 || flags == 0x09;
        if (encrypted) return;
        const double a = std::numbers::sqrt2 - std::cos(2.0 * std::numbers::pi * cutoff / double(sample_rate));
        const double b = std::numbers::sqrt2 - 1.0;
        const double c = (a - std::sqrt((a + b) * (a - b))) / b;
        coef1 = int32_t(std::floor(c * 8192.0));
        coef2 = int32_t(std::floor(c * c * -4096.0));
        history.assign(channels, {0, 0});
        header_done = true;
        o = data_start;
    }
    const size_t frame = size_t(block_size) * channels;
    const size_t per_block = (block_size - 2) * 2;
    for (; o + frame <= pending.size(); o += frame) {
        const size_t base = out.size();
        out.resize(base + per_block * channels);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint8_t* blk = pending.data() + o + size_t(ch) * block_size;
            const uint16_t raw = be16(blk);
            if (raw & 0x8000) {  // end-of-stream marker
                ended = true;
                out.resize(base);
                pending.clear();
                return;
            }
            const int32_t scale = int32_t(raw) + 1;
            auto& h = history[ch];
            for (size_t i = 0; i < per_block; ++i) {
                const uint8_t byte = blk[2 + i / 2];
                int32_t nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
                if (nib & 8) nib -= 16;
                int32_t s = nib * scale + ((coef1 * h[0] + coef2 * h[1]) >> 12);
                s = std::clamp(s, -32768, 32767);
                h[1] = h[0];
                h[0] = s;
                out[base + i * channels + ch] = int16_t(s);
            }
        }
    }
    pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(o));
}

} // namespace nim
