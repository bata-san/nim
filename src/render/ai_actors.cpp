#include "nim/render/ai_actors.hpp"

#include "nim/core/package_manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <set>
#include <unordered_set>

namespace nim {

namespace {
constexpr float kClipFps = 30;     // clip rate (not decoded, 3.69)
constexpr float kBlendSeconds = 0.25f;

BoneTransform blend_bone(const BoneTransform& a, const BoneTransform& b, float t) {
    BoneTransform r;
    float d = 0;
    for (size_t k = 0; k < 4; ++k) d += a.rotation[k] * b.rotation[k];
    const float sign = d < 0 ? -1.0f : 1.0f;
    float len = 0;
    for (size_t k = 0; k < 4; ++k) {
        r.rotation[k] = a.rotation[k] * (1 - t) + sign * b.rotation[k] * t;  // nlerp
        len += r.rotation[k] * r.rotation[k];
    }
    len = len > 0 ? 1 / std::sqrt(len) : 1;
    for (auto& q : r.rotation) q *= len;
    for (size_t k = 0; k < 3; ++k) r.translation[k] = a.translation[k] * (1 - t) + b.translation[k] * t;
    r.scale = a.scale * (1 - t) + b.scale * t;
    return r;
}

const MotionClip* chosen_clip(const MotionSet& m, const MotionChoice& choice) {
    if (choice.locomotion && *choice.locomotion < m.locomotion.size()) return &m.locomotion[*choice.locomotion];
    return m.idle ? &*m.idle : nullptr;
}
const MotionClip* clip_by_tag(const MotionSet& m, TagHash tag) {
    for (const auto& c : m.locomotion)
        if (c.clip == tag) return &c;
    if (m.idle && m.idle->clip == tag) return &*m.idle;
    return nullptr;
}
}  // namespace

ActorSources index_actor_sources(PackageManager& pm) {
    ActorSources s;
    pm.build_hash64_index();
    s.materials = index_dynamic_model_materials(pm);
    s.rigs = index_model_skeletons(pm, s.materials, &s.assemblies, &s.motions);
    s.clips = index_animation_clips(pm);
    return s;
}

std::vector<TagHash> entity_dynamic_models(PackageManager& pm, TagHash entity, const ActorSources& sources) {
    std::unordered_multimap<uint32_t, uint32_t> by_component;
    for (const auto& [model, m] : sources.materials) by_component.emplace(m.component.value, model);
    std::vector<TagHash> out;
    const auto data = pm.read(entity);
    std::unordered_set<uint32_t> seen;
    for (size_t o = 0; o + 4 <= data.size(); o += 4) {
        uint32_t v;
        std::memcpy(&v, data.data() + o, 4);
        if (!seen.insert(v).second) continue;
        const auto [a, b] = by_component.equal_range(v);
        for (auto m = a; m != b; ++m) out.push_back(TagHash(m->second));
    }
    return out;
}

ActorCharacter prepare_actor_character(PackageManager& pm, const ActorSources& sources, const MotionSet& motion) {
    ActorCharacter c;
    c.entity = motion.entity;
    c.motion = motion;
    try {
        auto models = entity_dynamic_models(pm, motion.entity, sources);
        if (models.empty()) throw std::runtime_error("the entity names no dynamic model");
        // The assembly root (the model the Models tab assembles the body, head and gear around).
        std::ranges::stable_sort(models, [&](TagHash a, TagHash b) {
            auto rank = [&](TagHash m) {
                const auto it = sources.assemblies.find(m.value);
                return (it != sources.assemblies.end() ? int(it->second.size()) * 4 : 0) + (sources.rigs.contains(m.value) ? 2 : 0);
            };
            return rank(a) > rank(b);
        });
        std::vector<ClipInfo> wanted;
        auto want = [&](const MotionClip& m) {
            const auto it = std::ranges::find(sources.clips, m.clip, &ClipInfo::tag);
            if (it != sources.clips.end()) wanted.push_back(*it);
        };
        for (const auto& m : motion.locomotion) want(m);
        if (motion.idle) want(*motion.idle);
        std::string why;
        for (const auto model : models) {
            auto mesh = prepare_entity_preview(pm, model, sources.materials, sources.assemblies);
            if (mesh.no_drawable) {
                why += std::format("{}{}: nothing drawable", why.empty() ? "" : "; ", model.str());
                continue;
            }
            // Rig compatibility only: the model's own authored bank (a different entity's) would
            // filter out the character entity's motion-bank clips.
            auto anim = prepare_preview_animation(pm, mesh, sources.rigs, wanted, {}, sources.clips, nullptr);
            if (!anim || anim->clips.empty()) {
                why += std::format("{}{}: the motion clips do not fit its rig ({} parts)", why.empty() ? "" : "; ", model.str(), mesh.attachments.size());
                continue;
            }
            for (const auto& info : anim->clips) {
                AnimationClip clip = load_animation_clip(pm, info.tag);
                restrict_preview_clip(clip, info);
                c.clips.emplace(info.tag, std::move(clip));
            }
            c.model = model;
            c.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
            c.binding = std::make_shared<const PreviewAnimation>(std::move(*anim));
            return c;
        }
        // The entity's own skeleton and rig (those of its motion bank) when the model's indexed
        // binding does not take the clips: the root follows it directly and each skinned part
        // bone for bone (推定: parts share the skeleton's bone order).
        if (!motion.skeleton.is_none() && !motion.rig.is_none()) {
            const Skeleton skeleton = load_skeleton(pm, motion.skeleton);
            AnimationRig rig = load_animation_rig(pm, motion.rig);
            auto fits = [&](const PreparedMesh& m) {
                bool skinned = false;
                for (const auto& b : m.buffers) {
                    if (b.skin.bones.empty()) continue;
                    skinned = true;
                    if (b.skin.max_bone >= skeleton.size()) return false;
                }
                return skinned;
            };
            for (const auto model : models) {
                auto mesh = prepare_entity_preview(pm, model, sources.materials, sources.assemblies);
                if (mesh.no_drawable || !fits(mesh)) continue;
                PreviewAnimation anim;
                anim.model = mesh.tag;
                anim.skeleton = skeleton;
                anim.rig = rig;
                anim.inferred_binding = true;
                std::vector<int32_t> identity(skeleton.size());
                for (size_t b = 0; b < identity.size(); ++b) identity[b] = int32_t(b);
                for (const auto& part : mesh.attachments)
                    if (fits(part)) anim.followers.push_back({part.tag, identity});
                for (const auto& info : wanted) {
                    if (info.bones != rig.size()) continue;
                    c.clips.emplace(info.tag, load_animation_clip(pm, info.tag));
                }
                if (c.clips.empty()) break;
                c.model = model;
                c.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
                c.binding = std::make_shared<const PreviewAnimation>(std::move(anim));
                c.entity_binding = true;
                return c;
            }
        }
        c.error = why.empty() ? "no model" : why;
    } catch (const std::exception& e) {
        c.error = e.what();
    }
    return c;
}

std::vector<ModelSkinPose> actor_poses(const ActorCharacter& c, const AiAgent& agent, ActorAnimator& anim, float dt) {
    if (!c.binding) return {};
    const MotionClip* now = chosen_clip(c.motion, agent.motion);
    if (!now || !c.clips.contains(now->clip)) {
        if (!c.motion.idle || !c.clips.contains(c.motion.idle->clip)) return {};
        now = &*c.motion.idle;
    }
    if (now->clip != anim.clip) {
        if (!anim.clip.is_none()) {
            anim.previous = anim.clip;
            anim.previous_time = anim.time;  // where the old clip was
            anim.blend = 0;
        }
        anim.clip = now->clip;
    }
    auto pose_at = [&](TagHash tag, float time) {
        const auto* info = clip_by_tag(c.motion, tag);
        const auto& clip = c.clips.at(tag);
        const float t = info ? motion_clip_time(*info, time) : std::fmod(time, std::max(clip.frame_count, 1u) / kClipFps);
        const float frame = std::min(t * kClipFps, float(std::max(clip.frame_count, 1u) - 1));
        return c.binding->poses(clip, frame);
    };
    anim.time = agent.motion_time;
    auto poses = pose_at(now->clip, agent.motion_time);
    if (anim.blend < 1 && c.clips.contains(anim.previous)) {
        anim.previous_time += dt;
        anim.blend = std::min(1.0f, anim.blend + dt / kBlendSeconds);
        const auto before = pose_at(anim.previous, anim.previous_time);
        for (auto& p : poses) {
            const auto other = std::ranges::find(before, p.model, &ModelSkinPose::model);
            if (other == before.end() || other->bones.size() != p.bones.size()) continue;
            for (size_t b = 0; b < p.bones.size(); ++b) p.bones[b] = blend_bone(other->bones[b], p.bones[b], anim.blend);
        }
    } else {
        anim.blend = 1;
    }
    return poses;
}

std::array<float, 12> actor_rows(const AiAgent& a) {
    const float c = std::cos(a.yaw), s = std::sin(a.yaw);
    return {c, -s, 0, a.position[0], s, c, 0, a.position[1], 0, 0, 1, a.position[2]};
}

void AiActors::build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                     std::vector<ActorCharacter> characters, const MapGameplay& gameplay, const AiSimulation& sim,
                     size_t per_character) {
    clear();
    notes_.clear();
    by_character_.assign(gameplay.characters.size(), std::nullopt);
    // One drawable character per entity: characters (squad slots) sharing an entity share it.
    std::unordered_map<uint32_t, size_t> by_entity;
    for (size_t i = 0; i < characters.size() && i < gameplay.characters.size(); ++i) {
        auto& c = characters[i];
        if (!c.mesh) {
            if (!c.error.empty()) notes_.push_back(std::format("{} ({}): {}", gameplay.characters[i].name_estimate, c.entity.str(), c.error));
            continue;
        }
        auto [it, fresh] = by_entity.try_emplace(c.entity.value, characters_.size());
        if (fresh) characters_.push_back(std::move(c));
        by_character_[i] = it->second;
    }
    std::vector<size_t> agents(characters_.size());
    for (const auto& a : sim.agents())
        if (a.character < by_character_.size() && by_character_[a.character]) ++agents[*by_character_[a.character]];
    for (size_t c = 0; c < characters_.size(); ++c) {
        const size_t n = std::min(per_character, agents[c]);
        for (size_t k = 0; k < n; ++k) {
            try {
                const uint32_t actor = scene.add_actor(gpu, *characters_[c].mesh, layouts);
                slots_.push_back({c, actor, std::nullopt, {}});
            } catch (const std::exception& e) {
                notes_.push_back(std::format("{}: {}", characters_[c].entity.str(), e.what()));
                break;
            }
        }
    }
}

