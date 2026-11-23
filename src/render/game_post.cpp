// Post-processing with the engine's own pipelines (docs/PROGRESS.md 3.5): the bloom pyramid, auto
// exposure and the tonemap / grading pass screen_area_global_lut3d. Pass order and the bloom
// structure follow a Destiny 2 frame of the same engine (Alkahest docs/bloom_pipeline.txt); the
// values the engine sets at run time are estimates, listed with the others.
#include "nim/render/game_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace nim {

namespace {
// Extern blocks of the post passes (TFX extern ids).
constexpr uint8_t kScreenArea = 0x1E;  // screen_area: the lut3d / color grading passes
constexpr uint32_t kGradingSize = 32;   // fixed lattice dimensions in both original shaders
constexpr float kSdrTonemapMode = 4.0f;
constexpr uint8_t kBloomDistortion = 0x38; // bloom_initial_downsample's signed offset inputs
constexpr uint32_t kDistortedHdrRtv = 11;
constexpr uint32_t kDofRtv = 3 * 4 + 9;  // first post RTV slot of the depth of field (after bloom, TAA, lens, line)
constexpr uint8_t kDof = 0x37;           // the depth of field settings the engine fills

float4 distortion_uv_scale(uint32_t w, uint32_t h) {
    // The reference engine uses (0.13281, 0.23611) at 1920x1080, or 255 pixels per axis.
    return {255.0f / float(w), 255.0f / float(h), 0, 0};
}

void viewport(ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h) {
    const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    const D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
}

float4 size4(uint32_t w, uint32_t h) { return {float(w), float(h), 1.0f / float(w), 1.0f / float(h)}; }

// The engine fills the postprocess scope (cb11) of the separable blurs: [0] target size, [3] the
// weights of five bilinear taps (outer, inner, centre), [4] / [5] their offsets along the axis and
// the fixed offset across it, [6] scale, [7] bias. A 9-texel gaussian folded into the five taps.
std::array<float4, 12> blur_scope(uint32_t w, uint32_t h, bool vertical, float sigma) {
    float g[5];
    for (int k = 0; k < 5; ++k) g[k] = std::exp(-float(k * k) / (2 * sigma * sigma));
    const float w1 = g[1] + g[2], w2 = g[3] + g[4];
    const float total = g[0] + 2 * (w1 + w2);
    const float o1 = (g[1] + 2 * g[2]) / w1, o2 = (3 * g[3] + 4 * g[4]) / w2;
    const float texel = vertical ? 1.0f / float(h) : 1.0f / float(w);
    std::array<float4, 12> cb{};
    cb[0] = size4(w, h);
    cb[3] = {w2 / total, w1 / total, g[0] / total, 0};
    cb[4] = {-o2 * texel, -o1 * texel, 0, 0};
    cb[5] = {o1 * texel, o2 * texel, 0, 0};
    cb[6] = {1, 1, 1, 1};
    return cb;
}
} // namespace

void GameLighting::load_post(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes) {
    auto load = [&](GamePipeline& gp, const char* name, GamePipeline::Options opt) {
        const auto tag = globals_.pipeline(name);
        try {
            if (tag.is_none()) throw std::runtime_error("not in render globals");
            gp.load(pm, gpu, tag, scopes, opt);
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("{} not loaded: {}", name, ex.what()));
        }
    };
    constexpr auto f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    {
        // SV_Target1 is only needed by the FXAA input. The TAA shader derives its own luma.
        GamePipeline::Options opt{DXGI_FORMAT_R8G8B8A8_UNORM, false};
        opt.drop_extra_targets = true;
        load(post_tonemap_, "screen_area_global_lut3d", opt);
        // FXAA reads one RGBA texture. Write the native tonemap MRTs together and
        // copy only the second target's alpha into the colour target.
        GamePipeline::Options luma{DXGI_FORMAT_R8G8B8A8_UNORM, false, {DXGI_FORMAT_R8G8B8A8_UNORM}};
        luma.write_masks = {D3D12_COLOR_WRITE_ENABLE_ALL, D3D12_COLOR_WRITE_ENABLE_ALPHA};
        load(post_tonemap_luma_, "screen_area_global_lut3d", luma);
        GamePipeline::Options pack{DXGI_FORMAT_R8G8B8A8_UNORM, false};
        pack.write_masks = {D3D12_COLOR_WRITE_ENABLE_ALPHA};
        load(post_pack_luma_, "copy_texture", pack);
        load(fxaa_, "fxaa", {DXGI_FORMAT_R8G8B8A8_UNORM, false});
        // TAA: the variant with neighbourhood clipping in YCoCg (the decoded one, docs/PROGRESS.md 3.8).
        load(taa_, "resolve_taa_use_clipping_use_ycocg", {f16, false});
        load(taa_copy_, "copy_texture_bilinear", {DXGI_FORMAT_R8G8B8A8_UNORM, false});
        load(taa_seed_, "copy_texture", {f16, false});
    }
    load(bloom_initial_, "bloom_initial_downsample_block_3x3", {f16, false});
    load(bloom_down_, "downsample_block_2x2", {f16, false});
    load(bloom_down_finite_, "downsample_block_2x2_with_nan_kill", {f16, false});
    load(bloom_blur_h_, "gaussian_10_horz", {f16, false});
    load(bloom_blur_v_, "gaussian_10_vert", {f16, false});
    load(bloom_add_, "weighted_add", {f16, false});
    load(bloom_line_down_, "downsample_gaussian_8x1", {f16, false});
    load(bloom_line_blur_, "downsample_gaussian_16x1", {f16, false});
    load(bloom_line_combine_, "combined_bloom_line_blur", {f16, false});
    load(ae_columns_, "autoexposure_sample_columns", {DXGI_FORMAT_R32G32B32A32_FLOAT, false});

    {
        // Cinematic depth of field (v2_dof_cinematic_*). Target formats are estimates: the CoC and
        // tile maxima are single values, the colour stays HDR.
        constexpr auto r16 = DXGI_FORMAT_R16_FLOAT;
        load(dof_prepare_, "v2_dof_cinematic_decode_and_prepare", {r16, false});
        load(dof_tile_h_, "v2_dof_cinematic_tile_max_horz_12", {r16, false});
        load(dof_tile_v_, "v2_dof_cinematic_tile_max_vert_12", {r16, false});
        load(dof_neighbor_, "v2_dof_cinematic_neighbor_max", {r16, false});
        GamePipeline::Options down{f16, false, {r16}};
        down.drop_extra_targets = true;  // SV_Target4 is declared but never written
        load(dof_down_, "v2_dof_cinematic_downsample", down);
        load(dof_apply_, "v2_dof_cinematic_apply", {f16, false});
        load(dof_blur_, "v2_dof_cinematic_gaussian_blur", {f16, false});
        load(dof_combine_, "v2_dof_cinematic_final_combine", {f16, false});
    }
    load(grading_fill_, "color_grading_fill_using_matrix", {f16, false});
    load(grading_convert_, "color_grading_convert_to_volume_texture_sdr", {});
    Texture black;
    black.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    black.width = black.height = 1;
    black.data = {0, 0, 0, 0};
    black_ = gpu.texture(black);

    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kDofRtv + 9};
    check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&post_rtv_heap_)), "post RTV heap");
}

