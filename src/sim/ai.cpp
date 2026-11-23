#include "nim/sim/ai.hpp"

#include "nim/content/animation.hpp"
#include "nim/content/object_channels.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <numbers>
#include <unordered_map>

namespace nim {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr uint32_t kDropshipModel = 0x80ACA5E7;  // the UESC dropship (3.71)

float wrap_angle(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

float dist2d(const std::array<float, 3>& a, const std::array<float, 3>& b) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

}  // namespace

// ---- walkable floor --------------------------------------------------------------------------------

void WalkGrid::set_free(const std::vector<uint8_t>& free) {
    // Nearest blocked cell by two-pass propagation of nearest-site coordinates over 8 neighbours.
    const size_t n = size_t(w) * h;
    std::vector<int32_t> sx(n, -1), sy(n, -1);
    auto d2 = [](int32_t ax, int32_t ay, int32_t bx, int32_t by) { return int64_t(ax - bx) * (ax - bx) + int64_t(ay - by) * (ay - by); };
    for (uint32_t r = 0; r < h; ++r)
        for (uint32_t c = 0; c < w; ++c)
            if (!free[size_t(r) * w + c]) sx[size_t(r) * w + c] = int32_t(c), sy[size_t(r) * w + c] = int32_t(r);
    auto relax = [&](int32_t c, int32_t r, int32_t oc, int32_t orow) {
        if (oc < 0 || orow < 0 || oc >= int32_t(w) || orow >= int32_t(h)) return;
        const size_t i = size_t(r) * w + size_t(c), j = size_t(orow) * w + size_t(oc);
        if (sx[j] < 0) return;
        if (sx[i] < 0 || d2(c, r, sx[j], sy[j]) < d2(c, r, sx[i], sy[i])) sx[i] = sx[j], sy[i] = sy[j];
    };
    for (int32_t r = 0; r < int32_t(h); ++r) {
        for (int32_t c = 0; c < int32_t(w); ++c) relax(c, r, c - 1, r - 1), relax(c, r, c, r - 1), relax(c, r, c + 1, r - 1), relax(c, r, c - 1, r);
        for (int32_t c = int32_t(w) - 1; c >= 0; --c) relax(c, r, c + 1, r);
    }
    for (int32_t r = int32_t(h) - 1; r >= 0; --r) {
        for (int32_t c = int32_t(w) - 1; c >= 0; --c) relax(c, r, c + 1, r + 1), relax(c, r, c, r + 1), relax(c, r, c - 1, r + 1), relax(c, r, c + 1, r);
        for (int32_t c = 0; c < int32_t(w); ++c) relax(c, r, c - 1, r);
    }
    room.assign(n, 0);
    open.assign(n, 0);
    for (int32_t r = 0; r < int32_t(h); ++r)
        for (int32_t c = 0; c < int32_t(w); ++c) {
            const size_t i = size_t(r) * w + size_t(c);
            if (!free[i]) continue;
            // From the cell's centre to the nearest blocked cell's edge, or to the grid's edge.
            float d = float(std::min({c, r, int32_t(w) - 1 - c, int32_t(h) - 1 - r}) + 1) * cell - 0.5f * cell;
            if (sx[i] >= 0) d = std::min(d, std::sqrt(float(d2(c, r, sx[i], sy[i]))) * cell - 0.5f * cell);
            room[i] = std::max(d, 0.0f);
            open[i] = room[i] >= 0.3f;
        }
}

bool WalkGrid::walkable(float x, float y, float radius) const {
    if (!w) return true;
    const float fx = (x - x0) / cell, fy = (y - y0) / cell;
    if (fx < 0 || fy < 0 || fx >= float(w) || fy >= float(h)) return false;
    const size_t i = size_t(fy) * w + size_t(fx);
    return room.size() == open.size() ? room[i] > 0 && room[i] >= radius : open[i] != 0;
}

std::optional<float> WalkGrid::surface(float x, float y) const {
    if (!w || height.size() != size_t(w) * h) return std::nullopt;
    const float fx = (x - x0) / cell, fy = (y - y0) / cell;
    if (fx < 0 || fy < 0 || fx >= float(w) || fy >= float(h)) return std::nullopt;
    const float v = height[size_t(fy) * w + size_t(fx)];
    if (std::isnan(v)) return std::nullopt;
    return v;
}

// The grids and patches whose bounds hold (x, y), from 16 m buckets rebuilt after a change.
template <class F> void AiSimulation::each_grid(float x, float y, F&& f) const {
    constexpr float kBucket = 16;
    auto key = [](int64_t bx, int64_t by) { return (bx << 32) ^ int64_t(uint32_t(int32_t(by))); };
    if (grid_index_dirty_) {
        grid_index_.clear();
        for (uint8_t list = 0; list < 2; ++list) {
            const auto& grids = list ? patches_ : grids_;
            for (uint32_t i = 0; i < grids.size(); ++i) {
                const auto& g = grids[i];
                if (!g.w) continue;
                for (int64_t bx = int64_t(std::floor(g.x0 / kBucket)); bx <= int64_t(std::floor((g.x0 + float(g.w) * g.cell) / kBucket)); ++bx)
                    for (int64_t by = int64_t(std::floor(g.y0 / kBucket)); by <= int64_t(std::floor((g.y0 + float(g.h) * g.cell) / kBucket)); ++by)
                        grid_index_[key(bx, by)].push_back({list, i});
            }
        }
        grid_index_dirty_ = false;
    }
    const auto it = grid_index_.find(key(int64_t(std::floor(x / kBucket)), int64_t(std::floor(y / kBucket))));
    if (it == grid_index_.end()) return;
    for (const auto& [list, i] : it->second) f(list ? patches_[i] : grids_[i]);
}

std::optional<float> AiSimulation::ground(float x, float y, float z) const {
    std::optional<float> best;
    each_grid(x, y, [&](const WalkGrid& g) {
        const auto s = g.surface(x, y);
        if (s && std::abs(*s - z) < 1.5f && (!best || std::abs(*s - z) < std::abs(*best - z))) best = s;
    });
    return best;
}

bool AiSimulation::open_at(float x, float y, float z, float radius) const {
    bool open = false;
    each_grid(x, y, [&](const WalkGrid& g) {
        if (!open && g.walkable(x, y, radius))
            if (const auto s = g.surface(x, y); s && std::abs(*s - z) < 0.6f) open = true;
    });
    return open;
}

// Follows the floor from a to b in 0.125 m steps: each step must land on an open cell (of any
// grid or patch) at most 0.45 m above or below the last (推定 step height). The first `relaxed` m
// may pass cells narrower than the body (an agent spawned against a wall walks out). The height
// reached at b, if every step lands.
std::optional<float> AiSimulation::follow(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius, float relaxed,
                                          float spacing, bool skip_start) const {
    const float d = std::hypot(b[0] - a[0], b[1] - a[1]);
    const int steps = std::max(1, int(std::ceil(d / spacing)));
    float z = a[2];
    for (int i = skip_start ? 1 : 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        const float x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t;
        const float r = d * t < relaxed ? 0.01f : radius;
        std::optional<float> best;
        each_grid(x, y, [&](const WalkGrid& g) {
            if (!g.walkable(x, y, r)) return;
            const auto s = g.surface(x, y);
            if (s && std::abs(*s - z) <= 0.45f && (!best || std::abs(*s - z) < std::abs(*best - z))) best = s;
        });
        if (!best) return std::nullopt;
        z = *best;
    }
    return z;
}

bool AiSimulation::path_clear(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius) const {
    if (std::ranges::none_of(grids_, [](const WalkGrid& g) { return g.w != 0; }) && patches_.empty()) return true;
    const auto z = follow(a, b, radius, 1.0f);
    return z && std::abs(*z - b[2]) < 0.8f;
}

bool AiSimulation::line_of_sight(const std::array<float, 3>& eye, const std::array<float, 3>& target) const {
    if (std::ranges::none_of(grids_, [](const WalkGrid& g) { return g.w != 0; }) && patches_.empty()) return true;
    const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
    const float d = std::hypot(dx, dy);
    if (d < 1.5f) return true;
    const float z_hi = std::max(eye[2], target[2]) + 8.0f;
    for (float s = 0.75f; s < d - 0.75f; s += 0.5f) {
        const float t = s / d;
        const float x = eye[0] + dx * t, y = eye[1] + dy * t, rz = eye[2] + dz * t;
        bool blocked = false;
        each_grid(x, y, [&](const WalkGrid& g) {
            if (blocked) return;
            if (const auto h = g.surface(x, y); h && *h > rz + 0.25f && *h < z_hi) blocked = true;
        });
        if (blocked) return false;
    }
    return true;
}

// A* (推定: the engine's navigation is not in game data) over lattice points 0.5 m apart, each
// edge a floor-following walk; nodes at one lattice point are told apart by height (0.6 m), so
// floors above one another stay separate. Bounded to an ellipse around a and b.
std::optional<std::vector<std::array<float, 3>>> AiSimulation::find_path(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                                                         float radius, size_t max_nodes) const {
    return route(a, &b, radius, max_nodes, {}, 0);
}

// With `b`: A* to it. Without: Dijkstra within 40 m of a, then the path to one of the reached
// lattice points `accept` admits (the one at `pick` 0..1 of them in reach order).
std::optional<std::vector<std::array<float, 3>>> AiSimulation::route(const std::array<float, 3>& a, const std::array<float, 3>* to_b,
                                                                     float radius, size_t max_nodes,
                                                                     const std::function<bool(float, float, float)>& accept, float pick) const {
    const std::array<float, 3> b = to_b ? *to_b : a;
    auto h = [&](float x, float y) { return to_b ? std::hypot(x - b[0], y - b[1]) : 0.0f; };
    std::vector<uint32_t> candidates;
    constexpr float kStep = 0.5f;
    struct Node { float x, y, z, g; int32_t parent; };
    std::vector<Node> nodes{{a[0], a[1], a[2], 0, -1}};
    std::unordered_map<int64_t, std::vector<uint32_t>> at;
    auto key = [](int64_t ix, int64_t iy) { return (ix << 32) ^ int64_t(uint32_t(int32_t(iy))); };
    using Entry = std::pair<float, uint32_t>;  // f, node
    // The floors at each 0.25 m lattice point (surface, room), gathered once per search: edge
    // samples fall on that lattice (straight edges at 0.25 / 0.5 m, diagonals at the half / end).
    struct Floor { float z, room; };
    std::unordered_map<int64_t, std::vector<Floor>> floors;
    auto stand = [&](int64_t qx, int64_t qy, float z, float r) -> std::optional<float> {
        auto [it, fresh] = floors.try_emplace(key(qx, qy));
        if (fresh) {
            const float x = float(qx) * 0.25f, y = float(qy) * 0.25f;
            each_grid(x, y, [&](const WalkGrid& g) {
                const float fx = (x - g.x0) / g.cell, fy = (y - g.y0) / g.cell;
                if (fx < 0 || fy < 0 || fx >= float(g.w) || fy >= float(g.h)) return;
                const size_t i = size_t(fy) * g.w + size_t(fx);
                if (std::isnan(g.height[i])) return;
                const float room = g.room.size() == g.open.size() ? g.room[i] : g.open[i] ? 1e9f : 0.0f;
                if (room > 0) it->second.push_back({g.height[i], room});
            });
        }
        std::optional<float> best;
        for (const auto& f : it->second)
            if (f.room >= r && std::abs(f.z - z) <= 0.45f && (!best || std::abs(f.z - z) < std::abs(*best - z))) best = f.z;
        return best;
    };
    std::vector<Entry> heap{{h(a[0], a[1]), 0}};
    const float direct = dist2d(a, b), limit = to_b ? direct + 30 : 80;
    std::optional<uint32_t> reached;
    while (!heap.empty()) {
        std::ranges::pop_heap(heap, std::greater<>{});
        const auto [f, index] = heap.back();
        heap.pop_back();
        const Node n = nodes[index];
        if (f > n.g + h(n.x, n.y) + 1e-3f) continue;  // stale entry
        const std::array<float, 3> here{n.x, n.y, n.z};
        const float from_start = std::hypot(n.x - a[0], n.y - a[1]);
        if (!to_b) {
            if (index && from_start > 1.0f && accept(n.x, n.y, n.z)) candidates.push_back(index);
        } else if (std::hypot(n.x - b[0], n.y - b[1]) < 1.0f) {
            if (const auto z = follow(here, b, radius, std::max(0.0f, 1.0f - from_start)); z && std::abs(*z - b[2]) < 0.8f) {
                reached = index;
                break;
            }
        }
        if (nodes.size() >= max_nodes) break;
        const int64_t ix = int64_t(std::lround(n.x / kStep)), iy = int64_t(std::lround(n.y / kStep));
        const float r = from_start < 1.0f ? 0.01f : radius;  // out of a tight spawn spot
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                if (!dx && !dy) continue;
                const std::array<float, 3> to{float(ix + dx) * kStep, float(iy + dy) * kStep, n.z};
                if (std::hypot(to[0] - a[0], to[1] - a[1]) + std::hypot(to[0] - b[0], to[1] - b[1]) > limit) continue;
                std::optional<float> z;
                if (index == 0) {
                    z = follow(here, to, radius, 1.0f);  // the start is off the lattice
                } else {
                    // Halfway (a 0.25 m lattice point), then the end.
                    if (const auto mid = stand(2 * ix + dx, 2 * iy + dy, n.z, r)) z = stand(2 * (ix + dx), 2 * (iy + dy), *mid, r);
                }
                if (!z) continue;
                const float g = n.g + std::hypot(to[0] - n.x, to[1] - n.y);
                auto& list = at[key(ix + dx, iy + dy)];
                const auto same = std::ranges::find_if(list, [&](uint32_t k) { return std::abs(nodes[k].z - *z) < 0.6f; });
                uint32_t k;
                if (same != list.end()) {
                    k = *same;
                    if (g >= nodes[k].g) continue;
                    nodes[k].g = g, nodes[k].parent = int32_t(index), nodes[k].z = *z;
                } else {
                    k = uint32_t(nodes.size());
                    nodes.push_back({to[0], to[1], *z, g, int32_t(index)});
                    list.push_back(k);
                }
                heap.push_back({g + h(to[0], to[1]), k});
                std::ranges::push_heap(heap, std::greater<>{});
            }
    }
    std::vector<std::array<float, 3>> points;
    if (!to_b) {
        if (candidates.empty()) return std::nullopt;
        reached = candidates[std::min(candidates.size() - 1, size_t(pick * float(candidates.size())))];
    } else {
        if (!reached) return std::nullopt;
        points.push_back(b);
    }
    for (int32_t k = int32_t(*reached); k >= 0; k = nodes[size_t(k)].parent) points.push_back({nodes[size_t(k)].x, nodes[size_t(k)].y, nodes[size_t(k)].z});
    std::ranges::reverse(points);  // a ... b
    // String pulling: from each kept point, the farthest of the next 40 reached by a straight walk.
    std::vector<std::array<float, 3>> out;
    out.reserve(points.size());
    for (size_t i = 0; i + 1 < points.size();) {
        size_t j = std::min(points.size() - 1, i + 40);
        while (j > i + 1) {
            const auto z = follow(points[i], points[j], radius, i ? 0.0f : 1.0f);
            if (z && std::abs(*z - points[j][2]) < 0.6f) break;
            --j;
        }
        out.push_back(points[j]);
        i = j;
    }
    return out;
}

