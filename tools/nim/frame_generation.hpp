#pragma once

#include "nim/render/scene.hpp"
#include <memory>

namespace nim {

class FrameGeneration {
public:
    enum class Marker { simulation_start, simulation_end, render_start, render_end, present_start, present_end };

    FrameGeneration();
    ~FrameGeneration();
    void init();
    ComPtr<ID3D12Device> attach(ID3D12Device* device);
    bool upgrade_factory(ComPtr<IDXGIFactory6>& factory);
    bool supported() const;
    bool built() const;
    void set_active(bool active);
    void begin_frame(uint32_t index);
    void mark(Marker marker);
    void tag(ID3D12GraphicsCommandList* cl, const GBuffer& gb, const View& camera,
             const ViewMatrices& vm, uint32_t x, uint32_t y);
    void after_present();
    void reset_history();
    void shutdown();
    unsigned presents_per_frame() const;
    const std::string& status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace nim