void GameLighting::release_post(Gpu& gpu) {
    post_ldr_.Reset();
    lens_blur_.Reset();
    for (GamePipeline* gp : {&post_tonemap_, &post_tonemap_luma_, &post_pack_luma_, &fxaa_, &taa_, &taa_copy_, &taa_seed_, &bloom_initial_, &bloom_down_, &bloom_down_finite_, &bloom_blur_h_, &bloom_blur_v_, &bloom_add_, &ae_columns_, &grading_fill_, &grading_convert_})
        gp->release(gpu);
    for (auto& t : taa_history_) t.Reset();
    taa_valid_ = taa_prev_valid_ = false;
    for (auto& l : bloom_) l = {};
    for (GamePipeline* gp : {&bloom_line_down_, &bloom_line_blur_, &bloom_line_combine_}) gp->release(gpu);
    for (GamePipeline* gp : {&dof_prepare_, &dof_tile_h_, &dof_tile_v_, &dof_neighbor_, &dof_down_, &dof_apply_, &dof_blur_, &dof_combine_})
        gp->release(gpu);
    for (ComPtr<ID3D12Resource>* r : {std::addressof(dof_coc_), std::addressof(dof_tile_h_rt_), std::addressof(dof_tile_v_rt_), std::addressof(dof_neighbor_rt_), std::addressof(dof_half_), std::addressof(dof_half_coc_), std::addressof(dof_apply_rt_), std::addressof(dof_blur_rt_), std::addressof(dof_out_)})
        r->Reset();
    for (auto& r : bloom_line_) r.Reset();
    ae_columns_rt_.Reset();
    grading_strip_.Reset();
    grading_lut_.Reset();
    black_.Reset();
    post_rtv_heap_.Reset();
    for (auto& r : ae_readback_) r.Reset();
    ae_fence_ = {};
    ae_valid_ = false;
}

void GameLighting::generate_grading_lut(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& scopes) {
    constexpr auto sampled = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto compute_read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr uint32_t width = kGradingSize * kGradingSize;
    auto* dev = gpu.device();
    grading_strip_ = gpu.texture2d(width, kGradingSize, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
    const auto base = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const auto increment = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE strip_rtv{base.ptr + (3 * kBloomLevels + 5) * increment};
    dev->CreateRenderTargetView(grading_strip_.Get(), nullptr, strip_rtv);

    D3D12_RESOURCE_DESC volume{};
    volume.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    volume.Width = volume.Height = volume.DepthOrArraySize = kGradingSize;
    volume.MipLevels = 1;
    volume.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    volume.SampleDesc.Count = 1;
    volume.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> lut;
    check_hr(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &volume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         nullptr, IID_PPV_ARGS(&lut)), "SDR color grading volume");

    // Scope color_grading_ubershader b7: 241 float4s with the layer count at [0].x.
    // Authored layer records are unresolved. Use the native matrix pass with no active layers.
    constexpr size_t grading_scope_bytes = 241 * sizeof(float4);
    const auto cb = gpu.dynamic(grading_scope_bytes);
    std::memset(cb.cpu, 0, grading_scope_bytes);
    const ScopeBufferFn grading_scopes = [&, va = cb.gpu](ShaderStage stage, uint32_t reg) {
        return stage == ShaderStage::Pixel && reg == 7 ? va : scopes(stage, reg);
    };
    ExternStore grading;
    grading.set(kScreenArea, 0xE0, float4{});
    grading.set(kScreenArea, 0xF0, float4{});
    grading.set(kScreenArea, 0x120, M4{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}});
    grading.set(kScreenArea, 0x160, 0.0f);
    grading.set(kScreenArea, 0x1A0, kSdrTonemapMode);
    for (uint32_t field : {0x1A4u, 0x1A8u, 0x1ACu, 0x1B0u}) grading.set(kScreenArea, field, 0.0f);
    viewport(cl, width, kGradingSize);
    cl->OMSetRenderTargets(1, &strip_rtv, FALSE, nullptr);
    grading_fill_.draw(gpu, cl, grading, grading_scopes);

    Gpu::barrier(cl, grading_strip_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, compute_read);
    grading.set_texture(kScreenArea, 0x00, grading_strip_.Get());
    grading.set_texture(kScreenArea, 0x60, lut.Get());
    grading_convert_.dispatch(gpu, cl, grading, scopes, kGradingSize / 8, kGradingSize / 8, kGradingSize);
    Gpu::barrier(cl, lut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, sampled);
    grading_lut_ = std::move(lut);
}

