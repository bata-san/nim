#include "nim/render/event_playback.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace nim {

std::vector<PreparedEventTrack> prepare_event_tracks(PackageManager& pm, MapLayout& map) {
    if (map.event_placements.empty()) return {};
    const auto sources = index_actor_sources(pm);
    struct Resource {
        std::shared_ptr<const PreparedMesh> mesh;
        std::shared_ptr<const PreviewAnimation> binding;
        std::shared_ptr<const AnimationClip> clip;
        std::shared_ptr<const EntitySounds> sounds;
    };
    std::map<std::pair<TagHash, TagHash>, Resource> resources;
    std::vector<TagHash> ship_entities;
    for (const auto& p : map.event_placements)
        if (p.model.value == 0x80ACA5E7) ship_entities.push_back(p.entity);
    std::ranges::sort(ship_entities);
    ship_entities.erase(std::unique(ship_entities.begin(), ship_entities.end()), ship_entities.end());
    const auto ships = ship_entities.empty() ? ActorDropship{} :
        prepare_actor_dropship(pm, sources, TagHash(0x80ACA5E7), ship_entities);
    std::vector<PreparedEventTrack> tracks;
    for (auto placement : map.event_placements) {
        if (placement.model.is_none()) {
            if (placement.entity.value != 0x80B08991 && placement.entity.value != 0x80B0D412) continue;
            const auto models = entity_dynamic_models(pm, placement.entity, sources);
            if (models.size() != 1) continue;
            placement.model = models.front();
        }
        const auto [it, fresh] = resources.try_emplace({placement.entity, placement.model});
        auto& r = it->second;
        const bool payload = !map.event_route.empty() &&
            (placement.entity.value == 0x80AC9EEB || placement.entity.value == 0x80B0F90C);
        if (fresh) {
            try {
                if (payload) {
                    auto mesh = prepare_entity_preview(pm, placement.model, sources.materials, sources.assemblies);
                    if (!mesh.no_drawable) {
                        r.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
                        r.sounds = std::make_shared<const EntitySounds>(load_entity_sounds(pm, std::span(&placement.entity, 1)));
                    }
                } else if (placement.model.value == 0x80ACA5E7) {
                    const auto spawner = std::ranges::find(ships.spawners, placement.entity, &ActorDropship::Spawner::entity);
                    if (spawner != ships.spawners.end() && spawner->arrival >= 0) {
                        r.mesh = ships.mesh;
                        r.binding = ships.binding;
                        r.clip = std::make_shared<const AnimationClip>(ships.arrivals[size_t(spawner->arrival)]);
                        r.sounds = spawner->sounds;
                    }
                } else if (sources.rigs.contains(placement.model.value)) {
                    auto mesh = prepare_entity_preview(pm, placement.model, sources.materials, sources.assemblies);
                    auto binding = prepare_preview_animation(pm, mesh, sources.rigs, sources.clips, placement.model);
                    if (!mesh.no_drawable && binding && binding->model == placement.model && binding->clips.size() == 1) {
                        const auto& info = binding->clips.front();
                        if (placement.model.value != 0x80B580E0 || info.tag.value == 0x80B580F4) {
                            auto clip = load_animation_clip(pm, info.tag);
                            restrict_preview_clip(clip, info);
                            r.clip = std::make_shared<const AnimationClip>(std::move(clip));
                            r.binding = std::make_shared<const PreviewAnimation>(std::move(*binding));
                            r.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
                            r.sounds = std::make_shared<const EntitySounds>(load_entity_sounds(pm, std::span(&placement.entity, 1)));
                        }
                    }
                }
            } catch (const std::exception&) {
                // A candidate that fails resolution retains its ordinary unanimated placement.
                r = {};
            }
        }
        if (!r.mesh || (!payload && (!r.clip || !r.clip->frame_count))) continue;
        PreparedEventTrack track{placement, r.mesh, r.binding, r.clip, r.sounds};
        if (payload) {
            const auto& rows = placement.rows;
            const std::array start{rows[3], rows[7], rows[11]};
            const auto distance = [](const auto& a, const auto& b) {
                return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
            };
            const auto first = std::ranges::min_element(map.event_route, {}, [&](const auto& point) { return distance(point, start); });
            track.route.push_back({0, start});
            for (auto point = first; point != map.event_route.end(); ++point) {
                const auto& last = track.route.back();
                const float segment = distance(last.position, *point);
                if (std::isfinite(segment) && segment > 0.01f) track.route.push_back({last.distance + segment, *point});
            }
            if (track.route.size() < 2) continue;
        }
        if (!map.event_actor_models.try_emplace(placement.node, placement.model).second) continue;
        tracks.push_back(std::move(track));
    }
    return tracks;
}

