// Screen-space reflections: the engine's hi_z_generate_* and ssr_* compute pipelines, run as they
// are. See docs/PROGRESS.md 3.12 for the pass graph and how each input was read from the shaders.
// The engine-side settings (extern 0x6D "ssr" / 0x6F "ssr_trace" scalars) are not game data: they
// are explicit estimates, listed in the notes.
#include "nim/render/game_lighting.hpp"
#include <cmath>
#include <cstring>
#include <format>

namespace nim {
namespace {
constexpr auto kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint8_t kSsr = 0x6D, kTrace = 0x6F;

M4 inverse4(const M4& m) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m.c[c][r];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    for (int i = 0; i < 4; ++i) {
        int pivot = i;
        for (int r = i + 1; r < 4; ++r)
            if (std::fabs(a[r][i]) > std::fabs(a[pivot][i])) pivot = r;
        if (std::fabs(a[pivot][i]) < 1e-30) throw std::runtime_error("SSR: singular view matrix");
        if (pivot != i)
            for (int c = 0; c < 8; ++c) std::swap(a[i][c], a[pivot][c]);
        const double d = a[i][i];
        for (int c = 0; c < 8; ++c) a[i][c] /= d;
        for (int r = 0; r < 4; ++r) {
            if (r == i) continue;
            const double f = a[r][i];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[i][c];
        }
    }
    M4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.c[c][r] = float(a[r][c + 4]);
    return out;
}

}
M4 GameLighting::inverse_matrix(const M4& m) { return inverse4(m); }
namespace {
uint32_t groups(uint32_t n, uint32_t per) { return std::max(1u, (n + per - 1) / per); }

ComPtr<ID3D12Resource> make_texture(Gpu& gpu, uint32_t w, uint32_t h, uint32_t mips, DXGI_FORMAT format,
                                    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const char* what) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = UINT16(mips); rd.Format = format; rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> r;
    check_hr(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), what);
    return r;
}

ComPtr<ID3D12Resource> make_buffer(Gpu& gpu, uint64_t bytes, const char* what) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> r;
    check_hr(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kUav, nullptr, IID_PPV_ARGS(&r)), what);
    return r;
}

void uav_barrier(ID3D12GraphicsCommandList* cl) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &b);
}
}

