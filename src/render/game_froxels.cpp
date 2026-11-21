// Original density and front-to-back atmosphere integration CS. Runtime grid/cascade
// parameters are explicit estimates; the shaders and channel expressions are unchanged.
#include "nim/render/game_lighting.hpp"
#include <cmath>
#include <cstring>
#include <format>

namespace nim {
namespace {
constexpr auto kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
}

void GameLighting::load_froxels(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes) {
    for (auto [pipeline, name] : {std::pair{&froxel_density_, "cascade_raymarch_atmosphere_volume"},
                                 std::pair{&froxel_inscatter_, "cascade_raymarch_atmosphere_volume_inscatter"},
                                 std::pair{&froxel_density_no_sun_, "cascade_raymarch_atmosphere_volume_no_sun"},
                                 std::pair{&froxel_inscatter_no_sun_, "cascade_raymarch_atmosphere_volume_inscatter_no_sun"},
                                 std::pair{&froxel_density_noise_, "cascade_raymarch_atmosphere_volume_noise"},
                                 std::pair{&froxel_density_noise_no_sun_, "cascade_raymarch_atmosphere_volume_noise_no_sun"}}) {
        const auto tag = globals_.pipeline(name);
        if (tag.is_none()) throw std::runtime_error(std::format("Missing original {}", name));
        pipeline->load(pm, gpu, tag, scopes, {});
    }
}

void GameLighting::release_froxels(Gpu& gpu) {
    froxel_density_.release(gpu);
    froxel_inscatter_.release(gpu);
    froxel_density_no_sun_.release(gpu);
    froxel_inscatter_no_sun_.release(gpu);
    froxel_density_noise_.release(gpu);
    froxel_density_noise_no_sun_.release(gpu);
    for (auto& volume : froxel_density_vol_) volume.Reset();
    froxel_vol_.Reset();
    froxel_w_ = froxel_h_ = 0;
    froxel_history_valid_ = false;
}

bool GameLighting::froxels_on(const LightingParams& p) const {
    return p.atmosphere && p.froxel_fog && atm_.valid && froxel_density_.loaded() && froxel_inscatter_.loaded();
}

void GameLighting::render_froxels(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm,
                                 const LightingParams& p, const SunShadow* shadow) {
    if (!froxels_on(p)) return;
    // Both shaders use [numthreads(8,8,1)] without xy guards. Round the physical grid
    // and its normalization together so every dispatched thread owns one valid texel.
    const uint32_t width = ((w_ + kFroxelTile - 1) / kFroxelTile + 7) & ~7u;
    const uint32_t height = ((h_ + kFroxelTile - 1) / kFroxelTile + 7) & ~7u;
    if (!froxel_vol_ || width != froxel_w_ || height != froxel_h_) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = kFroxelDepth;
        rd.MipLevels = 1; rd.Format = kFormat; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        for (auto* resource : {std::addressof(froxel_density_vol_[0]), std::addressof(froxel_density_vol_[1]), std::addressof(froxel_vol_)})
            check_hr(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kRead, nullptr,
                                                          IID_PPV_ARGS(resource->ReleaseAndGetAddressOf())), "atmosphere volume");
        froxel_w_ = width; froxel_h_ = height;
        froxel_history_valid_ = false;
    }
    const bool shadowed = shadow && shadow->valid && shadow->depth;
    const bool covered = p.interior_fog && overhead_ && overhead_->valid && overhead_->mask;
    // The density grid is unjittered. Remove TAA's NDC offset from the previous projection
    // before storing it for the CS, which reconstructs rays from field of view alone.
    M4 volume_projection = vm.vp_abs;
    for (auto& column : volume_projection.c) {
        column[0] -= vm.proj.c[2][0] * column[3];
        column[1] -= vm.proj.c[2][1] * column[3];
    }
    const FroxelHistoryView current{volume_projection, vm.eye, vm.fwd, vm.proj.c[3][2],
                                    p.time, p.time_of_day, p.day_night_state,
                                    p.froxel_noise, shadowed, covered, p.environment};
    const auto& previous = froxel_history_view_;
    const V3 motion = current.eye - previous.eye;
    const bool temporal = p.froxel_temporal && !vm.ortho && std::isfinite(current.near_plane) && current.near_plane > 0;
    const bool reproject = temporal && froxel_history_valid_ &&
        dot(motion, motion) < 32.0f * 32.0f && dot(current.forward, previous.forward) > 0.5f &&
        std::abs(current.time - previous.time) <= 0.25f &&
        current.near_plane == previous.near_plane && current.time_of_day == previous.time_of_day &&
        current.day_night_state == previous.day_night_state && current.noise == previous.noise &&
        current.shadowed == previous.shadowed && current.interior == previous.interior &&
        current.environment == previous.environment;
    const uint32_t write_index = froxel_density_index_ ^ 1;
    auto* density_volume = froxel_density_vol_[write_index].Get();
    auto* history = reproject ? froxel_density_vol_[froxel_density_index_].Get() : no_atmosphere_.Get();
    constexpr float kHistoryWeight = 0.75f;
    constexpr uint8_t F = 0x65, S = 6, I = 81;
    ext_.set(F, 0x00, float4{float(width), float(height), float(kFroxelDepth), 0});
    // Previous reverse-Z device depth reconstructs as 1 / (A + B * depth).
    ext_.set(F, 0x10, float4{1 / vm.fx, 1 / vm.fy, 0, reproject ? 1 / previous.near_plane : 0});
    ext_.set(F, 0x20, float4{kFroxelNear, kFroxelFar, kFroxelCurve, reproject ? kHistoryWeight : 0});
    ext_.set(F, 0x30, float4{});  // no volume jitter or additional sun attenuation
    // +0x40.y blends the extinction and the sun in-scatter toward their distance lookups
    // (extern 7 +0x178.. / +0x180..): on. The maps author those ranges (Outpost 5..10 m,
    // Perimeter 50..100 m, Marathon Deck 10..10000 m), and they have no other reader in the
    // volume passes; the engine's value is not in game data.
    ext_.set(F, 0x40, float4{0, 1, 0, 0});
    ext_.note_estimate("extern 0x65 +0x40.y = 1 (froxel fog follows the authored distance lookups)",
                       "engine value unknown; the lookup ranges are authored per map and only this factor applies them");
    ext_.set(F, 0x50, reproject ? previous.projection : current.projection);
    ext_.set_texture(F, 0x90, history);
    ext_.set_texture(F, 0x98, density_volume, kFormat);
    ext_.set_texture(F, 0xA0, density_volume, kFormat);
    ext_.set_texture(F, 0xA8, froxel_vol_.Get(), kFormat);
    // The density shader reconstructs from (x,y,-viewDepth) in an engine camera
    // whose third axis points backward. SunShadow stores reversed depth, hence a
    // negative comparison slope with the original CS's (receiver - map) test.
    M4 camera_to_world{{{vm.right.x, vm.right.y, vm.right.z, 0}, {vm.up.x, vm.up.y, vm.up.z, 0},
                       {-vm.fwd.x, -vm.fwd.y, -vm.fwd.z, 0}, {vm.eye.x, vm.eye.y, vm.eye.z, 1}}};
    ext_.set(S, 0xC0, shadowed ? mul(shadow->vm.vp_abs, camera_to_world) : M4{});
    const float size = shadowed ? float(shadow->size) : 1;
    ext_.set(S, 0x18, size); ext_.set(S, 0x1C, size);
    ext_.set(S, 0x30, float4{shadowed ? -size : 0, 0, 0, 0});
    ext_.set_texture(S, 0x00, shadowed ? shadow->depth.Get() : unshadowed_.Get(),
                     shadowed ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8_UNORM);
    // Interior (extern 81): the density CS marks a froxel indoor when its depth in this top-down map
    // lies behind the stored depth (w = 1: covered), and indoor froxels take fog_density_interior.
    // The engine's render_overhead_visibility_map_interior producer is unknown; nim's straight-down
    // depth (the rain / overhead occlusion map) stands in, so everything under geometry is indoor.
    if (covered) {
        ext_.estimate_texture(I, 0x00, overhead_->mask.Get(), "interior height map = nim straight-down depth (everything under geometry is indoor)");
        ext_.set(I, 0x10, overhead_->scale);
        ext_.set(I, 0x20, overhead_->offset);
    } else {
        ext_.estimate_texture(I, 0x00, unshadowed_.Get(), "interior height map unavailable: no froxel is indoor");
        ext_.set(I, 0x10, float4{});
        ext_.set(I, 0x20, float4{});
    }
    const V3 sun = sun_toward(p);
    ext_.set_channel("sun_light_direction", float4{sun.x, sun.y, sun.z, 0});

    const auto view = evaluate_scope(view_scope_, ScopeStage::Compute, ext_);
    if (view.size() < 8) throw std::runtime_error("Original froxel view scope is incomplete");
    const auto frame = ext_.frame_constants();
    auto upload = [&](const auto& values) {
        auto a = gpu.dynamic(values.size() * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    const auto cb12 = upload(view), cb13 = upload(frame);
    const ScopeBufferFn scopes = [&](ShaderStage, uint32_t reg) { return reg == 12 ? cb12 : reg == 13 ? cb13 : 0; };
    auto finite_constants = [](const GamePipeline& pipeline) {
        const auto& values = pipeline.stage_constants(0);
        for (size_t row = 0; row < values.size(); ++row)
            for (size_t component = 0; component < 4; ++component)
                if (!std::isfinite(values[row][component]))
                    throw std::runtime_error(std::format("Atmosphere {}: non-finite TFX cb0[{}][{}]",
                                                         pipeline.tag().str(), row, component));
    };
    Gpu::barrier(cl, unshadowed_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kRead);
    Gpu::barrier(cl, no_atmosphere_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kRead);
    if (p.froxel_noise) Gpu::barrier(cl, atmosphere_noise_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kRead);
    if (shadowed) Gpu::barrier(cl, shadow->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
    if (covered) Gpu::barrier(cl, overhead_->mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, kRead);
    Gpu::barrier(cl, density_volume, kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto& density = p.froxel_noise ? (shadowed ? froxel_density_noise_ : froxel_density_noise_no_sun_)
                                  : (shadowed ? froxel_density_ : froxel_density_no_sun_);
    density.dispatch(gpu, cl, ext_, scopes, width / 8, height / 8, kFroxelDepth);
    finite_constants(density);
    Gpu::barrier(cl, density_volume, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    Gpu::barrier(cl, froxel_vol_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // One xy thread walks the complete depth column in increasing z.
    auto& inscatter = shadowed ? froxel_inscatter_ : froxel_inscatter_no_sun_;
    inscatter.dispatch(gpu, cl, ext_, scopes, width / 8, height / 8, 1);
    finite_constants(inscatter);
    Gpu::barrier(cl, froxel_vol_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    if (covered) Gpu::barrier(cl, overhead_->mask.Get(), kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (shadowed) Gpu::barrier(cl, shadow->depth.Get(), kRead, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    Gpu::barrier(cl, no_atmosphere_.Get(), kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (p.froxel_noise) Gpu::barrier(cl, atmosphere_noise_.Get(), kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, unshadowed_.Get(), kRead, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ext_.set_texture(ext::Atmosphere, 0x108, froxel_vol_.Get(), kFormat);
    ext_.set_texture(ext::Transparent, 0x78, froxel_vol_.Get(), kFormat);
    froxel_density_index_ = write_index;
    froxel_history_view_ = current;
    froxel_history_valid_ = temporal;
    if (temporal)
        ext_.note_estimate("froxel history: original density reprojection, weight 0.75",
                           "engine history weight, jitter and cut thresholds unknown; reset on view cuts, mapping, or settings changes");
    ext_.note_estimate(p.froxel_noise ? "froxel density: original spatial noise permutation" : "froxel density: original uniform permutation",
                       "engine permutation selection unavailable; noise uses authored density ranges/remaps and game-time motion; uniform uses their midpoint");
    if (!shadowed) ext_.note_estimate("atmosphere froxels: original no_sun permutation",
                                    "sun-shadow producer unavailable: original no_sun CS, no sun in-scatter");
    ext_.note_estimate(std::format("atmosphere froxels {}x{}x{}: one cascade, {}..{}m, curve {}",
                                  width, height, kFroxelDepth, kFroxelNear, kFroxelFar, kFroxelCurve),
                       "original density/inscatter CS; engine cascade size, range, history, density modulation and shadow filter unknown; nim sun depth; interior map unavailable");
}
} // namespace nim
