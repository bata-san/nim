#include "nim/content/movies.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace nim {

std::vector<MovieEntry> list_movies(PackageManager& pm) {
    std::vector<MovieEntry> movies;
    std::unordered_map<uint32_t, size_t> by_usm;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 27 || e.subtype != 1 || e.file_size < 0x100) continue;
            const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
            if (by_usm.contains(t.value)) continue;
            MovieEntry m;
            m.usm = t;
            m.size = e.file_size;
            try {
                auto head = p.read_entry_range(i, 0, 0x40000);
                if (head.size() < 4 || std::memcmp(head.data(), "CRID", 4) != 0) continue;
                m.info = parse_usm_info(head);
            } catch (const std::exception& ex) {
                m.error = ex.what();
            }
            by_usm.emplace(t.value, movies.size());
            movies.push_back(std::move(m));
        }
    }
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassMovie || e.file_size < 0x20) continue;
            try {
                auto d = p.read_entry(i);
                TagReader r(d);
                auto it = by_usm.find(r.tag_at(0x18).value);
                if (it != by_usm.end()) movies[it->second].descriptors.push_back(TagHash::make(info.pkg_id, uint16_t(i)));
            } catch (...) {
            }
        }
    }
    std::sort(movies.begin(), movies.end(), [](const MovieEntry& a, const MovieEntry& b) {
        return a.info.filename != b.info.filename ? a.info.filename < b.info.filename : a.usm < b.usm;
    });
    return movies;
}

} // namespace nim
