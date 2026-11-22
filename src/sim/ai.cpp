#include "nim/sim/ai.hpp"

#include "nim/content/animation.hpp"
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
        add({d.position[0] - 6 * c, d.position[1] - 6 * s, d.position[2]}, true);  // behind the bay
    }
    return out;
}

void AiSimulation::add_ground_patch(WalkGrid patch) {
    if (patch.w) patches_.push_back(std::move(patch));
    grid_index_dirty_ = true;
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
        auto pick_idle = [](Found& f) -> std::optional<MotionClip> {
            std::optional<float> upright;
            if (!f.locomotion_head.empty()) {
                upright = *std::ranges::max_element(f.locomotion_head);
                std::vector<MotionClip> kept;
                for (size_t k = 0; k < f.locomotion.size(); ++k)
                    if (*upright - f.locomotion_head[k] <= 0.12f) kept.push_back(f.locomotion[k]);
                f.locomotion = std::move(kept);
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
        {"spawn", 0, "squads without a dropship spawn at once at their positions (member i at position i mod n)"},
        {"dropship_call_range", p.dropship_call_range, "m: a dropship squad's ship is called when the player comes this close to its spawner (the engine's encounter trigger is script code)"},
        {"dropship_call_delay", p.dropship_call_delay, "s, uniform random delay after the call"},
        {"dropship_arrive", p.dropship_arrive, "s: arrival clip length at the 30 fps clip rate"},
        {"dropship_depart", p.dropship_depart, "s: departure clip length at 30 fps"},
        {"dropship_first_exit", p.dropship_first_exit, "s after landing until the first member walks out"},
        {"dropship_exit_interval", p.dropship_exit_interval, "s between members"},
        {"dropship_exit_walk", p.dropship_exit_walk, "s walking 3 m away after the drop; the drop itself falls freely (9.81 m/s2) from the hovering ship's bay (its origin, the spawner node) to the floor below: the highest walk-grid surface under it, else the floor of the squad's nearest firing area below it"},
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
    for (size_t s = 0; s < g.squads.size(); ++s)
        if (const auto o = g.squads[s].objective)
            for (const auto a : g.objectives[*o].firing_areas)
                if (!areas_[a].triangles.empty() && areas_[a].cumulative.back() > 0.01f) squad_areas_[s].push_back(a);
    std::uniform_real_distribution<float> angle(-kPi, kPi);
    for (size_t s = 0; s < g.squads.size(); ++s) {
        const auto& sq = g.squads[s];
        if (sq.positions.empty()) continue;
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
            a.position = p.position;
            a.position[0] += 0.8f * float(m / sq.positions.size());  // members sharing a position stand apart
            a.goal = a.position;
            a.yaw = angle(rng_);
            if (ship) {
                a.active = false;
                a.dropship = ship;
                a.position = dropships_[*ship].position;
                dropships_[*ship].members.push_back(agents_.size());
            }
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
    grid_index_dirty_ = true;
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
    // An invisible camera (the default) is neither seen nor heard, however close: awareness only decays.
    const bool noticeable = !params_.player_invisible && std::abs(dz) < params_.vertical_limit;
    if (noticeable && d < range) {
        const bool inside = !cone || d < 1e-3f || (dx * std::cos(yaw) + dy * std::sin(yaw)) / d >= cos_half_fov;
        if (inside) {
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
            if (now <= 0 || next >= r || (next > 0 && (overlapped || next >= now - 0.1f))) a.position[0] = nx, a.position[1] = ny;
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

float AiSimulation::exit_seconds(const AiDropship& d) const {
    const float h = std::max(0.0f, d.position[2] - floor_below(d.squad, d.position[0], d.position[1], d.position[2]));
    return std::sqrt(2 * h / 9.81f) + params_.dropship_exit_walk;
}

float AiSimulation::unload_seconds(const AiDropship& d) const {
    const float n = float(std::max<size_t>(d.members.size(), 1));
    return params_.dropship_first_exit + (n - 1) * params_.dropship_exit_interval + exit_seconds(d) + params_.dropship_linger;
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

// Dropships (3.71, 推定 timing): called by the player's distance; members leave the landed ship one
// by one, dropping from the open rear of the hovering ship to the floor below and walking off.
void AiSimulation::step_dropships(float dt, const std::array<float, 3>& player) {
    for (size_t i = 0; i < dropships_.size(); ++i) {
        auto& d = dropships_[i];
        if (d.start < 0 && dist2d(d.position, player) < params_.dropship_call_range)
            d.start = time_ + std::uniform_real_distribution<float>(0, params_.dropship_call_delay)(rng_);
        const auto state = dropship_state(i);
        if (state.phase != AiDropshipPhase::Unloading && state.phase != AiDropshipPhase::Departing && state.phase != AiDropshipPhase::Gone)
            continue;
        const float since_landing = (time_ - d.start) - params_.dropship_arrive;
        const float c = std::cos(d.yaw), s = std::sin(d.yaw);
        for (size_t k = 0; k < d.members.size(); ++k) {
            auto& a = agents_[d.members[k]];
            if (a.active && a.exit < 0) continue;
            const float t = since_landing - params_.dropship_first_exit - float(k) * params_.dropship_exit_interval;
            if (t < 0) continue;
            const float lateral = (float(k % 3) - 1) * 1.3f;
            const float x0 = -4.5f;  // under the open rear of the bay (ship space)
            if (!a.active) {
                a.active = true;
                a.exit_floor = floor_below(d.squad, d.position[0] + c * x0 - s * lateral, d.position[1] + s * x0 + c * lateral, d.position[2]);
            }
            // Ship space: dropped from the hovering ship's bay (its origin height) straight down,
            // then a walk 3 m further back, three abreast (1.3 m apart).
            const float h = std::max(0.0f, d.position[2] - a.exit_floor);
            const float fall = std::sqrt(2 * h / 9.81f);
            const float walk = std::clamp((t - fall) / params_.dropship_exit_walk, 0.0f, 1.0f);
            const float x = x0 - 3.0f * walk;
            const float z = t < fall ? d.position[2] - 0.5f * 9.81f * t * t : a.exit_floor;
            const std::array<float, 3> before = a.position;
            a.position = {d.position[0] + c * x - s * lateral, d.position[1] + s * x + c * lateral, z};
            a.yaw = wrap_angle(d.yaw + kPi);
            a.velocity = t < fall ? std::array<float, 3>{} : std::array<float, 3>{(a.position[0] - before[0]) / std::max(dt, 1e-4f),
                                                                                (a.position[1] - before[1]) / std::max(dt, 1e-4f), 0};
            if (walk >= 1) {
                a.exit = -1;
                a.position[2] = a.exit_floor;
                a.goal = a.position;
                a.velocity = {};
                a.wait = std::uniform_real_distribution<float>(0.5f, params_.wait_min)(rng_);
            } else {
                a.exit = t;
            }
        }
    }
}

void AiSimulation::step(float dt, const std::array<float, 3>& player) {
    time_ += dt;
    search_budget_ = std::min(search_budget_ + dt * 8, 2.0f);  // 8 path searches per simulated second
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
        const bool react = !params_.player_invisible && a.alert != AiAlert::Unaware;
        float speed = 0;
        std::array<float, 3> target = a.goal;
        float face = a.yaw;
        if (react) {
            face = std::atan2(player[1] - a.position[1], player[0] - a.position[0]);
            if (a.alert == AiAlert::Alerted && !squad_areas_[a.squad].empty()) {
                target = nearest_objective_point(a.squad, player);
                speed = run_speed(a);
            }
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
            a.velocity = {dir[0] * step / dt, dir[1] * step / dt, 0};
            a.position[0] += dir[0] * step;
            a.position[1] += dir[1] * step;
            a.position[2] += (target[2] - a.position[2]) * std::min(1.0f, step / d);
        } else {
            a.velocity = {};
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
