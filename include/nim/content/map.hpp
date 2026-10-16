#pragma once
// Maps ("bubbles"): the placed static geometry of a level.
//
//   SBubbleParent 8080AB27 -> SBubbleDefinition 8080A7BB -> 8080B0B5 -> SMapContainer 8080A7C1
//   -> 8080BA05 -> SMapNodeTable 8080B1A7 (nodes; a node's static-instances component payload
//   names an 808082D5) -> SStaticMeshInstances 8080A7F1.
// SStaticMeshInstances (verified on Marathon data): 0x40 Vec<transform, 0x60 bytes: quat xyzw,
// translation xyz, uniform scale>, 0x78 Vec<static mesh TagHash>, 0x88 Vec<group, 16 bytes:
// instance start, instance count, static index, ?>. The nodes holding them sit at the origin, so
// the transforms are world space.

#include "nim/core/package_manager.hpp"

#include <array>
#include <string>
#include <vector>

namespace nim {

inline constexpr uint32_t kClassBubbleParent = 0x8080AB27;
inline constexpr uint32_t kClassStaticInstances = 0x8080A7F1;

struct MapEntry {
    TagHash bubble;
    std::string package;  // the package the bubble tag is in (w64_perimeter_0147 ...)
    size_t statics = 0, instances = 0;
};
// Every bubble with static geometry, in package order. Walks each bubble's structure tags.
std::vector<MapEntry> list_maps(PackageManager& pm);

struct MapStatic {
    TagHash mesh;
    // Per instance: the 3x4 object-to-world rows (rotation * scale, translation in .w).
    std::vector<std::array<float, 12>> rows;
};
struct MapLayout {
    TagHash bubble;
    std::vector<TagHash> instance_tags;  // the SStaticMeshInstances found
    std::vector<MapStatic> statics;      // one per distinct mesh
    size_t instances = 0;
    std::array<float, 3> lo{}, hi{};     // bounds of the instance origins
};
MapLayout load_map_layout(PackageManager& pm, TagHash bubble);

}  // namespace nim
