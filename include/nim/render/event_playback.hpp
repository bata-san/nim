#pragma once
#include "nim/render/ai_actors.hpp"

namespace nim {

// One model of a palette event (3.132 / 3.133). Every event of the map is prepared with the map;
// selecting one in the palette shows and plays it without reloading.
struct PreparedEventTrack {
    struct RoutePoint {
        float distance;
        std::array<float, 3> position;
    };
    uint64_t event = 0;         // MapEventGroup::id
    bool idle_visible = false;  // drawn at its placement while its event does not play
    MapEventPlacement placement;
    std::shared_ptr<const PreparedMesh> mesh;
    std::shared_ptr<const PreviewAnimation> binding;  // null with no clip
    std::shared_ptr<const AnimationClip> clip;        // null: shown still at its placement
    std::shared_ptr<const EntitySounds> sounds;
    std::vector<RoutePoint> route;
};

// Tracks for every event group of `map`. Records their nodes in map.event_actor_models, so
// load_map_entities leaves those models to the tracks. Dropship events get none: the enemy AI's
// spawning plays them with their squads (3.133).
std::vector<PreparedEventTrack> prepare_event_tracks(PackageManager& pm, MapLayout& map);

class EventPlayback {
public:
    static constexpr float frames_per_second = 30;  // viewer estimate, as for normal actor clips
    // Adds one actor per track. The caller makes sure the GPU is idle (actor buffers grow).
    void build(Gpu& gpu, StaticMeshScene& scene, const std::vector<std::vector<InputElement>>& layouts,
               std::shared_ptr<const std::vector<PreparedEventTrack>> prepared);
    void play(StaticMeshScene& scene, uint64_t event);  // restarts `event`; every other event idles
    void stop(StaticMeshScene& scene);                    // every event idles
    void step(StaticMeshScene& scene, float dt);
    uint64_t active() const { return active_; }
    size_t size() const { return slots_.size(); }    // tracks
    size_t actors() const { return actors_; }         // scene actors they share
    size_t tracks(uint64_t event) const;
    const std::vector<ActorSound>& sounds() const { return sounds_; }
    const std::vector<std::pair<uint64_t, std::array<float, 3>>>& emitters() const { return emitters_; }

private:
    struct Slot { uint32_t actor; size_t track; };
    void idle(StaticMeshScene& scene);
    std::shared_ptr<const std::vector<PreparedEventTrack>> prepared_;
    std::vector<Slot> slots_;
    std::vector<ActorSound> sounds_;
    std::vector<std::pair<uint64_t, std::array<float, 3>>> emitters_;
    std::vector<float> previous_frames_;
    uint64_t active_ = 0;
    size_t actors_ = 0;
    float seconds_ = 0;
};

}  // namespace nim
