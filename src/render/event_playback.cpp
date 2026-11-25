#include "nim/render/event_playback.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace nim {

std::vector<PreparedEventTrack> prepare_event_tracks(PackageManager& pm, MapLayout& map) {
    if (map.event_groups.empty()) return {};
    const auto sources = index_actor_sources(pm);
    struct Resource {
        std::shared_ptr<const PreparedMesh> mesh;
        std::shared_ptr<const PreviewAnimation> binding;
        std::shared_ptr<const AnimationClip> clip;
        std::shared_ptr<const EntitySounds> sounds;
    };
    std::map<std::pair<TagHash, TagHash>, Resource> resources;
    std::vector<PreparedEventTrack> tracks;
    for (const auto& group : map.event_groups) {
        // Dropship spawners belong to the enemy AI's spawning (3.133): its ships bring their
        // squads; a ship with no squad is never drawn.
        if (group.dropship) continue;
        for (auto placement : group.placements) {
            // Dropship spawners in other events (the convoy's five) are the AI's too: drawn as
            // tracks they stood still with no squad (3.133).
            const uint32_t e = placement.entity.value;
            if (e == 0x80ACA699 || e == 0x80ACA69F || e == 0x80ACA6A4) continue;
            const bool hidden = map.hidden_nodes.contains(placement.node);
            const bool lockdown = placement.entity.value == 0x80B08991 || placement.entity.value == 0x80B0D412;
            if (placement.model.is_none()) {
                if (!lockdown) continue;
                const auto models = entity_dynamic_models(pm, placement.entity, sources);
                if (models.size() != 1) continue;
                placement.model = models.front();
            }
            const bool payload = !group.route.empty() &&
                (placement.entity.value == 0x80AC9EEB || placement.entity.value == 0x80B0F90C);
            const auto [it, fresh] = resources.try_emplace({placement.entity, placement.model});
            auto& r = it->second;
            if (fresh) {
                try {
                    auto mesh = prepare_entity_preview(pm, placement.model, sources.materials, sources.assemblies);
                    if (!mesh.no_drawable) {
                        decltype(prepare_preview_animation(pm, mesh, sources.rigs, sources.clips, placement.model)) binding;
                        if (sources.rigs.contains(placement.model.value) && !payload)
                            binding = prepare_preview_animation(pm, mesh, sources.rigs, sources.clips, placement.model);
                        if (binding && binding->model == placement.model && binding->clips.size() == 1) {
                            const auto& info = binding->clips.front();
                            if (placement.model.value != 0x80B580E0 || info.tag.value == 0x80B580F4) {
                                auto clip = load_animation_clip(pm, info.tag);
                                restrict_preview_clip(clip, info);
                                r.clip = std::make_shared<const AnimationClip>(std::move(clip));
                                r.binding = std::make_shared<const PreviewAnimation>(std::move(*binding));
                            }
                        }
                        r.mesh = std::make_shared<const PreparedMesh>(std::move(mesh));
                        r.sounds = std::make_shared<const EntitySounds>(load_entity_sounds(pm, std::span(&placement.entity, 1)));
                    }
                } catch (const std::exception&) {
                    // A candidate that fails resolution retains its ordinary placement.
                    r = {};
                }
            }
            if (!r.mesh) continue;
            const bool animated = r.clip && r.clip->frame_count;
            // A visible model with nothing to play stays with the map's ordinary entities.
            if (!hidden && !animated && !payload) continue;
            PreparedEventTrack track{group.id, !hidden, placement, r.mesh, animated ? r.binding : nullptr,
                                     animated ? r.clip : nullptr, r.sounds};
            if (payload) {
                const auto& rows = placement.rows;
                const std::array start{rows[3], rows[7], rows[11]};
                const auto distance = [](const auto& a, const auto& b) {
                    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
                };
                const auto first = std::ranges::min_element(group.route, {}, [&](const auto& point) { return distance(point, start); });
                track.route.push_back({0, start});
                for (auto point = first; point != group.route.end(); ++point) {
                    const auto& last = track.route.back();
                    const float segment = distance(last.position, *point);
                    if (std::isfinite(segment) && segment > 0.01f) track.route.push_back({last.distance + segment, *point});
                }
                if (track.route.size() < 2) continue;
            }
            if (!map.event_actor_models.try_emplace(placement.node, placement.model).second) continue;
            tracks.push_back(std::move(track));
        }
    }
    return tracks;
}

void EventPlayback::build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
                          std::shared_ptr<const std::vector<PreparedEventTrack>> prepared) {
    prepared_ = std::move(prepared);
    slots_.clear();
    active_ = 0;
    if (!prepared_) return;
    // Only one event plays at a time, so the k-th copy of a mesh in every event shares one actor
    // (83 Outpost dropship events need one ship). Tracks drawn while idle keep their own.
    std::map<std::pair<const PreparedMesh*, size_t>, uint32_t> shared;
    std::map<std::pair<uint64_t, const PreparedMesh*>, size_t> copies;
    for (size_t i = 0; i < prepared_->size(); ++i) {
        const auto& track = (*prepared_)[i];
        const PreparedMesh* mesh = track.mesh.get();
        if (track.idle_visible) {
            slots_.push_back({scene.add_actor(gpu, *mesh, layouts), i});
            continue;
        }
        const size_t k = copies[{track.event, mesh}]++;
        const auto [it, fresh] = shared.try_emplace({mesh, k}, 0u);
        if (fresh) it->second = scene.add_actor(gpu, *mesh, layouts);
        slots_.push_back({it->second, i});
    }
    actors_ = shared.size() + size_t(std::ranges::count(*prepared_, true, &PreparedEventTrack::idle_visible));
    previous_frames_.assign(slots_.size(), -1);
    idle(scene);
}

size_t EventPlayback::tracks(uint64_t event) const {
    if (!prepared_ || !event) return 0;
    return size_t(std::ranges::count(*prepared_, event, &PreparedEventTrack::event));
}

void EventPlayback::idle(StaticMeshScene& scene) {
    sounds_.clear();
    emitters_.clear();
    for (const auto& slot : slots_)
        if (!(*prepared_)[slot.track].idle_visible) scene.set_actor(slot.actor, std::nullopt);
    for (const auto& slot : slots_)
        if (const auto& track = (*prepared_)[slot.track]; track.idle_visible) scene.set_actor(slot.actor, track.placement.rows);
}

void EventPlayback::play(StaticMeshScene& scene, uint64_t event) {
    idle(scene);
    active_ = event;
    seconds_ = 0;
    std::ranges::fill(previous_frames_, -1.0f);
}

void EventPlayback::stop(StaticMeshScene& scene) {
    active_ = 0;
    idle(scene);
}

void EventPlayback::step(StaticMeshScene& scene, float dt) {
    sounds_.clear();
    emitters_.clear();
    if (!active_) return;
    seconds_ += std::clamp(dt, 0.0f, 0.1f);
    for (size_t i = 0; i < slots_.size(); ++i) {
        const auto& slot = slots_[i];
        const auto& track = (*prepared_)[slot.track];
        if (track.event != active_) continue;
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
        if (track.placement.model.value == 0x80B580E0 && !poses.empty() && poses.front().bones.size() > 1 &&
            track.binding->skeleton.bind.size() > 1) {
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