void AiActors::update(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float dt, float range) {
    const auto& agents = sim.agents();
    // Per character: its agents within range, nearest first.
    std::vector<std::vector<std::pair<float, size_t>>> nearby(characters_.size());
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto& a = agents[i];
        if (a.character >= by_character_.size() || !by_character_[a.character]) continue;
        const float d = std::hypot(a.position[0] - eye[0], a.position[1] - eye[1], a.position[2] - eye[2]);
        if (d <= range) nearby[*by_character_[a.character]].push_back({d, i});
    }
    std::vector<std::unordered_set<size_t>> wanted(characters_.size());
    std::vector<size_t> capacity(characters_.size());
    for (const auto& s : slots_) ++capacity[s.character];
    for (size_t c = 0; c < characters_.size(); ++c) {
        auto& list = nearby[c];
        const size_t n = std::min(capacity[c], list.size());
        std::partial_sort(list.begin(), list.begin() + ptrdiff_t(n), list.end());
        for (size_t k = 0; k < n; ++k) wanted[c].insert(list[k].second);
    }
    // Keep agents that stay wanted in their slot (their crossfade state), then fill free slots.
    for (auto& s : slots_) {
        if (s.agent && wanted[s.character].erase(*s.agent)) continue;
        s.agent.reset();
        s.anim = {};
    }
    for (auto& s : slots_) {
        if (s.agent || wanted[s.character].empty()) continue;
        s.agent = *wanted[s.character].begin();
        wanted[s.character].erase(wanted[s.character].begin());
    }
    shown_ = 0;
    for (auto& s : slots_) {
        if (!s.agent) {
            scene.set_actor(s.actor, std::nullopt);
            continue;
        }
        const auto& a = agents[*s.agent];
        auto poses = actor_poses(characters_[s.character], a, s.anim, dt);
        if (poses.empty()) {
            scene.set_actor(s.actor, std::nullopt);
            continue;
        }
        scene.set_actor(s.actor, actor_rows(a), std::move(poses));
        ++shown_;
    }
}

