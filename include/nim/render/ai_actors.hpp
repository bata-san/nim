#pragma once
#include "nim/content/animation.hpp"
#include "nim/content/entity_audio.hpp"
#include "nim/content/map.hpp"
#include "nim/content/tfx.hpp"
#include "nim/render/scene.hpp"
#include "nim/sim/ai.hpp"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nim {

// AI characters drawn in a map scene (docs/PROGRESS.md 3.70): each agent of the estimated
// simulation (3.69) is the character entity's own assembled model, skinned on the CPU with the
// motion-bank clip the simulation picked. Indicators (awareness, markers, debug) are nim's own
// overlay, not a game HUD.

// The indexes a character preview needs (as the Models tab builds them), built once.
struct ActorSources {
    std::unordered_map<uint32_t, DynamicModelMaterials> materials;
    ModelAssemblyIndex assemblies;
    ModelMotionIndex motions;
    ModelRigIndex rigs;
    std::vector<ClipInfo> clips;
};
ActorSources index_actor_sources(PackageManager& pm);

// The dynamic models an entity's components name (SDynamicModelComponent, as load_map_entities).
std::vector<TagHash> entity_dynamic_models(PackageManager& pm, TagHash entity, const ActorSources& sources);

// One character entity, ready to place any number of times.
struct ActorCharacter {
    TagHash entity, model;
    std::shared_ptr<const PreparedMesh> mesh;
    std::shared_ptr<const PreviewAnimation> binding;
    std::unordered_map<TagHash, AnimationClip> clips;  // the motion set's clips the binding plays
    MotionSet motion;
    bool entity_binding = false;  // bound with the entity's own skeleton and rig (推定)
    std::vector<TagHash> models;  // every dynamic model the entity names, the drawn one first
    std::shared_ptr<const EntitySounds> sounds;  // what its clips and components play (3.90)
    std::optional<RegionPermutations> regions;  // the permutations its spawn tags select (3.92)
    // The weapon its spawn tags select (3.93): its model is one of the mesh's attachments, every
    // bone posed at the body skeleton's b_r_grip (the model's root bone b_handle sits there).
    std::optional<EntityWeapon> weapon;
    TagHash weapon_model;
    size_t weapon_bones = 0;
    int32_t grip_bone = -1;
    std::string error;  // why it cannot be drawn, if it cannot
};
// `tags`: the spawned character's tags (MapCharacter::tags); they select the model's region
// permutations (3.92). Empty: every permutation's parts are drawn.
ActorCharacter prepare_actor_character(PackageManager& pm, const ActorSources& sources, const MotionSet& motion,
                                       std::span<const std::pair<uint32_t, uint32_t>> tags = {});

// The UESC dropship (3.71): its model and its own animation bank, sorted by the root track
// (the ship's flight is root motion): arrivals start far away and end at the origin with the rear
// bay open, departures start there and fly off, `open` is the static open-bay pose.
struct ActorDropship {
    TagHash model;
    std::shared_ptr<const PreparedMesh> mesh;
    std::shared_ptr<const PreviewAnimation> binding;
    std::vector<AnimationClip> arrivals, departures;
    std::vector<uint32_t> departure_frames;  // per departure: its last frame away from the origin
    std::optional<AnimationClip> open;
    // Each spawner entity (3.90) has its own sound table, and the names in it are the events of one
    // arrival and one departure clip: that pair is its ship's (-1: none matched, the variant picks).
    struct Spawner {
        TagHash entity;
        std::shared_ptr<const EntitySounds> sounds;
        int arrival = -1, departure = -1;
    };
    std::vector<Spawner> spawners;
    std::string error;
};
ActorDropship prepare_actor_dropship(PackageManager& pm, const ActorSources& sources, TagHash model,
                                     std::span<const TagHash> entities = {});
// The entities of the dropship squads' spawner nodes (model 80ACA5E7), each once.
std::vector<TagHash> dropship_entities(const MapGameplay& gameplay);
// Scan drones (docs/PROGRESS.md 3.94): the entity of a patrol point group's owner (the
// uesc_scan_drone component's own node, o_uesc_scan_drone), none when it places no node.
TagHash drone_entity(const MapGameplay& gameplay, size_t point_group);
// Every scan drone entity (point groups with a spline owned by uesc_scan_drone.bs), each once.
std::vector<TagHash> drone_entities(const MapGameplay& gameplay);
// A drone entity as a character: with its motion set's clips when they fit its rig, else its
// first dynamic model unposed (rest pose; no binding).
ActorCharacter prepare_actor_drone(PackageManager& pm, const ActorSources& sources, TagHash entity);

