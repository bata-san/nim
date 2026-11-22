#pragma once
#include "nim/content/animation.hpp"
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
    std::string error;  // why it cannot be drawn, if it cannot
};
ActorCharacter prepare_actor_character(PackageManager& pm, const ActorSources& sources, const MotionSet& motion);

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
    std::string error;
};
ActorDropship prepare_actor_dropship(PackageManager& pm, const ActorSources& sources, TagHash model);

// Crossfades between the clips an agent plays.
struct ActorAnimator {
    TagHash clip, previous;
    float time = 0, previous_time = 0, blend = 1;  // blend 0 -> 1: previous -> clip
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
    void clear() { slots_.clear(), characters_.clear(), by_character_.clear(), ship_slots_.clear(), ship_ = {}; }
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

private:
    struct Slot {
        size_t character = 0;  // characters_ index
        uint32_t actor = 0;
        std::optional<size_t> agent;
        ActorAnimator anim;
        float lift = std::numeric_limits<float>::quiet_NaN();  // grounding offset (m)
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