// ---- walkable floor -----------------------------------------------------------------------------

WalkGrid build_walk_grid(Gpu& gpu, StaticMeshScene& scene, const MapFiringArea& area, SunShadow& view, float cell) {
    WalkGrid grid;
    if (area.outline.size() < 3 || area.triangles.empty()) return grid;
    float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f, lo_z = 1e9f, hi_z = -1e9f;
    for (const auto& v : area.outline) {
        lo_x = std::min(lo_x, v[0]), hi_x = std::max(hi_x, v[0]);
        lo_y = std::min(lo_y, v[1]), hi_y = std::max(hi_y, v[1]);
        lo_z = std::min(lo_z, v[2]), hi_z = std::max(hi_z, v[2]);
    }
    const float cx = (lo_x + hi_x) / 2, cy = (lo_y + hi_y) / 2;
    float half = std::max(hi_x - lo_x, hi_y - lo_y) / 2 + 0.5f;
    const uint32_t size = std::clamp(uint32_t(std::ceil(2 * half / cell)), 16u, 1024u);
    cell = 2 * half / float(size);
    const float top = hi_z + 2.0f, range = top - (lo_z - 2.0f);
    view.resize(gpu, size);
    // Straight down from `top`: x right = world x, y up = world y, reversed depth over `range`.
    auto& vm = view.vm;
    vm = {};
    vm.fwd = {0, 0, -1};
    vm.right = {1, 0, 0};
    vm.up = {0, 1, 0};
    vm.eye = {cx, cy, top};
    vm.ortho = half;
    vm.fx = vm.fy = 1 / half;
    view.half = half;
    view.range = range;
    M4 rot;
    rot.c[0] = {1, 0, 0, 0};
    rot.c[1] = {0, 1, 0, 0};
    rot.c[2] = {0, 0, -1, 0};
    rot.c[3] = {0, 0, 0, 1};
    M4 proj;
    proj.c[0] = {1 / half, 0, 0, 0};
    proj.c[1] = {0, 1 / half, 0, 0};
    proj.c[2] = {0, 0, -1 / range, 0};
    proj.c[3] = {0, 0, 1, 1};
    vm.proj = proj;
    vm.vp_rel = mul(proj, rot);
    M4 tr;
    tr.c[0] = {1, 0, 0, 0};
    tr.c[1] = {0, 1, 0, 0};
    tr.c[2] = {0, 0, 1, 0};
    tr.c[3] = {-cx, -cy, -top, 1};
    vm.vp_abs = mul(vm.vp_rel, tr);
    vm.vp_prev = vm.vp_abs;
    scene.render_shadow(gpu, gpu.immediate(), view);
    const auto bytes = gpu.read_texture(view.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 4);
    const float* depth = reinterpret_cast<const float*>(bytes.data());

    grid.x0 = cx - half, grid.y0 = cy - half, grid.cell = cell, grid.w = grid.h = size;
    std::vector<uint8_t> open(size_t(size) * size, 0);
    for (uint32_t r = 0; r < size; ++r) {
        const float y = grid.y0 + (float(r) + 0.5f) * cell;
        for (uint32_t c = 0; c < size; ++c) {
            const float x = grid.x0 + (float(c) + 0.5f) * cell;
            // The floor height here, from the triangle that holds the point (none: outside).
            std::optional<float> floor;
            for (const auto& t : area.triangles) {
                const auto &a = area.outline[t[0]], &b = area.outline[t[1]], &d = area.outline[t[2]];
                const float den = (b[1] - d[1]) * (a[0] - d[0]) + (d[0] - b[0]) * (a[1] - d[1]);
                if (std::abs(den) < 1e-9f) continue;
                const float l0 = ((b[1] - d[1]) * (x - d[0]) + (d[0] - b[0]) * (y - d[1])) / den;
                const float l1 = ((d[1] - a[1]) * (x - d[0]) + (a[0] - d[0]) * (y - d[1])) / den;
                const float l2 = 1 - l0 - l1;
                if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                floor = l0 * a[2] + l1 * b[2] + l2 * d[2];
                break;
            }
            if (!floor) continue;
            const float dv = depth[size_t(size - 1 - r) * size + c];  // texture row 0 = largest y
            if (dv <= 0) continue;                                      // no surface: not floor
            const float surface = top - (1 - dv) * range;
            open[size_t(r) * size + c] = surface > *floor - 0.35f && surface < *floor + 0.45f;
        }
    }
    // Clearance for a body (0.3 m).
    const int reach = std::max(1, int(std::ceil(0.3f / cell)));
    grid.open.assign(open.size(), 0);
    for (int r = 0; r < int(size); ++r)
        for (int c = 0; c < int(size); ++c) {
            bool ok = open[size_t(r) * size + size_t(c)] != 0;
            for (int dr = -reach; ok && dr <= reach; ++dr)
                for (int dc = -reach; ok && dc <= reach; ++dc) {
                    if (dr * dr + dc * dc > reach * reach) continue;
                    const int rr = r + dr, cc = c + dc;
                    ok = rr >= 0 && cc >= 0 && rr < int(size) && cc < int(size) && open[size_t(rr) * size + size_t(cc)];
                }
            grid.open[size_t(r) * size + size_t(c)] = ok;
        }
    return grid;
}

