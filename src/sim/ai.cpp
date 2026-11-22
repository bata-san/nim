#include "nim/sim/ai.hpp"

#include "nim/content/animation.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace nim {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

float wrap_angle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float dist2d(const std::array<float, 3>& a, const std::array<float, 3>& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

}  // namespace

// ---- walkable floor --------------------------------------------------------------------------------

bool WalkGrid::walkable(float x, float y) const {
    if (!w) return true;
    const float fx = (x - x0) / cell, fy = (y - y0) / cell;
    if (fx < 0 || fy < 0 || fx >= float(w) || fy >= float(h)) return false;
    return open[size_t(fy) * w + size_t(fx)] != 0;
}

std::optional<float> WalkGrid::surface(float x, float y) const {
    if (!w || height.size() != size_t(w) * h) return std::nullopt;
    const float fx = (x - x0) / cell, fy = (y - y0) / cell;
    if (fx < 0 || fy < 0 || fx >= float(w) || fy >= float(h)) return std::nullopt;
    const float v = height[size_t(fy) * w + size_t(fx)];
    if (std::isnan(v)) return std::nullopt;
    return v;
}

std::optional<float> AiSimulation::ground(float x, float y, float z) const {
    std::optional<float> best;
    for (const auto& g : grids_) {
        const auto s = g.surface(x, y);
        if (s && std::abs(*s - z) < 1.5f && (!best || std::abs(*s - z) < std::abs(*best - z))) best = s;
    }
    return best;
}

