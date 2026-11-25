#pragma once
#include "nim/content/strings.hpp"
#include "nim/content/usm.hpp"
#include "nim/core/package_manager.hpp"

#include <optional>
#include <string>
#include <vector>

namespace nim {

// Movie descriptor (class 80809E99, 0x20 bytes):
//   0x08 u32 width, 0x0C u32 height, 0x10 u32 frame count, 0x14 f32 frame rate,
//   0x18 Tag USM file (type 27/1), 0x1C Tag video surface set (class 80807F61).
struct MovieEntry {
    TagHash usm;                       // the USM file (type 27/1)
    std::vector<TagHash> descriptors;  // descriptor tags pointing at it (none for a loose file)
    uint64_t size = 0;
    UsmInfo info;
    std::string error;                 // non-empty when the header could not be read
};

// Every USM in the install (all type 27/1 entries that start with "CRID"), with their descriptors.
std::vector<MovieEntry> list_movies(PackageManager& pm);

// The localized USMs use their own audio channel order, independent of string-table slots.
std::optional<TextLanguage> movie_audio_language(const MovieEntry& movie, int channel);
bool movie_audio_is_placeholder(const MovieEntry& movie, const UsmInfo::Audio& track);
// Prefers the requested language, then English, then the first playable recording. -1 = no audio.
int default_movie_audio_channel(const MovieEntry& movie, TextLanguage language);

} // namespace nim
