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
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <unordered_map>
#include <unordered_set>
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
    std::string name;
    TagHash activity;                    // none = DEIMOS MapTab; otherwise ActivityTab
    uint32_t activity_map = 0, bubble_name = 0;
};
// Every bubble with static geometry, in package order. Walks each bubble's structure tags.
std::vector<MapEntry> list_maps(PackageManager& pm);
std::vector<MapEntry> list_activity_maps(PackageManager& pm);
// DEIMOS ActivityTab: selected SActivity.unk50 row plus matching ambient activity row.
std::vector<TagHash> load_activity_node_tables(PackageManager& pm, TagHash activity, uint32_t bubble_name, uint32_t map_index);

struct RegionPermutations;
struct MapStatic {
    TagHash mesh;
    // Per instance: the 3x4 object-to-world rows (rotation * scale, translation in .w).
    std::vector<std::array<float, 12>> rows;
    std::vector<float> sky_order;  // one authored sort key per row for sky collections
    bool patrol_start = false;  // static drone placement replaced while the patrol simulation is shown
    // Placed entities: the part-group permutations the entity's region selector picks with no
    // spawn tags (3.125); null = no selector, every part group drawn.
    std::shared_ptr<const RegionPermutations> regions;
};
inline constexpr uint32_t kBubbleOutpost = 0x80AF9550;  // exp_outpost
inline constexpr uint32_t kClassMapDecals = 0x808081B2;
struct MapDecal {
    TagHash collection, technique, index, vertex;
    uint32_t ordinal = 0, triangle_count = 0;
    std::array<float, 12> rows{};
    std::array<float, 4> scale{}, offset{}, uv{};
};

struct MapDecalVolumeSet {
    TagHash technique;
    uint16_t first = 0, count = 0;
    std::array<float, 4> sphere{};
    float half_thickness = 0;
};

struct MapDecalVolumes {
    TagHash tag, instances, vertices;
    std::vector<MapDecalVolumeSet> sets;
};

struct MapEventPreview {
    uint64_t id = 0;  // authored node ID for a ship, or the script component tag
    std::string name;
    std::array<float, 3> position{};
    size_t placements = 0;
};
struct MapLayout {
    TagHash bubble;
    std::vector<TagHash> instance_tags;  // the SStaticMeshInstances found
    std::vector<TagHash> node_tables;    // the SMapNodeTables (placed entities, lights, ...)
    std::vector<MapStatic> statics;      // one per distinct mesh
    std::vector<Terrain> terrains;       // terrain patches
    std::vector<TagHash> decorator_groups; // placement streams referenced by the map nodes
    std::vector<TagHash> decal_collections, decal_volumes;
    size_t instances = 0;
    std::array<float, 3> lo{}, hi{};     // bounds of the instance origins
    // Nodes (id at node +0x70) nim leaves out by user decision (3.111), filled by hide_scripted_nodes:
    std::unordered_set<uint64_t> hidden_nodes;     // model, FX, lights and ambient emitters are omitted
    std::unordered_set<uint64_t> hidden_fx_nodes;  // their FX (particles and FX lights) not run
    std::unordered_set<uint64_t> patrol_drone_nodes;
    std::vector<MapEventPreview> event_previews;
    std::unordered_map<uint64_t, TagHash> preview_models;
    bool hidden(uint64_t node, bool fx) const { return hidden_nodes.contains(node) || (fx && hidden_fx_nodes.contains(node)); }
};
MapLayout load_map_layout(PackageManager& pm, TagHash bubble, std::span<const TagHash> additional_tables = {});
std::vector<MapDecal> load_map_decals(PackageManager& pm, const MapLayout& map);
std::vector<MapDecalVolumes> load_map_decal_volumes(PackageManager& pm, const MapLayout& map,
                                                  std::array<float, 3> center, float radius);

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
    // SLight +0x00..+0x5F, copied to deferred_light +0x100..+0x15F: four world-space clip planes
    // (n, d) at +0x00..+0x3F (unused ones (0, 0, -1, 500000)), their exponents at +0x40 and the
    // diffuse / shadow weights at +0x50 (+0x150 / +0x154).
    std::array<float, 24> params{};
    std::array<float, 3> lo{}, hi{};   // world bounds of the light volume
    bool shadowing = false;
    TagHash shadow_technique;          // SShadowingLight +0xD8: lighting_apply with the shadow map
    TagHash shadow_volumetrics;        // SShadowingLight +0xE0: light shafts with the shadow map
    // SShadowingLight +0xFC..+0x108: camera distances (light fade start, end, shaft fade start,
    // end); (0, 0) for none. The shaft pair is set only on lights with a shaft (+0xDC) and is
    // reversed on some (start > end: the shaft fades in with distance). Collection lights have
    // no such fields: all 0.
    std::array<float, 4> fade_distances{};
    // A light an FX sequence carries (load_map_fx_lights): drawn with the map's FX only.
    bool fx = false;
    // The light's fade (deferred_light +0x160) and its shaft's fade at a camera distance
    // (estimate: saturate((end - d) / (end - start)), linear; 1 where start == end).
    float light_fade(float distance) const { return fade_at(fade_distances[0], fade_distances[1], distance); }
    float shaft_fade(float distance) const { return fade_at(fade_distances[2], fade_distances[3], distance); }
    static float fade_at(float start, float end, float d) {
        if (!(end != start)) return 1.0f;
        const float f = (end - d) / (end - start);
        return f < 0 ? 0.0f : f > 1 ? 1.0f : f;
    }
};
std::vector<MapLight> load_map_lights(PackageManager& pm, const MapLayout& map);
// The lights whose volume comes within `radius` of `center`.
std::vector<const MapLight*> map_light_region(const std::vector<MapLight>& lights, std::array<float, 3> center, float radius);