bool WalkGrid::clear(const std::array<float, 3>& a, const std::array<float, 3>& b) const {
    if (!w) return true;
    const float d = std::hypot(b[0] - a[0], b[1] - a[1]);
    const int steps = std::max(1, int(d / (cell * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        if (!walkable(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)) return false;
    }
    return true;
}

size_t WalkGrid::open_count() const { return size_t(std::count(open.begin(), open.end(), uint8_t(1))); }

// ---- motion ----------------------------------------------------------------------------------------

MotionSet build_motion_set(PackageManager& pm, TagHash entity, float fps) {
    MotionSet set;
    set.entity = entity;
    try {
        const auto bytes = pm.read(entity);
        TagReader r(bytes);
        const auto comps = r.vec_at(0x08);
        TagHash sk, rg;
        for (uint64_t k = 0; k < comps.count; ++k) {
            const TagHash c(r.u32_at(comps.data + size_t(k) * 12));
            if (!c.is_pkg_ref()) continue;
            if (is_skeleton_component(pm, c)) {
                sk = c;
                continue;
            }
            try {
                if (load_animation_rig(pm, c).size()) rg = c;
            } catch (...) {
            }
        }
        if (sk.is_none() || rg.is_none()) {
            set.error = "no skeleton and rig on the entity";
            return set;
        }
        set.skeleton = sk;
        set.rig = rg;
        const auto skeleton = load_skeleton(pm, sk);
        const auto rig = load_animation_rig(pm, rg);
        const auto feet = find_feet(skeleton);
        if (!feet) {
            set.error = "no ankle pair in the skeleton";
            return set;
        }
        std::optional<MotionClip> still;  // non-cyclic idle candidate
        for (const auto tag : entity_motion_clips(pm, entity)) {
            AnimationClip clip;
            try {
                clip = load_animation_clip(pm, tag);
            } catch (...) {
                continue;
            }
            if (clip.bone_count != rig.size()) continue;
            const auto gait = measure_clip_gait(skeleton, rig, clip, *feet, fps);
            if (!gait.cyclic) {
                if (gait.speed < 0.15f && gait.lift < 0.08f && gait.seconds >= 1.0f && (!still || gait.seconds > still->seconds))
                    still = MotionClip{tag, gait.seconds, 0, {}, false};
                continue;
            }
            const MotionClip m{tag, gait.seconds, gait.speed, gait.direction};
            if (gait.speed >= 0.6f && gait.lift >= 0.1f && gait.lift <= 0.6f) set.locomotion.push_back(m);
            else if (gait.speed < 0.15f && gait.lift < 0.08f && gait.seconds >= 1.0f && (!set.idle || gait.seconds > set.idle->seconds))
                set.idle = MotionClip{tag, gait.seconds, 0, {}};
        }
        if (!set.idle) set.idle = still;
        if (set.locomotion.empty() && !set.idle) set.error = "no usable clips";
    } catch (const std::exception& e) {
        set.error = e.what();
    }
    return set;
}

float motion_clip_time(const MotionClip& clip, float time) {
    if (clip.seconds <= 0) return 0;
    if (clip.cyclic) return std::fmod(std::max(time, 0.0f), clip.seconds);
    const float t = std::fmod(std::max(time, 0.0f), 2 * clip.seconds);
    return t > clip.seconds ? 2 * clip.seconds - t : t;
}

MotionChoice select_motion(const MotionSet& set, std::array<float, 2> v) {
    MotionChoice c;
    const float speed = std::hypot(v[0], v[1]);
    if (speed < 0.2f || set.locomotion.empty()) return c;
    float best = 1e9f;
    for (size_t i = 0; i < set.locomotion.size(); ++i) {
        const auto& m = set.locomotion[i];
        const float d = std::hypot(m.speed * m.direction[0] - v[0], m.speed * m.direction[1] - v[1]);
        if (d < best) best = d, c.locomotion = i;
    }
    c.rate = std::clamp(speed / std::max(set.locomotion[*c.locomotion].speed, 0.1f), 0.6f, 1.6f);
    return c;
}

// ---- estimates -------------------------------------------------------------------------------------

std::vector<AiEstimate> ai_estimates(const AiParams& p) {
    return {
        {"walk_speed", p.walk_speed, "m/s patrolling between random points of the squad's firing areas"},
        {"run_speed", p.run_speed, "m/s when alerted (player visible only)"},
        {"turn_rate", p.turn_rate, "rad/s"},
        {"wait_min", p.wait_min, "s at a patrol point (uniform to wait_max)"},
        {"wait_max", p.wait_max, "s"},
        {"sight_base", p.sight_base, "m, tier base (tier from the character name estimate)"},
        {"sight_elite", p.sight_elite, "m"},
        {"sight_miniboss", p.sight_miniboss, "m"},
        {"sight_boss", p.sight_boss, "m"},
        {"fov_degrees", p.fov_degrees, "full horizontal cone"},
        {"vertical_limit", p.vertical_limit, "m height difference that blocks sight; no occlusion otherwise (no collision data)"},
        {"hearing_range", p.hearing_range, "m, any direction"},
        {"sight_rise", p.sight_rise, "awareness/s at point-blank, linear to 0 at sight range"},
        {"hearing_rise", p.hearing_rise, "awareness/s within hearing range"},
        {"decay", p.decay, "awareness/s lost when unnoticed"},
        {"suspicious_at", p.suspicious_at, "awareness threshold"},
        {"alerted_at", p.alerted_at, "awareness threshold"},
        {"drone_speed", p.drone_speed, "m/s along the patrol spline, back and forth"},
        {"drone_scan_range", p.drone_scan_range, "m, all around"},
        {"movement", 0, "straight lines inside firing areas; no navmesh was found, so walls are ignored"},
        {"spawn", 0, "every squad spawns at once at its positions (member i at position i mod n)"},
        {"walk_grid", 0, "patrol points and straight paths only over open floor: a top-down depth render of the map's geometry between the floor and 2 m above it, eroded by 0.3 m (no navmesh found)"},
        {"stationary", 0, "characters whose motion bank has no locomotion clip (sniper, ghost) hold their position"},
        {"motion_fps", 30, "clip rate used to measure gaits (not decoded)"},
    };
}

const char* ai_alert_name(AiAlert a) {
    switch (a) {
    case AiAlert::Unaware: return "unaware";
    case AiAlert::Suspicious: return "suspicious";
    case AiAlert::Alerted: return "alerted";
    }
    return "?";
}

// ---- simulation ------------------------------------------------------------------------------------

AiSimulation::AiSimulation(const MapGameplay& g, AiParams params, uint64_t seed, std::vector<MotionSet> motions)
    : g_(g), params_(params), rng_(seed), motions_(std::move(motions)) {
    for (const auto& fa : g.firing_areas) {
        Area a;
        float total = 0;
        for (const auto& t : fa.triangles) {
            const auto &p0 = fa.outline[t[0]], &p1 = fa.outline[t[1]], &p2 = fa.outline[t[2]];
            total += std::abs((p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1])) * 0.5f;
            a.triangles.push_back({p0, p1, p2});
            a.cumulative.push_back(total);
        }
        areas_.push_back(std::move(a));
    }
    squad_areas_.resize(g.squads.size());
    for (size_t s = 0; s < g.squads.size(); ++s)
        if (const auto o = g.squads[s].objective)
            for (const auto a : g.objectives[*o].firing_areas)
                if (!areas_[a].triangles.empty() && areas_[a].cumulative.back() > 0.01f) squad_areas_[s].push_back(a);
    std::uniform_real_distribution<float> angle(-kPi, kPi);
    for (size_t s = 0; s < g.squads.size(); ++s) {
        const auto& sq = g.squads[s];
        if (sq.positions.empty()) continue;
        for (size_t m = 0; m < sq.members.size(); ++m) {
            AiAgent a;
            a.squad = s;
            a.character = sq.members[m];
            const auto& p = sq.positions[m % sq.positions.size()];
            a.position = p.position;
            a.position[0] += 0.8f * float(m / sq.positions.size());  // members sharing a position stand apart
            a.goal = a.position;
            a.yaw = angle(rng_);
            const std::string& name = g.characters[a.character].name_estimate;
            a.sight = name.find("boss") != std::string::npos && name.find("miniboss") == std::string::npos ? params_.sight_boss
                    : name.find("miniboss") != std::string::npos                                            ? params_.sight_miniboss
                    : name.find("elite") != std::string::npos                                               ? params_.sight_elite
                                                                                                             : params_.sight_base;
            a.wait = std::uniform_real_distribution<float>(0, params_.wait_max)(rng_);
            agents_.push_back(a);
        }
    }
    for (size_t i = 0; i < g.point_groups.size(); ++i) {
        const auto& pg = g.point_groups[i];
        if (pg.spline.size() < 2 || !pg.owner || !g.scripts[*pg.owner].module.ends_with("uesc_scan_drone.bs")) continue;
        float len = 0;
        for (size_t k = 1; k < pg.spline.size(); ++k)
            len += std::hypot(pg.spline[k][0] - pg.spline[k - 1][0], pg.spline[k][1] - pg.spline[k - 1][1], pg.spline[k][2] - pg.spline[k - 1][2]);
        AiDrone d;
        d.group = i;
        d.position = pg.spline.front();
        drones_.push_back(d);
        drone_length_.push_back(len);
    }
}

void AiSimulation::set_walk_grid(size_t firing_area, WalkGrid grid) {
    if (grids_.size() < areas_.size()) grids_.resize(areas_.size());
    grids_.at(firing_area) = std::move(grid);
}

// A random open point of the squad's firing areas that a straight walk from `from` reaches
// (tries a few; keeps the agent where it is when none does).
std::array<float, 3> AiSimulation::sample_objective(size_t squad, const std::array<float, 3>& from) {
    const auto& list = squad_areas_[squad];
    if (list.empty()) return from;
    for (int attempt = 0; attempt < 24; ++attempt) {
        const size_t index = list[std::uniform_int_distribution<size_t>(0, list.size() - 1)(rng_)];
        const auto p = sample_area(index);
        if (!has_walk_grid(index)) return p;
        const auto& grid = grids_[index];
        if (!grid.walkable(p[0], p[1])) continue;
        // The path must stay on open floor of whichever area it crosses (the start's or the goal's).
        bool ok = grid.clear(from, p);
        if (!ok)
            for (const auto other : list)
                if (other != index && has_walk_grid(other) && grids_[other].walkable(from[0], from[1]) && grids_[other].walkable(p[0], p[1]) &&
                    grids_[other].clear(from, p))
                    ok = true;
        if (ok) return p;
    }
    return from;
}

std::array<float, 3> AiSimulation::sample_area(size_t index) {
    const auto& area = areas_[index];
    const float pick = std::uniform_real_distribution<float>(0, area.cumulative.back())(rng_);
    const size_t t = size_t(std::lower_bound(area.cumulative.begin(), area.cumulative.end(), pick) - area.cumulative.begin());
    const auto& tri = area.triangles[std::min(t, area.triangles.size() - 1)];
    float u = std::uniform_real_distribution<float>(0, 1)(rng_), v = std::uniform_real_distribution<float>(0, 1)(rng_);
    if (u + v > 1) u = 1 - u, v = 1 - v;
    std::array<float, 3> p{};
    for (size_t k = 0; k < 3; ++k) p[k] = tri[0][k] + u * (tri[1][k] - tri[0][k]) + v * (tri[2][k] - tri[0][k]);
    return p;
}

std::array<float, 3> AiSimulation::nearest_objective_point(size_t squad, const std::array<float, 3>& to) const {
    std::array<float, 3> best{};
    float bd = 1e18f;
    for (const auto a : squad_areas_[squad])
        for (const auto& tri : areas_[a].triangles) {
            std::array<float, 3> c{};
            for (size_t k = 0; k < 3; ++k) c[k] = (tri[0][k] + tri[1][k] + tri[2][k]) / 3;
            for (const auto& p : {tri[0], tri[1], tri[2], c})
                if (const float d = dist2d(p, to); d < bd) bd = d, best = p;
        }
    return best;
}

void AiSimulation::perceive(const std::array<float, 3>& eye, float yaw, float range, float cos_half_fov, bool cone,
                            const std::array<float, 3>& player, float dt, float& awareness, AiAlert& alert, bool& sees) const {
    const float dx = player[0] - eye[0], dy = player[1] - eye[1], dz = player[2] - eye[2];
    const float d = std::hypot(dx, dy);
    float rise = 0;
    sees = false;
    if (d < range && std::abs(dz) < params_.vertical_limit) {
        const bool inside = !cone || d < 1e-3f || (dx * std::cos(yaw) + dy * std::sin(yaw)) / d >= cos_half_fov;
        if (inside) {
            sees = true;
            rise += params_.sight_rise * (1 - d / range);
        }
    }
    if (d < params_.hearing_range && std::abs(dz) < params_.vertical_limit) rise += params_.hearing_rise;
    awareness = std::clamp(rise > 0 ? awareness + rise * dt : awareness - params_.decay * dt, 0.0f, 1.0f);
    // Hysteresis: an alerted enemy stays alerted until it falls below the suspicious threshold.
    if (awareness >= params_.alerted_at) alert = AiAlert::Alerted;
    else if (alert == AiAlert::Alerted && awareness >= params_.suspicious_at) alert = AiAlert::Alerted;
    else alert = awareness >= params_.suspicious_at ? AiAlert::Suspicious : AiAlert::Unaware;
}

void AiSimulation::step(float dt, const std::array<float, 3>& player) {
    time_ += dt;
    const float cos_half = std::cos(params_.fov_degrees * 0.5f * kPi / 180.0f);
    for (auto& a : agents_) {
        perceive({a.position[0], a.position[1], a.position[2] + 1.6f}, a.yaw, a.sight, cos_half, true, player, dt, a.awareness,
                 a.alert, a.sees_player);
        const bool react = !params_.player_invisible && a.alert != AiAlert::Unaware;
        float speed = 0;
        std::array<float, 3> target = a.goal;
        float face = a.yaw;
        if (react) {
            face = std::atan2(player[1] - a.position[1], player[0] - a.position[0]);
            if (a.alert == AiAlert::Alerted && !squad_areas_[a.squad].empty()) {
                target = nearest_objective_point(a.squad, player);
                speed = params_.run_speed;
            }
        } else if (!squad_areas_[a.squad].empty() && (a.character >= motions_.size() || !motions_[a.character].locomotion.empty())) {
            if (a.wait > 0) {
                a.wait -= dt;
                if (a.wait <= 0) a.goal = sample_objective(a.squad, a.position);
            } else if (dist2d(a.position, a.goal) < 0.3f) {
                a.wait = std::uniform_real_distribution<float>(params_.wait_min, params_.wait_max)(rng_);
            } else {
                speed = params_.walk_speed;
            }
            target = a.goal;
        }
        const float dx = target[0] - a.position[0], dy = target[1] - a.position[1], d = std::hypot(dx, dy);
        if (speed > 0 && d > 0.3f) {
            if (!react) face = std::atan2(dy, dx);
            const float step = std::min(speed * dt, d);
            a.velocity = {dx / d * step / dt, dy / d * step / dt, 0};
            a.position[0] += dx / d * step;
            a.position[1] += dy / d * step;
            a.position[2] += (target[2] - a.position[2]) * std::min(1.0f, step / d);
        } else {
            a.velocity = {};
        }
        const float turn = wrap_angle(face - a.yaw);
        a.yaw = wrap_angle(a.yaw + std::clamp(turn, -params_.turn_rate * dt, params_.turn_rate * dt));
        // Motion: velocity in the object's frame (x forward, y left).
        const float c = std::cos(a.yaw), s = std::sin(a.yaw);
        const std::array<float, 2> local{a.velocity[0] * c + a.velocity[1] * s, -a.velocity[0] * s + a.velocity[1] * c};
        const MotionChoice before = a.motion;
        if (a.character < motions_.size()) a.motion = select_motion(motions_[a.character], local);
        if (a.motion.locomotion != before.locomotion) a.motion_time = 0;
        else {
            float length = 0;
            if (a.character < motions_.size()) {
                const auto& set = motions_[a.character];
                if (a.motion.locomotion) length = set.locomotion[*a.motion.locomotion].seconds;
                else if (set.idle) length = set.idle->seconds;
            }
            a.motion_time += dt * a.motion.rate;
            if (length > 0) a.motion_time = std::fmod(a.motion_time, 2 * length);  // motion_clip_time folds it
        }
    }
    for (size_t i = 0; i < drones_.size(); ++i) {
        auto& d = drones_[i];
        const auto& spline = g_.point_groups[d.group].spline;
        const float len = drone_length_[i];
        d.along += float(d.heading) * params_.drone_speed * dt;
        if (d.along > len) d.along = 2 * len - d.along, d.heading = -1;
        if (d.along < 0) d.along = -d.along, d.heading = 1;
        float walked = 0;
        for (size_t k = 1; k < spline.size(); ++k) {
            const auto &p0 = spline[k - 1], &p1 = spline[k];
            const float seg = std::hypot(p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]);
            if (walked + seg >= d.along || k + 1 == spline.size()) {
                const float t = seg > 0 ? std::clamp((d.along - walked) / seg, 0.0f, 1.0f) : 0.0f;
                for (size_t a = 0; a < 3; ++a) d.position[a] = p0[a] + t * (p1[a] - p0[a]);
                d.yaw = std::atan2(float(d.heading) * (p1[1] - p0[1]), float(d.heading) * (p1[0] - p0[0]));
                break;
            }
            walked += seg;
        }
        perceive(d.position, d.yaw, params_.drone_scan_range, -1, false, player, dt, d.awareness, d.alert, d.sees_player);
    }
}

} // namespace nim
