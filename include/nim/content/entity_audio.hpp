#pragma once
#include "nim/core/package_manager.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace nim {

// The sounds an entity's animations trigger (docs/PROGRESS.md 3.90). Clip events
// (AnimationClip::events) name keys in tables the entity's components (8080BADB) hold:
//   8080AE30 {u32 name, Vec<8080AE34>} (0x18 bytes): clip events of kind 5 by name. Each
//     8080AE34 (0x38 bytes) is one choice: +0x08 u32 condition (811C9DC5 = none), +0x20 u64
//     hash64 of the audio event (8080B128).
//   8080B4D1 dialogue tables, Vec<8080AA34 {u32 key, rel pointer}> at +0x18: clip events of kind
//     4 by value; the entry's lines (8080AA35 -> 8080AA3A -> 8080AA3F +0x28) are its audio events.
// The tables are searched in the components and the tags they name, two levels deep.
// Component loops (`loops`) are found the same way (u32 tags or hash64 of audio events).
struct EntitySounds {
    std::unordered_map<uint32_t, std::vector<TagHash>> by_name;  // unconditional choice first
    std::unordered_map<uint32_t, std::vector<TagHash>> barks;    // one is picked per play
    // Loops the components start through their sequences (8080B66D, not decoded): audio events
    // they name outside the tables whose play starts an endless voice, that have a stop event, a
    // stream of 8 s or more and a reach (8080B12E max distance) of 40 m or more: the beds (engine,
    // idle hum). Played while the entity is shown (推定).
    std::vector<TagHash> loops;
    size_t tables = 0, dialogue_tables = 0;
    bool empty() const { return by_name.empty() && barks.empty(); }
    // The audio events a clip event starts (kind 5: its name, kind 4: its value), else empty.
    const std::vector<TagHash>* find(uint16_t kind, uint32_t name, uint32_t value) const;
};
// Needs the hash64 index (PackageManager::build_hash64_index).
EntitySounds load_entity_sounds(PackageManager& pm, std::span<const TagHash> entities, bool include_close_loops = false);

}  // namespace nim
