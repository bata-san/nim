// Sky ambient with the engine's own pipelines (docs/PROGRESS.md 3.7). Outside the map's cubemap
// volumes the only ambient light is the sky: the engine renders the atmosphere into a mirror-ball
// map at Deferred +0xF8 (z up: texel uv -> normal n = (2 uv - 1, sqrt(1 - r^2)), direction
// reflect(-z, n)) and cubemap_apply_sky_copy_ao samples it per pixel along the normal (diffuse,
// mip 6.47) and the reflection (specular, by roughness) into the light buffers.
//
// The map is built from six 90-degree views: hemisphere_sky_color_generate draws the sky lookups
// for a view's rays (VS: clip -> cb0[0..3] (View +0x160) -> cb12[4..7] direction), then
// sky_hemisphere_copy_frustum writes every hemisphere texel whose direction falls inside that
// view (cb12[0..3] world -> clip from cb12[7]), and sky_hemisphere_downsample_filter_ggx builds
// the mips (four taps at +-cb0[4].xy of mip cb0[3].x, extern 38 +0xC0).
#include "nim/render/game_lighting.hpp"

#include <cstring>
#include <format>

namespace nim {

namespace {
constexpr auto kF16 = DXGI_FORMAT_R16G16B16A16_FLOAT;

void viewport(ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h) {
    const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    const D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
}

void subresource_barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint32_t sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, sub, from, to};
    cl->ResourceBarrier(1, &b);
}
} // namespace

void GameLighting::load_sky_ambient(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes) {
    auto load = [&](GamePipeline& gp, const char* name, const GamePipeline::Options& opt) {
        const auto tag = globals_.pipeline(name);
        try {
            if (tag.is_none()) throw std::runtime_error("not in render globals");
            gp.load(pm, gpu, tag, scopes, opt);
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("{} not loaded: {}", name, ex.what()));
        }
    };
    load(hemi_sky_, "hemisphere_sky_color_generate", {kF16, false});
    {
        GamePipeline::Options opt{kF16, false};
        opt.drop_extra_targets = true;  // SV_Target1: the coverage mask
        load(hemi_copy_, "sky_hemisphere_copy_frustum", opt);
    }
    load(hemi_down_, "sky_hemisphere_downsample_filter_ggx", {kF16, false});
    {
        // SV_Target0 diffuse, 1 specular, 2 the G-buffer AO (for passes nim does not run).
        GamePipeline::Options opt{kF16, false, {kF16}};
        opt.drop_extra_targets = true;
        load(sky_apply_, "cubemap_apply_sky_copy_ao", opt);
    }
    ID3D12Device* dev = gpu.device();
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = kHemiSize;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = UINT16(kHemiMips);
    rd.Format = kF16;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check_hr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&hemi_)),
             "CreateCommittedResource (sky hemisphere)");
    D3D12_CLEAR_VALUE cv{kF16, {0, 0, 0, 0}};
    hemi_face_ = gpu.texture2d(kHemiFace, kHemiFace, kF16, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kHemiMips + 1};
    check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&hemi_rtv_heap_)), "sky hemisphere RTV heap");
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = hemi_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t m = 0; m < kHemiMips; ++m) {
        D3D12_RENDER_TARGET_VIEW_DESC d{};
        d.Format = kF16;
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = m;
        dev->CreateRenderTargetView(hemi_.Get(), &d, {base.ptr + size_t(m) * inc});
    }
    dev->CreateRenderTargetView(hemi_face_.Get(), nullptr, {base.ptr + size_t(kHemiMips) * inc});
}

void GameLighting::release_sky_ambient(Gpu& gpu) {
    for (GamePipeline* gp : {&hemi_sky_, &hemi_copy_, &hemi_down_, &sky_apply_}) gp->release(gpu);
    hemi_.Reset();
    hemi_face_.Reset();
    hemi_rtv_heap_.Reset();
}

bool GameLighting::sky_ambient_on(const LightingParams& p) const {
    return atm_.valid && p.atmosphere && p.sky_ambient && hemi_ && hemi_sky_.loaded() && hemi_copy_.loaded() && hemi_down_.loaded() &&
           sky_apply_.loaded();
}

