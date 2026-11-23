#pragma once

#include "nim/render/gpu.hpp"
#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace nim {

enum class DlssMode : uint8_t {
    Off = 0,
    DLAA = 1,
    Quality = 2,
    Balanced = 3,
    Performance = 4,
    UltraPerformance = 5,
};

class Dlss {
public:
    Dlss() = default;
    Dlss(const Dlss&) = delete;
    Dlss& operator=(const Dlss&) = delete;
    ~Dlss();

    static bool built();
    static const char* mode_name(DlssMode mode);
    static std::pair<uint32_t, uint32_t> render_size(uint32_t out_w, uint32_t out_h, DlssMode mode);

    const std::string& status() const { return status_; }
    bool prepare(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t in_w, uint32_t in_h,
                 uint32_t out_w, uint32_t out_h, DlssMode mode);
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
    uint32_t in_w_ = 0, in_h_ = 0, out_w_ = 0, out_h_ = 0;
    DlssMode mode_ = DlssMode::Off;
    bool attempted_ = false;
    bool supported_ = false;
    bool creation_failed_ = false;
    bool history_ = false;
};

} // namespace nim