void GameLighting::resize_post(Gpu& gpu, uint32_t w, uint32_t h) {
    if (!post_rtv_heap_) return;
    ID3D12Device* dev = gpu.device();
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R16G16B16A16_FLOAT, {0, 0, 0, 0}};
    auto target = [&](uint32_t tw, uint32_t th, DXGI_FORMAT f, uint32_t slot) {
        cv.Format = f;
        auto r = gpu.texture2d(tw, th, f, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
        dev->CreateRenderTargetView(r.Get(), nullptr, {base.ptr + size_t(slot) * inc});
        return r;
    };
    uint32_t lw = w, lh = h;
    for (int i = 0; i < kBloomLevels; ++i) {
        const uint32_t divisor = i == 0 ? 3 : 2;
        lw = (lw + divisor - 1) / divisor;
        lh = (lh + divisor - 1) / divisor;
        auto& l = bloom_[size_t(i)];
        l.w = lw;
        l.h = lh;
        l.down = target(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, uint32_t(3 * i));
        l.blur = target(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, uint32_t(3 * i + 1));
        l.sum = target(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, uint32_t(3 * i + 2));
    }
    const uint32_t columns = bloom_[kBloomLevels - 1].w;
    ae_columns_rt_ = target(columns, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 3 * kBloomLevels);
    post_ldr_ = target(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 3 * kBloomLevels + 1);
    for (uint32_t i = 0; i < 2; ++i) taa_history_[i] = target(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 3 * kBloomLevels + 2 + i);
    // The authored upsample copies HDR bloom values for the lens texture multiply.
    lens_blur_ = target((w + 3) / 4, (h + 3) / 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 3 * kBloomLevels + 4);
    if (bloom_line_down_.loaded() && bloom_line_blur_.loaded() && bloom_line_combine_.loaded()) {
        const auto& l = bloom_[kBloomLevels - 2];
        const uint32_t half = (l.w + 1) / 2, quarter = (half + 1) / 2;
        bloom_line_[0] = target(half, l.h, cv.Format, 3 * kBloomLevels + 6);
        bloom_line_[1] = target(quarter, l.h, cv.Format, 3 * kBloomLevels + 7);
        bloom_line_[2] = target(quarter, l.h, cv.Format, 3 * kBloomLevels + 8);
    }
    if (dof_ready()) {
        // Tiles of 12 full-resolution texels (the _12 tile passes); the gather runs at half resolution.
        constexpr auto r16 = DXGI_FORMAT_R16_FLOAT, f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dof_tw_ = (w + 11) / 12;
        dof_th_ = (h + 11) / 12;
        dof_hw_ = (w + 1) / 2;
        dof_hh_ = (h + 1) / 2;
        dof_coc_ = target(w, h, r16, kDofRtv);
        dof_tile_h_rt_ = target(dof_tw_, h, r16, kDofRtv + 1);
        dof_tile_v_rt_ = target(dof_tw_, dof_th_, r16, kDofRtv + 2);
        dof_neighbor_rt_ = target(dof_tw_, dof_th_, r16, kDofRtv + 3);
        dof_half_ = target(dof_hw_, dof_hh_, f16, kDofRtv + 4);
        dof_half_coc_ = target(dof_hw_, dof_hh_, r16, kDofRtv + 5);
        dof_apply_rt_ = target(dof_hw_, dof_hh_, f16, kDofRtv + 6);
        dof_blur_rt_ = target(dof_hw_, dof_hh_, f16, kDofRtv + 7);
        dof_out_ = target(w, h, f16, kDofRtv + 8);
    }
    taa_valid_ = false;
    const size_t pitch = (size_t(columns) * 16 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~size_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    for (auto& r : ae_readback_) r = gpu.buffer(pitch, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    ae_fence_ = {};
}

void GameLighting::render_distortions(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb,
                                       const DistortionFn& draw, const ScopeBufferFn& scopes) {
    constexpr auto sample = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    constexpr auto target = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const uint32_t w = (w_ + 1) / 2, h = (h_ + 1) / 2;
    auto* dev = gpu.device();
    const auto inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE vectors{base.ptr + 10 * inc}, resolved{base.ptr + kDistortedHdrRtv * inc};
    const D3D12_CPU_DESCRIPTOR_HANDLE minmax{base.ptr + 12 * inc};
    if (!distortion_) {
        if (!distortion_dsv_) {
            const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
            check_hr(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&distortion_dsv_)), "distortion DSV heap");
        }
        distortion_ = gpu.texture2d(w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, sample, nullptr);
        distorted_hdr_ = gpu.texture2d(w_, h_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, sample, nullptr);
        const D3D12_CLEAR_VALUE clear{GBuffer::kDepthFormat, {.DepthStencil = {0, 0}}};
        distortion_depth_ = gpu.texture2d(w, h, DXGI_FORMAT_R32G8X24_TYPELESS,
                                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear);
        distortion_view_depth_ = gpu.texture2d(w, h, DXGI_FORMAT_R16G16_FLOAT,
                                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, sample, nullptr);
        dev->CreateRenderTargetView(distortion_view_depth_.Get(), nullptr, minmax);
        dev->CreateRenderTargetView(distortion_.Get(), nullptr, vectors);
        dev->CreateRenderTargetView(distorted_hdr_.Get(), nullptr, resolved);
        const D3D12_DEPTH_STENCIL_VIEW_DESC desc{GBuffer::kDepthFormat, D3D12_DSV_DIMENSION_TEXTURE2D};
        dev->CreateDepthStencilView(distortion_depth_.Get(), &desc, distortion_dsv_->GetCPUDescriptorHandleForHeapStart());
    }
    const float4 offset = distortion_uv_scale(w_, h_);
    const auto dsv = distortion_dsv_->GetCPUDescriptorHandleForHeapStart();
    const float zero[4]{};
    viewport(cl, w, h);
    Gpu::barrier(cl, distortion_view_depth_.Get(), sample, target);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0, 0, 0, nullptr);
    // Target 0 holds the four individual depths; distortion only reads target 1 (min/max).
    const D3D12_CPU_DESCRIPTOR_HANDLE depth_targets[]{gb.null_rtv, minmax};
    cl->OMSetRenderTargets(2, depth_targets, FALSE, &dsv);
    constexpr uint8_t hdao = 0x21;
    ext_.set(hdao, 0x60, size4(w, h));
    ext_.set(hdao, 0x70, size4(w_, h_));
    ext_.set_texture(hdao, 0x50, gb.depth.Get(), GBuffer::kDepthSrvFormat);
    const float4 decode = ext_.get_v(ext::Deferred, 0x00);
    // Device depth is reversed and clears to zero. Decode that endpoint to the largest
    // finite value representable in the RG16F surface before the native reciprocal.
    constexpr float inv_far = 1.0f / 65504.0f;
    ext_.set(hdao, 0x80, float4{inv_far, decode[1] - inv_far, 0, 0});
    distortion_depth_downsample_.draw(gpu, cl, ext_, scopes);
    ext_.set_texture(hdao, 0x50, nullptr);
    Gpu::barrier(cl, distortion_view_depth_.Get(), target, sample);
    Gpu::barrier(cl, distortion_.Get(), sample, target);
    cl->ClearRenderTargetView(vectors, zero, 0, nullptr);
    cl->OMSetRenderTargets(1, &vectors, FALSE, &dsv);
    {
        ext_.set(ext::Deferred, 0x00, float4{0, 1, 0, 0});
        ext_.set_texture(ext::Deferred, 0x98, distortion_view_depth_.Get());
        ext_.set_texture(ext::Deferred, 0xD8, gb.depth.Get(), DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
        ext_.set(0x4C, 0x20, offset);
        ext_.note_estimate("distortion: sky depth capped at 65504 m; UV amplitude 255 / framebuffer size",
                           "finite RG16F endpoint for the viewer's infinite projection; amplitude inferred from a same-engine reference");
        draw(cl, w, h);
        ext_.set(ext::Deferred, 0x00, decode);
        ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), GBuffer::kDepthSrvFormat);
        ext_.set_texture(ext::Deferred, 0xD8, nullptr);
    }
    Gpu::barrier(cl, distortion_.Get(), target, sample);
    Gpu::barrier(cl, distorted_hdr_.Get(), sample, target);
    viewport(cl, w_, h_);
    cl->OMSetRenderTargets(1, &resolved, FALSE, nullptr);
    constexpr uint8_t resolve = 0x6A;
    ext_.set(resolve, 0x70, offset);
    ext_.set_texture(resolve, 0x68, distortion_.Get());
    ext_.set_texture(resolve, 0x58, hdr_.Get());
    distortion_resolve_.draw(gpu, cl, ext_, scopes);
    ext_.set_texture(resolve, 0x68, nullptr);
    ext_.set_texture(resolve, 0x58, nullptr);
    Gpu::barrier(cl, distorted_hdr_.Get(), target, sample);
}

