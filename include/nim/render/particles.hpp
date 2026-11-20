#pragma once
// GPU particle systems (FX) placed in a map, simulated and drawn with the game's own compute and
// draw techniques (808081E8). The engine side - when to spawn, the pools, the per-system extern
// values - is reconstructed from the shaders; every value no file holds is listed as an estimate.
#include "nim/content/map.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/render/gpu.hpp"
#include "nim/render/scene.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nim {

class ParticleScene {
public:
    ParticleScene();
    ~ParticleScene();
    // Loads the systems' pipelines and the pools. Systems whose techniques cannot be built are
    // left out and listed in notes().
    void load(PackageManager& pm, Gpu& gpu, std::span<const MapParticle> placements);
    void release(Gpu& gpu);  // caller makes sure the GPU is idle
    bool loaded() const { return !emitters_.empty(); }
    bool has_emitters_near(V3 eye) const;

    // Steps the emitters within `radius` of the eye by `dt` seconds (an emitter coming into range
    // is warmed over several frames) and draws them into the HDR target `hdr`
    // against the G-buffer depth (DEPTH_READ). Call from GameLighting's transparent callback,
    // after the scene's own transparents: `inputs` are that frame's externs.
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, D3D12_CPU_DESCRIPTOR_HANDLE hdr, const ViewMatrices& vm,
                const ExternStore& inputs, float dt);

    float radius = 100;  // viewer choice: emitters farther than this are not simulated (60 fps budget, §3.35)

    size_t system_count() const { return systems_.size(); }
    size_t emitter_count() const { return emitters_.size(); }
    size_t active_count() const { return active_; }
    size_t particle_capacity() const { return total_; }
    const std::vector<std::string>& notes() const { return notes_; }
    // Diagnostics: the first simulated emitter's state and instance records (flushes the GPU).
    std::string debug_dump(Gpu& gpu);
    const std::map<std::string, std::string>& estimates() const;

private:
    struct System;
    struct Emitter {
        uint32_t system = 0;
        std::array<float, 12> rows{};
        V3 position{};
        uint32_t offset = 0, capacity = 0;  // particles, in the shared pools
        uint32_t state_base = 0;  // float4 offset in the packed state pool
        uint32_t instance_base = 0;         // float4s, in the instance pool (the system's own record size)
        uint32_t head = 0;                  // next slot to spawn into
        uint64_t spawned = 0;
        float carry = 0;
        uint32_t seed = 0;
        uint32_t warm_steps = 0;
        bool active = false;
    };
    template <class F>
    void guard(System& s, F&& f);
    void step(Gpu& gpu, ID3D12GraphicsCommandList* cl, const std::vector<uint32_t>& list, const std::vector<float>& dts,
              const ScopeBufferFn& scopes, std::span<const D3D12_GPU_VIRTUAL_ADDRESS> collision_views);
    std::vector<std::unique_ptr<System>> systems_;
    std::vector<Emitter> emitters_;
    std::unique_ptr<ExternStore> ext_;
    ComPtr<ID3D12Resource> state_[2], history_, instances_, object_constants_;
    uint32_t parity_ = 0;  // state_[parity_] holds the current particles
    uint32_t total_ = 0, state_total_ = 0, instance_total_ = 0;  // slots / pool float4s
    size_t active_ = 0;
    std::unique_ptr<Scope> view_scope_, transparent_scope_, grid_scope_;
    std::vector<std::string> notes_;
    size_t gated_ = 0;  // placements left out: their sequence waits on an unset exfil activation
};

}  // namespace nim
