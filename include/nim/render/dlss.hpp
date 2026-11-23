#pragma once

#include "nim/render/gpu.hpp"
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace nim {

class Dlss {
public:
    Dlss() = default;
    Dlss(const Dlss&) = delete;
    Dlss& operator=(const Dlss&) = delete;
    ~Dlss();

    static bool built();
    const std::string& status() const { return status_; }
    bool prepare(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t width, uint32_t height);
    ID3D12Resource* evaluate(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* depth,
                             ID3D12Resource* motion, float jitter_x, float jitter_y, float pre_exposure,
                             float delta_seconds, bool reset);
    void reset_history() { history_ = false; }
    void resize(Gpu& gpu);
    void release(Gpu& gpu);

private:
    void shutdown();
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12Resource> output_;
    NVSDK_NGX_Handle* feature_ = nullptr;
    NVSDK_NGX_Parameter* parameters_ = nullptr;
    std::string status_;
    bool attempted_ = false;
    bool supported_ = false;
    bool creation_failed_ = false;
    bool history_ = false;
};

} // namespace nim
