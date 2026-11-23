#include "nim/render/dlss.hpp"

#if NIM_WITH_DLSS
#include <nvsdk_ngx_helpers_d3d.h>
#include <filesystem>
#include <format>
#endif

namespace nim {

Dlss::~Dlss() { shutdown(); }
bool Dlss::built() { return NIM_WITH_DLSS != 0; }

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

bool Dlss::prepare(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t width, uint32_t height) {
#if NIM_WITH_DLSS
    if (!attempted_) {
        attempted_ = true;
        std::error_code ec;
        auto data = std::filesystem::temp_directory_path(ec);
        if (!ec) {
            data /= L"nim-ngx";
            std::filesystem::create_directories(data, ec);
        }
        if (ec) {
            status_ = "DLAA could not create its NGX data directory";
            return false;
        }
        const auto init = NVSDK_NGX_D3D12_Init_with_ProjectID(
            "aedc444d-de0e-4c32-845e-3ecdf4c643be", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "nim 0.3",
            data.c_str(), gpu.device());
        if (NVSDK_NGX_FAILED(init)) {
            status_ = std::format("DLAA NGX initialization failed (0x{:08X})", unsigned(init));
            return false;
        }
        device_ = gpu.device();
        int available = 0;
        if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters_)) ||
            NVSDK_NGX_FAILED(parameters_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available)) || !available) {
            status_ = "DLAA is unavailable on this GPU, driver or SDK runtime";
            return false;
        }
        supported_ = true;
    }
    if (!supported_ || creation_failed_) return false;
    // NGX has a minimum input size. Keep native TAA for small viewports.
    if (width < 256 || height < 144) return false;
    if (feature_) return true;
    NVSDK_NGX_DLSS_Create_Params params{};
    params.Feature.InWidth = params.Feature.InTargetWidth = width;
    params.Feature.InHeight = params.Feature.InTargetHeight = height;
    params.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    params.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    const auto result = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &feature_, parameters_, &params);
    if (NVSDK_NGX_FAILED(result)) {
        creation_failed_ = true;
        status_ = std::format("DLAA creation failed (0x{:08X})", unsigned(result));
        return false;
    }
    output_ = gpu.texture2d(width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
    return true;
#else
    (void)gpu; (void)cl; (void)width; (void)height;
    status_ = "This build has no NVIDIA NGX SDK";
    return false;
#endif
}

ID3D12Resource* Dlss::evaluate(ID3D12GraphicsCommandList* cl, ID3D12Resource* color, ID3D12Resource* depth,
                              ID3D12Resource* motion, float jitter_x, float jitter_y, float pre_exposure,
                              float delta_seconds, bool reset) {
#if NIM_WITH_DLSS
    const auto size = output_->GetDesc();
    NVSDK_NGX_D3D12_DLSS_Eval_Params params{};
    params.Feature.pInColor = color;
    params.Feature.pInOutput = output_.Get();
    params.pInDepth = depth;
    params.pInMotionVectors = motion;
    params.InJitterOffsetX = jitter_x;
    params.InJitterOffsetY = jitter_y;
    params.InRenderSubrectDimensions = {unsigned(size.Width), size.Height};
    params.InMVScaleX = float(size.Width);
    params.InMVScaleY = float(size.Height); // Tiger RT3 already stores previous minus current in screen UVs.
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
    status_ = history_ ? "NVIDIA DLAA active" : std::format("DLAA evaluation failed (0x{:08X})", unsigned(result));
    return history_ ? output_.Get() : nullptr;
#else
    (void)cl; (void)color; (void)depth; (void)motion; (void)jitter_x; (void)jitter_y;
    (void)pre_exposure; (void)delta_seconds; (void)reset;
    return nullptr;
#endif
}
} // namespace nim
