#include "nim/render/dlss.hpp"

#if NIM_WITH_DLSS
#include <nvsdk_ngx_helpers_d3d.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#else
#include <algorithm>
#include <cmath>
#endif

namespace nim {

Dlss::~Dlss() { shutdown(); }
bool Dlss::built() { return NIM_WITH_DLSS != 0; }

const char* Dlss::mode_name(DlssMode mode) {
    switch (mode) {
    case DlssMode::DLAA: return "DLAA";
    case DlssMode::Quality: return "Quality";
    case DlssMode::Balanced: return "Balanced";
    case DlssMode::Performance: return "Performance";
    case DlssMode::UltraPerformance: return "Ultra Performance";
    default: return "Off";
    }
}

std::pair<uint32_t, uint32_t> Dlss::render_size(uint32_t out_w, uint32_t out_h, DlssMode mode) {
    float scale = 1.0f;
    switch (mode) {
    case DlssMode::Quality: scale = 1.0f / 1.5f; break;
    case DlssMode::Balanced: scale = 1.0f / 1.724f; break;
    case DlssMode::Performance: scale = 0.5f; break;
    case DlssMode::UltraPerformance: scale = 1.0f / 3.0f; break;
    default: return {out_w, out_h};
    }
    if (out_w < 256 || out_h < 144) return {out_w, out_h};
    auto round_even = [](uint32_t v, uint32_t lo, uint32_t hi) {
        v = std::clamp(v, lo, hi);
        return (v & ~1u) ? (v & ~1u) : v;
    };
    const uint32_t rw = round_even(uint32_t(std::lround(float(out_w) * scale)), 256u, out_w);
    const uint32_t rh = round_even(uint32_t(std::lround(float(out_h) * scale)), 144u, out_h);
    return {rw, rh};
}

void Dlss::resize(Gpu& gpu) {
#if NIM_WITH_DLSS
    if (feature_) {
        gpu.wait_idle();
        NVSDK_NGX_D3D12_ReleaseFeature(feature_);
        feature_ = nullptr;
    }
#else
    (void)gpu;
#endif
    output_.Reset();
    in_w_ = in_h_ = out_w_ = out_h_ = 0;
    mode_ = DlssMode::Off;
    history_ = false;
    creation_failed_ = false;
}

void Dlss::shutdown() {
#if NIM_WITH_DLSS
    if (feature_) NVSDK_NGX_D3D12_ReleaseFeature(feature_);
    if (parameters_) NVSDK_NGX_D3D12_DestroyParameters(parameters_);
    if (device_) NVSDK_NGX_D3D12_Shutdown1(device_.Get());
#endif
    feature_ = nullptr;
    parameters_ = nullptr;
    device_.Reset();
    attempted_ = false;
    supported_ = false;
}

void Dlss::release(Gpu& gpu) {
    resize(gpu);
    shutdown();
    status_.clear();
}

bool Dlss::prepare(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t in_w, uint32_t in_h,
                   uint32_t out_w, uint32_t out_h, DlssMode mode) {
#if NIM_WITH_DLSS
    if (mode == DlssMode::Off) return false;
    if (!attempted_) {
        attempted_ = true;
        std::error_code ec;
        auto data = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            data /= L"nim-ngx";
            std::filesystem::create_directories(data, ec);
        }
        if (ec) {
            status_ = "DLSS could not create its NGX data directory";
            return false;
        }
        const auto init = NVSDK_NGX_D3D12_Init_with_ProjectID(
            "aedc444d-de0e-4c32-845e-3ecdf4c643be", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "nim 0.3",
            data.c_str(), gpu.device());
        if (NVSDK_NGX_FAILED(init)) {
            status_ = std::format("DLSS NGX initialization failed (0x{:08X})", unsigned(init));
            return false;
        }
        device_ = gpu.device();
        int available = 0;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_)) ||
            NVSDK_NGX_FAILED(parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
            status_ = "DLSS is unavailable on this GPU, driver or SDK runtime";
            return false;
        }
        supported_ = true;
    }
    if (!supported_) return false;
    if (in_w < 256 || in_h < 144 || out_w < in_w || out_h < in_h) return false;
    if (feature_ && in_w == in_w_ && in_h == in_h_ && out_w == out_w_ && out_h == out_h_ && mode == mode_) return true;
    if (feature_) resize(gpu);
    if (creation_failed_) return false;
    auto perf_quality = [](DlssMode m) {
        switch (m) {
        case DlssMode::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case DlssMode::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssMode::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssMode::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default: return NVSDK_NGX_PerfQuality_Value_DLAA;
        }
    };
    NVSDK_NGX_DLSS_Create_Params params{};
    params.Feature.InWidth = in_w;
    params.Feature.InHeight = in_h;
    params.Feature.InTargetWidth = out_w;
    params.Feature.InTargetHeight = out_h;
    params.Feature.InPerfQualityValue = perf_quality(mode);
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const auto result = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &feature_, parameters_, &params);
    if (NVSDK_NGX_FAILED(result)) {
        creation_failed_ = true;
        status_ = std::format("DLSS {} creation failed (0x{:08X})", mode_name(mode), unsigned(result));
        return false;
    }
    in_w_ = in_w;
    in_h_ = in_h;
    out_w_ = out_w;
    out_h_ = out_h;
    mode_ = mode;
    output_ = gpu.texture2d(out_w, out_h, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
    return true;
#else
    (void)gpu; (void)cl; (void)in_w; (void)in_h; (void)out_w; (void)out_h; (void)mode;
    status_ = "This build has no NVIDIA NGX SDK";
    return false;
#endif
}

