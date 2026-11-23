#include "frame_generation.hpp"

#if NIM_WITH_STREAMLINE
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
#include <DirectXMath.h>
#include <cstring>
#include <filesystem>
#include <format>
#endif

namespace nim {

struct FrameGeneration::Impl {
    std::string status;
    unsigned presents = 1;
#if NIM_WITH_STREAMLINE
    HMODULE module = nullptr;
    bool initialized = false, available = false, active = false;
    sl::FrameToken* token = nullptr;
    const sl::ViewportHandle viewport{0};
    sl::DLSSGState state{};
    std::optional<ViewMatrices> previous;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slSetD3DDevice* set_device = nullptr;
    PFun_slIsFeatureSupported* is_supported = nullptr;
    PFun_slGetFeatureFunction* feature_function = nullptr;
    PFun_slGetNewFrameToken* start_frame = nullptr;
    PFun_slSetConstants* constants = nullptr;
    PFun_slSetTagForFrame* tags = nullptr;
    PFun_slDLSSGSetOptions* set_options = nullptr;
    PFun_slDLSSGGetState* get_state = nullptr;
    PFun_slReflexSetOptions* reflex_options = nullptr;
    PFun_slReflexSleep* sleep = nullptr;
    PFun_slPCLSetMarker* mark = nullptr;

    template<class T>
    T* load(const char* name) { return reinterpret_cast<T*>(GetProcAddress(module, name)); }

    template<class T>
    T* feature(sl::Feature id, const char* name) {
        void* value = nullptr;
        return feature_function(id, name, value) == sl::Result::eOk ? reinterpret_cast<T*>(value) : nullptr;
    }

    template<class T>
    bool upgrade_com(ComPtr<T>& ptr) {
        // Streamline consumes the interface reference if it substitutes a proxy.
        auto* value = ptr.Detach();
        const auto result = upgrade(reinterpret_cast<void**>(&value));
        ptr.Attach(value);
        return result == sl::Result::eOk;
    }

    void failed(const char* action, sl::Result result) {
        status = std::format("DLSS Frame Generation: {} ({})", action, int(result));
    }
#endif
};

FrameGeneration::FrameGeneration() : impl_(std::make_unique<Impl>()) {}
FrameGeneration::~FrameGeneration() {
    shutdown();
#if NIM_WITH_STREAMLINE
    if (impl_->module) FreeLibrary(impl_->module);
#endif
}
bool FrameGeneration::built() const { return NIM_WITH_STREAMLINE != 0; }
const std::string& FrameGeneration::status() const { return impl_->status; }
unsigned FrameGeneration::presents_per_frame() const { return impl_->presents; }

void FrameGeneration::init() {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    wchar_t exe[32768]{};
    const auto length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!length || length >= std::size(exe)) return;
    const auto dir = std::filesystem::path(exe).parent_path();
    const auto path = dir / L"sl.interposer.dll";
    if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
        s.status = "DLSS Frame Generation: signed Streamline runtime unavailable";
        return;
    }
    s.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!s.module) return;
    const auto init = s.load<PFun_slInit>("slInit");
    s.shutdown = s.load<PFun_slShutdown>("slShutdown");
    s.upgrade = s.load<PFun_slUpgradeInterface>("slUpgradeInterface");
    s.set_device = s.load<PFun_slSetD3DDevice>("slSetD3DDevice");
    s.is_supported = s.load<PFun_slIsFeatureSupported>("slIsFeatureSupported");
    s.feature_function = s.load<PFun_slGetFeatureFunction>("slGetFeatureFunction");
    s.start_frame = s.load<PFun_slGetNewFrameToken>("slGetNewFrameToken");
    s.constants = s.load<PFun_slSetConstants>("slSetConstants");
    s.tags = s.load<PFun_slSetTagForFrame>("slSetTagForFrame");
    if (!init || !s.shutdown || !s.upgrade || !s.set_device || !s.is_supported ||
        !s.feature_function || !s.start_frame || !s.constants || !s.tags) {
        s.status = "DLSS Frame Generation: incomplete Streamline runtime";
        return;
    }
    const sl::Feature requested[]{sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const wchar_t* paths[]{dir.c_str()};
    sl::Preferences prefs{};
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                  sl::PreferenceFlags::eUseDXGIFactoryProxy | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    prefs.engine = sl::EngineType::eCustom;
    prefs.engineVersion = "nim 0.3";
    prefs.projectId = "aedc444d-de0e-4c32-845e-3ecdf4c643be";
    prefs.pathsToPlugins = paths;
    prefs.numPathsToPlugins = 1;
    prefs.featuresToLoad = requested;
    prefs.numFeaturesToLoad = uint32_t(std::size(requested));
    const auto result = init(prefs, sl::kSDKVersion);
    s.initialized = result == sl::Result::eOk;
    if (!s.initialized) s.failed("initialization failed", result);
#endif
}

