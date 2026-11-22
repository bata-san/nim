#include "nim/render/ai_actors.hpp"

#include <limits>

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
// The UESC robots' face indicator (material 80A4B5C4, docs/PROGRESS.md 3.77), unnamed object
// channels read by its pixel TFX: the state (x 0.25 through a spline and a gradient: 0 white
// ring, 1 yellow brackets, 2-3.5 red ring, 4 yellow) and a hide term (1 discards the panel; with
// no authored default the generic unset value 1 hid it).
constexpr uint32_t kIndicatorState = 0x319CE56E;
constexpr uint32_t kIndicatorHidden = 0xBE78BFD7;

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

// A skin transform is posed object space times inverse bind. Interpolating those transforms
// directly moves two connected joints along unrelated chords. Blend the posed local transforms
// and compose the result down the skeleton before putting it back in skinning space.
void blend_skin_pose(const Skeleton& skeleton, std::span<const BoneTransform> from,
                     std::span<BoneTransform> to, float weight) {
    std::vector<BoneTransform> a, b;
    a.reserve(to.size());
    b.reserve(to.size());
    for (size_t i = 0; i < to.size(); ++i) {
        a.push_back(from[i] * skeleton.bind[i]);
        b.push_back(to[i] * skeleton.bind[i]);
    }
    for (size_t i = 0; i < to.size(); ++i) {
        const int parent = skeleton.parents[i];
        to[i] = parent < 0 ? blend_bone(a[i], b[i], weight)
            : blend_bone(inverse(a[parent]) * a[i], inverse(b[parent]) * b[i], weight);
    }
    std::vector<uint8_t> done(to.size());
    auto compose = [&](auto&& self, size_t i) -> const BoneTransform& {
        if (!done[i]) {
            const int parent = skeleton.parents[i];
            a[i] = parent < 0 ? to[i] : self(self, size_t(parent)) * to[i];
            done[i] = 1;
        }
        return a[i];
    };
    for (size_t i = 0; i < to.size(); ++i) compose(compose, i);
    for (size_t i = 0; i < to.size(); ++i) to[i] = a[i] * skeleton.inverse_bind[i];
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

ActorDropship prepare_actor_dropship(PackageManager& pm, const ActorSources& sources, TagHash model) {
    ActorDropship d;
    d.model = model;
    try {
        auto mesh = prepare_entity_preview(pm, model, sources.materials, sources.assemblies);
        if (mesh.no_drawable) throw std::runtime_error("nothing drawable");
        auto anim = prepare_preview_animation(pm, mesh, sources.rigs, sources.clips, {}, {}, &sources.motions);
        if (!anim || anim->clips.empty()) throw std::runtime_error("no clips fit its rig");
        // The root track: the rig node carrying the flight (the first with translation keys away
        // from the origin in any clip).
        for (const auto& info : anim->clips) {
            AnimationClip clip = load_animation_clip(pm, info.tag);
            const auto track = std::ranges::find_if(clip.translations, [](const auto& t) { return t.bone == 1 && !t.frames.empty(); });
            if (track == clip.translations.end() || clip.translations.size() < 2) continue;
            auto length = [](const std::array<float, 3>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
            const float first = length(track->frames.front()), last = length(track->frames.back());
            float farthest = 0;
            uint32_t away = 0;  // the last frame still away (departures snap back to the origin on their final key)
            for (size_t f = 0; f < track->frames.size(); ++f) farthest = std::max(farthest, length(track->frames[f]));
            for (size_t f = 0; f < track->frames.size(); ++f)
                if (length(track->frames[f]) >= 0.9f * farthest) away = uint32_t(f);
            if (first > 50 && first < 1000 && last < 1) d.arrivals.push_back(std::move(clip));
            else if (first < 1 && farthest > 50 && farthest < 1000) {
                d.departure_frames.push_back(away);
                d.departures.push_back(std::move(clip));
            }
            else if (first < 1 && last < 1 && clip.frame_count < 100 && !d.open) d.open = std::move(clip);
        }
        if (d.arrivals.empty() || d.departures.empty() || !d.open) throw std::runtime_error(std::format(
            "clips: {} arrivals, {} departures, {} open poses", d.arrivals.size(), d.departures.size(), d.open ? 1 : 0));
        d.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
        d.binding = std::make_shared<const PreviewAnimation>(std::move(*anim));
    } catch (const std::exception& e) {
        d.error = e.what();
    }
    return d;
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
            anim.previous_rate = anim.rate;
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
    anim.rate = agent.motion.locomotion && now == chosen_clip(c.motion, agent.motion) ? agent.motion.rate : 1;
    auto poses = pose_at(now->clip, agent.motion_time);
    if (anim.blend < 1 && c.clips.contains(anim.previous)) {
        anim.previous_time += dt * anim.previous_rate;
        anim.blend = std::min(1.0f, anim.blend + dt / kBlendSeconds);
        const auto before = pose_at(anim.previous, anim.previous_time);
        for (auto& p : poses) {
            const auto other = std::ranges::find(before, p.model, &ModelSkinPose::model);
            if (other == before.end() || other->bones.size() != p.bones.size()) continue;
            const auto part = std::ranges::find(c.binding->animated_parts, p.model, &PreviewAnimation::AnimatedPart::model);
            const Skeleton* skeleton = p.model == c.binding->model ? &c.binding->skeleton
                : part == c.binding->animated_parts.end() ? nullptr : &part->skeleton;
            if (skeleton && skeleton->size() == p.bones.size())
                blend_skin_pose(*skeleton, other->bones, p.bones, anim.blend);
        }
        const auto& source = poses.front().bones;
        for (const auto& follower : c.binding->followers) {
            auto p = std::ranges::find(poses, follower.model, &ModelSkinPose::model);
            if (p == poses.end() || p == poses.begin() || p->bones.size() != follower.bone_map.size()) continue;
            for (size_t i = 0; i < follower.bone_map.size(); ++i) {
                const auto b = follower.bone_map[i];
                if (b >= 0 && size_t(b) < source.size()) p->bones[i] = source[b];
            }
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
    radius_.assign(characters_.size(), 0);
    for (size_t c = 0; c < characters_.size(); ++c) {
        const size_t n = std::min(per_character, agents[c]);
        for (size_t k = 0; k < n; ++k) {
            try {
                const uint32_t actor = scene.add_actor(gpu, *characters_[c].mesh, layouts);
                slots_.push_back({c, actor, std::nullopt, {}});
                if (k) continue;
                // The feet's reach in the idle pose (the points within 0.3 m of the lowest).
                AiAgent idle;
                ActorAnimator anim;
                const auto points = scene.actor_points(actor, actor_poses(characters_[c], idle, anim, 0), 2);
                if (points.empty()) continue;
                const float lowest = std::ranges::min(points, {}, &V3::z).z;
                float reach = 0;
                for (const auto& p : points)
                    if (p.z < lowest + 0.3f) reach = std::max(reach, std::hypot(p.x, p.y));
                radius_[c] = std::clamp(reach + 0.1f, 0.3f, 0.9f);
            } catch (const std::exception& e) {
                notes_.push_back(std::format("{}: {}", characters_[c].entity.str(), e.what()));
                break;
            }
        }
    }
}

void AiActors::apply_radii(AiSimulation& sim) const {
    for (size_t i = 0; i < by_character_.size(); ++i)
        if (by_character_[i] && radius_[*by_character_[i]] > 0) sim.set_character_radius(i, radius_[*by_character_[i]]);
}

void AiActors::build_dropships(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                               ActorDropship ship, size_t slots) {
    ship_slots_.clear();
    ship_ = std::move(ship);
    if (!ship_.mesh) {
        if (!ship_.error.empty()) notes_.push_back(std::format("dropship {}: {}", ship_.model.str(), ship_.error));
        return;
    }
    for (size_t k = 0; k < slots; ++k) {
        try {
            ship_slots_.push_back(scene.add_actor(gpu, *ship_.mesh, layouts));
        } catch (const std::exception& e) {
            notes_.push_back(std::format("dropship {}: {}", ship_.model.str(), e.what()));
            break;
        }
    }
}

void AiActors::update_dropships(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float range) {
    ships_shown_ = 0;
    if (ship_slots_.empty()) return;
    if (range < 0) {  // hidden
        for (const auto slot : ship_slots_) scene.set_actor(slot, std::nullopt);
        return;
    }
    // Ships in flight or unloading, nearest first (they fly in from up to ~270 m away).
    std::vector<std::pair<float, size_t>> list;
    for (size_t i = 0; i < sim.dropships().size(); ++i) {
        const auto state = sim.dropship_state(i);
        if (state.phase == AiDropshipPhase::Waiting || state.phase == AiDropshipPhase::Gone) continue;
        const auto& d = sim.dropships()[i];
        const float dist = std::hypot(d.position[0] - eye[0], d.position[1] - eye[1], d.position[2] - eye[2]);
        if (dist <= range + 300) list.push_back({dist, i});
    }
    std::ranges::sort(list);
    for (size_t k = 0; k < ship_slots_.size(); ++k) {
        if (k >= list.size()) {
            scene.set_actor(ship_slots_[k], std::nullopt);
            continue;
        }
        const auto& d = sim.dropships()[list[k].second];
        const auto state = sim.dropship_state(list[k].second);
        const AnimationClip* clip = &*ship_.open;
        float seconds = state.seconds;
        if (state.phase == AiDropshipPhase::Arriving) clip = &ship_.arrivals[d.variant % ship_.arrivals.size()];
        float last = float(std::max(clip->frame_count, 1u) - 1);
        if (state.phase == AiDropshipPhase::Departing) {
            const size_t v = (d.variant / 7) % ship_.departures.size();
            clip = &ship_.departures[v];
            last = float(ship_.departure_frames[v]);
        }
        const float frame = std::clamp(seconds * kClipFps, 0.0f, last);
        const float c = std::cos(d.yaw), s = std::sin(d.yaw);
        const std::array<float, 12> rows{c, -s, 0, d.position[0], s, c, 0, d.position[1], 0, 0, 1, d.position[2]};
        scene.set_actor(ship_slots_[k], rows, ship_.binding->poses(*clip, frame));
        ++ships_shown_;
    }
}

void AiActors::update(StaticMeshScene& scene, const AiSimulation& sim, std::array<float, 3> eye, float dt, float range) {
    update_dropships(scene, sim, eye, range);
    const auto& agents = sim.agents();
    // Per character: its agents within range, nearest first.
    std::vector<std::vector<std::pair<float, size_t>>> nearby(characters_.size());
    for (size_t i = 0; i < agents.size(); ++i) {
        const auto& a = agents[i];
        if (!a.active || a.character >= by_character_.size() || !by_character_[a.character]) continue;
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
        s.lift = std::numeric_limits<float>::quiet_NaN();
    }
    for (auto& s : slots_) {
        if (s.agent || wanted[s.character].empty()) continue;
        s.agent = *wanted[s.character].begin();
        wanted[s.character].erase(wanted[s.character].begin());
    }
    shown_ = 0;
    checks_.clear();
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
        // Grounding (推定, in place of the engine's foot IK, not in game data): the lowest point
        // of the posed mesh is put on the floor surface under the agent, and no point of the feet
        // (within 0.15 m of the lowest) goes below the surface under that point (a step or slope
        // up to 0.5 m above the floor under the agent), eased at 12/s.
        auto rows = actor_rows(a);
        const auto points = scene.actor_points(s.actor, poses, 4);
        if (!points.empty()) {
            const float lowest = std::ranges::min(points, {}, &V3::z).z;
            // Leaving a dropship: the path's own height (falling from the ship, then the floor).
            const float floor = a.exit >= 0 ? a.position[2] : sim.ground(a.position[0], a.position[1], a.position[2]).value_or(a.position[2]);
            float target = floor - lowest - a.position[2];
            if (a.exit < 0)
                for (const auto& p : points) {
                    if (p.z > lowest + 0.15f) continue;
                    const float x = rows[0] * p.x + rows[1] * p.y + rows[3], y = rows[4] * p.x + rows[5] * p.y + rows[7];
                    if (const auto top = sim.ground(x, y, floor); top && *top > floor && *top <= floor + 0.5f)
                        target = std::max(target, *top - p.z - a.position[2]);
                }
            s.lift = std::isnan(s.lift) || dt <= 0 ? target : s.lift + (target - s.lift) * std::min(1.0f, dt * 12);
            rows[11] += s.lift;
            if (checking_) {
                GroundCheck g{*s.agent, floor, 0, 0, sim.ground(a.position[0], a.position[1], a.position[2]).has_value()};
                size_t body = 0, inside = 0;
                for (const auto& p : scene.actor_points(s.actor, poses, 2)) {
                    const float x = rows[0] * p.x + rows[1] * p.y + rows[3], y = rows[4] * p.x + rows[5] * p.y + rows[7], z = p.z + rows[11];
                    const auto top = sim.ground(x, y, floor);
                    if (z < floor + 0.5f && top) g.sink = std::max(g.sink, *top - z);
                    if (z > floor + 0.3f && z < floor + 2) {
                        ++body;
                        inside += top && *top > z + 0.1f;
                    }
                }
                g.inside = body ? float(inside) / float(body) : 0;
                checks_.push_back(g);
            }
        }
        // The face indicator (UESC robots, material 80A4B5C4): its state follows the agent's alert.
        if (const int state = int(indicator_state(a.alert)); state != s.indicator) {
            const float v = float(state);
            scene.set_actor_channels(s.actor, {{kIndicatorState, {v, v, v, v}}, {kIndicatorHidden, {0, 0, 0, 0}}});
            s.indicator = state;
        }
        scene.set_actor(s.actor, rows, std::move(poses));
        ++shown_;
    }
}

// ---- walkable floor -----------------------------------------------------------------------------

namespace {

// Top surface heights (NaN: nothing drawn) of a size x size grid of `cell` m centred on (cx, cy),
// row-major from the smallest y, looking straight down from `top` over `range` m (the shadow
// pass: opaque geometry with shadow parts, alpha tested as the game's shadow shaders do).
std::vector<float> top_down_heights(Gpu& gpu, StaticMeshScene& scene, SunShadow& view, float cx, float cy, float half, uint32_t size,
                                    float top, float range) {
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
    std::vector<float> out(size_t(size) * size, std::numeric_limits<float>::quiet_NaN());
    for (uint32_t r = 0; r < size; ++r)
        for (uint32_t c = 0; c < size; ++c)
            if (const float dv = depth[size_t(size - 1 - r) * size + c]; dv > 0)  // texture row 0 = largest y
                out[size_t(r) * size + c] = top - (1 - dv) * range;
    return out;
}

// Spreads `free` from its cells over 4-neighbours whose surface differs by at most `step` m and
// that `allow` admits (推定: the step a body walks up).
template <class Allow>
void flood_floor(std::vector<uint8_t>& free, const std::vector<float>& height, uint32_t size, float step, Allow allow) {
    std::vector<uint32_t> queue;
    for (uint32_t i = 0; i < free.size(); ++i)
        if (free[i]) queue.push_back(i);
    for (size_t q = 0; q < queue.size(); ++q) {
        const uint32_t i = queue[q], r = i / size, c = i % size;
        const uint32_t next[4] = {c > 0 ? i - 1 : UINT32_MAX, c + 1 < size ? i + 1 : UINT32_MAX, r > 0 ? i - size : UINT32_MAX,
                                  r + 1 < size ? i + size : UINT32_MAX};
        for (const uint32_t j : next) {
            if (j == UINT32_MAX || free[j] || std::isnan(height[j]) || std::abs(height[j] - height[i]) > step || !allow(j)) continue;
            free[j] = 1;
            queue.push_back(j);
        }
    }
}

} // namespace

WalkGrid build_walk_grid(Gpu& gpu, StaticMeshScene& scene, const MapFiringArea& area, SunShadow& view, float cell,
                         std::span<const std::array<float, 3>> extra) {
    WalkGrid grid;
    if (area.outline.size() < 3 || area.triangles.empty()) return grid;
    float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f, lo_z = 1e9f, hi_z = -1e9f;
    for (const auto& v : area.outline) {
        lo_x = std::min(lo_x, v[0]), hi_x = std::max(hi_x, v[0]);
        lo_y = std::min(lo_y, v[1]), hi_y = std::max(hi_y, v[1]);
        lo_z = std::min(lo_z, v[2]), hi_z = std::max(hi_z, v[2]);
    }
    // Spawn points of the squads using the area, at its height and within 40 m: their ground too.
    const float area_lo_x = lo_x, area_hi_x = hi_x, area_lo_y = lo_y, area_hi_y = hi_y;
    for (const auto& p : extra)
        if (p[2] > lo_z - 1.5f && p[2] < hi_z + 1.5f && p[0] > area_lo_x - 40 && p[0] < area_hi_x + 40 && p[1] > area_lo_y - 40 && p[1] < area_hi_y + 40) {
            lo_x = std::min(lo_x, p[0] - 2), hi_x = std::max(hi_x, p[0] + 2);
            lo_y = std::min(lo_y, p[1] - 2), hi_y = std::max(hi_y, p[1] + 2);
        }
    const float cx = (lo_x + hi_x) / 2, cy = (lo_y + hi_y) / 2;
    float half = std::max(hi_x - lo_x, hi_y - lo_y) / 2 + 0.5f;
    const uint32_t size = std::clamp(uint32_t(std::ceil(2 * half / cell)), 16u, 1024u);
    cell = 2 * half / float(size);
    const float top = hi_z + 2.0f, range = top - (lo_z - 2.0f);
    grid.x0 = cx - half, grid.y0 = cy - half, grid.cell = cell, grid.w = grid.h = size;
    grid.height = top_down_heights(gpu, scene, view, cx, cy, half, size, top, range);
    std::vector<uint8_t> free(size_t(size) * size, 0), inside(size_t(size) * size, 0);
    std::vector<float> polygon(size_t(size) * size, std::numeric_limits<float>::quiet_NaN());
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
            const size_t i = size_t(r) * size + c;
            inside[i] = 1;
            polygon[i] = *floor;
        }
    }
    // The floor's height over the polygon: some areas' polygons lie on the floor, others about
    // 0.95 m below it (Outpost). The most common surface offset from the polygon within -0.5..2 m
    // (0.1 m bins, smoothed over neighbours) is the floor; open floor is within -0.35..+0.45 m of it.
    std::array<uint32_t, 25> bins{};
    for (size_t i = 0; i < polygon.size(); ++i)
        if (inside[i] && !std::isnan(grid.height[i]))
            if (const float d = grid.height[i] - polygon[i]; d >= -0.5f && d < 2.0f) ++bins[size_t((d + 0.5f) * 10)];
    size_t mode = 5;  // offset 0
    uint32_t best = 0;
    for (size_t b = 0; b < bins.size(); ++b) {
        const uint32_t v = bins[b] * 2 + (b ? bins[b - 1] : 0) + (b + 1 < bins.size() ? bins[b + 1] : 0);
        if (v > best) best = v, mode = b;
    }
    const float offset = best ? float(mode) / 10 - 0.5f + 0.05f : 0.0f;
    grid.floor_offset = offset;
    // A floor well above the polygon: look again from 2 m above that floor (props up to head height).
    if (offset > 0.2f) grid.height = top_down_heights(gpu, scene, view, cx, cy, half, size, top + offset, range + offset);
    for (size_t i = 0; i < polygon.size(); ++i)
        if (inside[i] && !std::isnan(grid.height[i]))
            free[i] = grid.height[i] > polygon[i] + offset - 0.35f && grid.height[i] < polygon[i] + offset + 0.45f;
    lo_z += offset, hi_z += offset;
    // Outside the polygon: the floor that continues from its open floor (steps up to 0.3 m), at
    // the area's height (to reach spawn points next to it).
    flood_floor(free, grid.height, size, 0.3f, [&](uint32_t j) { return !inside[j] && grid.height[j] > lo_z - 1.5f && grid.height[j] < hi_z + 1.0f; });
    grid.set_free(free);
    return grid;
}

WalkGrid build_ground_patch(Gpu& gpu, StaticMeshScene& scene, const std::array<float, 3>& at, bool from_above, SunShadow& view, float half,
                            float cell) {
    WalkGrid grid;
    const uint32_t size = std::clamp(uint32_t(std::ceil(2 * half / cell)), 16u, 1024u);
    cell = 2 * half / float(size);
    const float top = from_above ? at[2] - 0.5f : at[2] + 2.0f, range = from_above ? 80.0f : 8.0f;
    grid.x0 = at[0] - half, grid.y0 = at[1] - half, grid.cell = cell, grid.w = grid.h = size;
    grid.height = top_down_heights(gpu, scene, view, at[0], at[1], half, size, top, range);
    // The floor the point stands on (from above: whatever is highest below it), nearest the centre
    // within 1 m, and the floor that continues from it (steps up to 0.3 m).
    std::vector<uint8_t> free(size_t(size) * size, 0);
    const int centre = int(size / 2), reach = int(std::ceil(1.0f / cell));
    std::optional<size_t> seed;
    int best = INT32_MAX;
    for (int dr = -reach; dr <= reach; ++dr)
        for (int dc = -reach; dc <= reach; ++dc) {
            const size_t i = size_t(centre + dr) * size + size_t(centre + dc);
            const float h = grid.height[i];
            if (std::isnan(h) || (!from_above && std::abs(h - at[2]) > 1.0f) || dr * dr + dc * dc >= best) continue;
            best = dr * dr + dc * dc, seed = i;
        }
    if (seed) {
        free[*seed] = 1;
        flood_floor(free, grid.height, size, 0.3f, [](uint32_t) { return true; });
    }
    grid.set_free(free);
    return grid;
}

void add_ground_patches(Gpu& gpu, StaticMeshScene& scene, AiSimulation& sim, SunShadow& view) {
    for (const auto& need : sim.uncovered()) sim.add_ground_patch(build_ground_patch(gpu, scene, need.at, need.from_above, view));
}

std::vector<std::array<float, 3>> squad_spawn_points(const MapGameplay& gameplay, size_t firing_area) {
    std::vector<std::array<float, 3>> out;
    for (const auto& sq : gameplay.squads)
        if (sq.objective && std::ranges::contains(gameplay.objectives[*sq.objective].firing_areas, firing_area))
            for (const auto& p : sq.positions) out.push_back(p.position);
    return out;
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

}  // namespace

float AiActors::indicator_state(AiAlert alert) {
    switch (alert) {
        case AiAlert::Unaware: return 0;
        case AiAlert::Suspicious: return 1;
        case AiAlert::Alerted: return 2;
    }
    return 0;
}

namespace {

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
        if (!a.active || distance(a.position, eye) > opt.range) continue;
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
            if (a.active && distance(a.position, eye) <= opt.range)
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
        if (!a.active || d > opt.range) continue;
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