ID3D12Resource* Dlss::evaluate(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* depth,
                              ID3D12Resource* motion, float jitter_x, float jitter_y, float pre_exposure,
                              float delta_seconds, bool reset) {
#if NIM_WITH_DLSS
    NVSDK_NGX_D3D12_DLSS_Eval_Params params{};
    params.Feature.pInColor = color;
    params.Feature.pInOutput = output_.Get();
    params.pInDepth = depth;
    params.pInMotionVectors = motion;
    params.InJitterOffsetX = jitter_x;
    params.InJitterOffsetY = jitter_y;
    params.InRenderSubrectDimensions = {in_w_, in_h_};
    params.InMVScaleX = float(in_w_);
    params.InMVScaleY = float(in_h_); // Tiger RT3 already stores previous minus current in screen UVs.
    params.InPreExposure = pre_exposure;
    params.InFrameTimeDeltaInMsec = delta_seconds * 1000;
    params.InReset = reset || !history_;
    constexpr auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto compute = read | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Gpu::barrier(cl, color, read, compute);
    Gpu::barrier(cl, depth, read, compute);
    Gpu::barrier(cl, motion, read, compute);
    Gpu::barrier(cl, output_.Get(), read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto result = NGX_D3D12_EVALUATE_DLSS_EXT(cl, feature_, parameters_, &params);
    Gpu::barrier(cl, output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, read);
    Gpu::barrier(cl, motion, compute, read);
    Gpu::barrier(cl, depth, compute, read);
    Gpu::barrier(cl, color, compute, read);
    history_ = NVSDK_NGX_SUCCEED(result);
    if (history_) {
        status_ = (in_w_ == out_w_ && in_h_ == out_h_)
            ? std::format("NVIDIA DLAA active ({}x{})", out_w_, out_h_)
            : std::format("NVIDIA DLSS {} active ({}x{} -> {}x{})", mode_name(mode_), in_w_, in_h_, out_w_, out_h_);
    } else {
        status_ = std::format("DLSS {} evaluation failed (0x{:08X})", mode_name(mode_), unsigned(result));
    }
    return history_ ? output_.Get() : nullptr;
#else
    (void)cl; (void)color; (void)depth; (void)motion; (void)jitter_x; (void)jitter_y;
    (void)pre_exposure; (void)delta_seconds; (void)reset;
    return nullptr;
#endif
}
} // namespace nim