// A reflection / ambient probe volume: a node's SCubemapComponent (80807F3C, inline, 0x1D0 bytes,
// world space). Layout per goriath SCubemapComponent, checked against the cubemap_apply shaders.
inline constexpr uint32_t kClassCubemapComponent = 0x80807F3C;
struct MapCubemap {
    std::array<float, 3> extents{}, center{};   // +0x10 half extents, +0x20 bounds centre
    std::array<float, 3> origin{};              // +0x110: auxiliary shape transform origin; lighting use unverified
    std::array<float, 4> rotation{};             // +0x100 quaternion (x, y, z, w)
    std::array<float, 16> local_to_world{};      // +0xC0, unit cube -> world, column-major
    std::array<float, 16> axes{};                // +0x120 rotation (columns) + centre
    std::array<uint32_t, 3> probes{1, 1, 1};     // +0x160 diffuse probe grid resolution
    std::array<float, 4> probe_params{};         // +0x170
    std::array<float, 4> flags{};                // +0x180 (.y > 0 relighting, .z > 0 && .w != 0 alpha)
    float unk30 = 0, fade_start = 0, fade_end = 0;  // +0x30, +0x3C, +0x40
    std::array<float, 4> shape{};                // +0x70 (D2 +0x60): xyz > 0 rounds that axis pair, .w > 0 = cube_sphere variant
    std::array<float, 4> unk80{};                // +0x80 (D2 +0x70)
    std::array<float, 3> blend_neg{}, blend_pos{};  // +0x50 / +0x60: per-face blend widths (fraction of the half extent)
    TagHash specular, alpha, diffuse;            // +0x1B8 cube, +0x1BC cube, +0x1C0 3D probe volume
    TagHash shape_vertices;                     // +0x1C4: authored CSG boundary, float3 triangle list
    TagHash shape_records;                      // +0x1B4: authored add/subtract shape collection
    std::array<float, 3> lo{}, hi{};             // world bounds
    bool use_alpha() const { return flags[2] > 0 && flags[3] != 0; }
    bool use_relighting() const { return flags[1] > 0; }
    bool use_probes() const { return probes[0] * probes[1] * probes[2] > 1; }
};
std::vector<MapCubemap> load_map_cubemaps(PackageManager& pm, const MapLayout& map);
std::vector<const MapCubemap*> map_cubemap_region(const std::vector<MapCubemap>& cubemaps, std::array<float, 3> center, float radius);

