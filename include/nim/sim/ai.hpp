#pragma once
#include "nim/content/map.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nim {

// Estimated AI simulation (docs/PROGRESS.md 3.69).
//
// What comes from the game's data (3.65-3.67): which characters each squad spawns and where, the
// squad's objective and its firing areas (floor polygons), drone patrol splines, and each
// character's motion-bank clips. What does not: every behaviour rule. BlockScript's code is not
// in the packages and Marathon.exe's code is encrypted on disk (3.68), so movement, perception
// and alert rules below are explicit estimates, listed by `ai_estimates` and shown as 推定.
//
// The player is the camera. By default enemies neither notice nor react to it (`player_invisible`):
// the camera raises no awareness, and awareness from a visible stretch only decays.

// ---- motion (character locomotion from the motion bank) -----------------------------------------
struct MotionClip {
    TagHash clip;
    float seconds = 0;
    float speed = 0;                   // m/s the clip stands for (root motion / duration; from its feet when it has none, 推定)
    std::array<float, 2> direction{};  // object space, x forward
    bool cyclic = true;                // false: played forwards and back (an idle that does not loop)
    bool authored = false;             // speed and direction from the clip's root motion
    float yaw = 0;                     // turns in place: radians over the clip (+ = left)
};
// Seconds into a clip of `length` for a clock `time` (back and forth when not cyclic).
float motion_clip_time(const MotionClip& clip, float time);
struct MotionSet {
    TagHash entity;
    TagHash skeleton, rig;               // the entity's own components the clips were measured with
    std::vector<MotionClip> locomotion;  // cyclic clips that travel
    std::optional<MotionClip> idle;      // the longest cyclic clip that stands still
    std::vector<MotionClip> turns;       // clips whose root motion turns in place (20..180 degrees, < 4 cm travel)
    // The character's own forward locomotion speeds (slowest and fastest forward clip with root
    // motion); 0 when it has none. The simulation patrols and runs at these instead of AiParams.
    float walk_speed = 0, run_speed = 0;
    // Parts taken from other robots' clips of the shared bank (map_clip_groups) because the
    // character has none of its own: 1 locomotion, 2 turns, 4 idle.
    int borrowed = 0;
    std::string error;                   // why the set is empty, if it is
};
// Reads the entity's skeleton, rig and motion-bank clips and measures each clip's gait at `fps`
// (推定: the clip rate is not decoded; speeds come from the clips' root motion where they have it).
// Locomotion: cyclic, 0.6 m/s or faster, foot lift 0.1..0.6 m. Idle: slower than 0.15 m/s, foot lift under 0.08 m; the longest loop
// (0.5 s or more), else the longest clip that does not loop (1 s or more, played back and forth). Locomotion keeps the upright
// posture: the head within 0.12 m of the highest head among the locomotion clips (crouched moves are left out, 推定).
// Idle candidates come from the entity's motion data (entity_motion_data, docs/PROGRESS.md 3.91): clips of its parameter table
// that no action-graph node plays (the graph's clips are actions such as melee or grenade_launch). The parameter table's
// "idle_slow" clip is the idle when it is a candidate; otherwise the longest candidate (推定). Without the data, the upright filter
// above also applies to idles.
// Characters with no locomotion clip hold their position in the simulation (推定).
MotionSet build_motion_set(PackageManager& pm, TagHash entity, float fps);
struct MotionChoice {
    std::optional<size_t> locomotion;  // index into MotionSet::locomotion; none = idle
    std::optional<size_t> turn;        // index into MotionSet::turns while turning in place
    float rate = 1;                    // playback rate: actual speed / clip speed, clamped 0.6..1.6
    bool operator==(const MotionChoice&) const = default;
};
// The locomotion clip whose velocity (speed x direction) is nearest `local_velocity` (object space,
// x forward); idle below 0.2 m/s.
MotionChoice select_motion(const MotionSet& set, std::array<float, 2> local_velocity);