ComPtr<ID3D12Device> FrameGeneration::attach(ID3D12Device* device) {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (!s.initialized) return {};
    auto result = s.set_device(device);
    if (result != sl::Result::eOk) {
        s.failed("device registration failed", result);
        return {};
    }
    LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    adapter.deviceLUIDSizeInBytes = sizeof(luid);
    result = s.is_supported(sl::kFeatureDLSS_G, adapter);
    if (result != sl::Result::eOk) {
        s.failed("unavailable on this adapter or OS configuration", result);
        return {};
    }
    s.set_options = s.feature<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
    s.get_state = s.feature<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState");
    s.reflex_options = s.feature<PFun_slReflexSetOptions>(sl::kFeatureReflex, "slReflexSetOptions");
    s.sleep = s.feature<PFun_slReflexSleep>(sl::kFeatureReflex, "slReflexSleep");
    s.mark = s.feature<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker");
    if (!s.set_options || !s.get_state || !s.reflex_options || !s.sleep || !s.mark) {
        s.status = "DLSS Frame Generation: Reflex or Frame Generation plugin unavailable";
        return {};
    }
    ComPtr<ID3D12Device> proxy = device;
    if (!s.upgrade_com(proxy)) return {};
    s.available = true;
    s.status.clear();
    return proxy;
#else
    (void)device;
    return {};
#endif
}

bool FrameGeneration::initialize_device(ID3D12Device* device) {
#if NIM_WITH_STREAMLINE
    if (!impl_->initialized) init();
    if (impl_->initialized && !impl_->available) attach(device);
    return impl_->available;
#else
    (void)device;
    return false;
#endif
}

bool FrameGeneration::supported() const {
#if NIM_WITH_STREAMLINE
    return impl_->available;
#else
    return false;
#endif
}

bool FrameGeneration::upgrade_factory(ComPtr<IDXGIFactory6>& factory) {
#if NIM_WITH_STREAMLINE
    return supported() && impl_->upgrade_com(factory);
#else
    (void)factory;
    return false;
#endif
}

void FrameGeneration::set_active(bool active) {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (!s.available || active == s.active) return;
    sl::ReflexOptions reflex{};
    reflex.mode = active ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    if (s.reflex_options(reflex) != sl::Result::eOk && active) return;
    sl::DLSSGOptions options{};
    options.mode = active ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    const auto result = s.set_options(s.viewport, options);
    if (result == sl::Result::eOk) {
        s.active = active;
        s.presents = 1;
        s.previous.reset();
    } else {
        s.failed("mode change failed", result);
    }
#else
    (void)active;
#endif
}

void FrameGeneration::begin_frame(uint32_t index) {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    s.token = nullptr;
    if (s.active && s.start_frame(s.token, &index) == sl::Result::eOk && s.token)
        s.sleep(*s.token);
#else
    (void)index;
#endif
}

void FrameGeneration::mark(Marker marker) {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (s.active && s.token) s.mark(static_cast<sl::PCLMarker>(marker), *s.token);
#else
    (void)marker;
#endif
}

void FrameGeneration::reset_history() {
#if NIM_WITH_STREAMLINE
    impl_->previous.reset();
#endif
}