// ---- indicators --------------------------------------------------------------------------------

namespace {
constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }
const uint32_t kAlertColor[3] = {rgba(80, 200, 110), rgba(240, 190, 40), rgba(235, 60, 50)};

struct Projector {
    const ViewMatrices& vm;
    float w, h;
    std::array<float, 4> clip(const std::array<float, 3>& p) const {
        std::array<float, 4> c{};
        const float v[4] = {p[0], p[1], p[2], 1};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) c[size_t(row)] += vm.vp_abs.c[col][row] * v[col];
        return c;
    }
    std::array<float, 2> screen(const std::array<float, 4>& c) const {
        return {(c[0] / c[3] * 0.5f + 0.5f) * w, (0.5f - c[1] / c[3] * 0.5f) * h};
    }
    std::optional<std::array<float, 2>> point(const std::array<float, 3>& p) const {
        const auto c = clip(p);
        if (c[3] < 0.1f) return std::nullopt;
        return screen(c);
    }
    // A world segment clipped against the near plane.
    void line(AiOverlay& o, const std::array<float, 3>& a, const std::array<float, 3>& b, uint32_t color, float width) const {
        auto ca = clip(a), cb = clip(b);
        constexpr float kNear = 0.1f;
        if (ca[3] < kNear && cb[3] < kNear) return;
        auto cut = [&](std::array<float, 4>& behind, const std::array<float, 4>& front) {
            const float t = (kNear - behind[3]) / (front[3] - behind[3]);
            for (size_t k = 0; k < 4; ++k) behind[k] += (front[k] - behind[k]) * t;
        };
        if (ca[3] < kNear) cut(ca, cb);
        if (cb[3] < kNear) cut(cb, ca);
        o.lines.push_back({screen(ca), screen(cb), color, width});
    }
};