// A player start of an activity (estimate): the nodes of component 80802D32, present only in the
// activity node tables (Outpost: four nodes a few metres apart at (148, 52, -4.7), next to the two
// 808035B8 nodes). Payload +0x00: a name hash. The node transform gives position and facing.
inline constexpr uint32_t kClassStartComponent = 0x80802D32;
inline constexpr uint32_t kClassAiSquad = 0x80805BE0;       // native squad slot (3.66)
inline constexpr uint32_t kClassAiObjective = 0x80805B33;   // a squad's objective: firing areas (3.67)
inline constexpr uint32_t kClassAiFiringArea = 0x8080B0D1;  // floor polygon prism (3.67)
inline constexpr uint32_t kClassPointGroup = 0x8080B831;    // named points, optionally a spline (3.67)
inline constexpr uint32_t kClassPoint = 0x8080B83C;         // one point of a point group (3.67)
inline constexpr float kStartEyeHeight = 1.7f;  // viewer eye above a start (estimate, not engine data)
struct MapStart {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};  // quaternion x, y, z, w
    uint32_t name = 0;
    std::array<float, 3> forward() const;      // the node's +X in world space
};
std::vector<MapStart> load_map_starts(PackageManager& pm, const MapLayout& map);
// The map's own starts, else those of the first activity on the same bubble that has some.
std::vector<MapStart> find_map_starts(PackageManager& pm, const MapLayout& map);

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
// A sequencer input (opcode 0x4A n), from the name table of the entity's sequence component
// (8080AF85 entries: namespace, name, slot; input n is slot `outputs + n`). Namespace "" is a
// sequence parameter (input 0, the time of day); 005921DB names a world global channel
// (rain_amount, sky_transition_weather, fog_amount), `world_slot` its slot in the world channel
// registry (-1: not registered).
struct SequencerInput {
    uint32_t name = 0, name_space = 0;
    int world_slot = -1;
    bool world() const { return name_space != 0x811C9DC5u; }
};
struct MapChannelExpression {
    // The channel the output is written to: the name at the record's index (+0x28) in the
    // component's output table (80809F46 structs, +0x30 the FNV-1 name). The record's own first
    // word (`record_id`) equals it for most outputs but not all (sun_intensity, sun_glow_color,
    // sky_color_override ... carry another hash there), so it is never used as the name.
    uint32_t channel = 0;
    uint32_t record_id = 0;
    uint32_t index = 0;
    float start = 0, cycle = 0;
    std::vector<uint8_t> bytecode;
    std::vector<std::array<float, 4>> constants;
    TagHash component;
    // The inputs of this expression's own sequence, by input number; null: no slot table found.
    std::shared_ptr<const std::vector<SequencerInput>> inputs;
};
struct MapAtmosphere {
    // +0x80..+0xBF: the sky lookups (3D, one slice per time-of-day key): near0, far0, near1, far1.
    std::array<TagHash, 4> lookups{};
    TagHash density_lut;                    // +0xC0: 2D, time x distance
    std::array<float, 16> slice_times{};    // +0xC8: the time of day of each lookup slice
    // 80808346 +0x00 -> 80808349: four 8080ACD3 direction tables (table 0 = the moving sun), one
    // entry per 1/1800 of the day; +0x08 the map's time of day (estimate).
    std::array<std::vector<std::array<float, 3>>, 4> paths;
    TagHash lens_glare, lens_upsample;      // techniques from the atmosphere's 80808351 tag
    float default_time = 0.5f;
    const char* default_time_source = "sun path component +0x08 (estimate)";
    std::vector<MapChannelExpression> channel_expressions;  // the map environment, in data order
    bool valid() const { return !lookups[1].is_none(); }
    // The sky keyframe time (slice_times) nearest to midnight: half a day after the moving sun's
    // highest point. The night look.
    float night_time() const;
    // The direction (toward the sun) of path `i` at time `t` (0..1), interpolated.
    std::array<float, 3> path_direction(int i, float t) const;
    // The lookup slice coordinate (0..1, at texel centres) for time `t`.
    float slice_coordinate(float t) const;
};
MapAtmosphere load_map_atmosphere(PackageManager& pm, const MapLayout& map);
std::vector<MapCubemap> load_entity_preview_probes(PackageManager& pm);