// A sound the actors start (docs/PROGRESS.md 3.90): an audio event at an emitter. Emitters are
// agents ((1 << 32) | agent index), dropships ((2 << 32) | dropship index) and scan drones ((3 << 32) | drone index).
struct ActorSound {
    TagHash event;
    uint64_t emitter = 0;
};

// Crossfades between the clips an agent plays.
struct ActorAnimator {
    TagHash clip, previous;
    float time = 0, previous_time = 0, blend = 1;  // blend 0 -> 1: previous -> clip
    float rate = 1, previous_rate = 1;
};
// The poses for `agent` (its motion choice and clock), blending out of the previous clip over
// 0.25 s (推定). Empty when the character has no clip for the choice.
std::vector<ModelSkinPose> actor_poses(const ActorCharacter& c, const AiAgent& agent, ActorAnimator& anim, float dt);
// 3x4 object-to-world rows for an agent (rotation about +z by yaw, model forward +x).
std::array<float, 12> actor_rows(const AiAgent& agent);

// Actor slots in a scene, reassigned every frame to the agents nearest the camera.
class AiActors {
public:
    // Adds `per_character` hidden actors per drawable character (capped by its agent count).
    void build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
               std::vector<ActorCharacter> characters, const MapGameplay& gameplay, const AiSimulation& sim,
               size_t per_character);
    // Places the nearest agents of each character (within `range` m of `eye`), poses them and
    // hides the rest.
    void update(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float dt, float range);
    // Adds `slots` hidden dropships (drawn while arriving, unloading or departing, nearest first).
    void build_dropships(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                         ActorDropship ship, size_t slots);
    // Adds `slots` hidden scan drones per drawable drone entity (`drones`: one per entity, as
    // prepare_actor_character), drawn at the simulation's drones nearest the camera (3.94).
    void build_drones(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                      std::vector<ActorCharacter> drones, const MapGameplay& gameplay, const AiSimulation& sim, size_t slots);
    size_t drones_shown() const { return drones_shown_; }
    const std::vector<ActorCharacter>& drone_kinds() const { return drone_kinds_; }
    void clear() {
        drone_kinds_.clear(), drone_kind_.clear(), drone_slots_.clear();
        slots_.clear(), characters_.clear(), by_character_.clear(), ship_slots_.clear(), ship_ = {};
        sounds_.clear(), stopped_.clear(), emitters_.clear(), shown_emitters_.clear(), ship_frames_.clear(), dropship_entity_.clear();
    }
    size_t ships_shown() const { return ships_shown_; }
    // Each drawable character's clearance from walls: how far its feet reach from its centre in
    // the idle pose (+0.1 m, 0.3 to 0.9 m), given to the simulation.
    void apply_radii(AiSimulation& sim) const;
    // Ground audit (nimshade --ai-ground-audit): per shown agent after each update, how far its
    // posed mesh goes below the floor surface under each vertex (`sink`, m, vertices up to 0.5 m
    // above the floor) and the share of its body (0.3-2 m) under the walk grid's top surface.
    struct GroundCheck { size_t agent; float floor, sink, inside; bool on_grid; };
    void set_ground_checks(bool on) { checking_ = on; }
    const std::vector<GroundCheck>& ground_checks() const { return checks_; }
    size_t slots() const { return slots_.size(); }
    size_t shown() const { return shown_; }
    const std::vector<ActorCharacter>& characters() const { return characters_; }
    const std::vector<std::string>& notes() const { return notes_; }
    // The face indicator's object state per alert level (docs/PROGRESS.md 3.77): channel 319CE56E
    // (0 white, 1 yellow, 2 red), and BE78BFD7 = 0 so the panel is shown. Both are estimates.
    static float indicator_state(AiAlert alert);
    // Sounds (3.90), after each update(): the audio events started this frame (clip events the
    // shown actors' and ships' clips passed; component loops of emitters just shown), the
    // emitters hidden this frame (their sounds stop) and every shown emitter's position.
    const std::vector<ActorSound>& sounds() const { return sounds_; }
    const std::vector<uint64_t>& stopped_emitters() const { return stopped_; }
    const std::vector<std::pair<uint64_t, std::array<float, 3>>>& emitters() const { return emitters_; }

