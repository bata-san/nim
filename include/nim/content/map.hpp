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
#include <unordered_map>
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
// The same over any set of placed meshes (e.g. MapEntities::models).
std::vector<MapStatic> map_region(const std::vector<MapStatic>& placed, std::array<float, 3> center, float radius);
// The terrain patches whose bounds come within `radius` of `center`, nearest first.
std::vector<const Terrain*> map_terrain_region(const MapLayout& map, std::array<float, 3> center, float radius);

// A placed local light: an SShadowingLight (80808557) from a node's shadowing-light component
// (80808544), or one SLight (80808556) of an SLightCollection (8080854B, component 80808335).
// Matrices are column-major 4x4.
inline constexpr uint32_t kClassLightCollection = 0x8080854B, kClassShadowingLight = 0x80808557;
struct MapLight {
    TagHash source;                    // the SShadowingLight / SLightCollection tag
    TagHash technique;                 // lighting_apply technique (deferred light into the light buffers)
    TagHash volumetrics;               // light shaft technique (SShadowingLight +0xDC, collection light +0xC8)
    std::array<float, 16> node{};      // light local -> world (node / collection transform)
    std::array<float, 16> light_space{};  // unit cube (NDC) -> light local, projective
    std::array<float, 20> params{};    // SLight +0x00..+0x4F: two clip planes, two more, plane exponents
    std::array<float, 3> lo{}, hi{};   // world bounds of the light volume
    bool shadowing = false;
};
std::vector<MapLight> load_map_lights(PackageManager& pm, const MapLayout& map);
// The lights whose volume comes within `radius` of `center`.
std::vector<const MapLight*> map_light_region(const std::vector<MapLight>& lights, std::array<float, 3> center, float radius);

// A reflection / ambient probe volume: a node's SCubemapComponent (80807F3C, inline, 0x1D0 bytes,
// world space). Layout per goriath SCubemapComponent, checked against the cubemap_apply shaders.
inline constexpr uint32_t kClassCubemapComponent = 0x80807F3C;
struct MapCubemap {
    std::array<float, 3> extents{}, center{};   // +0x10 half extents, +0x20 centre
    std::array<float, 4> rotation{};             // +0x100 quaternion (x, y, z, w)
    std::array<float, 16> local_to_world{};      // +0xC0, unit cube -> world, column-major
    std::array<float, 16> axes{};                // +0x120 rotation (columns) + centre
    std::array<uint32_t, 3> probes{1, 1, 1};     // +0x160 diffuse probe grid resolution
    std::array<float, 4> probe_params{};         // +0x170
    std::array<float, 4> flags{};                // +0x180 (.y > 0 relighting, .z > 0 && .w != 0 alpha)
    float unk30 = 0, fade_start = 0, fade_end = 0;  // +0x30, +0x3C, +0x40
    std::array<float, 3> blend_neg{}, blend_pos{};  // +0x50 / +0x60: per-face blend widths (fraction of the half extent)
    TagHash specular, alpha, diffuse;            // +0x1B8 cube, +0x1BC cube, +0x1C0 3D probe volume
    std::array<float, 3> lo{}, hi{};             // world bounds
    bool use_alpha() const { return flags[2] > 0 && flags[3] != 0; }
    bool use_relighting() const { return flags[1] > 0; }
    bool use_probes() const { return probes[0] * probes[1] * probes[2] > 1; }
};
std::vector<MapCubemap> load_map_cubemaps(PackageManager& pm, const MapLayout& map);
std::vector<const MapCubemap*> map_cubemap_region(const std::vector<MapCubemap>& cubemaps, std::array<float, 3> center, float radius);

// ---- atmosphere ----------------------------------------------------------------------------
// A map's sky / atmosphere: the node component 808084A0 (D2 SAtmosphereDataComponent layout,
// payload = record + 0x10) and the sun path component 80808346. See docs/PROGRESS.md 3.2.
inline constexpr uint32_t kClassAtmosphereComponent = 0x808084A0;
inline constexpr uint32_t kClassSunPathComponent = 0x80808346;
// A map's environment (sun, ambient, fog, sky ... over the day): an entity placed on a map node
// whose components (8080BADB) hold sequence nodes 8080B64E (cf. D2 SSequenceGlobalChannel), each a
// sequencer program computing one render-globals channel, named by its FNV-1 hash, from the time
// of day. Record (payload = class + 4): +0x00 channel hash, +0x14 / +0x18 start / cycle seconds
// (3600: the day), +0x30 bytecode vector, +0x40 constant vector. See docs/PROGRESS.md 3.2.
inline constexpr uint32_t kClassSequenceGlobalChannel = 0x8080B64E;
struct MapChannelExpression {
    uint32_t channel = 0;
    float start = 0, cycle = 0;
    std::vector<uint8_t> bytecode;
    std::vector<std::array<float, 4>> constants;
    TagHash component;
};
struct MapAtmosphere {
    // +0x80..+0xBF: the sky lookups (3D, one slice per time-of-day key): near0, far0, near1, far1.
    std::array<TagHash, 4> lookups{};
    TagHash density_lut;                    // +0xC0: 2D, time x distance
    std::array<float, 16> slice_times{};    // +0xC8: the time of day of each lookup slice
    // 80808346 +0x00 -> 80808349: four 8080ACD3 direction tables (table 0 = the moving sun), one
    // entry per 1/1800 of the day; +0x08 the map's time of day (estimate).
    std::array<std::vector<std::array<float, 3>>, 4> paths;
    float default_time = 0.5f;
    std::vector<MapChannelExpression> channel_expressions;  // the map environment, in data order
    bool valid() const { return !lookups[1].is_none(); }
    // The direction (toward the sun) of path `i` at time `t` (0..1), interpolated.
    std::array<float, 3> path_direction(int i, float t) const;
    // The lookup slice coordinate (0..1, at texel centres) for time `t`.
    float slice_coordinate(float t) const;
};
MapAtmosphere load_map_atmosphere(PackageManager& pm, const MapLayout& map);

// ---- entities ------------------------------------------------------------------------------
// A map's placed entities: every SMapNodeTable of the map's package family (the activity layers
// as well as the bubble's own) names, per node, an entity (8080BAAD) by hash64 at +0x30, at the
// node's transform (quat xyzw, translation, uniform scale at +0x00). An entity lists its
// components (8080BADB); a component that is the SDynamicModelComponent of a dynamic model
// (index_dynamic_model_materials) puts that model at the node.
struct DynamicModelMaterials;
struct MapEntities {
    std::vector<MapStatic> models;  // per dynamic model, the 3x4 rows of its placements
    size_t node_tables = 0, nodes = 0, entities = 0, placements = 0;
};
// The map's sky objects: node components 80808378 (SSkyObjectCollectionComponent) name a
// collection (8080837C: +0x08 Vec of 0x90-byte objects: +0x00 transform (column-major mat4),
// +0x40 bounds, +0x60 model ref 80808383 (+0x08 the dynamic model), +0x64 draw order, +0x70 kind).
// The distant scenery (mountains, terrain far out, celestial shapes) placed kilometres away; per
// dynamic model, the 3x4 rows of its placements. Kind 5 is left out (goriath: draws solid red).
std::vector<MapStatic> load_map_sky_objects(PackageManager& pm, const MapLayout& map);
// Needs the hash64 index (PackageManager::build_hash64_index).
MapEntities load_map_entities(PackageManager& pm, TagHash bubble,
                              const std::unordered_map<uint32_t, DynamicModelMaterials>& materials);

}  // namespace nim