std::vector<AiSimulation::GroundNeed> AiSimulation::uncovered() const {
    std::vector<GroundNeed> out;
    auto add = [&](const std::array<float, 3>& p, bool above) {
        for (const auto& n : out)
            if (n.from_above == above && std::hypot(n.at[0] - p[0], n.at[1] - p[1]) < 6 && std::abs(n.at[2] - p[2]) < 1.5f) return;
        out.push_back({p, above});
    };
    for (const auto& a : agents_)
        if (!a.dropship && !open_at(a.position[0], a.position[1], a.position[2], 0.01f)) add(a.position, false);
    for (const auto& d : dropships_) {
        const float c = std::cos(d.yaw), s = std::sin(d.yaw);
        const float up = std::max(0.0f, params_.dropship_jump_up);
        // From 2.2 m over the ramp's top (3.117): ground rising above the ship's origin (Perimeter
        // (-251, -7, 32.5): 2 m higher behind it) was cut by a probe at the jump's apex, and the
        // members fell to a firing area 13 m below inside the rock.
        const float probe_top = d.position[2] + params_.dropship_lip_height + std::max(up * up / (2 * 9.81f), 2.0f) + 0.2f;
        // Centre the ground patch along the drop and departure, below the jump's highest point.
        const float behind = params_.dropship_jump_x - 3.0f;
        add({d.position[0] + behind * c, d.position[1] + behind * s, probe_top}, true);
    }
    return out;
}

void AiSimulation::add_ground_patch(WalkGrid patch) {
    if (patch.w) patches_.push_back(std::move(patch));
    grid_index_dirty_ = true;
    unload_cache_.clear();
    landing_cache_.clear();
}

void AiSimulation::set_character_radius(size_t character, float radius) {
    if (radius_.size() <= character) radius_.resize(character + 1, 0.3f);
    radius_[character] = radius;
}

float AiSimulation::walk_speed(const AiAgent& a) const {
    if (a.character < motions_.size() && motions_[a.character].walk_speed > 0) return motions_[a.character].walk_speed;
    return params_.walk_speed;
}

float AiSimulation::run_speed(const AiAgent& a) const {
    if (a.character < motions_.size() && motions_[a.character].run_speed > 0) return motions_[a.character].run_speed;
    return std::max(params_.run_speed, walk_speed(a));
}

float AiSimulation::radius(const AiAgent& a) const { return a.character < radius_.size() ? radius_[a.character] : 0.3f; }

