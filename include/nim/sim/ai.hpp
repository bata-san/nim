#pragma once
#include "nim/content/map.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
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
// The player is the camera. By default enemies never react to it (`player_invisible`); their
// awareness of it is still computed so an indicator can show what they would notice.

// ---- motion (character locomotion from the motion bank) -----------------------------------------
struct MotionClip {
    TagHash clip;
    float seconds = 0;
    float speed = 0;                   // m/s the clip stands for (from its feet, 推定)
    std::array<float, 2> direction{};  // object space, x forward
};
struct MotionSet {
    TagHash entity;
    std::vector<MotionClip> locomotion;  // cyclic clips that travel
    std::optional<MotionClip> idle;      // the longest cyclic clip that stands still
    std::string error;                   // why the set is empty, if it is
};
// Reads the entity's skeleton, rig and motion-bank clips and measures each clip's gait at `fps`
// (推定: the clip rate is not decoded). Locomotion: cyclic, 0.6 m/s or faster, foot lift
// 0.1..0.6 m. Idle: cyclic, slower than 0.15 m/s, foot lift under 0.08 m, at least 1 s.
MotionSet build_motion_set(PackageManager& pm, TagHash entity, float fps);
struct MotionChoice {
    std::optional<size_t> locomotion;  // index into MotionSet::locomotion; none = idle
    float rate = 1;                    // playback rate: actual speed / clip speed, clamped 0.6..1.6
};
// The locomotion clip whose velocity (speed x direction) is nearest `local_velocity` (object space,
// x forward); idle below 0.2 m/s.
MotionChoice select_motion(const MotionSet& set, std::array<float, 2> local_velocity);

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
    bool player_invisible = true;   // enemies compute awareness but never act on it
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
    const AiParams& params() const { return params_; }
    const std::vector<MotionSet>& motions() const { return motions_; }
    float time() const { return time_; }

private:
    struct Area { std::vector<std::array<std::array<float, 3>, 3>> triangles; std::vector<float> cumulative; };
    std::array<float, 3> sample_objective(size_t squad);
    std::array<float, 3> nearest_objective_point(size_t squad, const std::array<float, 3>& to) const;
    void perceive(const std::array<float, 3>& eye, float yaw, float range, float cos_half_fov, bool cone,
                  const std::array<float, 3>& player, float dt, float& awareness, AiAlert& alert, bool& sees) const;

    const MapGameplay& g_;
    AiParams params_;
    std::mt19937_64 rng_;
    std::vector<MotionSet> motions_;
    std::vector<Area> areas_;                     // MapGameplay::firing_areas
    std::vector<std::vector<size_t>> squad_areas_;  // per squad: firing areas of its objective
    std::vector<AiAgent> agents_;
    std::vector<AiDrone> drones_;
    std::vector<float> drone_length_;
    float time_ = 0;
};

} // namespace nim