private:
    struct Slot {
        size_t character = 0;  // characters_ index
        uint32_t actor = 0;
        std::optional<size_t> agent;
        ActorAnimator anim;
        float lift = std::numeric_limits<float>::quiet_NaN();  // grounding offset (m)
        int indicator = -1;  // the face indicator state last given to the actor (-1: none yet)
        std::optional<size_t> cue_agent;  // the agent the clip position below belongs to
        TagHash cue_clip;
        float cue_frame = -1;
    };
    std::vector<ActorCharacter> characters_;
    std::vector<std::optional<size_t>> by_character_;  // MapGameplay::characters -> characters_
    std::vector<float> radius_;                        // per characters_ (0: unknown)
    std::vector<Slot> slots_;
    std::vector<std::string> notes_;
    size_t shown_ = 0;
    ActorDropship ship_;
    std::vector<uint32_t> ship_slots_;  // scene actors
    size_t ships_shown_ = 0;
    bool checking_ = false;
    std::vector<GroundCheck> checks_;
    std::vector<ActorSound> sounds_;
    std::vector<uint64_t> stopped_;
    std::vector<std::pair<uint64_t, std::array<float, 3>>> emitters_;
    std::unordered_set<uint64_t> shown_emitters_, now_shown_;
    std::unordered_map<size_t, std::pair<const AnimationClip*, float>> ship_frames_;  // per dropship
    std::vector<uint32_t> dropship_entity_;  // per dropship: its spawner node's entity (0: unknown)
    std::vector<ActorCharacter> drone_kinds_;          // per drawable drone entity
    std::vector<std::optional<size_t>> drone_kind_;    // per AiSimulation drone -> drone_kinds_
    struct DroneSlot { size_t kind = 0; uint32_t actor = 0; };
    std::vector<DroneSlot> drone_slots_;
    float drone_clock_ = 0;
    size_t drones_shown_ = 0;
    void update_drones(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float dt, float range);
    uint32_t sound_rng_ = 0x2545F491u;
    void emit_clip_sounds(const EntitySounds& sounds, const AnimationClip& clip, float from, float to, bool wrapped, uint64_t emitter);
    void emit_loops(const EntitySounds& sounds, uint64_t emitter);
    void update_dropships(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float range);
};

// Open floor of a firing area: the scene drawn straight down into `view` (resized as needed;
// reused between calls) from 2 m above the floor, each cell open when the top surface there is
// the floor itself, then eroded by 0.3 m. Actors must be hidden. Waits for the GPU.
WalkGrid build_walk_grid(Gpu& gpu, StaticMeshScene& scene, const MapFiringArea& area, SunShadow& view, float cell = 0.25f,
                         std::span<const std::array<float, 3>> extra = {});
// The ground around a point no firing-area grid covers (a spawn point, a dropship's drop point;
// AiSimulation::uncovered): heights and the floor connected to the point's own.
WalkGrid build_ground_patch(Gpu& gpu, StaticMeshScene& scene, const std::array<float, 3>& at, bool from_above, SunShadow& view,
                            float half = 8.0f, float cell = 0.25f);
void add_ground_patches(Gpu& gpu, StaticMeshScene& scene, AiSimulation& sim, SunShadow& view);
// The spawn points of the squads whose objective uses `firing_area` (build_walk_grid's `extra`).
std::vector<std::array<float, 3>> squad_spawn_points(const MapGameplay& gameplay, size_t firing_area);

// ---- indicators --------------------------------------------------------------------------------
struct AiOverlayOptions {
    bool awareness = true;  // meters and ? / ! above enemies
    bool markers = true;    // firing-area and drone-route markers with distance
    bool debug = false;     // floor polygons, view cones, hearing rings, goals, labels
    float range = 80;       // m from the camera
};
struct AiOverlay {
    struct Line { std::array<float, 2> a, b; uint32_t color; float width; };
    struct Rect { std::array<float, 2> min, max; uint32_t color; bool filled; };
    struct Text { std::array<float, 2> at; uint32_t color; std::string text; float scale; };
    std::vector<Line> lines;
    std::vector<Rect> rects;
    std::vector<Text> texts;
};
// Colours are 0xAABBGGRR (ImGui's IM_COL32). Screen pixels, y down.
AiOverlay build_ai_overlay(const AiSimulation& sim, const MapGameplay& gameplay, const ViewMatrices& view, float width,
                           float height, const AiOverlayOptions& options);

} // namespace nim