bool WalkGrid::clear(const std::array<float, 3>& a, const std::array<float, 3>& b, float radius) const {
    if (!w) return true;
    const float d = std::hypot(b[0] - a[0], b[1] - a[1]);
    const int steps = std::max(1, int(d / (cell * 0.5f)));
    for (int i = 0; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        if (!walkable(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, radius)) return false;
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
        // The height the clip's foot IK targets stand at (rig nodes whose chain ends at a bone at
        // ground level): the 10th percentile over both feet and all frames, the planted foot;
        // none without foot targets.
        auto foot_floor = [&](const AnimationClip& clip) -> std::optional<float> {
            std::vector<float> zs;
            for (const auto& t : clip.translations) {
                if (t.bone >= rig.size() || rig.ik_root.size() != rig.size() || rig.ik_root[t.bone] < 0) continue;
                const size_t end = rig.skeleton_bone[t.bone];
                if (end >= skeleton.size() || std::fabs(rig.default_pose[t.bone].translation[2] - skeleton.bind[end].translation[2]) > 0.05f)
                    continue;
                for (const auto& f : t.frames) zs.push_back(f[2]);
            }
            if (zs.empty()) return std::nullopt;
            std::ranges::nth_element(zs, zs.begin() + zs.size() / 10);
            return zs[zs.size() / 10];
        };
        float ankle = 0;
        for (const int b : *feet) ankle += skeleton.bind[size_t(b)].translation[2] * 0.5f;
        // Posture: the mean object-space height of the head (the highest bind bone) over the clip.
        size_t head = 0;
        for (size_t b = 0; b < skeleton.size(); ++b)
            if (skeleton.bind[b].translation[2] > skeleton.bind[head].translation[2]) head = b;
        auto head_height = [&](const AnimationClip& clip) {
            std::vector<BoneTransform> pose;
            const uint32_t samples = std::min<uint32_t>(clip.frame_count, 12);
            float sum = 0;
            for (uint32_t k = 0; k < samples; ++k) {
                pose_skeleton(skeleton, rig, clip, float(k * clip.frame_count / samples), pose);
                sum += head < pose.size() ? pose[head].translation[2] : 0.0f;
            }
            return samples ? sum / float(samples) : 0.0f;
        };
        struct Stand {
            MotionClip clip;
            float head = 0;
        };
        struct Found {
            std::vector<MotionClip> locomotion, turns;
            std::vector<float> locomotion_head;
            std::vector<Stand> stands;  // clips that stand still (cyclic or played back and forth)
        } own, borrowed;
        // The animation data (3.91): the locomotion parameter table names the stances the engine
        // plays ("idle_slow"), and the action graph's clips (melee, fire, grenade launch, ...) are
        // actions, never an idle.
        std::optional<EntityMotionData> data;
        try {
            data = entity_motion_data(pm, entity);
        } catch (const std::exception&) {
        }
        for (const auto tag : entity_motion_clips(pm, entity)) {
            AnimationClip clip;
            try {
                clip = load_animation_clip(pm, tag);
            } catch (...) {
                continue;
            }
            Found* into = &own;
            bool planted = true;  // its foot IK targets (if any) stand at this skeleton's ankle height
            if (clip.bone_count != rig.size()) {
                // Another robot's clip of the shared bank, rebound by bone group. Only clips whose
                // planted feet stand at this skeleton's ankle height (within 5 mm: a body of the same
                // size; the assault, 1.1 times the others, plants at 0.135 m against 0.126) are taken.
                auto mapped = map_clip_groups(clip, rig);
                if (!mapped) continue;
                const auto floor = foot_floor(*mapped);
                if (!floor || std::fabs(*floor - ankle) > 0.005f) continue;
                clip = std::move(*mapped);
                into = &borrowed;
            } else if (const auto floor = foot_floor(clip)) {
                // Own clips: only tells a stance (feet near the ankle height; the grenadier's idles
                // plant 8 mm low) from a layer whose foot targets sit at the origin.
                planted = std::fabs(*floor - ankle) <= 0.03f;
            }
            const auto gait = measure_clip_gait(skeleton, rig, clip, *feet, fps);
            const float travel = std::hypot(clip.root_motion.translation[0], clip.root_motion.translation[1]);
            if (std::fabs(gait.yaw) >= 0.35f && travel < 0.04f && gait.seconds >= 0.3f) {
                MotionClip t{tag, gait.seconds, 0, {}, false};
                t.yaw = gait.yaw;
                into->turns.push_back(t);
                continue;
            }
            // An idle is a full-body stance: clips whose foot targets sit at the origin (the recruit's
            // 80A4AB6C, 173 frames) are layers without a stance of their own.
            if (planted && gait.speed < 0.15f && gait.lift < 0.08f && gait.seconds >= (gait.cyclic ? 0.5f : 1.0f)) {
                // With the animation data, only the parameter table's stances that no action plays.
                if (!data || (data->parameter_clips.contains(tag.value) && !data->action_clips.contains(tag.value)))
                    into->stands.push_back({MotionClip{tag, gait.seconds, 0, {}, gait.cyclic}, head_height(clip)});
                continue;
            }
            if (!gait.cyclic) continue;
            MotionClip m{tag, gait.seconds, gait.speed, gait.direction};
            m.authored = gait.authored;
            // A stride (root travel over one cycle) beyond 4 m is a dash or a multi-cycle move, not a gait (推定).
            if (gait.speed >= 0.6f && gait.lift >= 0.1f && gait.lift <= 0.6f && travel <= 4.0f) {
                into->locomotion.push_back(m);
                into->locomotion_head.push_back(head_height(clip));
            }
        }
        // Upright clips only (推定: the simulation has no crouch state): the bank also holds crouched
        // moves and idles (the recruit's crouched runs hold the head at 1.17-1.27 m and its idle
        // 80A4AA6B at 1.14 m, against 1.42-1.49 m walking and running upright). A clip is upright
        // when its head is within 0.12 m of the highest head among the same clips' locomotion. The
        // idle is the longest upright loop, else the longest upright clip played back and forth.
        // With the animation data the idle is the parameter table's "idle_slow" when this body can
        // play it (its own clip, or a shared one planted at this ankle height), else the longest
        // stance of the table no action plays; the posture is the data's (no upright filter).
        const std::vector<TagHash>* idle_slow = nullptr;
        if (data)
            if (const auto it = data->named.find(fnv1("idle_slow")); it != data->named.end()) idle_slow = &it->second;
        auto pick_idle = [&](Found& f) -> std::optional<MotionClip> {
            std::optional<float> upright;
            if (!f.locomotion_head.empty()) {
                upright = *std::ranges::max_element(f.locomotion_head);
                std::vector<MotionClip> kept;
                for (size_t k = 0; k < f.locomotion.size(); ++k)
                    if (*upright - f.locomotion_head[k] <= 0.12f) kept.push_back(f.locomotion[k]);
                f.locomotion = std::move(kept);
            }
            if (data) {
                upright.reset();
                if (idle_slow)
                    for (const auto& c : f.stands)
                        if (std::ranges::find(*idle_slow, c.clip.clip) != idle_slow->end()) return c.clip;
            }
            const Stand* best = nullptr;
            for (const auto& c : f.stands) {
                if (upright && std::fabs(c.head - *upright) > 0.12f) continue;
                if (!best || (c.clip.cyclic && !best->clip.cyclic) || (c.clip.cyclic == best->clip.cyclic && c.clip.seconds > best->clip.seconds))
                    best = &c;
            }
            return best ? std::optional<MotionClip>(best->clip) : std::nullopt;
        };
        const auto own_idle = pick_idle(own), borrowed_idle = pick_idle(borrowed);
        // The character's own clips first; what it lacks comes from the shared family (推定: the
        // motion graph's choice is not decoded).
        set.locomotion = !own.locomotion.empty() ? own.locomotion : borrowed.locomotion;
        set.turns = !own.turns.empty() ? own.turns : borrowed.turns;
        set.idle = own_idle ? own_idle : borrowed_idle;
        // The Compiler (80A97147) hovers rather than walking with foot root motion.
        // Motion graph 80B80986 node 1 ("idle", 6FB760FF) has two sequences:
        //   seq 0 = 80A97171 (45f, additive — 15 Hz bone-7 head/neck tremor)
        //   seq 1 = 80A97148 (31f, static, full-body — calm upright idle pose, 0 stretch)
        // own_idle picked 80A97171 because it is longer, causing severe neck trembling and a
        // hit-flinch appearance. Use seq 1 (80A97148) for idle and calm hover so the neck stays
        // steady and the lower cape does not billow.
        if (entity.value == 0x80A97147u) {
            set.locomotion.clear();
            set.turns.clear();
            try {
                const AnimationClip idle = load_animation_clip(pm, TagHash(0x80A97148u));
                if (idle.frame_count > 1) {
                    MotionClip m;
                    m.clip = TagHash(0x80A97148u);
                    m.seconds = float(idle.frame_count - 1) / fps;
                    m.cyclic = true;
                    m.authored = true;
                    set.idle = m;
                    m.speed = 1.6f;
                    m.direction = {1.0f, 0.0f};
                    set.locomotion.push_back(m);
                }
            } catch (...) {
            }
        }
        set.borrowed = (own.locomotion.empty() && !borrowed.locomotion.empty() ? 1 : 0) +
                       (own.turns.empty() && !borrowed.turns.empty() ? 2 : 0) + (!own_idle && set.idle ? 4 : 0);
        for (const auto& m : set.locomotion) {
            if (!m.authored || m.direction[0] < 0.9f) continue;
            set.walk_speed = set.walk_speed > 0 ? std::min(set.walk_speed, m.speed) : m.speed;
            set.run_speed = std::max(set.run_speed, m.speed);
        }
        if (set.run_speed < set.walk_speed * 1.3f) set.run_speed = 0;
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
        {"spawn", 0, "squads without a dropship spawn at their positions (member i at position i mod n) when the player comes within spawn_range; a member whose node is taken stands on the nearest open floor around it, 0.9 m from the others"},
        {"spawn_crowd", float(p.spawn_crowd), "active agents within 4 m of a squad's spawn nodes that make it wait to spawn (squads of several groups share staging spots; it spawns once they walk off)"},
        {"squad_alternatives", 0, "within a squad group, squads sharing a spawn node (or nodes within 2 m) are alternatives (fodder_squad / elite_squad ...): one of them, picked by the seed, spawns"},
        {"spawn_range", p.spawn_range, "m from the player to a squad's spawn points at which it appears (the engine's encounter scripts are code)"},
        {"despawn_range", p.despawn_range, "m: a squad is removed when the player and all its members are farther; a dropship's squad once its ship has left (the ship comes again)"},
        {"partial_walk", 3, "m: a patrol point behind an obstacle sends the agent to the farthest open point along the straight line (3 m or more) instead of waiting for a path search"},
        {"dropship_seats", 10, "members ride in the bay two abreast (y +-0.6 m), 0.9 m apart from x -4.6 m forward (ten seats); the bay floor (2.0 m) and the ramp (top face 2.45 m, x -5.4..-7.8) are the ship model's, the seats are not data"},
        {"dropship_bay_walk", p.dropship_bay_walk, "m/s from the seat down the ramp to the jump point"},
        {"dropship_jump_x", p.dropship_jump_x, "m (ship space): the point near the ramp's end (x -7.8) it steps off; the walk rises from the floor (2.0 m) to the ramp (2.45 m) between x -4.6 and -5.4, eased"},
        {"dropship_ramp", 11, "frames: the inner panel's clip (80ACA682) folds the ramp down from the start of unloading and back up from the start of the departure, at the clip rate; no clip plays it with the ship's others"},
        {"dropship_jump_back", p.dropship_jump_back, "m/s backwards off the ramp; the fall is free (9.81 m/s2), in the idle pose (no jump clip found)"},
        {"dropship_jump_up", p.dropship_jump_up, "m/s up off the ramp's end (a step off that clears it)"},
        {"dropship_call_range", p.dropship_call_range, "m: a dropship squad's ship is called when the player comes this close to its spawner (the engine's encounter trigger is script code)"},
        {"dropship_max_active", float(p.dropship_max_active), "ships in the air at once; the squads of one spawner are alternatives (one rides; 3.117, the encounter scripts are code)"},
        {"dropship_call_gap", p.dropship_call_gap, "s between two ship calls"},
        {"dropship_return_min", p.dropship_return_min, "s before a ship that has left can come again (uniform to dropship_return_max)"},
        {"dropship_return_max", p.dropship_return_max, "s"},
        {"dropship_call_delay", p.dropship_call_delay, "s, uniform random delay after the call"},
        {"dropship_arrive", p.dropship_arrive, "s: arrival clip length at the 30 fps clip rate"},
        {"dropship_depart", p.dropship_depart, "s: departure clip length at 30 fps"},
        {"dropship_first_exit", p.dropship_first_exit, "s after landing until the first member walks out"},
        {"dropship_exit_interval", p.dropship_exit_interval, "s between members"},
        {"dropship_exit_walk", p.dropship_exit_walk, "s walking 3 m away after the drop; the drop itself falls freely (9.81 m/s2) from the hovering ship's open rear (the bay floor, dropship_bay_height above the spawner node) to the floor below the landing point: the highest walk-grid surface under it, else the floor of the squad's nearest firing area below it"},
        {"dropship_linger", p.dropship_linger, "s the bay stays open after the last member"},
        {"path_search", 8, "path searches per second over all agents: Dijkstra over a 0.5 m lattice of floor-following walks (steps up to 0.45 m), 1500 lattice points, within 40 m; the goal is a random reached point of the squad's areas; paths string-pulled (the engine's navigation is not in game data)"},
        {"walk_grid", 0, "patrol points and straight paths only over open floor: a top-down depth render of the map's geometry between the floor and 2 m above it, eroded by 0.3 m (no navmesh found)"},
        {"stationary", 0, "characters without a locomotion clip of their own or of the shared bank (rebound by bone group) hold their position"},
        {"shared_clips", 0, "a robot without its own locomotion, turn or idle clips takes those of the shared bank whose planted feet stand at its ankle height (within 5 mm); the motion graph's own choice is not decoded"},
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
    squad_spawned_.assign(g.squads.size(), 0);
    ground_squad_members_.resize(g.squads.size());
    spawn_retry_at_.assign(g.squads.size(), 0.0f);
    for (size_t s = 0; s < g.squads.size(); ++s)
        if (const auto o = g.squads[s].objective)
            for (const auto a : g.objectives[*o].firing_areas)
                if (!areas_[a].triangles.empty() && areas_[a].cumulative.back() > 0.01f) squad_areas_[s].push_back(a);
    std::uniform_real_distribution<float> angle(-kPi, kPi);
    // Alternatives (3.112, 推定: the squad groups' choice is script code). A squad group lists squads
    // such as fodder_squad / fodder_squad_1 / elite_squad that spawn at the same nodes (Outpost
    // 80AF9E70: five squads on one node); spawning all of them stacked several squads on one
    // point. Within a group, squads sharing a spawn node (or nodes within 2 m) are alternatives and one of them spawns
    // (picked by the seed); squads on other nodes, and squads without a group, all spawn.
    std::vector<char> chosen(g.squads.size(), 1);
    {
        std::map<size_t, std::vector<size_t>> by_group;
        for (size_t s = 0; s < g.squads.size(); ++s)
            if (g.squads[s].group && !g.squads[s].positions.empty()) by_group[*g.squads[s].group].push_back(s);
        for (const auto& [group, list] : by_group) {
            std::vector<size_t> cluster(list.size());
            for (size_t i = 0; i < list.size(); ++i) cluster[i] = i;
            auto root = [&](size_t i) {
                while (cluster[i] != i) i = cluster[i] = cluster[cluster[i]];
                return i;
            };
            for (size_t i = 0; i < list.size(); ++i)
                for (size_t j = i + 1; j < list.size(); ++j) {
                    bool shared = false;
                    for (const auto& p : g.squads[list[i]].positions)
                        for (const auto& q : g.squads[list[j]].positions)
                            shared = shared || p.id == q.id ||
                                     std::hypot(p.position[0] - q.position[0], p.position[1] - q.position[1], p.position[2] - q.position[2]) < 2;
                    if (shared) cluster[root(i)] = root(j);
                }
            std::map<size_t, std::vector<size_t>> members;
            for (size_t i = 0; i < list.size(); ++i) members[root(i)].push_back(list[i]);
            for (const auto& [r, squads] : members) {
                if (squads.size() < 2) continue;
                const size_t pick = std::uniform_int_distribution<size_t>(0, squads.size() - 1)(rng_);
                for (size_t i = 0; i < squads.size(); ++i) chosen[squads[i]] = i == pick;
            }
        }
    }
    // One squad per dropship spawner (3.117, 推定): squads whose spawners sit at the same
    // coordinates (in different groups or none) shared one ship and all came out of it
    // (Perimeter: 64 squads in 47 ships); they are alternatives like those above.
    {
        std::map<std::array<int64_t, 3>, std::vector<size_t>> by_spawner;
        for (size_t s = 0; s < g.squads.size(); ++s) {
            if (!chosen[s]) continue;
            for (const auto& p : g.squads[s].positions)
                if (p.model.value == kDropshipModel) {
                    by_spawner[{std::llround(p.position[0] * 100), std::llround(p.position[1] * 100), std::llround(p.position[2] * 100)}].push_back(s);
                    break;
                }
        }
        for (const auto& [_, squads] : by_spawner) {
            if (squads.size() < 2) continue;
            const size_t pick = std::uniform_int_distribution<size_t>(0, squads.size() - 1)(rng_);
            for (size_t i = 0; i < squads.size(); ++i) chosen[squads[i]] = i == pick;
        }
    }
    for (size_t s = 0; s < g.squads.size(); ++s) {
        const auto& sq = g.squads[s];
        if (sq.positions.empty() || !chosen[s]) {
            if (sq.positions.empty() == false) ++alternatives_dropped_;
            continue;
        }
        // A squad whose spawner is a uesc_dropship_spawner node (its entity's model is the UESC
        // dropship, 80ACA5E7; the spawners sit in the `dropships` containers) arrives by ship.
        std::optional<size_t> ship;
        for (const auto& p : sq.positions)
            if (p.model.value == kDropshipModel) {
                // Squads whose spawners sit at the same coordinates share one ship: their members
                // leave it one after another (two ships in one place would overlap).
                if (const auto same = std::ranges::find_if(dropships_, [&](const AiDropship& o) {
                        return std::fabs(o.position[0] - p.position[0]) < 0.01f && std::fabs(o.position[1] - p.position[1]) < 0.01f &&
                               std::fabs(o.position[2] - p.position[2]) < 0.01f;
                    }); same != dropships_.end()) {
                    ship = size_t(same - dropships_.begin());
                    same->squads.push_back(s);
                    break;
                }
                AiDropship d;
                d.squad = s;
                d.position = p.position;
                const auto& q = p.rotation;
                d.yaw = std::atan2(2 * (q[3] * q[2] + q[0] * q[1]), 1 - 2 * (q[1] * q[1] + q[2] * q[2]));
                d.variant = uint32_t(s * 2654435761u >> 7);
                d.squads = {s};
                ship = dropships_.size();
                dropships_.push_back(d);
                break;
            }
        for (size_t m = 0; m < sq.members.size(); ++m) {
            AiAgent a;
            a.squad = s;
            a.character = sq.members[m];
            const auto& p = sq.positions[m % sq.positions.size()];
            a.position = p.position;  // members sharing a node stand apart when they spawn (spawn)
            a.goal = a.position;
            a.home = a.position;
            a.yaw = angle(rng_);
            a.active = false;  // spawned by distance (step_spawns) or by its ship
            if (ship) {
                a.dropship = ship;
                a.position = dropships_[*ship].position;
                // Bay seats (推定): two abreast from the rear, 0.9 m apart; members past ten
                // wait for a seat (step_dropships).
                const size_t k = dropships_[*ship].members.size() % 10;
                a.seat = {-4.6f + 0.9f * float(k / 2), k % 2 ? 0.6f : -0.6f, params_.dropship_bay_height};
                dropships_[*ship].members.push_back(agents_.size());
            }
            const auto& character = g.characters[a.character];
            static const uint32_t type = fnv1("type"), elite = fnv1("elite"), miniboss = fnv1("miniboss"), boss = fnv1("boss");
            const auto tier = std::ranges::find(character.tags, type, &std::pair<uint32_t, uint32_t>::first);
            const auto& name = character.name_estimate;
            a.sight = (tier != character.tags.end() ? tier->second == miniboss : name.find("miniboss") != std::string::npos) ? params_.sight_miniboss
                    : (tier != character.tags.end() ? tier->second == boss : name.find("boss") != std::string::npos) ? params_.sight_boss
                    : (tier != character.tags.end() ? tier->second == elite : name.find("elite") != std::string::npos) ? params_.sight_elite
                    : params_.sight_base;
            a.is_compiler = character.entity.value == 0x80A97147u || name.find("compiler") != std::string::npos;
            if (a.is_compiler) {
                a.sight = 45.0f;
                a.preferred_range = 9.0f;
            } else if (name.find("sniper") != std::string::npos) {
                a.preferred_range = 22.0f;
            } else if (name.find("ghost") != std::string::npos || name.find("shotgun") != std::string::npos) {
                a.preferred_range = 6.0f;
            } else if (name.find("assault") != std::string::npos || name.find("grenadier") != std::string::npos) {
                a.preferred_range = 12.0f;
            } else {
                a.preferred_range = 10.0f;
            }
            a.strafe_dir = (agents_.size() % 2) ? 1.0f : -1.0f;
            a.strafe_timer = std::uniform_real_distribution<float>(2.0f, 4.5f)(rng_);
            a.wait = std::uniform_real_distribution<float>(0, params_.wait_max)(rng_);
            if (!ship) ground_squad_members_[s].push_back(agents_.size());
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
    grid_index_dirty_ = true;
    unload_cache_.clear();
    landing_cache_.clear();
}

// A random open point of the squad's firing areas that a straight walk from `from` reaches
// (tries a few; keeps the agent where it is when none does).
// A patrol goal: a random open point of the squad's firing areas, reached by a straight walk when
// the floor allows one, else a random point of those areas on the floor reachable from the agent
// (a bounded Dijkstra search, route()) and the path to it (8 searches per simulated second over all agents, about 6 ms each; an agent
// whose turn is deferred asks again next step). No goal reached: it stays.
bool AiSimulation::in_area(size_t fa, float x, float y, float z, float radius) const {
    if (!has_walk_grid(fa) || !grids_[fa].walkable(x, y, radius)) return false;
    for (const auto& t : areas_[fa].triangles) {
        const auto &p0 = t[0], &p1 = t[1], &p2 = t[2];
        const float den = (p1[1] - p2[1]) * (p0[0] - p2[0]) + (p2[0] - p1[0]) * (p0[1] - p2[1]);
        if (std::abs(den) < 1e-9f) continue;
        const float l0 = ((p1[1] - p2[1]) * (x - p2[0]) + (p2[0] - p1[0]) * (y - p2[1])) / den;
        const float l1 = ((p2[1] - p0[1]) * (x - p2[0]) + (p0[0] - p2[0]) * (y - p2[1])) / den;
        const float l2 = 1 - l0 - l1;
        if (l0 < 0 || l1 < 0 || l2 < 0) continue;
        return std::abs(l0 * p0[2] + l1 * p1[2] + l2 * p2[2] + grids_[fa].floor_offset - z) < 0.6f;
    }
    return false;
}

void AiSimulation::choose_goal(AiAgent& a) {
    const auto& list = squad_areas_[a.squad];
    a.path.clear();
    a.path_next = 0;
    if (list.empty()) return;
    const float r = radius(a);
    bool searched = false;
    for (int attempt = 0; attempt < 24; ++attempt) {
        const size_t index = list[std::uniform_int_distribution<size_t>(0, list.size() - 1)(rng_)];
        auto p = sample_area(index);
        if (!has_walk_grid(index)) {
            a.goal = p;
            return;
        }
        if (!grids_[index].walkable(p[0], p[1], r)) continue;
        if (const auto s = grids_[index].surface(p[0], p[1])) p[2] = *s;
        if (path_clear(a.position, p, r)) {
            a.goal = p;
            ++path_stats_.straight;
            return;
        }
        // Blocked on the way (3.111, 推定): the farthest open point along the line that a straight
        // walk reaches, 3 m away or more, so the agent moves on without waiting for a path search.
        if (attempt < 6) {  // the first few points only (each tries up to 8 walks)
            const float dx = p[0] - a.position[0], dy = p[1] - a.position[1], len = std::hypot(dx, dy);
            bool partial = false;
            for (float t = 0.85f; t >= 0.15f && !partial; t -= 0.1f) {
                if (len * t < 3.0f) break;
                std::array<float, 3> q{a.position[0] + dx * t, a.position[1] + dy * t, a.position[2]};
                const auto z = ground(q[0], q[1], a.position[2]);
                if (!z) continue;
                q[2] = *z;
                if (open_at(q[0], q[1], q[2], r) && path_clear(a.position, q, r)) {
                    a.goal = q;
                    ++path_stats_.partial;
                    partial = true;
                }
            }
            if (partial) return;
        }
        if (searched) continue;
        if (search_budget_ < 1) {
            a.wait = std::uniform_real_distribution<float>(0.1f, 1.0f)(rng_);  // ask again soon
            ++path_stats_.deferred;
            return;
        }
        search_budget_ -= 1;
        searched = true;
        // The floor reachable from here (Dijkstra, 40 m): a point of the squad's areas in it.
        const float pick = std::uniform_real_distribution<float>(0, 1)(rng_);
        auto accept = [&](float x, float y, float z) {
            for (const auto fa : list)
                if (in_area(fa, x, y, z, r)) return true;
            return false;
        };
        if (auto found = route(a.position, nullptr, r, 1500, accept, pick)) {
            a.goal = found->back();
            a.path = std::move(*found);
            ++path_stats_.found;
            return;
        }
        ++path_stats_.failed;
        a.wait = params_.wait_max;  // nothing reachable from here this time: stand a while longer
        a.goal = a.position;
        return;
    }
    a.goal = a.position;
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
            for (const auto& p : {tri[0], tri[1], tri[2], c}) {
                const float dx = p[0] - to[0], dy = p[1] - to[1], dz = p[2] - to[2];
                if (const float d = dx * dx + dy * dy + dz * dz; d < bd) bd = d, best = p;
            }
        }
    return best;
}

void AiSimulation::perceive(const std::array<float, 3>& eye, float yaw, float range, float cos_half_fov, bool cone,
                            const std::array<float, 3>& player, float dt, float& awareness, AiAlert& alert, bool& sees) const {
    const float dx = player[0] - eye[0], dy = player[1] - eye[1], dz = player[2] - eye[2];
    const float d = std::hypot(dx, dy);
    float rise = 0;
    sees = false;
    // An invisible camera (the default) is neither seen nor heard, however close: awareness only decays.
    const bool noticeable = !params_.player_invisible && std::abs(dz) < params_.vertical_limit;
    if (noticeable && d < range) {
        const bool inside = !cone || d < 1e-3f || (dx * std::cos(yaw) + dy * std::sin(yaw)) / d >= cos_half_fov;
        if (inside && line_of_sight(eye, player)) {
            sees = true;
            rise += params_.sight_rise * (1 - d / range);
        }
    }
    if (noticeable && d < params_.hearing_range) rise += params_.hearing_rise;
    awareness = std::clamp(rise > 0 ? awareness + rise * dt : awareness - params_.decay * dt, 0.0f, 1.0f);
    // Hysteresis: an alerted enemy stays alerted until it falls below the suspicious threshold.
    if (awareness >= params_.alerted_at) alert = AiAlert::Alerted;
    else if (alert == AiAlert::Alerted && awareness >= params_.suspicious_at) alert = AiAlert::Alerted;
    else alert = awareness >= params_.suspicious_at ? AiAlert::Suspicious : AiAlert::Unaware;
}

// Bodies keep apart (推定; the engine's avoidance is not in game data): agents closer than the
// sum of their radii (the feet's reach, AiActors::apply_radii) are pushed apart evenly, onto open
// floor only, and an agent closer to a wall or prop than its radius steps 0.05 m per step towards
// more room (spawn points next to props).
void AiSimulation::separate() {
    constexpr float kCell = 1.8f;  // >= the largest radius sum
    std::unordered_map<int64_t, std::vector<size_t>> cells;
    auto key = [](int64_t x, int64_t y) { return (x << 32) ^ int64_t(uint32_t(int32_t(y))); };
    for (size_t i = 0; i < agents_.size(); ++i)
        if (agents_[i].active && agents_[i].exit < 0)
            cells[key(int64_t(std::floor(agents_[i].position[0] / kCell)), int64_t(std::floor(agents_[i].position[1] / kCell)))].push_back(i);
    std::vector<std::array<float, 2>> push(agents_.size(), {0, 0});
    for (size_t i = 0; i < agents_.size(); ++i) {
        const auto& a = agents_[i];
        if (!a.active || a.exit >= 0) continue;
        const int64_t cx = int64_t(std::floor(a.position[0] / kCell)), cy = int64_t(std::floor(a.position[1] / kCell));
        for (int64_t ox = -1; ox <= 1; ++ox)
            for (int64_t oy = -1; oy <= 1; ++oy) {
                const auto it = cells.find(key(cx + ox, cy + oy));
                if (it == cells.end()) continue;
                for (const size_t j : it->second) {
                    if (j <= i) continue;
                    const auto& b = agents_[j];
                    if (std::abs(a.position[2] - b.position[2]) > 1.5f) continue;
                    const float gap = radius(a) + radius(b);
                    float dx = b.position[0] - a.position[0], dy = b.position[1] - a.position[1];
                    const float d = std::hypot(dx, dy);
                    if (d >= gap) continue;
                    if (d < 1e-4f) dx = std::cos(float(i) * 2.4f), dy = std::sin(float(i) * 2.4f);
                    else dx /= d, dy /= d;
                    const float s = 0.5f * (gap - d);
                    push[i][0] -= dx * s, push[i][1] -= dy * s;
                    push[j][0] += dx * s, push[j][1] += dy * s;
                }
            }
    }
    for (size_t i = 0; i < agents_.size(); ++i) {
        auto& a = agents_[i];
        if (!a.active || a.exit >= 0) continue;
        const float r = radius(a);
        if (push[i][0] != 0 || push[i][1] != 0) {
            const float nx = a.position[0] + push[i][0], ny = a.position[1] + push[i][1];
            // Onto floor with room for it or no less room than it has, onto any free floor while
            // badly overlapped (members spawned on one point), anywhere off the grids.
            const float now = room_at(a.position[0], a.position[1], a.position[2]), next = room_at(nx, ny, a.position[2]);
            const bool overlapped = std::hypot(push[i][0], push[i][1]) > 0.25f * r;
            if (next >= r || (next > 0 && (overlapped || next >= now - 0.1f)) ||
                (now <= 0 && !ground(a.position[0], a.position[1], a.position[2]))) {
                const auto floor = ground(nx, ny, a.position[2]);
                if (!floor || std::abs(*floor - a.position[2]) <= 0.45f)
                    a.position = {nx, ny, floor.value_or(a.position[2])};
            }
            continue;  // no step towards room while pushed (it would gather them again)
        }
        const float here = room_at(a.position[0], a.position[1], a.position[2]);
        if (here <= 0 || here >= r) continue;  // off the grids, or clear
        float best = here;
        std::array<float, 2> to{};
        for (int k = 0; k < 8; ++k) {
            const float ang = float(k) * kPi / 4, x = a.position[0] + 0.05f * std::cos(ang), y = a.position[1] + 0.05f * std::sin(ang);
            if (const float room = room_at(x, y, a.position[2]); room > best) best = room, to = {x, y};
        }
        if (best > here) a.position[0] = to[0], a.position[1] = to[1];
    }
}

float AiSimulation::room_at(float x, float y, float z) const {
    float best = 0;
    each_grid(x, y, [&](const WalkGrid& g) {
        if (g.room.size() != g.open.size()) return;
        const float fx = (x - g.x0) / g.cell, fy = (y - g.y0) / g.cell;
        if (fx < 0 || fy < 0 || fx >= float(g.w) || fy >= float(g.h)) return;
        const size_t i = size_t(fy) * g.w + size_t(fx);
        if (!std::isnan(g.height[i]) && std::abs(g.height[i] - z) < 0.6f) best = std::max(best, g.room[i]);
    });
    return best;
}

float AiSimulation::floor_below(size_t squad, float x, float y, float z) const {
    std::optional<float> best;
    each_grid(x, y, [&](const WalkGrid& g) {
        if (const auto s = g.surface(x, y); s && *s <= z + 0.2f && (!best || *s > *best)) best = s;
    });
    if (best) return *best;
    // The squad's firing area nearest in plan whose floor is below.
    float nearest = 1e18f;
    for (const auto a : squad_areas_[squad])
        for (const auto& tri : areas_[a].triangles) {
            const float fz = (tri[0][2] + tri[1][2] + tri[2][2]) / 3;
            if (fz > z + 0.2f) continue;
            const float d = std::hypot((tri[0][0] + tri[1][0] + tri[2][0]) / 3 - x, (tri[0][1] + tri[1][1] + tri[2][1]) / 3 - y);
            if (d < nearest) nearest = d, best = fz;
        }
    return best.value_or(z);
}

// A member's way out of the bay (3.111, 推定): from its seat along the bay floor and down the
// folded ramp (rising to its top face between x -4.6 and -5.4 in ship space) to dropship_jump_x at
// dropship_bay_walk, a jump off the ramp (backwards and up) falling freely (9.81 m/s2) to the floor
// below the landing point, then 2 s walking 3 m further back.
AiSimulation::ExitPose AiSimulation::exit_pose(const AiDropship& d, const AiAgent& a, float t) const {
    constexpr float kLip = -4.6f, kRamp = -5.4f, kG = 9.81f;  // the floor rises to the ramp's top between them
    // Ground near or above the ramp's top (3.117, 推定): stepped onto in kStepTime instead of a jump.
    constexpr float kStepUp = 2.0f, kStepDown = 0.3f, kStepTime = 0.4f;
    const float kEdge = params_.dropship_jump_x;
    const float walk = std::max(0.0f, (a.seat[0] - kEdge) / params_.dropship_bay_walk);
    const float bay = params_.dropship_lip_height;  // the jump starts from the lip
    ExitPose p;
    if (t < walk) {
        const float x = a.seat[0] - params_.dropship_bay_walk * t;
        const float u = std::clamp((kLip - x) / (kLip - kRamp), 0.0f, 1.0f);
        const float lip = u * u * (3 - 2 * u);  // eased, no kink at either end
        p.at = {x, a.seat[1], a.seat[2] + lip * (params_.dropship_lip_height - a.seat[2])};
        p.velocity = {-params_.dropship_bay_walk, 0, 0};
        return p;
    }
    // The floor under where it lands (ship space z), from the drop height.
    const float c = std::cos(d.yaw), s = std::sin(d.yaw);
    auto floor_at = [&](float x) {
        const float wx = d.position[0] + c * x - s * a.seat[1], wy = d.position[1] + s * x + c * a.seat[1];
        // Ground up to kStepUp above the ramp's top counts (a ship hovering low over a rise lands its
        // members on it); below that, whatever is highest.
        return floor_below(d.squad, wx, wy, d.position[2] + bay + kStepUp) - d.position[2];
    };
    auto fall_time = [&](float floor) {
        if (floor > bay - kStepDown) return kStepTime;  // a step, not a fall
        const float up = params_.dropship_jump_up, h = bay - floor;
        return (up + std::sqrt(std::max(0.0f, up * up + 2 * kG * h))) / kG;
    };
    auto height = [&](float time) {
        return bay + params_.dropship_jump_up * time - 0.5f * kG * time * time;
    };
    if (landing_cache_.empty()) landing_cache_.resize(agents_.size());
    auto& landing = landing_cache_[size_t(&a - agents_.data())];
    if (!landing) {
        const float initial = fall_time(floor_at(kEdge - 1.0f));
        const float endpoint = floor_at(kEdge - params_.dropship_jump_back * initial);
        landing = Landing{fall_time(endpoint), endpoint};
        // A platform can intercept the descent before its projected endpoint.
        if (endpoint <= bay - kStepDown) {
            const float apex = std::max(0.0f, params_.dropship_jump_up / kG);
            const float end = landing->time;
            const int samples = std::max(1, int(std::ceil((end - apex) * 60)));
            for (int i = 1; i <= samples; ++i) {
                const float sample = apex + (end - apex) * float(i) / float(samples);
                const float x = kEdge - params_.dropship_jump_back * sample;
                const float wx = d.position[0] + c * x - s * a.seat[1], wy = d.position[1] + s * x + c * a.seat[1];
                std::optional<float> surface;
                each_grid(wx, wy, [&](const WalkGrid& grid) {
                    const auto z = grid.surface(wx, wy);
                    if (z && *z <= d.position[2] + bay + kStepUp + 0.2f && (!surface || *z > *surface)) surface = z;
                });
                if (!surface || height(sample) > *surface - d.position[2]) continue;
                landing = Landing{sample, *surface - d.position[2]};
                break;
            }
        }
    }
    const auto [fall, floor] = *landing;
    const float land = kEdge - params_.dropship_jump_back * fall;
    const float tj = t - walk;
    p.duration = walk + fall + params_.dropship_exit_walk;
    if (tj < fall && floor > bay - kStepDown) {
        const float u = std::clamp(tj / fall, 0.0f, 1.0f);
        p.at = {kEdge - params_.dropship_jump_back * tj, a.seat[1], bay + (floor - bay) * u * u * (3 - 2 * u)};
        p.velocity = {-params_.dropship_jump_back, 0, 0};
        return p;
    }
    if (tj < fall) {
        p.at = {kEdge - params_.dropship_jump_back * tj, a.seat[1], height(tj)};
        p.velocity = {};  // the idle pose in the air (no jump clip found)
        p.airborne = true;
        return p;
    }
    // Away from the drop point, fanned out by seat (3.112, 推定): the seat's row goes 1.2 m further
    // back per row and its side 2.4 m out, so a shipload does not stand on two spots.
    const float row = std::round((a.seat[0] + 4.6f) / 0.9f);
    const float dx = -3.0f - 1.2f * std::fmod(row, 3.0f), dy = a.seat[1] * 4.0f - a.seat[1];
    const float w = std::clamp((tj - fall) / params_.dropship_exit_walk, 0.0f, 1.0f);
    p.at = {land + dx * w, a.seat[1] + dy * w, floor};
    p.velocity = {w < 1 ? dx / params_.dropship_exit_walk : 0.0f, w < 1 ? dy / params_.dropship_exit_walk : 0.0f, 0};
    p.leaving = w < 1;
    auto world = [&](float x, float y, float z) {
        return std::array<float, 3>{d.position[0] + c * x - s * y, d.position[1] + s * x + c * y, d.position[2] + z};
    };
    auto last = world(land, a.seat[1], floor);
    if (ground(last[0], last[1], last[2])) {
        const int steps = std::max(1, int(std::ceil(std::hypot(dx, dy) * w / 0.25f)));
        for (int i = 1; i <= steps; ++i) {
            const float f = w * float(i) / float(steps);
            auto next = world(land + dx * f, a.seat[1] + dy * f, last[2] - d.position[2]);
            const auto z = follow(last, next, 0.01f, 0, 0.125f, true);
            if (!z) {
                const float previous = w * float(i - 1) / float(steps);
                p.at = {land + dx * previous, a.seat[1] + dy * previous, last[2] - d.position[2]};
                p.velocity = {};
                break;
            }
            next[2] = *z;
            p.at[2] = next[2] - d.position[2];
            last = next;
        }
    }
    return p;
}

float AiSimulation::exit_seconds(const AiDropship& d) const {
    // The longest way out of any member (the rear seats walk least, the front seats most).
    float longest = 0;
    for (const auto m : d.members) {
        longest = std::max(longest, exit_pose(d, agents_[m], 60).duration);
    }
    return longest;
}

float AiSimulation::unload_seconds(const AiDropship& d) const {
    // Cached per ship (the floor below it only changes when grids or patches are added).
    const size_t i = size_t(&d - dropships_.data());
    if (unload_cache_.size() != dropships_.size()) unload_cache_.assign(dropships_.size(), -1.0f);
    if (unload_cache_[i] < 0) {
        const float n = float(std::max<size_t>(d.members.size(), 1));
        unload_cache_[i] = params_.dropship_first_exit + (n - 1) * params_.dropship_exit_interval + exit_seconds(d) + params_.dropship_linger;
    }
    return unload_cache_[i];
}

AiDropshipState AiSimulation::dropship_state(size_t i) const {
    const auto& d = dropships_.at(i);
    if (d.start < 0 || time_ < d.start) return {AiDropshipPhase::Waiting, 0};
    float t = time_ - d.start;
    if (t < params_.dropship_arrive) return {AiDropshipPhase::Arriving, t};
    t -= params_.dropship_arrive;
    if (const float u = unload_seconds(d); t < u) return {AiDropshipPhase::Unloading, t};
    else t -= u;
    if (t < params_.dropship_depart) return {AiDropshipPhase::Departing, t};
    return {AiDropshipPhase::Gone, t - params_.dropship_depart};
}

void AiSimulation::call_dropship(size_t i) {
    if (dropships_.at(i).start < 0) dropships_[i].start = time_;
}

void AiSimulation::call_all_dropships() {
    for (auto& d : dropships_)
        if (d.start < 0) d.start = time_;
}

// Dropships (3.71 / 3.111, 推定 timing): called by the player's distance. The members ride in the
// bay from the arrival on (AiAgent::riding, drawn on the ship by the renderer); after landing they
// leave one by one: along the bay to the open rear, a jump off it to the floor below, a walk off.
void AiSimulation::step_dropships(float dt, const std::array<float, 3>& player) {
    for (size_t i = 0; i < dropships_.size(); ++i) {
        auto& d = dropships_[i];
        if (d.start < 0 && time_ >= d.ready_at && time_ >= next_dropship_call_ && dist2d(d.position, player) < params_.dropship_call_range) {
            size_t flying = 0;
            for (size_t j = 0; j < dropships_.size(); ++j) {
                const auto phase = dropship_state(j).phase;
                flying += dropships_[j].start >= 0 && phase != AiDropshipPhase::Gone;
            }
            if (flying < params_.dropship_max_active) {
                d.start = time_ + std::uniform_real_distribution<float>(0, params_.dropship_call_delay)(rng_);
                next_dropship_call_ = time_ + params_.dropship_call_gap;
            }
        }
        const auto state = dropship_state(i);
        if (state.phase == AiDropshipPhase::Waiting) continue;
        const float since_landing = (time_ - d.start) - params_.dropship_arrive;
        const float c = std::cos(d.yaw), s = std::sin(d.yaw);
        auto world = [&](const std::array<float, 3>& p) {
            return std::array<float, 3>{d.position[0] + c * p[0] - s * p[1], d.position[1] + s * p[0] + c * p[1], d.position[2] + p[2]};
        };
        for (size_t k = 0; k < d.members.size(); ++k) {
            auto& a = agents_[d.members[k]];
            if (a.active && a.exit < 0) continue;  // out and patrolling
            const float t = since_landing - params_.dropship_first_exit - float(k) * params_.dropship_exit_interval;
            if (t < 0) {
                // In the bay, facing the rear; past ten members, once the member ten ahead has left its seat.
                if (state.phase != AiDropshipPhase::Arriving && state.phase != AiDropshipPhase::Unloading) continue;
                a.riding = k < 10 || agents_[d.members[k - 10]].active;
                a.position = world(a.seat);
                a.yaw = wrap_angle(d.yaw + kPi);
                a.velocity = {};
                a.motion = MotionChoice{};
                a.motion_time += dt;
                continue;
            }
            a.riding = false;
            a.active = true;
            const auto p = exit_pose(d, a, t);
            a.position = world(p.at);
            a.yaw = wrap_angle(d.yaw + kPi);
            a.exit_floor = d.position[2] + p.at[2];
            a.velocity = {c * p.velocity[0] - s * p.velocity[1], s * p.velocity[0] + c * p.velocity[1], 0};
            if (!p.leaving) {
                a.exit = -1;
                a.goal = a.position;
                a.velocity = {};
                a.wait = std::uniform_real_distribution<float>(0.5f, params_.wait_min)(rng_);
            } else {
                a.exit = t;
            }
        }
    }
}

bool AiSimulation::spawn(AiAgent& a) {
    a.riding = false;
    a.exit = -1;
    a.position = a.home;
    // Not on top of another (3.112, 推定): the nearest open floor around the node, in rings 0.9 m
    // apart, with space for both bodies and at least 0.9 m from every active agent.
    auto free_at = [&](const std::array<float, 3>& p) {
        for (const auto& o : agents_)
            if (&o != &a && o.active && std::fabs(o.position[2] - p[2]) < 2 &&
                std::hypot(o.position[0] - p[0], o.position[1] - p[1]) < std::max(0.9f, radius(a) + radius(o)))
                return false;
        return true;
    };
    const bool has_floor = std::ranges::any_of(grids_, [](const WalkGrid& g) { return g.w != 0; }) || !patches_.empty();
    if (const auto z = ground(a.position[0], a.position[1], a.position[2])) a.position[2] = *z;
    const auto origin = a.position;
    const bool origin_open = open_at(origin[0], origin[1], origin[2], 0.01f);
    if (!free_at(origin) || (has_floor && !open_at(origin[0], origin[1], origin[2], radius(a)))) {
        bool placed = false;
        for (int ring = 1; ring <= 4 && !placed; ++ring)
            for (int k = 0; k < 6 * ring && !placed; ++k) {
                const float t = 2 * kPi * float(k) / float(6 * ring), r = 0.9f * float(ring);
                std::array<float, 3> p{a.home[0] + r * std::cos(t), a.home[1] + r * std::sin(t), a.home[2]};
                const auto z = ground(p[0], p[1], p[2]);
                if (z) p[2] = *z;
                const bool reachable = z && open_at(p[0], p[1], p[2], radius(a)) &&
                    (origin_open ? path_clear(origin, p, 0.3f) : std::fabs(p[2] - origin[2]) <= 0.45f);
                if ((!has_floor || reachable) && free_at(p))
                    a.position = p, placed = true;
            }
        if (!placed) return false;
    }
    a.active = true;
    a.goal = a.position;
    a.path.clear();
    a.path_next = 0;
    a.velocity = {};
    a.awareness = 0;
    a.alert = AiAlert::Unaware;
    a.sees_player = false;
    a.motion = MotionChoice{};
    a.motion_time = 0;
    a.wait = std::uniform_real_distribution<float>(0, params_.wait_max)(rng_);
    return true;
}

// Spawning by distance (3.111, 推定: the engine's encounter scripts are code): squads appear when the
// player comes within spawn_range of their spawn points and are removed when the player and all
// their members are beyond despawn_range. A dropship squad's ship is reset once it has left and
// its members are far, so it comes again.
void AiSimulation::step_spawns(const std::array<float, 3>& player) {
    auto far = [&](const std::array<float, 3>& p) {
        return std::hypot(p[0] - player[0], p[1] - player[1], p[2] - player[2]) > params_.despawn_range;
    };
    std::vector<float> nearest(g_.squads.size(), 1e30f);
    std::vector<uint8_t> members_far(g_.squads.size(), 1);
    for (const auto& a : agents_) {
        nearest[a.squad] = std::min(nearest[a.squad], std::hypot(a.home[0] - player[0], a.home[1] - player[1], a.home[2] - player[2]));
        if (a.active && !far(a.position)) members_far[a.squad] = 0;
    }
    // Room to spawn (3.112, 推定): squads of several groups share one staging spot (Outpost
    // (88.9, -60.1): five groups); a squad waits while its spawn nodes already have
    // spawn_crowd active agents within 4 m, and spawns once they have walked off (in waves).
    auto crowded = [&](size_t s) {
        size_t n = 0;
        for (const auto& a : agents_) {
            if (!a.active || a.squad == s) continue;
            for (const auto& p : g_.squads[s].positions)
                if (std::hypot(a.position[0] - p.position[0], a.position[1] - p.position[1]) < 4 && std::fabs(a.position[2] - p.position[2]) < 2) {
                    ++n;
                    break;
                }
        }
        return n >= params_.spawn_crowd;
    };
    for (size_t s = 0; s < g_.squads.size(); ++s) {
        const auto& members = ground_squad_members_[s];
        if (nearest[s] < params_.spawn_range && time_ >= spawn_retry_at_[s] &&
            std::ranges::any_of(members, [&](size_t i) { return !agents_[i].active; })) {
            spawn_retry_at_[s] = time_ + 0.25f;
            if (crowded(s)) continue;
            for (size_t i : members) {
                if (!agents_[i].active && spawn(agents_[i])) {
                    ++path_stats_.spawned;
                    squad_spawned_[s] = 1;
                }
            }
        } else if (squad_spawned_[s] && nearest[s] > params_.despawn_range && members_far[s]) {
            squad_spawned_[s] = 0;
            spawn_retry_at_[s] = 0.0f;
            for (size_t i : members)
                if (agents_[i].active) agents_[i].active = false, ++path_stats_.despawned;
        }
    }
    for (size_t i = 0; i < dropships_.size(); ++i) {
        auto& d = dropships_[i];
        if (d.start < 0 || dropship_state(i).phase != AiDropshipPhase::Gone || !far(d.position)) continue;
        if (std::ranges::any_of(d.members, [&](size_t m) { return agents_[m].active && !far(agents_[m].position); })) continue;
        d.start = -1;
        d.ready_at = time_ + std::uniform_real_distribution<float>(params_.dropship_return_min, params_.dropship_return_max)(rng_);
        for (const auto m : d.members) {
            auto& a = agents_[m];
            path_stats_.despawned += a.active;
            a.active = false;
            a.riding = false;
            a.exit = -1;
            a.position = d.position;
        }
    }
}

void AiSimulation::step(float dt, const std::array<float, 3>& player) {
    time_ += dt;
    search_budget_ = std::min(search_budget_ + dt * 8, 2.0f);  // 8 path searches per simulated second
    step_spawns(player);
    step_dropships(dt, player);
    const float cos_half = std::cos(params_.fov_degrees * 0.5f * kPi / 180.0f);
    for (auto& a : agents_) {
        if (!a.active) continue;
        if (a.exit >= 0) {
            // Leaving the ship: step_dropships moves it; only the walk clip follows its velocity.
            const float c = std::cos(a.yaw), s = std::sin(a.yaw);
            const std::array<float, 2> local{a.velocity[0] * c + a.velocity[1] * s, -a.velocity[0] * s + a.velocity[1] * c};
            const MotionChoice before = a.motion;
            if (a.character < motions_.size()) a.motion = select_motion(motions_[a.character], local);
            if (a.motion.locomotion != before.locomotion) a.motion_time = 0;
            else a.motion_time += dt * a.motion.rate;
            continue;
        }
        perceive({a.position[0], a.position[1], a.position[2] + 1.6f}, a.yaw, a.sight, cos_half, true, player, dt, a.awareness,
                 a.alert, a.sees_player);
        if (a.sees_player) {
            a.last_seen = player;
            a.has_last_seen = true;
        }
        // Squad alert sharing (推定): an alerted member who sees the player alerts nearby squadmates.
        if (!params_.player_invisible && a.alert == AiAlert::Alerted && a.sees_player) {
            for (auto& mate : agents_) {
                if (&mate == &a || !mate.active || mate.squad != a.squad || mate.exit >= 0) continue;
                if (dist2d(mate.position, a.position) < 18.0f && std::abs(mate.position[2] - a.position[2]) < 5.0f) {
                    mate.awareness = std::max(mate.awareness, params_.suspicious_at + 0.15f);
                    if (mate.alert == AiAlert::Unaware) mate.alert = AiAlert::Suspicious;
                    mate.last_seen = player;
                    mate.has_last_seen = true;
                }
            }
        }
        const bool react = !params_.player_invisible && a.alert != AiAlert::Unaware;
        a.replan_timer = std::max(0.0f, a.replan_timer - dt);
        a.strafe_timer -= dt;
        if (a.strafe_timer <= 0) {
            a.strafe_dir = -a.strafe_dir;
            a.strafe_timer = std::uniform_real_distribution<float>(2.5f, 4.5f)(rng_);
        }
        float speed = 0;
        std::array<float, 3> target = a.goal;
        float face = a.yaw;
        const float r = radius(a);
        auto plan_to = [&](const std::array<float, 3>& dest) {
            a.goal = dest;
            if (path_clear(a.position, dest, r)) {
                a.path.clear();
                a.path_next = 0;
                return;
            }
            if (a.replan_timer <= 0 && search_budget_ >= 1.0f) {
                search_budget_ -= 1.0f;
                a.replan_timer = 0.6f;
                if (auto found = find_path(a.position, dest, r, 1200)) {
                    a.path = std::move(*found);
                    a.path_next = 0;
                    ++path_stats_.found;
                } else {
                    a.path.clear();
                    a.path_next = 0;
                }
            }
        };
        if (a.is_compiler) {
            // Compiler (80A97147) encounter behavior (推定): hovers around its Cryo chamber
            // terminals at rest; charges its aura and orbits/surges when alerted.
            if (react) {
                const auto& focus = a.sees_player ? player : (a.has_last_seen ? a.last_seen : player);
                face = std::atan2(focus[1] - a.position[1], focus[0] - a.position[0]);
                if (a.alert == AiAlert::Alerted) {
                    a.compiler_phase_time = std::fmod(a.compiler_phase_time + dt, 5.5f);
                    const bool charging = a.compiler_phase_time < 3.5f;
                    const float desired_charge = charging ? (0.4f + 0.6f * (a.compiler_phase_time / 3.5f))
                                                          : (1.0f - 0.65f * ((a.compiler_phase_time - 3.5f) / 2.0f));
                    a.compiler_charge += (desired_charge - a.compiler_charge) * std::min(1.0f, dt * 3.0f);
                    const float dp = dist2d(a.position, focus);
                    const float ang = std::atan2(a.position[1] - focus[1], a.position[0] - focus[0]) + a.strafe_dir * (charging ? 0.35f : 0.75f);
                    std::array<float, 3> dest{focus[0] + a.preferred_range * std::cos(ang), focus[1] + a.preferred_range * std::sin(ang), a.position[2]};
                    if (dp < a.preferred_range * 0.65f) {
                        dest[0] = focus[0] + a.preferred_range * std::cos(ang - a.strafe_dir * 0.25f);
                        dest[1] = focus[1] + a.preferred_range * std::sin(ang - a.strafe_dir * 0.25f);
                    }
                    plan_to(dest);
                    speed = charging ? 1.8f : 3.2f;
                } else {
                    a.compiler_charge += (0.45f - a.compiler_charge) * std::min(1.0f, dt * 2.0f);
                    if (dist2d(a.position, focus) > 2.5f) {
                        plan_to(focus);
                        speed = 1.4f;
                    }
                }
            } else {
                a.compiler_charge = std::max(0.0f, a.compiler_charge - dt * 0.5f);
                a.compiler_phase_time = 0;
                if (a.wait > 0) {
                    a.wait -= dt;
                    if (a.wait <= 0) {
                        // Pick a chamber patrol point 6..14 m around home (matching the aura terminals).
                        for (int attempt = 0; attempt < 16; ++attempt) {
                            const float ang = std::uniform_real_distribution<float>(0, 2 * kPi)(rng_);
                            const float rad = std::uniform_real_distribution<float>(5.0f, 13.0f)(rng_);
                            std::array<float, 3> p{a.home[0] + rad * std::cos(ang), a.home[1] + rad * std::sin(ang), a.home[2]};
                            if (const auto s = ground(p[0], p[1], a.home[2])) p[2] = *s;
                            if (!ground(a.position[0], a.position[1], a.position[2]) || (open_at(p[0], p[1], p[2], r) && path_clear(a.position, p, r))) {
                                a.goal = p;
                                a.path.clear();
                                a.path_next = 0;
                                break;
                            }
                        }
                    }
                } else if (dist2d(a.position, a.goal) < 0.6f) {
                    a.wait = std::uniform_real_distribution<float>(3.0f, 6.0f)(rng_);
                } else {
                    speed = 1.5f;
                }
            }
            target = a.goal;
            while (a.path_next < a.path.size() && a.path_next + 1 < a.path.size() && dist2d(a.position, a.path[a.path_next]) < 0.6f) ++a.path_next;
            if (a.path_next < a.path.size()) target = a.path[a.path_next];
        } else if (react) {
            const auto& focus = a.sees_player ? player : (a.has_last_seen ? a.last_seen : player);
            face = std::atan2(focus[1] - a.position[1], focus[0] - a.position[0]);
            if (a.alert == AiAlert::Alerted && !squad_areas_[a.squad].empty()) {
                std::array<float, 3> dest = focus;
                const float dp = dist2d(a.position, focus);
                if (a.sees_player) {
                    const float from_player = std::atan2(a.position[1] - focus[1], a.position[0] - focus[0]);
                    if (dp < a.preferred_range * 0.7f) {
                        // Too close: back off slightly while strafing.
                        dest = {focus[0] + a.preferred_range * std::cos(from_player + a.strafe_dir * 0.25f),
                                focus[1] + a.preferred_range * std::sin(from_player + a.strafe_dir * 0.25f), a.position[2]};
                    } else if (dp <= a.preferred_range * 1.3f) {
                        // In combat range: strafe laterally around the player.
                        dest = {focus[0] + dp * std::cos(from_player + a.strafe_dir * 0.35f),
                                focus[1] + dp * std::sin(from_player + a.strafe_dir * 0.35f), a.position[2]};
                    } else {
                        // Advance to preferred range with per-agent angular spread.
                        const float spread = float((&a - agents_.data()) % 23) * 2.399963f;
                        dest = {focus[0] + a.preferred_range * 0.85f * std::cos(spread),
                                focus[1] + a.preferred_range * 0.85f * std::sin(spread), a.position[2]};
                    }
                    dest = nearest_objective_point(a.squad, dest);
                    speed = dp > a.preferred_range * 1.3f ? run_speed(a) : walk_speed(a);
                } else if (a.has_last_seen && dp > 2.0f) {
                    dest = nearest_objective_point(a.squad, focus);
                    speed = run_speed(a);
                }
                if (speed > 0) plan_to(dest);
            } else if (a.alert == AiAlert::Suspicious && a.has_last_seen && !squad_areas_[a.squad].empty()) {
                // Investigate last known player position instead of freezing in place.
                const auto dest = nearest_objective_point(a.squad, focus);
                if (dist2d(a.position, dest) > 2.0f) {
                    plan_to(dest);
                    speed = walk_speed(a);
                }
            }
            target = a.goal;
            while (a.path_next < a.path.size() && a.path_next + 1 < a.path.size() && dist2d(a.position, a.path[a.path_next]) < 0.5f) ++a.path_next;
            if (a.path_next < a.path.size()) target = a.path[a.path_next];
        } else if (!squad_areas_[a.squad].empty() && (a.character >= motions_.size() || !motions_[a.character].locomotion.empty())) {
            if (a.wait > 0) {
                a.wait -= dt;
                if (a.wait <= 0) choose_goal(a);
            } else if (dist2d(a.position, a.goal) < 0.3f) {
                a.wait = std::uniform_real_distribution<float>(params_.wait_min, params_.wait_max)(rng_);
            } else {
                speed = walk_speed(a);
            }
            target = a.goal;
            // Along the path's waypoints (the last is the goal).
            while (a.path_next < a.path.size() && a.path_next + 1 < a.path.size() && dist2d(a.position, a.path[a.path_next]) < 0.5f) ++a.path_next;
            if (a.path_next < a.path.size()) target = a.path[a.path_next];
        }
        const float dx = target[0] - a.position[0], dy = target[1] - a.position[1], d = std::hypot(dx, dy);
        if (speed > 0 && d > 0.3f && !react) face = std::atan2(dy, dx);
        const float turn = wrap_angle(face - a.yaw);
        const MotionSet* set = a.character < motions_.size() ? &motions_[a.character] : nullptr;
        // Turning in place with the character's own turn clip: started when the facing is off by more
        // than 40 degrees while standing (or about to walk), played to its end at its authored yaw rate.
        if (set && a.motion.turn && *a.motion.turn < set->turns.size()) {
            const auto& tc = set->turns[*a.motion.turn];
            a.motion_time += dt;
            if (a.motion_time < tc.seconds) {
                const float rate = std::fabs(tc.yaw) / tc.seconds;
                if (turn * tc.yaw > 0) a.yaw = wrap_angle(a.yaw + std::clamp(turn, -rate * dt, rate * dt));
                a.velocity = {};
                continue;
            }
            a.motion.turn.reset();
            a.motion_time = 0;
        }
        if (set && !set->turns.empty() && std::fabs(turn) > 0.7f && !react &&
            std::hypot(a.velocity[0], a.velocity[1]) < 0.3f) {
            size_t best = set->turns.size();
            for (size_t k = 0; k < set->turns.size(); ++k) {
                if (set->turns[k].yaw * turn <= 0) continue;
                if (best == set->turns.size() || std::fabs(std::fabs(set->turns[k].yaw) - std::fabs(turn)) <
                                                     std::fabs(std::fabs(set->turns[best].yaw) - std::fabs(turn)))
                    best = k;
            }
            if (best < set->turns.size()) {
                a.motion = MotionChoice{};
                a.motion.turn = best;
                a.motion_time = 0;
                a.velocity = {};
                continue;
            }
        }
        a.yaw = wrap_angle(a.yaw + std::clamp(turn, -params_.turn_rate * dt, params_.turn_rate * dt));
        if (speed > 0 && d > 0.3f) {
            // Patrolling: turn towards the goal, then walk along the facing (推定), slowing while the
            // goal is off to the side, so the gait is the forward one rather than a sidestep.
            // Reacting (facing the player): move straight at the point (strafing clips).
            std::array<float, 2> dir{dx / d, dy / d};
            if (!react) {
                const float off = std::cos(wrap_angle(face - a.yaw));
                speed *= std::clamp((off - 0.3f) / 0.7f, 0.0f, 1.0f);
                dir = {std::cos(a.yaw), std::sin(a.yaw)};
            }
            const float step = std::min(speed * dt, d);
            auto next = a.position;
            next[0] += dir[0] * step;
            next[1] += dir[1] * step;
            if (ground(a.position[0], a.position[1], a.position[2])) {
                // Enforce body-radius wall/container clearance (clamped when walking out of a tight
                // spawn so the agent never moves deeper into a wall). Slide tangentially on block.
                const float here_room = room_at(a.position[0], a.position[1], a.position[2]);
                const float move_r = here_room > 0 ? std::min(r, std::max(0.18f, here_room - 0.02f)) : 0.18f;
                auto try_step = [&](std::array<float, 2> d2, float s) -> std::optional<std::array<float, 3>> {
                    std::array<float, 3> cand{a.position[0] + d2[0] * s, a.position[1] + d2[1] * s, a.position[2]};
                    if (const auto z = follow(a.position, cand, move_r, 0, 0.125f, true)) {
                        cand[2] = *z;
                        return cand;
                    }
                    return std::nullopt;
                };
                if (auto ok = try_step(dir, step)) {
                    next = *ok;
                } else {
                    bool slid = false;
                    constexpr float kSlideAngles[] = {0.785f, -0.785f, 1.309f, -1.309f};
                    for (const float ang : kSlideAngles) {
                        const float ca = std::cos(ang), sa = std::sin(ang);
                        if (auto ok = try_step({dir[0] * ca - dir[1] * sa, dir[0] * sa + dir[1] * ca}, step * 0.7f)) {
                            next = *ok;
                            slid = true;
                            break;
                        }
                    }
                    if (!slid) next = a.position;
                }
            } else {
                next[2] += (target[2] - a.position[2]) * std::min(1.0f, step / d);
            }
            const float moved = dist2d(a.position, next);
            if (step > 1e-3f && moved < step * 0.15f) {
                a.stuck_time += dt;
                if (a.stuck_time > 0.8f) {
                    a.stuck_time = 0;
                    a.path.clear();
                    a.path_next = 0;
                    a.replan_timer = 0;
                    if (!react) {
                        a.goal = a.position;
                        a.wait = 0.35f;
                    }
                }
            } else {
                a.stuck_time = std::max(0.0f, a.stuck_time - dt * 2.0f);
            }
            a.velocity = {(next[0] - a.position[0]) / dt, (next[1] - a.position[1]) / dt, 0};
            a.position = next;
        } else {
            a.velocity = {};
            a.stuck_time = 0;
        }
        // On the floor under it (the surface nearest its height), where a grid or patch has one.
        if (const auto s = ground(a.position[0], a.position[1], a.position[2])) a.position[2] = *s;
        // Motion: velocity in the object's frame (x forward, y left).
        const float c = std::cos(a.yaw), s = std::sin(a.yaw);
        const std::array<float, 2> local{a.velocity[0] * c + a.velocity[1] * s, -a.velocity[0] * s + a.velocity[1] * c};
        const MotionChoice before = a.motion;
        if (a.character < motions_.size()) a.motion = select_motion(motions_[a.character], local);
        if (a.motion.locomotion != before.locomotion || before.turn) a.motion_time = 0;
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
    separate();
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