// Runs after render_sky_lookups (extern 7 +0x40 / +0x58 hold the far lookup set, as the sky
// pipeline reads them). Leaves Deferred +0xF8 = the hemisphere and restores the externs it borrows.
void GameLighting::render_sky_hemisphere(Gpu& gpu, ID3D12GraphicsCommandList* cl) {
    ID3D12Device* dev = gpu.device();
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = hemi_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto rtv = [&](uint32_t slot) { return D3D12_CPU_DESCRIPTOR_HANDLE{base.ptr + size_t(slot) * inc}; };
    const uint8_t pp = ext::Postprocess;
    // Borrowed: View +0x160 (the generate VS's clip transform: identity, so its clip position is
    // the face's own), Postprocess +0x00 / +0xC0.
    float4 view160[4];
    for (uint32_t i = 0; i < 4; ++i) view160[i] = ext_.get_v(ext::View, 0x160 + 16 * i);
    const auto pp_tex = ext_.texture(pp, 0x00);
    const float4 pp_c0 = ext_.get_v(pp, 0xC0);
    {
        M4 identity;
        for (int i = 0; i < 4; ++i) identity.c[i][size_t(i)] = 1;
        ext_.set(ext::View, 0x160, identity);
    }
    const auto cb12_vs = evaluate_scope(view_scope_, ScopeStage::Vertex, ext_);
    const auto cb12_ps = evaluate_scope(view_scope_, ScopeStage::Pixel, ext_);
    auto upload = [&](std::vector<float4> v) {
        if (v.size() < 16) v.resize(16);
        auto a = gpu.dynamic(v.size() * sizeof(float4));
        std::memcpy(a.cpu, v.data(), v.size() * sizeof(float4));
        return a.gpu;
    };

    // D3D cube faces (the cube filter's axes); any orthonormal frame per face would do.
    static const V3 kForward[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const V3 kRight[6] = {{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {-1, 0, 0}, {-1, 0, 0}, {1, 0, 0}};
    static const V3 kUp[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    Gpu::barrier(cl, hemi_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    for (int f = 0; f < 6; ++f) {
        const V3 r = kRight[f], u = kUp[f], fw = kForward[f];
        // The face: direction = right * clip.x + up * clip.y + forward (rows 4..7, clip.w = 1).
        auto sky_vs = cb12_vs;
        auto sky_ps = cb12_ps;
        for (auto* cb : {&sky_vs, &sky_ps}) {
            if (cb->size() < 16) cb->resize(16);
            (*cb)[4] = {r.x, r.y, r.z, 0};
            (*cb)[5] = {u.x, u.y, u.z, 0};
            (*cb)[6] = {0, 0, 0, 0};
            (*cb)[7] = {fw.x, fw.y, fw.z, 0};
        }
        const auto sky_vs_gpu = upload(sky_vs), sky_ps_gpu = upload(sky_ps);
        const ScopeBufferFn sky_scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
            if (reg == 12) return stage == ShaderStage::Vertex ? sky_vs_gpu : sky_ps_gpu;
            return 0;
        };
        Gpu::barrier(cl, hemi_face_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto face_rtv = rtv(kHemiMips);
        cl->OMSetRenderTargets(1, &face_rtv, FALSE, nullptr);
        viewport(cl, kHemiFace, kHemiFace);
        hemi_sky_.draw(gpu, cl, ext_, sky_scopes);
        Gpu::barrier(cl, hemi_face_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // The copy: world -> clip of the face from the origin (clip = (right, up, fwd / 2, fwd) . d).
        auto copy_vs = cb12_vs;
        auto copy_ps = cb12_ps;
        for (auto* cb : {&copy_vs, &copy_ps}) {
            if (cb->size() < 16) cb->resize(16);
            (*cb)[0] = {r.x, u.x, 0.5f * fw.x, fw.x};
            (*cb)[1] = {r.y, u.y, 0.5f * fw.y, fw.y};
            (*cb)[2] = {r.z, u.z, 0.5f * fw.z, fw.z};
            (*cb)[3] = {0, 0, 0, 0};
            (*cb)[7] = {0, 0, 0, 0};
        }
        const auto copy_vs_gpu = upload(copy_vs), copy_ps_gpu = upload(copy_ps);
        const ScopeBufferFn copy_scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
            if (reg == 12) return stage == ShaderStage::Vertex ? copy_vs_gpu : copy_ps_gpu;
            return 0;
        };
        ext_.set_texture(pp, 0x00, hemi_face_.Get());
        const auto mip0 = rtv(0);
        cl->OMSetRenderTargets(1, &mip0, FALSE, nullptr);
        viewport(cl, kHemiSize, kHemiSize);
        hemi_copy_.draw(gpu, cl, ext_, copy_scopes);
    }
    ext_.set(ext::View, 0x160, M4{{view160[0], view160[1], view160[2], view160[3]}});

    // Mips: each a four-tap box of the one above (taps half a source texel off the centre).
    const auto cb_vs = upload(evaluate_scope(view_scope_, ScopeStage::Vertex, ext_));
    const auto cb_ps = upload(evaluate_scope(view_scope_, ScopeStage::Pixel, ext_));
    const ScopeBufferFn scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 12) return stage == ShaderStage::Vertex ? cb_vs : cb_ps;
        return 0;
    };
    for (uint32_t m = 1; m < kHemiMips; ++m) {
        subresource_barrier(cl, hemi_.Get(), m - 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const uint32_t size = std::max(1u, kHemiSize >> m);
        const float offset = 0.5f / float(size * 2);
        ext_.set_texture(pp, 0x00, hemi_.Get(), DXGI_FORMAT_UNKNOWN, 0, 0, m - 1);  // that mip alone: sampled as mip 0
        ext_.set(pp, 0xC0, float4{0, offset, offset, 0});
        const auto h = rtv(m);
        cl->OMSetRenderTargets(1, &h, FALSE, nullptr);
        viewport(cl, size, size);
        hemi_down_.draw(gpu, cl, ext_, scopes);
    }
    subresource_barrier(cl, hemi_.Get(), kHemiMips - 1, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ext_.set_texture(pp, 0x00, pp_tex.resource, pp_tex.format, pp_tex.stride, pp_tex.elements, pp_tex.first_element);
    ext_.set(pp, 0xC0, pp_c0);
    ext_.set_texture(ext::Deferred, 0xF8, hemi_.Get());
    ext_.note_estimate(std::format("sky hemisphere: six {}^2 views of the far sky lookups into a {}^2 mirror ball, {} mips of 2x2 boxes",
                                   kHemiFace, kHemiSize, kHemiMips),
                       "engine view sizes and the downsample offsets (extern 38 +0xC0) are not data; the map's frame copy "
                       "(ground bounce) and the hemisphere fog / tint passes are not run");
}

} // namespace nim
