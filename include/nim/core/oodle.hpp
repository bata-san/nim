#pragma once
#include <cstdint>
#include <filesystem>
#include <span>

namespace nim {

// Oodle is loaded at runtime from the game's own install (bin/x64/oo2core_9_win64.dll).
// We never redistribute it.
class Oodle {
public:
    static bool load(const std::filesystem::path& dll);
    static bool loaded();
    // Returns decompressed size or 0 on failure.
    static size_t decompress(std::span<const uint8_t> src, std::span<uint8_t> dst);
};

} // namespace nim