void GameLighting::load_ssr(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes) {
    ssr_loaded_ = false;
    try {
        for (auto [pipeline, name] : {std::pair{&hiz_from_texture_, "hi_z_generate_from_texture"},
                                      std::pair{&hiz_from_target_, "hi_z_generate_from_target"},
                                      std::pair{&ssr_source_gen_, "ssr_generate_reflection_source_reproject"},
                                      std::pair{&ssr_init_, "ssr_initialize_radiance_buffer"},
                                      std::pair{&ssr_extract_, "ssr_raytrace_extract_rays"},
                                      std::pair{&ssr_args_, "ssr_compute_dispatch_args"},
                                      std::pair{&ssr_trace_, "ssr_trace_rays"},
                                      std::pair{&ssr_resolve_, "ssr_resolve_rays"},
                                      std::pair{&ssr_hole_, "ssr_hole_filling"},
                                      std::pair{&ssr_hblur_down_, "ssr_filter_horizontal_blur_downsample"},
                                      std::pair{&ssr_hblur_, "ssr_filter_horizontal_blur"},
                                      std::pair{&ssr_vblur_, "ssr_filter_vertical_blur"},
                                      std::pair{&ssr_apply_, "ssr_apply_reflection"}}) {
            const auto tag = globals_.pipeline(name);
            if (tag.is_none()) throw std::runtime_error(std::format("{} is not in render globals", name));
            pipeline->load(pm, gpu, tag, scopes, {});
        }
        const auto copy = globals_.pipeline("copy_texture");
        if (copy.is_none()) throw std::runtime_error("copy_texture is not in render globals");
        ssr_scale_history_.load(pm, gpu, copy, scopes, {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
        D3D12_INDIRECT_ARGUMENT_DESC arg{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        D3D12_COMMAND_SIGNATURE_DESC sd{};
        sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        check_hr(gpu.device()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&dispatch_sig_)), "dispatch command signature");
        ssr_loaded_ = true;
    } catch (const std::exception& ex) {
        shading_.notes.push_back(std::format("screen-space reflections not loaded: {}", ex.what()));
    }
}

void GameLighting::release_ssr(Gpu& gpu) {
    for (auto* p : {&hiz_from_texture_, &hiz_from_target_, &ssr_source_gen_, &ssr_init_, &ssr_extract_, &ssr_args_, &ssr_trace_,
                    &ssr_resolve_, &ssr_hole_, &ssr_hblur_down_, &ssr_hblur_, &ssr_vblur_, &ssr_apply_})
        p->release(gpu);
    for (auto* r : {std::addressof(hiz_), std::addressof(ssr_source_), std::addressof(ssr_radiance_), std::addressof(ssr_dir_),
                    std::addressof(ssr_color_), std::addressof(ssr_weight_), std::addressof(ssr_args_buf_),
                    std::addressof(ssr_rays_buf_), std::addressof(ssr_stencil_), std::addressof(ssr_history_)})
        r->Reset();
    ssr_scale_history_.release(gpu);
    ssr_copy_inputs_.reset();
    dispatch_sig_.Reset();
    ssr_w_ = ssr_h_ = 0;
    ssr_loaded_ = ssr_history_valid_ = false;
}

void GameLighting::resize_ssr(Gpu& gpu) {
    ssr_w_ = w_; ssr_h_ = h_;
    ssr_history_valid_ = false;
    const uint32_t hw = (w_ + 1) / 2, hh = (h_ + 1) / 2;
    // Hi-Z: mip 0 is half the view; hi_z_generate_from_texture writes four mips, _from_target four more.
    hiz_mips_ = (hw >> 7) && (hh >> 7) ? 8 : (hw >> 3) && (hh >> 3) ? 4 : 0;
    ssr_mips_ = 1;
    while (ssr_mips_ < 7 && (hw >> ssr_mips_) && (hh >> ssr_mips_)) ++ssr_mips_;
    if (!hiz_mips_) return;
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hiz_ = make_texture(gpu, hw, hh, hiz_mips_, DXGI_FORMAT_R32G32_FLOAT, uav, kRead, "hi-z");
    ssr_source_ = make_texture(gpu, hw, hh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kRead, "ssr reflection source");
    ssr_radiance_ = make_texture(gpu, hw, hh, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kUav, "ssr radiance");
    ssr_dir_ = make_texture(gpu, hw, hh, 1, DXGI_FORMAT_R32_UINT, uav, kUav, "ssr rays");
    ssr_color_ = make_texture(gpu, hw, hh, ssr_mips_, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kUav, "ssr reflection");
    ssr_weight_ = make_texture(gpu, hw, hh, ssr_mips_, DXGI_FORMAT_R16G16_FLOAT, uav, kUav, "ssr weight");
    // Committed resources start zeroed: the ray counter (word 3 of the arguments) and the stencil stand-in.
    ssr_args_buf_ = make_buffer(gpu, 16, "ssr dispatch arguments");
    ssr_rays_buf_ = make_buffer(gpu, 4ull * (uint64_t(hw) * hh + 64), "ssr ray list");
    ssr_stencil_ = make_texture(gpu, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOURCE_FLAG_NONE, kRead, "ssr stencil stand-in");
    ssr_history_ = make_texture(gpu, w_, h_, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_NONE, kRead, "ssr history");
}

void GameLighting::keep_ssr_history(ID3D12GraphicsCommandList* cl, const ViewMatrices& vm) {
    // A tiny viewport skips SSR allocation; the previous history may still have the old size.
    // Orthographic frames cannot produce history for the perspective trace either.
    if (vm.ortho > 0 || !hiz_mips_ || !ssr_history_ || ssr_w_ != w_ || ssr_h_ != h_) {
        ssr_history_valid_ = false;
        return;
    }
    Gpu::barrier(cl, hdr_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Gpu::barrier(cl, ssr_history_.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
    cl->CopyResource(ssr_history_.Get(), hdr_.Get());
    Gpu::barrier(cl, ssr_history_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
    Gpu::barrier(cl, hdr_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ssr_history_vp_ = vm.vp_abs;
    ssr_history_exposure_ = ext_.frame_constants()[1][0];
    ssr_history_valid_ = std::isfinite(ssr_history_exposure_) && ssr_history_exposure_ > 0;
}

// Called with the specular IBL buffer (env_) in RENDER_TARGET state, the G-buffer readable and the
// depth in DEPTH_READ | PIXEL_SHADER_RESOURCE; leaves them so.
bool GameLighting::render_ssr(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p) {
    ssr_drawn_ = false;
    if (!p.ssr || !ssr_loaded_ || vm.ortho > 0) return false;
    if (ssr_w_ != w_ || ssr_h_ != h_) resize_ssr(gpu);
    // The reflection source is the previous frame's lit image: nothing to reflect on the first frame.
    if (!hiz_mips_ || !ssr_history_valid_) return false;
    const float scale = ext_.frame_constants()[1][0] / ssr_history_exposure_;
    if (!std::isfinite(scale) || scale <= 0) return false;
    const uint32_t hw = (w_ + 1) / 2, hh = (h_ + 1) / 2;
    const float W = float(w_), H = float(h_);

    M4 pixel_to_ndc;
    pixel_to_ndc.c[0] = {2 / W, 0, 0, 0};
    pixel_to_ndc.c[1] = {0, -2 / H, 0, 0};
    pixel_to_ndc.c[2] = {0, 0, 1, 0};
    pixel_to_ndc.c[3] = {-1, 1, 0, 1};
    M4 clip_to_pixel;
    clip_to_pixel.c[0] = {W / 2, 0, 0, 0};
    clip_to_pixel.c[1] = {0, -H / 2, 0, 0};
    clip_to_pixel.c[2] = {0, 0, 1, 0};
    clip_to_pixel.c[3] = {W / 2, H / 2, 0, 1};
    const M4 clip_to_world = inverse4(vm.vp_abs);
    const M4 world_to_pixel = mul(clip_to_pixel, vm.vp_abs);
    const float near_z = vm.vp_rel.c[3][2];  // device depth = near / view depth

    // ssr_trace (0x6F): the depth, its min / max pyramid and the pixel <-> world transforms.
    ext_.set_texture(kTrace, 0x00, gb.depth.Get(), GBuffer::kDepthSrvFormat);
    ext_.set_texture(kTrace, 0x08, hiz_.Get());
    ext_.set(kTrace, 0x20, mul(clip_to_world, pixel_to_ndc));  // (pixel, depth, 1) -> world
    ext_.set(kTrace, 0x60, world_to_pixel);                    // world -> (pixel, depth)
    // .xy: view size. .z: a hit needs ray depth >= z * the cell's far depth (thickness). .w: view pixel
    // -> pixel of the buffer the reflection is applied to (the same size here).
    constexpr float kThickness = 0.9f, kIterations = 128;
    ext_.set(kTrace, 0xA0, float4{W, H, kThickness, 1});
    // .x: level the march starts at, .y: level a hit is accepted at (0 = the full depth), .z: top
    // level, .w: step limit.
    ext_.set(kTrace, 0xB0, float4{1, 0, float(hiz_mips_), kIterations});

    // ssr (0x6D). Two fills: the reflection source pass reads +0x150.zw / +0x170.xy as other values.
    constexpr float kEdgeFade = 0.1f, kRoughnessCutoff = 1.0f, kBlurRadius = 1.0f;
    auto main_fill = [&](float blur_size) {
        ext_.set(kSsr, 0x150, float4{W, H, 0, 1 / near_z});  // .zw: 1 / distance = z + w * depth
        ext_.set(kSsr, 0x160, float4{0, 0, kEdgeFade, 0});   // .z: screen-edge fade width, .w: 1 - strength
        ext_.set(kSsr, 0x170, float4{0, 0, kRoughnessCutoff, kBlurRadius});  // .x - .y: first word of the ray list
        ext_.set(kSsr, 0x180, float4{0, 1, 1, blur_size});   // mip = (radius + x) * 6 * (y: weight, z: colour)
    };
    ext_.set(kSsr, 0x110, world_to_pixel);
    ext_.set(kSsr, 0xD0, mul(ssr_history_vp_, clip_to_world));  // this frame's (ndc, depth) -> last frame's clip
    main_fill(0);
    ext_.note_estimate(std::format("SSR settings: thickness {}, {} steps, start level 1, hit level 0, edge fade {}, roughness cutoff {}, "
                                   "blur radius scale {}, mip bias 0 / scale 1, strength 1, no luminance clamp",
                                   kThickness, kIterations, kEdgeFade, kRoughnessCutoff, kBlurRadius),
                       "engine settings (extern 0x6D / 0x6F), not in game data");
    ext_.note_estimate("SSR buffers: half-resolution rays, RGBA16F reflection with 7 mips, applied to the specular IBL buffer; "
                       "reflection source = the previous frame's lit image (also as the current-frame input); stencil exclusion bits = 0",
                       "engine render targets and pass wiring read from the shaders' resource flow, not game data");

    auto view = evaluate_scope(view_scope_, ScopeStage::Compute, ext_);
    const auto frame = ext_.frame_constants();
    auto upload = [&](const auto& values) {
        auto a = gpu.dynamic(values.size() * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    const auto cb12 = upload(view), cb13 = upload(frame);
    const ScopeBufferFn scopes = [&](ShaderStage, uint32_t reg) { return reg == 12 ? cb12 : reg == 13 ? cb13 : 0; };
    auto mip_uav = [&](uint32_t offset, ID3D12Resource* r, uint32_t mip) { ext_.set_texture(kSsr, offset, r, DXGI_FORMAT_UNKNOWN, 0, 0, 0, mip); };

    const auto depth_ps = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const auto depth_cs = D3D12_RESOURCE_STATE_DEPTH_READ | kRead;
    const auto ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const bool ao = p.ssao && hdao_.loaded();
    ID3D12Resource* occlusion = ao ? ao_.Get() : unshadowed_.Get();
    Gpu::barrier(cl, gb.depth.Get(), depth_ps, depth_cs);
    Gpu::barrier(cl, gb.rt[1].Get(), ps, kRead);
    Gpu::barrier(cl, occlusion, ps, kRead);
    Gpu::barrier(cl, env_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);

    // 1. Hi-Z: min / max of the depth, mips 0..3 from the depth, 4..7 from mip 3.
    Gpu::barrier(cl, hiz_.Get(), kRead, kUav);
    for (uint32_t i = 0; i < 4; ++i) mip_uav(0x90 + 8 * i, hiz_.Get(), i);
    hiz_from_texture_.dispatch(gpu, cl, ext_, scopes, groups(hw, 8), groups(hh, 8), 1);
    uav_barrier(cl);
    if (hiz_mips_ == 8) {
        for (uint32_t i = 0; i < 5; ++i) mip_uav(0x88 + 8 * i, hiz_.Get(), 3 + i);
        hiz_from_target_.dispatch(gpu, cl, ext_, scopes, groups(hw >> 4, 8), groups(hh >> 4, 8), 1);
        uav_barrier(cl);
    }
    Gpu::barrier(cl, hiz_.Get(), kUav, kRead);

    // 2. Reflection source: reproject the previous frame in the current exposure domain.
    // The IBL buffer is already exposed. Use the HDR target as scratch before shading writes it.
    ID3D12Resource* history = ssr_history_.Get();
    constexpr auto render_target = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const bool reexpose = scale != 1;
    if (reexpose) {
        if (!ssr_copy_inputs_) ssr_copy_inputs_ = std::make_unique<ExternStore>();
        auto& copy = *ssr_copy_inputs_;
        copy.set_texture(ext::Postprocess, 0x00, history);
        copy.set(ext::Postprocess, 0x50, float4{W, H, 1 / W, 1 / H});
        copy.set(ext::Postprocess, 0xC0, float4{scale, scale, scale, 1});
        const auto rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        const D3D12_VIEWPORT viewport{0, 0, W, H, 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(w_), LONG(h_)};
        cl->RSSetViewports(1, &viewport);
        cl->RSSetScissorRects(1, &scissor);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ssr_scale_history_.draw(gpu, cl, copy, scopes);
        copy.set_texture(ext::Postprocess, 0x00, nullptr);
        cl->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        history = hdr_.Get();
        Gpu::barrier(cl, history, render_target, kRead);
    }
    ext_.set(kSsr, 0x150, float4{W, H, 65504.0f, 0});  // .z: luminance clamp, .w: tonemap weight factor
    ext_.set(kSsr, 0x160, float4{1, 1, kEdgeFade, 0}); // .x: weight of the reprojected image, .y: nearer depths are left out
    ext_.set(kSsr, 0x170, float4{2, 2, kRoughnessCutoff, kBlurRadius});  // .xy: source pixels per output pixel
    ext_.set_texture(kSsr, 0x10, history);
    ext_.set_texture(kSsr, 0x18, history);
    Gpu::barrier(cl, ssr_source_.Get(), kRead, kUav);
    mip_uav(0x68, ssr_source_.Get(), 0);
    ssr_source_gen_.dispatch(gpu, cl, ext_, scopes, groups(hw, 8), groups(hh, 8), 1);
    Gpu::barrier(cl, ssr_source_.Get(), kUav, kRead);
    ext_.set_texture(kSsr, 0x18, nullptr);
    if (reexpose) Gpu::barrier(cl, history, kRead, render_target);
    main_fill(0);

    // 3. One ray per 2x2 pixels that reflects: direction, blur radius and the ray list.
    mip_uav(0x60, ssr_radiance_.Get(), 0);
    ssr_init_.dispatch(gpu, cl, ext_, scopes, groups(hw, 8), groups(hh, 8), 1);
    uav_barrier(cl);
    ext_.set_texture(kSsr, 0x00, ssr_stencil_.Get());
    ext_.set_texture(kSsr, 0x08, gb.rt[1].Get());
    ext_.set_texture(kSsr, 0x50, ssr_args_buf_.Get());
    ext_.set_texture(kSsr, 0x58, ssr_rays_buf_.Get());
    mip_uav(0x70, ssr_dir_.Get(), 0);
    mip_uav(0x78, ssr_weight_.Get(), 0);
    ssr_extract_.dispatch(gpu, cl, ext_, scopes, groups(w_, 8), groups(h_, 8), 1);
    uav_barrier(cl);
    ssr_args_.dispatch(gpu, cl, ext_, scopes, 1, 1, 1);
    Gpu::barrier(cl, ssr_args_buf_.Get(), kUav, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    Gpu::barrier(cl, ssr_rays_buf_.Get(), kUav, kRead);

    // 4. March the rays through the pyramid, then shade the hits from the source.
    ext_.set_texture(kSsr, 0x40, ssr_rays_buf_.Get());
    ssr_trace_.dispatch_indirect(gpu, cl, ext_, scopes, dispatch_sig_.Get(), ssr_args_buf_.Get());
    uav_barrier(cl);
    ext_.set_texture(kSsr, 0x10, ssr_source_.Get());
    ext_.set_texture(kSsr, 0x30, env_.Get());
    ext_.set_texture(kSsr, 0x38, occlusion);
    ssr_resolve_.dispatch_indirect(gpu, cl, ext_, scopes, dispatch_sig_.Get(), ssr_args_buf_.Get());
    uav_barrier(cl);

    // 5. Fill the pixels without a ray, then the blurred mip chain.
    mip_uav(0x68, ssr_color_.Get(), 0);
    ssr_hole_.dispatch(gpu, cl, ext_, scopes, groups(hw, 8), groups(hh, 8), 1);
    uav_barrier(cl);
    for (uint32_t level = 0; level < ssr_mips_; ++level) {
        const uint32_t lw = std::max(1u, hw >> level), lh = std::max(1u, hh >> level);
        mip_uav(0xC0, ssr_color_.Get(), level);
        mip_uav(0xC8, ssr_weight_.Get(), level);
        main_fill(float(lw));
        if (level + 1 < ssr_mips_) {
            mip_uav(0xB0, ssr_color_.Get(), level + 1);
            mip_uav(0xB8, ssr_weight_.Get(), level + 1);
            ssr_hblur_down_.dispatch(gpu, cl, ext_, scopes, groups(lw, 128), groups(lh, 2), 1);
        } else {
            ssr_hblur_.dispatch(gpu, cl, ext_, scopes, groups(lw, 128), groups(lh, 2), 1);
        }
        uav_barrier(cl);
        main_fill(float(lh));
        ssr_vblur_.dispatch(gpu, cl, ext_, scopes, groups(lh, 128), groups(lw, 2), 1);
        uav_barrier(cl);
    }
    main_fill(0);

    // 6. Blend the reflection into the specular IBL buffer at the pixels of the ray list.
    Gpu::barrier(cl, ssr_color_.Get(), kUav, kRead);
    Gpu::barrier(cl, ssr_weight_.Get(), kUav, kRead);
    Gpu::barrier(cl, env_.Get(), kRead, kUav);
    ext_.set_texture(kSsr, 0x20, ssr_color_.Get());
    ext_.set_texture(kSsr, 0x28, ssr_weight_.Get());
    mip_uav(0x80, env_.Get(), 0);
    ssr_apply_.dispatch_indirect(gpu, cl, ext_, scopes, dispatch_sig_.Get(), ssr_args_buf_.Get());
    Gpu::barrier(cl, env_.Get(), kUav, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, ssr_color_.Get(), kRead, kUav);
    Gpu::barrier(cl, ssr_weight_.Get(), kRead, kUav);
    Gpu::barrier(cl, ssr_args_buf_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, kUav);
    Gpu::barrier(cl, ssr_rays_buf_.Get(), kRead, kUav);
    Gpu::barrier(cl, occlusion, kRead, ps);
    Gpu::barrier(cl, gb.rt[1].Get(), kRead, ps);
    Gpu::barrier(cl, gb.depth.Get(), depth_cs, depth_ps);
    ssr_drawn_ = true;
    return true;
}
}