void FrameGeneration::tag(ID3D12GraphicsCommandList* cl, const GBuffer& gb, const View& camera,
                          const ViewMatrices& vm, uint32_t x, uint32_t y) {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (!s.active || !s.token) return;
    using namespace DirectX;
    auto matrix = [](const M4& input) {
        XMFLOAT4X4 rows;
        std::memcpy(&rows, &input, sizeof(rows)); // M4 columns are the rows of the equivalent row-vector matrix.
        return XMLoadFloat4x4(&rows);
    };
    auto set_matrix = [](sl::float4x4& dest, FXMMATRIX src) {
        XMFLOAT4X4 rows;
        XMStoreFloat4x4(&rows, src);
        std::memcpy(&dest, &rows, sizeof(rows));
    };
    const ViewMatrices current = make_view(camera);
    const auto cur = matrix(current.vp_abs);
    const auto prev = matrix(s.previous ? s.previous->vp_abs : current.vp_abs);
    const auto to_prev = XMMatrixMultiply(XMMatrixInverse(nullptr, cur), prev);
    const auto projection = matrix(current.proj);
    sl::Constants constants{};
    set_matrix(constants.cameraViewToClip, projection);
    set_matrix(constants.clipToCameraView, XMMatrixInverse(nullptr, projection));
    set_matrix(constants.clipToPrevClip, to_prev);
    set_matrix(constants.prevClipToClip, XMMatrixInverse(nullptr, to_prev));
    constants.jitterOffset = {vm.proj.c[2][0] * float(gb.width()) * 0.5f, -vm.proj.c[2][1] * float(gb.height()) * 0.5f};
    constants.mvecScale = {1, 1};  // RT3 stores previous minus current in UVs.
    constants.cameraPinholeOffset = {0, 0};
    constants.cameraPos = {current.eye.x, current.eye.y, current.eye.z};
    constants.cameraRight = {current.right.x, current.right.y, current.right.z};
    constants.cameraUp = {current.up.x, current.up.y, current.up.z};
    constants.cameraFwd = {current.fwd.x, current.fwd.y, current.fwd.z};
    constants.cameraNear = camera.near_z;
    constants.cameraFar = 1000000.0f;  // same finite clear-depth distance as the renderer
    constants.cameraFOV = camera.fov_y;
    constants.cameraAspectRatio = float(gb.width()) / float(gb.height());
    constants.depthInverted = sl::eTrue;
    constants.cameraMotionIncluded = sl::eTrue;
    constants.motionVectors3D = sl::eFalse;
    const V3 jump = s.previous ? current.eye - s.previous->eye : V3{};
    constants.reset = !s.previous || dot(jump, jump) > 32.0f * 32.0f ||
                      dot(current.fwd, s.previous->fwd) < 0.5f ? sl::eTrue : sl::eFalse;
    if (const auto result = s.constants(constants, *s.token, s.viewport); result != sl::Result::eOk) {
        s.failed("constants rejected", result);
        set_active(false);
        return;
    }

    const sl::Extent whole{0, 0, gb.width(), gb.height()};
    const sl::Extent subrect{y, x, gb.width(), gb.height()};
    sl::Resource depth{sl::ResourceType::eTex2d, gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE};
    sl::Resource motion{sl::ResourceType::eTex2d, gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET};
    sl::Resource hudless{sl::ResourceType::eTex2d, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    const sl::ResourceTag tags[]{
        {&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &whole},
        {&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &whole},
        {&hudless, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &whole},
        {nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &subrect},
    };
    if (const auto result = s.tags(*s.token, s.viewport, tags, uint32_t(std::size(tags)), cl);
        result != sl::Result::eOk) {
        s.failed("resource tags rejected", result);
        set_active(false);
    } else {
        s.previous = current;
    }
#else
    (void)cl; (void)gb; (void)camera; (void)vm; (void)x; (void)y;
#endif
}

void FrameGeneration::after_present() {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (s.active && s.get_state(s.viewport, s.state, nullptr) == sl::Result::eOk) {
        s.presents = std::max(1u, s.state.numFramesActuallyPresented);
        if (s.state.status != sl::DLSSGStatus::eOk)
            s.status = std::format("DLSS Frame Generation status 0x{:X}", uint32_t(s.state.status));
        else s.status.clear();
    }
    s.token = nullptr;
#endif
}

void FrameGeneration::shutdown() {
#if NIM_WITH_STREAMLINE
    auto& s = *impl_;
    if (s.initialized) s.shutdown();
    s.initialized = false;
    s.available = false;
    s.active = false;

#endif
}

} // namespace nim