// Auto exposure: autoexposure_sample_columns writes, per column of the smallest bloom level, the
// centre-weighted sums of log2 luminance, luminance and weight over 22 rows; the engine reduces
// them on the CPU. The columns were sampled with 1 / exposure (extern 38 +0xC0.z), so the average
// is the scene's luminance before exposure. Use the newest finished measurement from a previous frame.
void GameLighting::update_exposure(Gpu& gpu, const LightingParams& p) {
    // prepare_frame also runs with post disabled, when render_post never gets to invalidate
    // history. The next TAA frame must seed from its own tonemap, even at unchanged exposure.
    if (!p.post) taa_valid_ = taa_prev_valid_ = false;
    const bool automatic = p.post && p.auto_exposure && ae_columns_.loaded() && bloom_initial_.loaded() && bloom_down_.loaded();
    if (!automatic) {
        ae_valid_ = false;
        ae_fence_ = {};
        exposure_ = p.exposure;
        return;
    }
    const uint64_t done = gpu.completed();
    int newest = -1;
    for (uint64_t back = 1; back <= ae_readback_.size() && back <= ae_frame_; ++back) {
        const size_t s = size_t((ae_frame_ - back) % ae_readback_.size());
        if (ae_fence_[s] && ae_readback_[s] && done >= ae_fence_[s]) { newest = int(s); break; }
    }
    if (newest >= 0) {
        ID3D12Resource* rb = ae_readback_[size_t(newest)].Get();
        const uint32_t columns = uint32_t(ae_columns_rt_->GetDesc().Width);
        float* v = nullptr;
        const D3D12_RANGE range{0, size_t(columns) * 16};
        double log_sum = 0, weight = 0;
        if (SUCCEEDED(rb->Map(0, &range, reinterpret_cast<void**>(&v)))) {
            for (uint32_t c = 0; c < columns; ++c) {
                const float log_luminance = v[c * 4], column_weight = v[c * 4 + 2];
                if (!std::isfinite(log_luminance) || !std::isfinite(column_weight) || column_weight <= 0) continue;
                log_sum += log_luminance;
                weight += column_weight;
            }
            const D3D12_RANGE none{0, 0};
            rb->Unmap(0, &none);
        }
        // An older result must never be applied after the newest completed measurement.
        for (auto& fence : ae_fence_) if (fence <= done) fence = 0;
        if (weight > 1e-6 && std::isfinite(log_sum)) {
            ae_luminance_ = float(std::max(0.0, std::exp2(log_sum / weight) - 1e-5));
            // Key and range: the channels autoexposure_max_stops (0x06) and its pair 0x05 (-3,
            // named visual_expositing by the hash list: read as the minimum stops) bound the
            // exposure; the key value is chosen.
            const auto ch = [&](size_t i, float d) {
                return i < ext_.channels.size() && std::isfinite(ext_.channels[i][0])
                    ? std::clamp(ext_.channels[i][0], -16.0f, 16.0f) : d;
            };
            const float min_stops = ch(0x05, -3), max_stops = ch(0x06, 3);
            const float lo = std::exp2(std::min(min_stops, max_stops)), hi = std::exp2(std::max(min_stops, max_stops));
            // ch(0x00) is autoexposure_zero_bias (0.25 across maps). Using 0.5 * ch00^2 (0.03125)
            // keeps twilight/indoor maps (lum ~0.031..0.048) near 0.65..1.0x exposure instead of
            // pegging at +1 stop (2.0x) and overexposing lights.
            const float zero_bias = ch(0x00, 0.25f);
            const float key = zero_bias > 0.0f ? 0.5f * zero_bias * zero_bias : 0.03125f;
            const float target = std::clamp(key * std::exp2(ch(0x01, 0)) / std::max(ae_luminance_, 1e-6f), lo, hi);
            if (!ae_valid_) ae_auto_ = target;
            else {
                const float dt = std::clamp(p.frame_delta_time, 0.0f, 0.25f);
                const float rate = target < ae_auto_ ? 3.0f : 1.5f;  // darken faster than brighten
                ae_auto_ = std::exp2(std::lerp(std::log2(ae_auto_), std::log2(target), 1 - std::exp(-rate * dt)));
            }
            ae_valid_ = true;
        }
    }
    const float next = (ae_valid_ ? ae_auto_ : 1.0f) * p.exposure;
    exposure_ = next;
}