float distance(const std::array<float, 3>& a, const std::array<float, 3>& b) { return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }

void meter(AiOverlay& o, std::array<float, 2> at, float awareness, AiAlert alert, bool sees, float scale) {
    const float w = 46 * scale, h = 6 * scale;
    const std::array<float, 2> lo{at[0] - w / 2, at[1] - h}, hi{at[0] + w / 2, at[1]};
    o.rects.push_back({lo, hi, rgba(0, 0, 0, 150), true});
    if (awareness > 0) o.rects.push_back({lo, {lo[0] + w * std::clamp(awareness, 0.0f, 1.0f), hi[1]}, kAlertColor[int(alert)], true});
    o.rects.push_back({lo, hi, rgba(255, 255, 255, 160), false});
    if (alert != AiAlert::Unaware)
        o.texts.push_back({{at[0] - 4 * scale, lo[1] - 18 * scale}, kAlertColor[int(alert)], alert == AiAlert::Alerted ? "!" : "?", 1.6f * scale});
    if (sees) o.texts.push_back({{hi[0] + 4, lo[1] - 4}, rgba(255, 255, 255, 220), "o", scale});
}
}  // namespace

AiOverlay build_ai_overlay(const AiSimulation& sim, const MapGameplay& g, const ViewMatrices& vm, float width, float height,
                           const AiOverlayOptions& opt) {
    AiOverlay o;
    const Projector pr{vm, width, height};
    const std::array<float, 3> eye{vm.eye.x, vm.eye.y, vm.eye.z};
    const auto& p = sim.params();
    // Firing areas the nearby squads use.
    std::set<size_t> areas;
    for (const auto& a : sim.agents()) {
        if (distance(a.position, eye) > opt.range) continue;
        if (const auto ob = g.squads[a.squad].objective)
            for (const auto fa : g.objectives[*ob].firing_areas) areas.insert(fa);
    }
    if (opt.debug)
        for (const auto fa : areas) {
            const auto& outline = g.firing_areas[fa].outline;
            for (size_t k = 0; k < outline.size(); ++k) {
                auto a = outline[k], b = outline[(k + 1) % outline.size()];
                a[2] += 0.05f, b[2] += 0.05f;
                pr.line(o, a, b, rgba(255, 170, 40, 200), 1.5f);
            }
        }
    if (opt.markers) {
        // One marker per objective of the nearby squads: its firing area nearest the camera.
        auto centre = [&](size_t fa) {
            const auto& area = g.firing_areas[fa];
            std::array<float, 3> c{};
            for (const auto& v : area.outline)
                for (size_t k = 0; k < 3; ++k) c[k] += v[k] / float(std::max<size_t>(area.outline.size(), 1));
            c[2] += 1.0f;
            return c;
        };
        std::set<size_t> marked;
        std::set<size_t> objectives;
        for (const auto& a : sim.agents())
            if (distance(a.position, eye) <= opt.range)
                if (const auto ob = g.squads[a.squad].objective) objectives.insert(*ob);
        for (const auto ob : objectives) {
            std::optional<size_t> best;
            float bd = opt.range;
            for (const auto fa : g.objectives[ob].firing_areas)
                if (const float d = distance(centre(fa), eye); d < bd) bd = d, best = fa;
            if (best) marked.insert(*best);
        }
        for (const auto fa : marked) {
            const auto& area = g.firing_areas[fa];
            const auto c = centre(fa);
            if (const auto s = pr.point(c)) {
                const float r = 6;
                const uint32_t col = rgba(255, 190, 70, 230);
                o.lines.push_back({{(*s)[0], (*s)[1] - r}, {(*s)[0] + r, (*s)[1]}, col, 2});
                o.lines.push_back({{(*s)[0] + r, (*s)[1]}, {(*s)[0], (*s)[1] + r}, col, 2});
                o.lines.push_back({{(*s)[0], (*s)[1] + r}, {(*s)[0] - r, (*s)[1]}, col, 2});
                o.lines.push_back({{(*s)[0] - r, (*s)[1]}, {(*s)[0], (*s)[1] - r}, col, 2});
                o.texts.push_back({{(*s)[0] + 9, (*s)[1] - 7}, col, std::format("{} {:.0f}m", area.name, distance(c, eye)), 1});
            }
        }
        for (const auto& d : sim.drones()) {
            const auto& spline = g.point_groups[d.group].spline;
            if (distance(d.position, eye) > opt.range * 2) continue;
            for (size_t k = 1; k < spline.size(); ++k) pr.line(o, spline[k - 1], spline[k], rgba(90, 150, 255, 170), 1.5f);
        }
    }
    auto cone = [&](const std::array<float, 3>& at, float yaw, float range, float half, uint32_t col) {
        constexpr int kSteps = 12;
        std::array<float, 3> prev = at;
        for (int k = 0; k <= kSteps; ++k) {
            const float a = yaw - half + 2 * half * float(k) / kSteps;
            const std::array<float, 3> q{at[0] + range * std::cos(a), at[1] + range * std::sin(a), at[2]};
            if (k == 0 || k == kSteps) pr.line(o, at, q, col, 1);
            if (k) pr.line(o, prev, q, col, 1);
            prev = q;
        }
    };
    auto ring = [&](const std::array<float, 3>& at, float r, uint32_t col) {
        constexpr int kSteps = 24;
        for (int k = 0; k < kSteps; ++k) {
            const float a0 = 2 * std::numbers::pi_v<float> * float(k) / kSteps, a1 = 2 * std::numbers::pi_v<float> * float(k + 1) / kSteps;
            pr.line(o, {at[0] + r * std::cos(a0), at[1] + r * std::sin(a0), at[2]}, {at[0] + r * std::cos(a1), at[1] + r * std::sin(a1), at[2]}, col, 1);
        }
    };
    const float half_fov = p.fov_degrees * 0.5f * std::numbers::pi_v<float> / 180;
    for (const auto& a : sim.agents()) {
        const float d = distance(a.position, eye);
        if (d > opt.range) continue;
        const float scale = std::clamp(18.0f / std::max(d, 1.0f), 0.6f, 1.2f);
        if (opt.debug) {
            const std::array<float, 3> feet{a.position[0], a.position[1], a.position[2] + 0.05f};
            cone(feet, a.yaw, std::min(a.sight, 12.0f), half_fov, rgba(255, 255, 255, 90));
            ring(feet, p.hearing_range, rgba(120, 200, 255, 70));
            if (a.velocity[0] != 0 || a.velocity[1] != 0) pr.line(o, feet, {a.goal[0], a.goal[1], a.goal[2] + 0.05f}, rgba(255, 255, 120, 140), 1);
        }
        if (opt.awareness || opt.debug) {
            if (const auto s = pr.point({a.position[0], a.position[1], a.position[2] + 2.15f})) {
                if (opt.awareness && (a.awareness > 0 || opt.debug)) meter(o, *s, a.awareness, a.alert, a.sees_player, scale);
                if (opt.debug)
                    o.texts.push_back({{(*s)[0] - 40, (*s)[1] + 4}, rgba(230, 230, 230, 220),
                                       std::format("{} {:.0f}m {}", g.characters[a.character].name_estimate, d, ai_alert_name(a.alert)), 0.85f});
            }
        }
    }
    for (const auto& dr : sim.drones()) {
        const float d = distance(dr.position, eye);
        if (d > opt.range * 2) continue;
        if (opt.debug) ring(dr.position, p.drone_scan_range, rgba(90, 150, 255, 110));
        if (const auto s = pr.point({dr.position[0], dr.position[1], dr.position[2] + 0.8f})) {
            if (opt.awareness && (dr.awareness > 0 || opt.debug)) meter(o, *s, dr.awareness, dr.alert, dr.sees_player, 1);
            if (opt.markers) o.texts.push_back({{(*s)[0] + 26, (*s)[1] - 10}, rgba(120, 170, 255, 230), std::format("drone {:.0f}m", d), 0.9f});
        }
    }
    return o;
}

} // namespace nim