// ---- walkable floor -----------------------------------------------------------------------------
// Which cells of a firing area's floor are open (no prop or wall up to head height), from a
// top-down depth render of the map's own geometry (render/ai_actors.hpp build_walk_grid), or of
// the ground around a spawn point (build_ground_patch).
struct WalkGrid {
    float x0 = 0, y0 = 0, cell = 0.25f;  // world xy of cell (0, 0)'s corner, cell size (m)
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> open;           // w x h, row-major from y0: room >= 0.3 m
    std::vector<float> room;             // w x h: m from the cell to the nearest blocked cell (0: blocked)
    std::vector<float> height;           // w x h top surface z (NaN: none drawn), for grounding
    float floor_offset = 0;              // the floor's height above the firing area's polygon (m)
    // Sets room and open from the cells a body may stand in (w x h, before any clearance).
    void set_free(const std::vector<uint8_t>& free);
    bool walkable(float x, float y, float radius = 0.3f) const;
    std::optional<float> surface(float x, float y) const;
    bool clear(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius = 0.3f) const;  // the straight segment
    size_t open_count() const;
};

// ---- simulation ------------------------------------------------------------------------------------
struct AiParams {  // every value here is an estimate (推定)
    float walk_speed = 1.2f;        // m/s while patrolling
    float run_speed = 2.5f;         // m/s when alerted
    float turn_rate = 3.0f;         // rad/s
    float wait_min = 2.0f, wait_max = 7.0f;  // s at each patrol point
    float sight_base = 30.0f, sight_elite = 40.0f, sight_miniboss = 45.0f, sight_boss = 50.0f;  // m
    float fov_degrees = 120.0f;     // full cone, horizontal
    float vertical_limit = 4.0f;    // m: a larger height difference blocks sight (no collision data)
    float hearing_range = 8.0f;     // m, any direction
    float sight_rise = 1.5f;        // awareness per second at point-blank, falling linearly to 0 at range
    float hearing_rise = 0.4f;      // awareness per second within hearing range
    float decay = 0.2f;             // awareness per second lost when unnoticed
    float suspicious_at = 0.35f, alerted_at = 1.0f;
    float drone_speed = 5.0f;       // m/s along its spline
    float drone_scan_range = 15.0f; // m, all around
    bool player_invisible = true;   // the camera raises no awareness and causes no reaction
    // Dropship squads (spawned at a uesc_dropship_spawner, 3.71): called when the player comes
    // within range, then the ship's own clips (arrival, open hold, departure) and the members
    // dropping from its open rear bay to the floor one after another.
    float dropship_call_range = 60.0f;   // m from the player
    float dropship_call_delay = 2.0f;    // s, uniform 0..this after the call
    float dropship_arrive = 129.0f / 30; // s: the arrival clips' length (129 frames at 30 fps)
    float dropship_depart = 153.0f / 30; // s: the departure clips' length
    float dropship_first_exit = 0.6f;    // s after landing
    float dropship_exit_interval = 0.8f; // s between members
    float dropship_exit_walk = 2.0f;     // s walking away from the drop point (3 m)
    float dropship_linger = 1.5f;        // s open after the last member is out
    // The members ride in the bay (3.111): seats two abreast, then each walks to the open rear
    // and down the ramp, jumping off it backwards and up and falling to the floor below. The bay
    // floor and the ramp are the ship model's own (80ACA5E7: upward faces of bone 2 at z 2.0 for
    // x -4..0, |y| < 2; the inner rear panel, bone 3, folds down about its foot at x -5.41, z 2.39
    // into a ramp whose top face lies at z 2.65 out to x -7.8; the outer door, bone 10, swings up
    // over it, 1.2 m above the ramp at x -6.4). Where on the ramp they jump is an estimate.
    float dropship_bay_height = 2.0f;    // m: the bay floor above the ship's origin
    float dropship_lip_height = 2.65f;   // m: the ramp's top face, where the jump starts
    float dropship_jump_x = -6.6f;       // m (ship space): the point on the ramp it jumps from
    float dropship_bay_walk = 2.0f;      // m/s to the rear edge
    float dropship_jump_back = 1.8f;     // m/s backwards off the edge
    float dropship_jump_up = 1.5f;       // m/s up off the edge
    // Spawning by distance (3.111): a squad appears at its spawn points when the player comes
    // within spawn_range of them, and is removed when the player and all its members are farther
    // than despawn_range (dropship squads: once their ship has left; the ship comes again).
    float spawn_range = 100.0f;          // m
    float despawn_range = 150.0f;        // m
};
struct AiEstimate {
    std::string name;
    float value = 0;
    std::string note;
};
std::vector<AiEstimate> ai_estimates(const AiParams& p);

enum class AiAlert : uint8_t { Unaware, Suspicious, Alerted };
const char* ai_alert_name(AiAlert a);

