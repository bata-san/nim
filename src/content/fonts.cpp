#include "nim/content/fonts.hpp"

#include <cstring>

namespace nim {
namespace {

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool is_font(const std::vector<uint8_t>& d) {
    if (d.size() < 12) return false;
    const uint32_t tag = be32(d.data());
    return tag == 0x4F54544F /* OTTO */ || tag == 0x00010000 || tag == 0x74727565 /* true */;
}

// Reads one name record (platform 3 / Windows Unicode, else platform 1 / Mac Roman).
std::string font_name(const std::vector<uint8_t>& d, uint16_t want_id) {
    const uint16_t tables = be16(d.data() + 4);
    for (uint16_t t = 0; t < tables; ++t) {
        const size_t rec = 12 + size_t(t) * 16;
        if (rec + 16 > d.size()) return {};
        if (std::memcmp(d.data() + rec, "name", 4) != 0) continue;
        const size_t base = be32(d.data() + rec + 8), len = be32(d.data() + rec + 12);
        if (base + 6 > d.size() || base + len > d.size()) return {};
        const uint16_t count = be16(d.data() + base + 2);
        const size_t strings = base + be16(d.data() + base + 4);
        std::string mac;
        for (uint16_t i = 0; i < count; ++i) {
            const uint8_t* r = d.data() + base + 6 + size_t(i) * 12;
            if (r + 12 > d.data() + d.size()) break;
            const uint16_t platform = be16(r), id = be16(r + 6), n = be16(r + 8), off = be16(r + 10);
            if (id != want_id || strings + off + n > d.size()) continue;
            const uint8_t* s = d.data() + strings + off;
            if (platform == 3 || platform == 0) {
                std::string out;
                for (uint16_t k = 0; k + 1 < n; k += 2) {
                    const uint16_t c = be16(s + k);
                    out += c < 0x80 ? char(c) : '?';
                }
                return out;
            }
            if (platform == 1 && mac.empty()) mac.assign(reinterpret_cast<const char*>(s), n);
        }
        return mac;
    }
    return {};
}

} // namespace

std::vector<GameFont> load_game_fonts(PackageManager& pm) {
    std::vector<GameFont> out;
    for (const auto& info : pm.packages()) {
        if (info.name.find("ui_") == std::string::npos) continue;
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 24 || e.file_size < 1024 || e.file_size > (16u << 20)) continue;
            std::vector<uint8_t> d;
            try {
                d = p.read_entry(i);
            } catch (...) {
                continue;
            }
            if (!is_font(d)) continue;
            GameFont f;
            f.tag = TagHash::make(info.pkg_id, uint16_t(i));
            f.family = font_name(d, 1);
            f.subfamily = font_name(d, 2);
            f.postscript = font_name(d, 6);
            f.data = std::move(d);
            out.push_back(std::move(f));
        }
    }
    return out;
}

} // namespace nim