// ---- entities ------------------------------------------------------------------------------
// A selection's placed entities: only its MapLayout.node_tables (bubble, or bubble plus the
// selected activity/ambient phases), never the whole package family. Each node names an entity
// (8080BAAD) by hash64 at +0x30, at the
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
// dynamic model, the 3x4 rows of its placements. Kinds 5 (goriath: solid red) and 6 (a greyscale
// whole-sky capture covering the other layers) are left out.
std::vector<MapStatic> load_map_sky_objects(PackageManager& pm, const MapLayout& map);
// Needs the hash64 index (PackageManager::build_hash64_index).
MapEntities load_map_entities(PackageManager& pm, const MapLayout& map,
                              const std::unordered_map<uint32_t, DynamicModelMaterials>& materials);

// ---- gameplay placements ------------------------------------------------------------------
// See docs/PROGRESS.md 3.65. An activity's node tables (bubble + phases) place gameplay objects:
//  - spawn points: a node naming (hash64 at +0x30) an entity whose components include the start
//    component 80802D32 (the class of the player starts). Each such entity is one spawn-point
//    archetype; which side or character uses an archetype is not decoded.
//  - loot containers: nodes carrying a label list (80802EC2) whose entity has a dynamic model.
//  - label lists (80802EC2, node component payload +0x10): u32 count, then FNV-1 name hashes. On
//    Outpost the first is a zone (the hash zone_region components test) and the second a kind.
//  - script objects: components of the map's package family (8080BADB). A BlockScript one names
//    an 80800000 definition holding its module path (e.g. "bs/world/prefabs/uesc_scan_drone.bs"
//    with backslashes); a native one (no definition) has its class id at +0xAC. They store node
//    placement ids (node +0x70), and refer to each other by slot id (3.66).
// Squads (3.66): native class 80805BE0 components listing characters by the 64-bit hash of an
// 8080B1A7 descriptor in the shared globals; the descriptor's entity has a character.bs
// component. A squad links by slot id to location components (natives 80805B64, 80805AD8,
// 80809943) whose placement ids give its spawn positions, and is linked from its
// slots/squad_group. These positions are nodes of their own, not the start-component spawn
// points, which no squad names.
struct MapGameplayPoint {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};  // quaternion x, y, z, w
    TagHash entity, model;                     // model: the entity's dynamic model, if any
    uint64_t id = 0;                           // node +0x70
    std::vector<uint32_t> labels;              // 80802EC2 name hashes (empty entries dropped)
};
// A character a squad can spawn: an 8080B1A7 descriptor (shared globals) whose entity (hash64 at
// +0x60) has a bs/character/character.bs component. labels: its 80802EC2 list.
struct MapCharacter {
    TagHash item, entity;
    std::vector<uint32_t> labels;
    // +0x130 Vec<8080402D> {name, value} FNV-1 pairs: the tags the entity spawns with, e.g.
    // type = elite, unit_type = assault, weapon_type = ... (they pick its model permutation, 3.92).
    std::vector<std::pair<uint32_t, uint32_t>> tags;
    // 推定: the squad member slot name most often given to this character in squads that spawn only
    // it, without "_core" / "_N" (e.g. uesc_assault_elite_lmg_uesc). No name string is stored with
    // the descriptor; empty when no such squad exists.
    std::string name_estimate;
};
struct MapScriptObject {
    TagHash component, definition;
    std::string module;         // the definition's module path, else "native CLASS"
    uint32_t native_class = 0;  // +0xAC of a component without a BlockScript definition
    uint64_t slot = 0;          // the slot's own 64-bit id (0x20 after the slot header)
    std::vector<std::pair<uint32_t, std::string>> names;            // name table: (field offset, name)
    std::vector<std::pair<uint32_t, MapGameplayPoint>> placements;  // (component offset, node)
    std::vector<std::pair<uint32_t, size_t>> characters;            // (offset, MapGameplay::characters)
    std::vector<std::pair<uint32_t, size_t>> links;                 // (offset, MapGameplay::scripts) by slot id
};
// AI behaviour areas and routes (3.67). Offsets are from the slot header.
//  - firing area (8080B0D1): world->local transform (quaternion h+0x70, translation h+0x80; a
//    point maps to local as R*p + t), a floor polygon (typed list 80800094 of float4, local z =
//    floor) triangulated by a byte list 8080BA19 (3 indices per triangle), and the prism height
//    at h+0x190. The component ends with its debug path; name = its last segment.
//  - objective (80805B33): links to firing areas. Each squad links to one objective.
//  - point group (8080B831): links to points (8080B83C: quaternion h+0x70, world position h+0x80)
//    and, for paths (e.g. uesc_scan_drone pg_drone_path), a typed list 80800090 of float4 world
//    positions sampling a spline.
struct MapFiringArea {
    size_t script = 0;                         // MapGameplay::scripts
    std::string name;                          // e.g. fa_walkway
    std::array<float, 4> rotation{0, 0, 0, 1}; // stored world->local quaternion
    std::array<float, 3> translation{};        // stored world->local translation
    std::vector<std::array<float, 3>> outline; // floor polygon, world space
    std::vector<std::array<uint8_t, 3>> triangles;
    float height = 0;
};
struct MapObjective {
    size_t script = 0;
    std::vector<size_t> firing_areas;  // MapGameplay::firing_areas
};
struct MapPointGroup {
    size_t script = 0;
    std::optional<size_t> owner;                // the first component linking to it
    std::vector<std::array<float, 3>> points;   // its 8080B83C points, in link order
    std::vector<std::array<float, 3>> spline;   // sampled path, if any
};
struct MapSquad {
    size_t script = 0;                       // the squad component (MapGameplay::scripts)
    std::optional<size_t> group;             // the squad_group linking to it (MapGameplay::scripts)
    std::optional<size_t> objective;         // MapGameplay::objectives
    std::vector<size_t> members;             // one per spawned member (MapGameplay::characters)
    std::vector<MapGameplayPoint> positions; // distinct nodes of the linked location components
};
struct MapGameplay {
    std::vector<MapGameplayPoint> spawns, loot;
    std::vector<MapScriptObject> scripts;  // components that place, spawn or link something, or are linked to
    std::vector<MapCharacter> characters;
    std::vector<MapSquad> squads;
    std::vector<MapFiringArea> firing_areas;
    std::vector<MapObjective> objectives;
    std::vector<MapPointGroup> point_groups;
    std::vector<std::pair<std::string, size_t>> modules;  // script components of the package family, by module
    size_t nodes = 0;
};
// Needs the hash64 index (PackageManager::build_hash64_index).
MapGameplay load_map_gameplay(PackageManager& pm, TagHash activity);
// Audit a single map selection without borrowing placements from other activity modes.
MapGameplay load_map_gameplay_selection(PackageManager& pm, const MapEntry& selection);