struct AiAgent {
    size_t squad = 0, character = 0;   // MapGameplay::squads / characters
    std::array<float, 3> position{};
    float yaw = 0;                     // radians about +z, 0 = +x
    std::array<float, 3> velocity{};
    std::array<float, 3> goal{};
    float wait = 0;                    // s left at the current patrol point
    float sight = 30;                  // m (by tier)
    float awareness = 0;               // 0..1 of the player
    AiAlert alert = AiAlert::Unaware;  // from awareness; acted on only when the player is visible
    bool sees_player = false;
    MotionChoice motion;
    float motion_time = 0;             // s into the current clip
    bool active = true;                // false until spawned (by distance, or its dropship) and after despawning
    bool riding = false;               // in its dropship's bay, not yet out (drawn on the ship; not active)
    std::array<float, 3> home{};       // where it spawns
    std::optional<size_t> dropship;    // AiSimulation::dropships
    std::array<float, 3> seat{};       // its place in the ship's bay (ship space, x forward, z up from the origin)
    float exit = -1;                   // s into the drop from the ship's bay (>= 0 while leaving it)
    float exit_floor = 0;              // z of the floor it drops to
    std::vector<std::array<float, 3>> path;  // waypoints to the goal (A*, 3.73); empty: straight
    size_t path_next = 0;
};
// A squad's dropship (3.71).
struct AiDropship {
    size_t squad = 0;
    std::array<float, 3> position{};   // the spawner node: where the ship lands
    float yaw = 0;
    float start = -1;                  // simulation time the arrival begins; < 0 until called
    uint32_t variant = 0;              // picks one of the arrival / departure clips
    std::vector<size_t> members;       // agents, in exit order
    std::vector<size_t> squads;        // every squad it carries: spawners at the same coordinates share one ship
};
enum class AiDropshipPhase : uint8_t { Waiting, Arriving, Unloading, Departing, Gone };
struct AiDropshipState {
    AiDropshipPhase phase = AiDropshipPhase::Waiting;
    float seconds = 0;                 // into the phase
};
struct AiDrone {
    size_t group = 0;                  // MapGameplay::point_groups
    float along = 0;                   // m along the spline
    int heading = 1;                   // +1 / -1 (ping-pong)
    std::array<float, 3> position{};
    float yaw = 0;
    float awareness = 0;
    AiAlert alert = AiAlert::Unaware;
    bool sees_player = false;
};

