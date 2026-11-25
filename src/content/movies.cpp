#include "nim/content/movies.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace nim {

namespace {

bool has_verified_dub_order(const MovieEntry& movie) {
    return movie.info.filename == "nux_intro.usm" || movie.info.filename == "zone_select_takeover.usm";
}

} // namespace

std::optional<TextLanguage> movie_audio_language(const MovieEntry& movie, int channel) {
    using enum TextLanguage;
    constexpr std::optional<TextLanguage> languages[] = {
        std::nullopt, English, French, Italian, German, Spanish, Japanese, Portuguese, SpanishLatAm,
        std::nullopt, ChineseSimplified, Russian, Polish, Korean
    };
    if (!has_verified_dub_order(movie) || channel < 0 || size_t(channel) >= std::size(languages)) return std::nullopt;
    return languages[channel];
}

bool movie_audio_is_placeholder(const MovieEntry& movie, const UsmInfo::Audio& track) {
    // These two files have a one-second silent mono stream alongside the full dubbed tracks.
    return has_verified_dub_order(movie) && track.channel == 0 && track.channels == 1 &&
           track.sample_rate != 0 && track.total_samples <= 2 * uint64_t(track.sample_rate);
}

int default_movie_audio_channel(const MovieEntry& movie, TextLanguage language) {
    if (language == TextLanguage::ChineseTraditional) language = TextLanguage::ChineseSimplified;
    int first = -1;
    int english = -1;
    for (const auto& track : movie.info.audio) {
        if ((track.codec != 2 && track.codec != 4) || movie_audio_is_placeholder(movie, track)) continue;
        const auto actual = movie_audio_language(movie, track.channel);
        if (actual == language) return track.channel;
        if (actual == TextLanguage::English) english = track.channel;
        if (first == -1) first = track.channel;
    }
    return english >= 0 ? english : first;
}

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