void GameLighting::render_post(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const LightingParams& p,
                               const ScopeBufferFn& scopes, bool distorted) {
    if (!grading_lut_) generate_grading_lut(gpu, cl, scopes);
    ID3D12Device* dev = gpu.device();
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto rtv = [&](uint32_t slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{base.ptr + size_t(slot) * inc}; };
    auto begin = [&](ID3D12Resource* r, uint32_t slot, uint32_t w, uint32_t h) {
        Gpu::barrier(cl, r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto h_rtv = rtv(slot);
        cl->OMSetRenderTargets(1, &h_rtv, FALSE, nullptr);
        viewport(cl, w, h);
    };
    auto end = [&](ID3D12Resource* r) { Gpu::barrier(cl, r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); };
    // The post passes read the lighting's externs (frame, view) plus their own blocks.
    auto& e = ext_;
    if (!e.zero_policy(kScreenArea)) e.zero_when_missing(kScreenArea, "screen_area value not set by nim: zero");
    if (!e.zero_policy(ext::Postprocess)) e.zero_when_missing(ext::Postprocess, "postprocess value not set by nim: zero");
    ID3D12Resource* image = distorted ? distorted_hdr_.Get() : hdr_.Get();
    const uint8_t pp = ext::Postprocess;
    constexpr std::string_view engine = "engine post setting (not in game data)";

    const bool pyramid = bloom_initial_.loaded() && bloom_down_.loaded();
    const bool bloom = p.bloom && pyramid && bloom_blur_h_.loaded() && bloom_blur_v_.loaded() && bloom_add_.loaded();
    const bool automatic = p.auto_exposure && pyramid && ae_columns_.loaded();
    if (bloom || automatic) {
        // bloom_initial_downsample: a 3x3 filter of HDR at third resolution, scaled by the
        // luminance curve x L^2 + y L + z (Postprocess +0xC0) with the luminance in .w.
        // The authored shader adds the sampled RG-minus-BA UV offset before its four HDR taps.
        const auto& l0 = bloom_[0];
        e.set_texture(kBloomDistortion, 0x00, distorted ? distortion_.Get() : black_.Get());
        e.set(kBloomDistortion, 0x20, distorted ? distortion_uv_scale(w_, h_) : float4{});
        e.set(kBloomDistortion, 0x30, float4{});
        e.set_texture(pp, 0x00, hdr_.Get());
        e.set(pp, 0x50, size4(l0.w, l0.h));
        e.set(pp, 0x60, size4(w_, h_));
        // Linear initial downsample (weight = 1): avoids quadratic L^2 blowout on bright lights.
        e.set(pp, 0xC0, float4{0.0f, 0.0f, 1.0f, 0.0f});
        ext_.note_estimate("bloom: initial downsample curve (extern 38 +0xC0) = 1 (linear, no threshold)", engine);
        begin(l0.down.Get(), 0, l0.w, l0.h);
        bloom_initial_.draw(gpu, cl, e, scopes);
        e.set_texture(kBloomDistortion, 0x00, nullptr);
        end(l0.down.Get());
        for (int i = 1; i < kBloomLevels; ++i) {
            const auto& src = bloom_[size_t(i - 1)];
            const auto& dst = bloom_[size_t(i)];
            e.set_texture(pp, 0x00, src.down.Get());
            e.set(pp, 0x60, size4(src.w, src.h));
            e.set(pp, 0x50, size4(dst.w, dst.h));
            const bool sanitize = i == 1 && bloom_down_finite_.loaded();
            // The first reduction sanitizes non-finite HDR samples before they enter the
            // rest of the pyramid or the logarithmic luminance meter.
            e.set(pp, 0xC0, sanitize ? float4{0, 0, 0, 1} : float4{});  // replacement or source mip
            if (sanitize)
                e.note_estimate("bloom: non-finite first reduction sample = (0,0,0,1)",
                                "original downsample_block_2x2_with_nan_kill; replacement from the same-engine reference");
            begin(dst.down.Get(), uint32_t(3 * i), dst.w, dst.h);
            (sanitize ? bloom_down_finite_ : bloom_down_).draw(gpu, cl, e, scopes);
            end(dst.down.Get());
        }
    }
    if (automatic) {
        const auto& last = bloom_[kBloomLevels - 1];
        e.set_texture(pp, 0x00, last.down.Get());
        e.set(pp, 0x50, size4(last.w, 1));
        e.set(pp, 0xC0, float4{0, 0, 1.0f / std::max(exposure_, 1e-6f), 0});
        begin(ae_columns_rt_.Get(), 3 * kBloomLevels, last.w, 1);
        ae_columns_.draw(gpu, cl, e, scopes);
        end(ae_columns_rt_.Get());
        const size_t slot = size_t(ae_frame_ % ae_readback_.size());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        const auto desc = ae_columns_rt_->GetDesc();
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);
        Gpu::barrier(cl, ae_columns_rt_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{ae_readback_[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{ae_columns_rt_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Gpu::barrier(cl, ae_columns_rt_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ae_fence_[slot] = gpu.pending_fence();
        ++ae_frame_;
        ext_.note_estimate("auto exposure: key = 0.5*ch00^2 (0.03125), autoexposure_bias as stops, bounds ch05..ch06, adapting 3 / 1.5 per s",
                           "engine reduction and adaptation are estimated; authored zero_bias/bias and the original columns shader are used");
    }
    ID3D12Resource* bloom_tex = black_.Get();
    if (bloom) {
        // Up the pyramid: blur the smallest level, then at each finer level weighted_add the
        // blurred coarser result to it and blur again (gaussian_10 with the engine's cb11 kernel).
        constexpr float strength = 0.08f;
        auto filter = [&](GamePipeline& pipeline, ID3D12Resource* src, ID3D12Resource* dst, uint32_t slot,
                          const std::array<float4, 12>& cb) {
            const auto a = gpu.dynamic(cb.size() * sizeof(float4));
            std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
            const ScopeBufferFn filter_scopes = [&, va = a.gpu](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
                return reg == 11 ? va : scopes(stage, reg);
            };
            const auto desc = dst->GetDesc();
            const std::pair<uint32_t, ID3D12Resource*> tex[] = {{0, src}};
            begin(dst, slot, uint32_t(desc.Width), desc.Height);
            pipeline.draw(gpu, cl, e, filter_scopes, std::span(tex));
            end(dst);
        };
        auto blur = [&](uint32_t level, ID3D12Resource* horizontal_source) {
            const auto& l = bloom_[level];
            filter(bloom_blur_h_, horizontal_source, l.blur.Get(), 3 * level + 1, blur_scope(l.w, l.h, false, 2.0f));
            auto cb = blur_scope(l.w, l.h, true, 2.0f);
            if (level == 0) cb[6] = {strength, strength, strength, strength};
            filter(bloom_blur_v_, l.blur.Get(), l.sum.Get(), 3 * level + 2, cb);
        };
        if (bloom_line_[0]) {
            auto line = [&](GamePipeline& pipeline, ID3D12Resource* src, uint32_t dst, float4 weights, float4 offsets) {
                const auto desc = bloom_line_[dst]->GetDesc();
                const float texel = 1.0f / float(src->GetDesc().Width);
                std::array<float4, 12> cb{};
                cb[0] = size4(uint32_t(desc.Width), desc.Height);
                cb[3] = weights;
                cb[4] = {offsets[0] * texel, offsets[1] * texel, 0, 0};
                cb[5] = {offsets[2] * texel, offsets[3] * texel, 0, 0};
                cb[6] = {1, 1, 1, 1};
                filter(pipeline, src, bloom_line_[dst].Get(), 3 * kBloomLevels + 6 + dst, cb);
            };
            constexpr float4 reduce_weights{0.12667f, 0.37333f, 0, 0}, reduce_offsets{2.87f, 0.875f, 0, 0};
            constexpr float4 wide_weights{0.04734f, 0.0858f, 0.14793f, 0.21893f}, wide_offsets{6.94f, 4.91f, 2.94f, 0.91f};
            line(bloom_line_down_, bloom_[kBloomLevels - 2].down.Get(), 0, reduce_weights, reduce_offsets);
            line(bloom_line_down_, bloom_line_[0].Get(), 1, reduce_weights, reduce_offsets);
            line(bloom_line_blur_, bloom_line_[1].Get(), 2, wide_weights, wide_offsets);
            line(bloom_line_blur_, bloom_line_[2].Get(), 1, wide_weights, wide_offsets);
        }
        blur(kBloomLevels - 1, bloom_.back().down.Get());
        for (int i = kBloomLevels - 2; i >= 0; --i) {
            const auto& l = bloom_[size_t(i)];
            e.set_texture(pp, 0x00, bloom_[size_t(i + 1)].sum.Get());
            e.set_texture(pp, 0x08, l.down.Get());
            const bool add_line = i == 0 && bloom_line_[1];
            const float coarse_w = add_line ? 0.50f : 0.55f;
            const float fine_w = add_line ? 0.38f : 0.45f;
            e.set(pp, 0xC0, float4{coarse_w, coarse_w, coarse_w, coarse_w});
            e.set(pp, 0xD0, float4{fine_w, fine_w, fine_w, fine_w});
            e.set(pp, 0xE0, add_line ? float4{0.12f, 0.12f, 0.12f, 0.12f} : float4{1, 1, 0, 0});
            e.set(pp, 0xF0, float4{1, 1, 0, 0});
            e.set(pp, 0x50, size4(l.w, l.h));
            if (add_line) e.set_texture(pp, 0x10, bloom_line_[1].Get());
            begin(l.sum.Get(), uint32_t(3 * i + 2), l.w, l.h);
            (add_line ? bloom_line_combine_ : bloom_add_).draw(gpu, cl, e, scopes);
            end(l.sum.Get());
            if (add_line) {
                e.set_texture(pp, 0x10, nullptr);
                ext_.note_estimate("bloom: two width reductions and two horizontal filters, final line mix 0.50 / 0.38 / 0.12",
                                   "native line shaders and same-engine reference topology; balanced mix, Marathon runtime weights unknown");
            }
            blur(uint32_t(i), l.sum.Get());
        }
        bloom_tex = bloom_.front().sum.Get();
        if (p.atmosphere && atm_.lens_upsample.loaded() && atm_.lens_glare.loaded()) {
            // The map's fullscreen lens technique multiplies its authored texture by a broad
            // bloom input. Its companion pass uses quarter-resolution SV_Position.
            constexpr float4 identity_uv{1, 1, 0, 0};
            e.set(pp, 0xC0, identity_uv);
            e.set_texture(pp, 0x00, bloom_.back().sum.Get());
            begin(lens_blur_.Get(), 3 * kBloomLevels + 4, (w_ + 3) / 4, (h_ + 3) / 4);
            atm_.lens_upsample.draw(gpu, cl, e, scopes);
            end(lens_blur_.Get());
            e.set_texture(pp, 0x00, lens_blur_.Get());
            const float prior = e.has(ext::Frame, 0x24, 4) ? e.get_f(ext::Frame, 0x24) : 0.0f;
            e.set(ext::Frame, 0x24, 1.0f);
            Gpu::barrier(cl, image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto hdr_rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            if (distorted) hdr_rtv.ptr += kDistortedHdrRtv * inc;
            cl->OMSetRenderTargets(1, &hdr_rtv, FALSE, nullptr);
            viewport(cl, w_, h_);
            atm_.lens_glare.draw(gpu, cl, e, scopes);
            end(image);
            e.set(ext::Frame, 0x24, prior);
            ext_.note_estimate("lens glare: coarsest blurred bloom level, quarter-resolution upsample, Frame +0x24 = 1",
                               "map-authored optics techniques and texture; original broad filter and brightness are unknown");
        }
        ext_.note_estimate("bloom: 4 levels (1/3..1/24), gaussian sigma 2 texels, strength 0.08",
                           "third-resolution chain follows a same-engine reference; Marathon weights and strength are unknown");
    }

    if (p.depth_of_field && p.dof_focus > 0 && dof_ready() && dof_out_) image = render_dof(gpu, cl, image, p, scopes);

    // screen_area_global_lut3d (SDR): tonemap curve (mode extern 0x1E +0x1A0), the 32^3 grading
    // LUT at +0x30, then out = LUT * t2 (+0x58) + t1 (+0x50).
    e.set_texture(kScreenArea, 0x00, image);
    e.set_texture(kScreenArea, 0x30, grading_lut_.Get());
    e.set_texture(kScreenArea, 0x38, grading_lut_.Get());
    e.set_texture(kScreenArea, 0x50, bloom_tex);
    e.set_texture(kScreenArea, 0x58, unshadowed_.Get());
    e.set(kScreenArea, 0x90, float4{float(kGradingSize), 0, 0, 0});
    e.set(kScreenArea, 0x170, float4{0, 1, 0, 0});  // chromatic aberration ramps (none)
    e.set(kScreenArea, 0x180, float4{0, 1, 0, 0});
    e.set(kScreenArea, 0x190, float4{1, 1, 0, 0});
    e.set(kScreenArea, 0x1A0, kSdrTonemapMode);
    ext_.note_estimate("tonemap: screen_area_global_lut3d mode 4 (the curve whose constants the technique carries), hue / highlight options 0, no chromatic aberration",
                       "engine screen_area settings (extern 0x1E) not in game data");
    ext_.note_estimate("grading: native SDR LUT from no layers and an identity matrix",
                       "grading layer data (scope color_grading_ubershader) source not found");
    ext_.note_estimate("tonemap t2 (multiplied after the LUT) = white", "screen overlay (vignette / damage) not drawn");
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const bool taa = temporal_aa_on(p) && post_ldr_ && taa_history_[0];
    const bool aa = !taa && p.antialias && post_tonemap_luma_.loaded() && post_pack_luma_.loaded() && fxaa_.loaded() && post_ldr_;
    if (!taa) taa_valid_ = false;
    if (taa) {
        const float current_exposure = e.frame_constants()[1][0];
        const float ratio = current_exposure / taa_history_exposure_;
        // History is already tonemapped. A linear RGB correction would alter its highlights
        // and grading. Reduce its weight continuously over one stop of exposure change.
        const float k = taa_valid_ && std::isfinite(ratio) && ratio > 0
            ? std::max(0.0f, 1.0f - std::abs(std::log2(ratio))) : 0.0f;
        const uint32_t cur = taa_current_ ^ 1;
        ext_.note_estimate("taa: fade history by exposure difference, zero at one stop",
                           "history is tonemapped; native exposure rejection settings are unknown");
        if (k == 0) {
            cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
            viewport(cl, w_, h_);
            post_tonemap_.draw(gpu, cl, e, scopes);
            end(gb.lit.Get());
            e.set_texture(pp, 0x00, gb.lit.Get());
            e.set(pp, 0x50, size4(w_, h_));
            e.set(pp, 0xC0, float4{1, 1, 1, 1});
            begin(taa_history_[cur].Get(), 3 * kBloomLevels + 2 + cur, w_, h_);
            taa_seed_.draw(gpu, cl, e, scopes);
            end(taa_history_[cur].Get());
            e.set_texture(pp, 0x00, nullptr);
            taa_current_ = cur;
            taa_history_exposure_ = current_exposure;
            taa_valid_ = true;
            return;
        }
        // Tonemap into the resolve's input, resolve it against the history into the other history
        // target, then copy that into lit.
        const auto ldr_rtv = rtv(3 * kBloomLevels + 1);
        Gpu::barrier(cl, post_ldr_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &ldr_rtv, FALSE, nullptr);
        viewport(cl, w_, h_);
        post_tonemap_.draw(gpu, cl, e, scopes);
        Gpu::barrier(cl, post_ldr_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // Extern 0x6A (resolve_taa, PS 80A74DA0): history weights cb0[3..6] (+0x00..+0x0C), the luma
        // difference that rejects the unclipped history cb0[7] (+0x10), the input size (+0x18) and
        // the resolve size (+0x20); t0 history (+0x48), t1 motion (+0x50, G-buffer RT3), t2 the
        // frame (+0x58), t3 device depth (+0x60, nearest of 3x3 picks the motion).
        //   w = cb0[4] + (cb0[5] - cb0[4]) * motion.z   history kept by the clip (luma change <= cb0[7])
        //   w = lerp(cb0[3], cb0[6], saturate(|motion px| / 50)^0.9)   otherwise
        //   out = frame * (1 - w) + clipped history * w
        constexpr uint8_t kTaa = 0x6A;
        e.set(kTaa, 0x00, 0.75f * k);
        e.set(kTaa, 0x04, 0.92f * k);
        e.set(kTaa, 0x08, 0.92f * k);
        e.set(kTaa, 0x0C, 0.5f * k);
        e.set(kTaa, 0x10, 0.1f);
        e.set(kTaa, 0x18, float(w_));
        e.set(kTaa, 0x1C, float(h_));
        e.set(kTaa, 0x20, float(w_));
        e.set(kTaa, 0x24, float(h_));
        e.set_texture(kTaa, 0x48, taa_history_[taa_current_].Get());
        e.set_texture(kTaa, 0x50, gb.rt[3].Get());
        e.set_texture(kTaa, 0x58, post_ldr_.Get());
        e.set_texture(kTaa, 0x60, gb.depth.Get(), GBuffer::kDepthSrvFormat);
        ext_.note_estimate("taa: history weight 0.92 (0.75 .. 0.5 by motion when the clip rejects it), luma rejection 0.1, Halton 2,3 jitter over 8 frames",
                           "engine TAA settings and jitter sequence not in game data");
        begin(taa_history_[cur].Get(), 3 * kBloomLevels + 2 + cur, w_, h_);
        taa_.draw(gpu, cl, e, scopes);
        end(taa_history_[cur].Get());
        taa_current_ = cur;
        taa_history_exposure_ = current_exposure;
        taa_valid_ = true;
        e.set_texture(pp, 0x00, taa_history_[cur].Get());
        e.set(pp, 0x50, size4(w_, h_));
        e.set(pp, 0xC0, float4{1, 1, 1, 1});
        cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
        viewport(cl, w_, h_);
        taa_copy_.draw(gpu, cl, e, scopes);
    } else if (!aa) {
        cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
        viewport(cl, w_, h_);
        post_tonemap_.draw(gpu, cl, e, scopes);
    } else {
        // Reuse the final output for luma until FXAA writes the resolved colour.
        const auto ldr_rtv = rtv(3 * kBloomLevels + 1);
        Gpu::barrier(cl, post_ldr_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[]{ldr_rtv, gb.lit_rtv};
        cl->OMSetRenderTargets(2, targets, FALSE, nullptr);
        viewport(cl, w_, h_);
        post_tonemap_luma_.draw(gpu, cl, e, scopes);
        cl->OMSetRenderTargets(1, &ldr_rtv, FALSE, nullptr);
        end(gb.lit.Get());
        e.set_texture(pp, 0x00, gb.lit.Get());
        e.set(pp, 0x50, size4(w_, h_));
        e.set(pp, 0xC0, float4{1, 1, 1, 1});
        post_pack_luma_.draw(gpu, cl, e, scopes);
        e.set_texture(pp, 0x00, nullptr);
        end(post_ldr_.Get());
        Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Extern 0x33: the input at +0x00; at +0x50..+0x58 FXAA 3.11's subpixel amount, edge threshold and
        // minimum edge threshold (cb0[3..5].x).
        constexpr uint8_t kAntialias = 0x33;
        e.set_texture(kAntialias, 0x00, post_ldr_.Get());
        e.set(kAntialias, 0x50, 0.75f);
        e.set(kAntialias, 0x54, 0.166f);
        e.set(kAntialias, 0x58, 0.0833f);
        ext_.note_estimate("fxaa: subpixel 0.75, edge threshold 0.166, minimum 0.0833 (extern 0x33 +0x50..+0x58)",
                           "engine anti-aliasing settings not in game data: the FXAA 3.11 quality defaults");
        cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
        fxaa_.draw(gpu, cl, e, scopes);
    }
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool GameLighting::dof_ready() const {
    for (const GamePipeline* gp : {&dof_prepare_, &dof_tile_h_, &dof_tile_v_, &dof_neighbor_, &dof_down_, &dof_apply_, &dof_blur_, &dof_combine_})
        if (!gp->loaded()) return false;
    return true;
}

// The engine's cinematic depth of field over the HDR image (PS 80A74A31 / 80A74A08 / 80A74A0D /
// 80A74A12 / 80A74A49 / 80A74A45 / 80A74A4F / 80A74A54). The signed circle of confusion is
// packed as 0.502 * coc + 0.502 (near < 0, far > 0, |coc| <= 1). Extern 0x37 is the engine's
// per-view setting block; nim fills it from the focus distance (estimates):
//   +0x30 / +0x34  near: full blur at / sharp from (view depth, m)   +0x40 near max |coc|
//   +0x38 / +0x3C  far:  sharp until / full blur at                    +0x44 far max |coc|
//   +0x60 / +0x70  a radial (screen-edge) minimum blur: centre, scale / near, far, start, end radius
//   +0x80          blur size: the gather radius is 6.5 * .xy half-resolution texels at |coc| = 1
ID3D12Resource* GameLighting::render_dof(Gpu& gpu, ID3D12GraphicsCommandList* cl, ID3D12Resource* image,
                                         const LightingParams& p, const ScopeBufferFn& scopes) {
    ID3D12Device* dev = gpu.device();
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = post_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto rtv = [&](uint32_t slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{base.ptr + size_t(kDofRtv + slot) * inc}; };
    struct Target {
        ID3D12Resource* r;
        uint32_t slot;
    };
    auto pass = [&](GamePipeline& gp, std::initializer_list<Target> targets, uint32_t w, uint32_t h) {
        D3D12_CPU_DESCRIPTOR_HANDLE handles[2]{};
        UINT n = 0;
        for (const Target& t : targets) {
            Gpu::barrier(cl, t.r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            handles[n++] = rtv(t.slot);
        }
        cl->OMSetRenderTargets(n, handles, FALSE, nullptr);
        viewport(cl, w, h);
        gp.draw(gpu, cl, ext_, scopes);
        for (const Target& t : targets)
            Gpu::barrier(cl, t.r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    };
    auto& e = ext_;
    const uint8_t pp = ext::Postprocess;
    const float f = p.dof_focus;
    e.set(kDof, 0x30, 0.35f * f);
    e.set(kDof, 0x34, 0.7f * f);
    e.set(kDof, 0x38, 1.5f * f);
    e.set(kDof, 0x3C, 4.0f * f);
    e.set(kDof, 0x40, 1.0f);
    e.set(kDof, 0x44, 1.0f);
    e.set(kDof, 0x60, float4{0.5f, 0.5f, 1, 1});
    e.set(kDof, 0x70, float4{0, 0, 0, 1});  // no radial blur
    e.set(kDof, 0x80, float4{1, 1, 0, 0});
    e.note_estimate(std::format("depth of field: focus {:.1f} m; near full blur 0.35x / sharp 0.7x, far sharp 1.5x / full blur 4x "
                                "the focus, max |coc| 1, blur size 1 (13 px), no radial blur, 12-texel tiles", f),
                    "engine depth of field settings (extern 0x37) are code; the cinematic passes are the game's");
    // No screen distortion offsets: the image is already distorted.
    e.set_texture(kBloomDistortion, 0x00, black_.Get());
    e.set(kBloomDistortion, 0x20, float4{});
    const uint32_t w = w_, h = h_;

    e.set(pp, 0x50, size4(w, h));
    pass(dof_prepare_, {{dof_coc_.Get(), 0}}, w, h);

    e.set_texture(pp, 0x00, dof_coc_.Get());
    e.set(pp, 0x50, size4(dof_tw_, h));
    pass(dof_tile_h_, {{dof_tile_h_rt_.Get(), 1}}, dof_tw_, h);
    e.set_texture(pp, 0x00, dof_tile_h_rt_.Get());
    e.set(pp, 0x50, size4(dof_tw_, dof_th_));
    pass(dof_tile_v_, {{dof_tile_v_rt_.Get(), 2}}, dof_tw_, dof_th_);
    e.set_texture(pp, 0x00, dof_tile_v_rt_.Get());
    pass(dof_neighbor_, {{dof_neighbor_rt_.Get(), 3}}, dof_tw_, dof_th_);

    // Half resolution: colour and CoC, weighted by the side (near / far) of the tile.
    e.set_texture(pp, 0x00, image);
    e.set_texture(pp, 0x08, dof_coc_.Get());
    e.set_texture(pp, 0x10, dof_neighbor_rt_.Get());
    e.set(pp, 0x50, size4(dof_hw_, dof_hh_));
    e.set(pp, 0x60, size4(w, h));
    pass(dof_down_, {{dof_half_.Get(), 4}, {dof_half_coc_.Get(), 5}}, dof_hw_, dof_hh_);

    e.set_texture(pp, 0x00, dof_half_.Get());
    e.set_texture(pp, 0x08, dof_neighbor_rt_.Get());
    e.set_texture(pp, 0x10, dof_half_coc_.Get());
    e.set(pp, 0x60, size4(dof_hw_, dof_hh_));
    pass(dof_apply_, {{dof_apply_rt_.Get(), 6}}, dof_hw_, dof_hh_);
    e.set_texture(pp, 0x00, dof_apply_rt_.Get());
    pass(dof_blur_, {{dof_blur_rt_.Get(), 7}}, dof_hw_, dof_hh_);

    // Full resolution: a small gather of the sharp image blended into the half-resolution result.
    e.set_texture(pp, 0x00, image);
    e.set_texture(pp, 0x08, dof_blur_rt_.Get());
    e.set_texture(pp, 0x10, dof_neighbor_rt_.Get());
    e.set_texture(pp, 0x18, dof_coc_.Get());
    e.set(pp, 0x50, size4(w, h));
    e.set(pp, 0x60, size4(w, h));
    pass(dof_combine_, {{dof_out_.Get(), 8}}, w, h);
    for (uint32_t o : {0x00u, 0x08u, 0x10u, 0x18u}) e.set_texture(pp, o, nullptr);
    e.set_texture(kBloomDistortion, 0x00, nullptr);
    return dof_out_.Get();
}

bool GameLighting::temporal_aa_on(const LightingParams& p) const {
    return p.post && p.temporal_aa && post_ready() && taa_.loaded() && taa_copy_.loaded() && taa_seed_.loaded();
}

// The jitter walks the Halton (2, 3) sequence over 8 frames, centred on the pixel (estimate: the
// engine's sequence is code). The previous view gets the same jitter, so a still camera has no
// motion and the history lines up with the frame's pixel centres.
ViewMatrices GameLighting::temporal_view(const View& v, const LightingParams& p) {
    if (!temporal_aa_on(p)) {
        taa_prev_valid_ = false;
        return make_view(v);
    }
    auto halton = [](uint32_t i, uint32_t base) {
        float f = 1, r = 0;
        for (; i; i /= base) {
            f /= float(base);
            r += f * float(i % base);
        }
        return r;
    };
    const uint32_t i = uint32_t(taa_index_++ % 8) + 1;
    View cur = v;
    cur.jitter_x = halton(i, 2) - 0.5f;
    cur.jitter_y = halton(i, 3) - 0.5f;
    ViewMatrices vm = make_view(cur);
    if (taa_prev_valid_) {
        View prev = taa_prev_view_;
        prev.width = cur.width;
        prev.height = cur.height;
        prev.jitter_x = cur.jitter_x;
        prev.jitter_y = cur.jitter_y;
        vm.vp_prev = make_view(prev).vp_abs;
    }
    taa_prev_view_ = v;
    taa_prev_valid_ = true;
    return vm;
}

} // namespace nim
