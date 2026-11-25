#pragma once
#include "nim/render/ai_actors.hpp"

namespace nim {

struct PreparedEventTrack {
    struct RoutePoint {
        float distance;
        std::array<float, 3> position;
    };
    MapEventPlacement placement;
    std::shared_ptr<const PreparedMesh> mesh;
    std::shared_ptr<const PreviewAnimation> binding;
    std::shared_ptr<const AnimationClip> clip;
    std::shared_ptr<const EntitySounds> sounds;
    std::vector<RoutePoint> route;
};

std::vector<PreparedEventTrack> prepare_event_tracks(PackageManager& pm, MapLayout& map);

class EventPlayback {
public:
    static constexpr float frames_per_second = 30;  // viewer estimate, as for normal actor clips
    void build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
               std::shared_ptr<const std::vector<PreparedEventTrack>> prepared);
    void play();
    void stop(StaticMeshScene& scene);
    void step(StaticMeshScene& scene, float dt);
    size_t size() const { return slots_.size(); }
    const std::vector<ActorSound>& sounds() const { return sounds_; }
    const std::vector<std::pair<uint64_t, std::array<float, 3>>>& emitters() const { return emitters_; }

private:
    struct Slot { uint32_t actor; size_t track; };
    std::shared_ptr<const std::vector<PreparedEventTrack>> prepared_;
    std::vector<Slot> slots_;
    std::vector<ActorSound> sounds_;
    std::vector<std::pair<uint64_t, std::array<float, 3>>> emitters_;
    std::vector<float> previous_frames_;
    float seconds_ = 0;
};

}  // namespace nim
