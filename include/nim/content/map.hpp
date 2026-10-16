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
inline constexpr uint32_t kClassMapNodeTable = 0x8080B1A7;
inline constexpr uint32_t kClassTerrain = 0x80808567;

// STerrain 80808567 (layout as goriath's, verified on Marathon data): 0x10 bounds min, 0x20 max,
// 0x30 the position offset its VS adds (cb11[0]), 0x50 Vec<group, 0x60 bytes: +0x20 texcoord
// transform (cb11[1]), +0x50 dyemap texture>, 0x60 vertex0 (int16x4 position), 0x64 vertex1
// (snorm16x4 normal, half2 texcoord), 0x68 index buffer (strips), 0x6C shadow / 0x70 depth-only
// techniques, 0x78 Vec<part, 12 bytes: technique, index start u32, count u16, group u8, detail
// u8 (0 = high ... 3 = thumbnail)>.
struct Terrain {
    TagHash tag;
    std::array<float, 3> lo{}, hi{};
    std::array<float, 4> offset{};
    struct Group {
        std::array<float, 4> texcoord{};
        TagHash dyemap;
    };
    std::vector<Group> groups;
    TagHash vertex0, vertex1, index, technique_shadow, technique_depth;
    struct Part {
        TagHash technique;
        uint32_t index_start = 0;
        uint16_t index_count = 0;
        uint8_t group = 0, detail = 0;
    };
    std::vector<Part> parts;
};
Terrain load_terrain(PackageManager& pm, TagHash tag);

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
    std::vector<TagHash> node_tables;    // the SMapNodeTables (placed entities, lights, ...)
    std::vector<MapStatic> statics;      // one per distinct mesh
    std::vector<Terrain> terrains;       // terrain patches
    size_t instances = 0;
    std::array<float, 3> lo{}, hi{};     // bounds of the instance origins
};
MapLayout load_map_layout(PackageManager& pm, TagHash bubble);

// Where the geometry is: the per-axis median of the instance origins (bounds are stretched by
// far-off outliers such as skyboxes).
std::array<float, 3> map_focus(const MapLayout& map);
// The instances whose origin lies within `radius` of `center`, grouped by mesh, nearest meshes first.
std::vector<MapStatic> map_region(const MapLayout& map, std::array<float, 3> center, float radius);
// The terrain patches whose bounds come within `radius` of `center`, nearest first.
std::vector<const Terrain*> map_terrain_region(const MapLayout& map, std::array<float, 3> center, float radius);

// A placed local light: an SShadowingLight (80808557) from a node's shadowing-light component
// (80808544), or one SLight (80808556) of an SLightCollection (8080854B, component 80808335).
// Matrices are column-major 4x4.
inline constexpr uint32_t kClassLightCollection = 0x8080854B, kClassShadowingLight = 0x80808557;
struct MapLight {
    TagHash source;                    // the SShadowingLight / SLightCollection tag
    TagHash technique;                 // lighting_apply technique (deferred light into the light buffers)
    std::array<float, 16> node{};      // light local -> world (node / collection transform)
    std::array<float, 16> light_space{};  // unit cube (NDC) -> light local, projective
    std::array<float, 20> params{};    // SLight +0x00..+0x4F: two clip planes, two more, plane exponents
    std::array<float, 3> lo{}, hi{};   // world bounds of the light volume
    bool shadowing = false;
};
std::vector<MapLight> load_map_lights(PackageManager& pm, const MapLayout& map);
// The lights whose volume comes within `radius` of `center`.
std::vector<const MapLight*> map_light_region(const std::vector<MapLight>& lights, std::array<float, 3> center, float radius);

}  // namespace nim
