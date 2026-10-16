#pragma once
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace nim {

// The game's own UI fonts (OpenType files stored whole in the ui packages). Read at runtime from
// the user's install, never shipped with nim.
struct GameFont {
    TagHash tag;
    std::string family, subfamily, postscript;  // from the font's 'name' table
    std::vector<uint8_t> data;
};
std::vector<GameFont> load_game_fonts(PackageManager& pm);

} // namespace nim