void EventPlayback::build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                          std::shared_ptr<const std::vector<PreparedEventTrack>> prepared) {
    prepared_ = std::move(prepared);
    slots_.clear();
    previous_frames_.clear();
    if (!prepared_) return;
    for (size_t i = 0; i < prepared_->size(); ++i) {
        const auto& track = (*prepared_)[i];
        slots_.push_back({scene.add_actor(gpu, *track.mesh, layouts), i});
    }
    previous_frames_.resize(slots_.size(), -1);
    stop(scene);
}

void EventPlayback::play() {
    seconds_ = 0;
    std::ranges::fill(previous_frames_, -1);
}

void EventPlayback::stop(StaticMeshScene& scene) {
    sounds_.clear();
    emitters_.clear();
    for (const auto& slot : slots_)
        scene.set_actor(slot.actor, (*prepared_)[slot.track].placement.rows);
}

void EventPlayback::step(StaticMeshScene& scene, float dt) {
    seconds_ += std::clamp(dt, 0.0f, 0.1f);
    sounds_.clear();
    emitters_.clear();
    for (size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        const auto& track = (*prepared_)[slot.track];
        auto rows = track.placement.rows;
        const float frame = track.clip ? std::min(seconds_ * frames_per_second, float(track.clip->frame_count - 1)) : 0;
        float& previous = previous_frames_[i];
        auto poses = track.clip ? track.binding->poses(*track.clip, frame) : std::vector<ModelSkinPose>{};
        if (!track.route.empty()) {
            constexpr float kPreviewConvoySpeed = 12;  // manual preview, metres per second
            const float distance = std::min(seconds_ * kPreviewConvoySpeed, track.route.back().distance);
            const auto end = std::lower_bound(track.route.begin() + 1, track.route.end(), distance,
                [](const auto& point, float at) { return point.distance < at; });
            const auto& start = *(end - 1);
            const auto& a = start.position;
            const auto& b = end->position;
            const float t = (distance - start.distance) / (end->distance - start.distance);
            const float yaw = std::atan2(b[1] - a[1], b[0] - a[0]) - std::atan2(rows[4], rows[0]);
            const auto placed = rows;
            for (size_t k = 0; k < 3; ++k) {
                rows[k] = std::cos(yaw) * placed[k] - std::sin(yaw) * placed[4 + k];
                rows[4 + k] = std::sin(yaw) * placed[k] + std::cos(yaw) * placed[4 + k];
                rows[4 * k + 3] = std::lerp(a[k], b[k], t);
            }
        }
        std::array<float, 3> position{rows[3], rows[7], rows[11]};
        if ((track.placement.model.value == 0x80B580E0 || track.placement.model.value == 0x80ACA5E7) &&
            !poses.empty() && poses.front().bones.size() > 1 && track.binding->skeleton.bind.size() > 1) {
            const auto point = (poses.front().bones[1] * track.binding->skeleton.bind[1]).translation;
            for (size_t axis = 0; axis < 3; ++axis)
                for (size_t k = 0; k < 3; ++k) position[axis] += rows[axis * 4 + k] * point[k];
        }
        const auto emitter = track.placement.node;
        if (track.sounds) {
            emitters_.push_back({emitter, position});
            if (previous < 0)
                for (const auto loop : track.sounds->loops) sounds_.push_back({loop, emitter});
            if (track.clip)
                for (const auto& marker : track.clip->events)
                    if (float(marker.frame) > previous && float(marker.frame) <= frame)
                        if (const auto events = track.sounds->find(marker.kind, marker.name, marker.value); events && !events->empty())
                            sounds_.push_back({events->front(), emitter});
        }
        previous = frame;
        scene.set_actor(slot.actor, rows, std::move(poses));
    }
}

}  // namespace nim
