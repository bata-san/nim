#pragma once
#include "nim/content/map.hpp"
#include "nim/core/package_manager.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace nim {

// Map ambient audio emitters decoded from bubble node components:
//   - 80802A95 -> 80802A97 (0x60-byte records, 80802A99): multi-point / polyline ambient emitters
//     with world-space vertices (80800094) and bounding radius.
//   - 80803135 -> 80803137 (0x40-byte records, 80803139): spatialized point ambient emitters.
//   - 8080B107 (0x10-byte record, 8080B120): map global ambient bed (if streams resolve).
// Each record names a Wwise AudioEvent (8080B128) via wide_hash at +0x00, whose vec_at(0x20)
// holds its .wem stream tags (type 26/7).
struct MapAudioEmitter {
    TagHash event;
    uint32_t event_id = 0;
    std::vector<TagHash> wems;
    std::array<float, 3> position{};
    std::vector<std::array<float, 3>> points;  // non-empty for polyline/multi-point emitters
    float radius = 0.0f;                       // authored bounding radius (m), 0 for point emitters
    float gain = 1.0f;                         // authored scale/gain (+0x38)
    bool polyline = false;
    bool global = false;

    // Closest point on this emitter to listener `p` (polyline segments or point position),
    // and Euclidean distance in metres.
    std::array<float, 3> closest_point(std::array<float, 3> p) const;
    float distance_to(std::array<float, 3> p) const;
};

struct MapObjectSound {
    TagHash entity, event;
    std::array<float, 3> position{};
    float max_distance = 0;
};

struct MapAudio {
    // Loops of placed entities (docs/PROGRESS.md 3.107 / 3.108): only those of entities that read
    // no object state. The components' sequences (not decoded) start the loops of the others from
    // their state (device power / lock / position, interaction, motion, rain, light flicker
    // mode ...), which nim does not simulate, so they are left out: `gated_loops` counts them per
    // the first state channel found.
    std::vector<MapObjectSound> objects;
    std::map<uint32_t, size_t> gated_loops;
    std::vector<MapAudioEmitter> emitters;
    size_t point_emitters = 0;
    size_t polyline_emitters = 0;
    size_t global_emitters = 0;
    size_t unique_events = 0;
    size_t unique_wems = 0;
    size_t events_with_wems = 0;
    size_t events_without_wems = 0;
};

// Loads all ambient audio emitters referenced by `layout.node_tables`.
MapAudio load_map_audio(PackageManager& pm, const MapLayout& layout);

}  // namespace nim