// Script-placed nodes nim hides by user decision (3.111, 2026-11-23), from the gameplay scripts of
// `activity` (none: of every activity using map.bubble):
//  - DCON: every node a script object of a dcon module (pf_dcon.bs, dcon_combiner.bs,
//    pf_dcon_extraction_machine.bs) places, itself or through the objects it links to (2 deep):
//    their FX are not run (MapLayout::hidden_fx_nodes; the device models stay).
//  - lockdown ships: the nodes lockdown.bs objects place (Dire Marsh: 80B08991 at z = 109), and
//    those placed by a spawner linking to the parts of a room_lockdown_system.bs (Outpost: 80B0D412
//    over the room at (87, 144)): not drawn at all (MapLayout::hidden_nodes).
// Expedition selections also hide authored ship placements and build an event palette.
// preview restores the selected models and linked FX for inspection. Returns the number hidden.
// Needs the hash64 index.
size_t hide_scripted_nodes(PackageManager& pm, MapLayout& map, TagHash activity = {}, uint64_t preview = 0);

// ---- particle systems (FX) ----------------------------------------------------------------
// A GPU particle system (808081E8, 64 bytes): +0x00 its definition (808081EF: header floats at
// +0x128 / +0x134 and an FX expression program not decoded), then techniques: +0x04
// cs_gpu_stateful_init (spawn), +0x08 cs_gpu_stateful_update, +0x10
// cs_gpu_stateless_update_instance (11 float4 per particle), +0x14 the draw (vs_gpu_update_draw
// + PS: a camera-facing 4-vertex quad per particle), +0x28 gpu_debug_cs (unused).
constexpr uint32_t kClassParticleSystem = 0x808081E8;
struct ParticleSystem {
    TagHash tag, definition, init, update, instance, draw;
    float lifetime = 0;  // definition +0x128: the longest particle life (seconds), estimate
    float rate = 0;      // definition +0x134: particles per second, estimate
    uint16_t state_float4s = 0, instance_float4s = 0;  // definition +0x48 / +0x4C
    // Mesh particles: +0x20 a 64-bit hash naming a mesh record (808081F1) that holds a dynamic
    // model (8080881C); its first mesh is drawn per particle (the UESC assault's shield: a unit cube).
    TagHash mesh;
};
ParticleSystem load_particle_system(PackageManager& pm, TagHash tag);
// The particle systems a map places: nodes -> entity (8080BAAD) -> components (8080BADB) that
// name systems (the sequencer data 8080B66D of a pattern). One per (node, system), at the node's
// transform. Needs the hash64 index (PackageManager::build_hash64_index).
struct MapParticle {
    TagHash system;
    std::array<float, 12> rows;  // rotation * scale | translation
    // An object channel the holding component's sequence reads (the exfil device's
    // exfil_activation_progress, a device's device_position), or kChannelTurret for a turret's FX,
    // 0 if none: the systems play on that run-time state.
    uint32_t gate = 0;
};
inline constexpr uint32_t kChannelExfilActivationProgress = 0x4AE64998;  // fnv1("exfil_activation_progress")
// An entity whose components name the object channel "turret" (the showcase turrets): its FX are the
// turret's aiming laser and shots, which play on its behaviour (3.111). MapParticle::gate = this.
inline constexpr uint32_t kChannelTurret = 0x9F6DB313;  // fnv1("turret")
// A device's FX component whose sequence reads device_position (e.g. 80B0DD90's red laser 80B0DD71)
// plays on the device's run-time state, like the exfil device (3.111). MapParticle::gate = this.
inline constexpr uint32_t kChannelDevicePosition = 0x6D408B83;  // fnv1("device_position")
std::vector<MapParticle> load_map_particles(PackageManager& pm, const MapLayout& map);
// The lights the same FX components carry (docs/PROGRESS.md 3.111): a placed entity's component
// (8080BADB) that names particle systems also lists its lights as sequence records
// {80808323, u32 attachment name hash (fnv1 "" = the entity origin), u32 index << 16, u32 flags,
// 0, 0, f32, f32, FFFF0023, FF, light tag} with the light an SShadowingLight (80808557) or an
// SLightCollection (8080854B). Each light is placed at the holding node (MapLight::fx = true);
// components gated on exfil_activation_progress are left out like their systems.
// `attached` counts the lights with a named attachment, placed at the node (estimate).
std::vector<MapLight> load_map_fx_lights(PackageManager& pm, const MapLayout& map, size_t* attached = nullptr);
// The particle systems of an entity's shield component (docs/PROGRESS.md 3.88, 推定): the
// components (8080BADB) whose sequence reads an object channel named *shield* (the UESC assault's
// assault_shield_recent_damage) and names particle systems. The assault's are cube mesh
// particles with the shield's light: its damage FX (nimshade --shield-fx plays them at b_shield).
std::vector<TagHash> entity_shield_systems(PackageManager& pm, TagHash entity);
inline constexpr uint32_t kBoneShield = 0x6981AD2B;  // fnv1("b_shield")

}  // namespace nim