class AiSimulation {
public:
    // motions: optional, indexed like MapGameplay::characters.
    AiSimulation(const MapGameplay& gameplay, AiParams params, uint64_t seed, std::vector<MotionSet> motions = {});
    void step(float dt, const std::array<float, 3>& player);
    const std::vector<AiAgent>& agents() const { return agents_; }
    const std::vector<AiDrone>& drones() const { return drones_; }
    const std::vector<AiDropship>& dropships() const { return dropships_; }
    AiDropshipState dropship_state(size_t dropship) const;
    // Calls every waiting dropship now (all squads present from the start, as before 3.71).
    void call_all_dropships();
    void call_dropship(size_t dropship);
    const AiParams& params() const { return params_; }
    void set_player_invisible(bool invisible) { params_.player_invisible = invisible; }
    const std::vector<MotionSet>& motions() const { return motions_; }
    // Restricts patrol points and paths in `firing_area` to its open floor; without one, the
    // whole polygon counts as open.
    void set_walk_grid(size_t firing_area, WalkGrid grid);
    const WalkGrid* walk_grid(size_t firing_area) const { return has_walk_grid(firing_area) ? &grids_[firing_area] : nullptr; }
    bool has_walk_grid(size_t firing_area) const { return firing_area < grids_.size() && grids_[firing_area].w != 0; }
    float time() const { return time_; }
    // The top surface under (x, y) nearest `z` (within 1.5 m) from the walk grids and ground
    // patches, if any.
    std::optional<float> ground(float x, float y, float z) const;
    // Ground around points not on the firing-area grids' open floor (spawn points, dropship drop
    // points): the caller renders a patch for each (build_ground_patch) and adds it. `from_above`:
    // the floor is whatever lies below (a dropship's drop point), not near the point's height.
    struct GroundNeed { std::array<float, 3> at; bool from_above = false; };
    std::vector<GroundNeed> uncovered() const;
    void add_ground_patch(WalkGrid patch);
    size_t ground_patches() const { return patches_.size(); }
    // A body's clearance from walls (m), per MapGameplay::characters (default 0.3).
    void set_character_radius(size_t character, float radius);
    float radius(const AiAgent& agent) const;
    // Patrol / alerted speeds: the character's own forward clips' root motion, else AiParams (推定).
    float walk_speed(const AiAgent& agent) const;
    float run_speed(const AiAgent& agent) const;
    // Open floor at (x, y) near height z for a body of `radius`, on any grid or patch.
    bool open_at(float x, float y, float z, float radius) const;
    // The most room (m to a wall or prop) at (x, y) near height z over the grids and patches (0: none).
    float room_at(float x, float y, float z) const;
    // A straight walk from a to b following the floor (steps up to 0.45 m) over open cells.
    bool path_clear(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius) const;
    // A* over a 0.5 m lattice of such walks (3.73): the waypoints after `a` up to b (string-pulled),
    // none when b is not reached within `max_nodes` lattice points.
    std::optional<std::vector<std::array<float, 3>>> find_path(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius,
                                                               size_t max_nodes = 3000) const;
    struct PathStats { size_t straight = 0, partial = 0, found = 0, failed = 0, deferred = 0, spawned = 0, despawned = 0; };
    const PathStats& path_stats() const { return path_stats_; }

private:
    struct Area { std::vector<std::array<std::array<float, 3>, 3>> triangles; std::vector<float> cumulative; };
    void choose_goal(AiAgent& a);
    bool in_area(size_t firing_area, float x, float y, float z, float radius) const;
    std::optional<std::vector<std::array<float, 3>>> route(const std::array<float, 3>& a, const std::array<float, 3>* b, float radius,
                                                           size_t max_nodes, const std::function<bool(float, float, float)>& accept,
                                                           float pick) const;
    std::optional<float> follow(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius, float relaxed, float spacing = 0.125f,
                                bool skip_start = false) const;
    template <class F> void each_grid(float x, float y, F&& f) const;
    mutable std::unordered_map<int64_t, std::vector<std::pair<uint8_t, uint32_t>>> grid_index_;  // 16 m buckets -> (list, index)
    mutable bool grid_index_dirty_ = true;
    PathStats path_stats_;
    float search_budget_ = 0;  // A* searches available now
    std::array<float, 3> sample_area(size_t firing_area);
    void separate();
    std::array<float, 3> nearest_objective_point(size_t squad, const std::array<float, 3>& to) const;
    void perceive(const std::array<float, 3>& eye, float yaw, float range, float cos_half_fov, bool cone,
                  const std::array<float, 3>& player, float dt, float& awareness, AiAlert& alert, bool& sees) const;

    const MapGameplay& g_;
    AiParams params_;
    std::mt19937_64 rng_;
    std::vector<MotionSet> motions_;
    std::vector<Area> areas_;                     // MapGameplay::firing_areas
    std::vector<WalkGrid> grids_;                 // per firing area, empty = no grid
    std::vector<WalkGrid> patches_;               // ground around uncovered spawn / drop points
    std::vector<float> radius_;                   // per MapGameplay::characters
    std::vector<std::vector<size_t>> squad_areas_;  // per squad: firing areas of its objective
    std::vector<AiAgent> agents_;
    std::vector<AiDrone> drones_;
    std::vector<AiDropship> dropships_;
    void step_dropships(float dt, const std::array<float, 3>& player);
    void step_spawns(const std::array<float, 3>& player);
    void spawn(AiAgent& a);
    std::vector<uint8_t> squad_spawned_;          // per squad without a dropship
    mutable std::vector<float> unload_cache_;     // per dropship: unload_seconds, < 0 = not yet
    // A member's drop from the bay (ship space): x, z and its velocity at `t` s after it starts
    // walking out, and whether it is still leaving (false once it has walked off).
    struct ExitPose { std::array<float, 3> at; std::array<float, 3> velocity; bool airborne = false, leaving = true; };
    ExitPose exit_pose(const AiDropship& d, const AiAgent& a, float t) const;
    float unload_seconds(const AiDropship& d) const;
    float floor_below(size_t squad, float x, float y, float z) const;
    float exit_seconds(const AiDropship& d) const;
    std::vector<float> drone_length_;
    float time_ = 0;
};

} // namespace nim
