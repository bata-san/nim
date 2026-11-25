#pragma once

#include "nim/content/movies.hpp"
#include "nim/content/strings.hpp"

#include <atomic>
#include <filesystem>

namespace nim::media {

// Audio channel -1 writes video only. Subtitle language -1 omits the SRT.
struct MovieExport {
    std::filesystem::path output;
    int channel = -1;
    int subtitle_language = -1;
    bool replace_mp4 = false;
};

struct MovieExportResult {
    uint64_t frames = 0;
    bool subtitles = false;
    bool cancelled = false;
};

MovieExportResult export_movie(nim::PackageManager& pm, const nim::MovieEntry& movie,
                               const MovieExport& settings, const nim::StringTable* strings,
                               std::atomic<unsigned>& progress, const std::atomic<bool>& cancel);

} // namespace media
