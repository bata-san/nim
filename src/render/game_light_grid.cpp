#include "nim/render/game_lighting.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace nim {
namespace {
constexpr auto kRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto kWrite = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr uint32_t kX = 16, kY = 16, kZ = 8, kCascades = 6, kSlices = 1 + kZ * kCascades;
constexpr float kCell = 2;
constexpr uint8_t kSampling = 68, kOverhead = 82;

ComPtr<ID3D12Resource> volume(Gpu& gpu, uint32_t w, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = w;
    desc.Height = kY;
    desc.DepthOrArraySize = kSlices;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> result;
    check_hr(gpu.device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, kRead, nullptr, IID_PPV_ARGS(&result)),
             "transparent light volume");
    return result;
}

void synchronize_uavs(ID3D12GraphicsCommandList* cl) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &barrier);
}
}

void GameLighting::load_light_grid(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes) {
    try {
        for (auto [pipeline, name] : {std::pair{&grid_sky_, "light_sample_sky_hemisphere_grid"},
                                      std::pair{&grid_probe_, "light_sample_local_cubemaps_grid"},
                                      std::pair{&grid_blend_, "light_sample_blend_grid_cascades"},
                                      std::pair{&grid_encode_, "light_sample_encode_from_rgba_to_rgb"},
                                      std::pair{&grid_points_, "light_sample_points_instance_grid"}}) {
            const auto tag = globals_.pipeline(name);
            if (tag.is_none()) throw std::runtime_error(std::format("Missing {}", name));
            pipeline->load(pm, gpu, tag, scopes, {});
        }
        for (auto& basis : grid_basis_) basis = volume(gpu, kX * 6, DXGI_FORMAT_R32_TYPELESS);
        for (auto& sh : grid_sh_) sh = volume(gpu, kX, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (const auto tag = globals_.pipeline("light_sample_points_shadow_compute"); !tag.is_none()) {
            GamePipeline::Options opt;
            opt.compute_stage = true;
            grid_sun_.load(pm, gpu, tag, scopes, opt);
            for (auto& v : grid_sun_vis_) v = volume(gpu, kX, DXGI_FORMAT_R16_FLOAT);
            grid_sun_cleared_ = false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 5};
        check_hr(gpu.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&grid_clear_heap_)), "light grid UAV descriptors");
    } catch (const std::exception& ex) {
        release_light_grid(gpu);
        shading_.notes.push_back(std::format("transparent light grid: {}", ex.what()));
    }
}

void GameLighting::release_light_grid(Gpu& gpu) {
    for (auto* pipeline : {&grid_sky_, &grid_probe_, &grid_blend_, &grid_encode_, &grid_points_, &grid_sun_}) pipeline->release(gpu);
    for (auto& resource : grid_sun_vis_) resource.Reset();
    for (auto& resource : grid_basis_) resource.Reset();
    for (auto& resource : grid_sh_) resource.Reset();
    grid_clear_heap_.Reset();
    grid_key_.reset();
    grid_ready_ = false;
}

void GameLighting::render_light_grid(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm,
                                     const ScopeBufferFn& scopes, bool sky, bool overhead, const CubemapVolume* preview,
                                     const SunShadow* shadow) {
    const float cell = preview ? 0.5f : kCell;
    const float4 center = preview ? float4{} : float4{std::floor(vm.eye.x / kCell) * kCell, std::floor(vm.eye.y / kCell) * kCell,
                        std::floor(vm.eye.z / kCell) * kCell, 0};
    // The map's cubemap volumes are applied to the grid as the deferred pass applies them to the
    // screen: largest first, each blending the cells inside its box toward its own probe by its face
    // weights (the CS reads the cell, lerps, writes it back). Only volumes near the grid centre are
    // run; the outer cascades are 32-64 m cells where the sky term dominates (radius: estimate).
    constexpr float kProbeRadius = 64;
    std::vector<const CubemapVolume*> probes;
    if (preview) probes.push_back(preview);
    else
        for (const auto& cv : cubemaps_) {
            float d2 = 0;
            for (size_t a = 0; a < 3; ++a) {
                const float d = center[a] - std::clamp(center[a], cv.volume.lo[a], cv.volume.hi[a]);
                d2 += d * d;
            }
            if (d2 <= kProbeRadius * kProbeRadius && cv.specular) probes.push_back(&cv);
        }
    // Local lights: each adds its SH into the cells (atomics) after the volumes have blended them.
    // Lights whose volume comes within this radius of the grid centre are run (estimate).
    constexpr float kLightRadius = 64;
    // Diagnostic: NIM_GRID_PARTS = bit mask of the grid inputs to run (1 sky, 2 volumes, 4 lights).
    const unsigned parts = [] {
        const char* v = std::getenv("NIM_GRID_PARTS");
        return v ? unsigned(std::strtoul(v, nullptr, 0)) : 7u;
    }();
    if (!(parts & 2)) probes.clear();
    const bool rebuild_lights = (parts & 4) && !preview && grid_points_.loaded() && !lights_.empty();
    if (!grid_clear_heap_ || (sky && !hemi_inputs_) || (!sky && probes.empty() && !rebuild_lights)) return;
    const float4 cover_scale = overhead ? overhead_scale_ : float4{};
    const float4 cover_offset = overhead ? overhead_offset_ : float4{};
    const auto& channels = ext_.channels;
    const std::array key{center, cover_scale, cover_offset,
                         float4{float(overhead_items_), float(overhead), channels.at(0x85)[0], channels.at(0x86)[0]},
                         float4{float(probes.size()), float(cubemaps_.size()), float(lights_.size()), rebuild_lights ? 1.0f : 0.0f},
                         float4{},
                         channels.at(0x54), channels.at(0xB7), channels.at(0x8C), channels.at(0x83)};

    // The grid scope's constants are laid out by 80A740DC. The shaders used here never read
    // register 12 (the undecoded operation on the cascade count).
    std::array<float4, 16> cb{};
    cb[0] = cb[1] = center;
    cb[2] = {float(kX), float(kY), float(kZ), 0};
    cb[3] = {kX * 0.5f, kY * 0.5f, kZ * 0.5f, 0};
    cb[4] = {float(kX), float(kY), float(kSlices), 0};
    cb[5] = {kX + 0.5f, kY + 0.5f, kSlices + 0.5f, 0};
    cb[6] = {cell, cell, cell, 0};
    cb[7][0] = cell;
    cb[8][0] = float(kCascades);
    cb[9][0] = 1;
    cb[10][0] = std::min({kX, kY, kZ}) * 0.5f;
    cb[13][0] = -1;
    cb[15] = {1, 1, 0, 0}; // full grid weight at visible depths, with no object samples in slice zero

    if (grid_key_ != key) {
        grid_key_.reset();
        auto& e = grid_ext_;
        e.channels = channels;
        e.set_texture(ext::Deferred, 0xF8, hemi_.Get());
        auto* cover = overhead ? overhead_vsm_tex_.Get() : unshadowed_.Get();
        e.set_texture(kOverhead, 0x08, cover);
        e.set(kOverhead, 0x10, overhead ? ext_.get_v(kOverhead, 0x10) : float4{0, 0, 0, 1});
        e.set(kOverhead, 0x20, overhead ? ext_.get_v(kOverhead, 0x20) : float4{0.5f, 0.5f, 0, 1});
        auto upload = [&](const auto& constants) {
            const auto a = gpu.dynamic(constants.size() * sizeof(float4));
            std::memcpy(a.cpu, constants.data(), constants.size() * sizeof(float4));
            return a.gpu;
        };
        // Only these two view registers are read. Put the entire grid in front of the
        // compute pass so cells remain valid at the edge of the camera and across a turn.
        std::array<float4, 16> view{};
        view[6] = {0, 0, -1, 0};
        view[7] = {center[0], center[1], center[2] - 1024, 1};
        const auto view_gpu = upload(view);
        auto cb3 = upload(cb);
        D3D12_GPU_VIRTUAL_ADDRESS cb11 = 0;
        const ScopeBufferFn grid_scopes = [&](ShaderStage stage, uint32_t reg) {
            if (stage == ShaderStage::Compute && reg == 12) return view_gpu;
            if (reg == 3) return cb3;
            if (reg == 11) return cb11;
            return scopes(stage, reg);
        };
        const auto first = gpu.transient_descriptors(3);
        const auto inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto cpu = grid_clear_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.Texture3D.WSize = kSlices;
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        constexpr UINT zeros[4]{};
        for (uint32_t i = 0; i < 3; ++i) {
            auto* basis = grid_basis_[i].Get();
            const D3D12_CPU_DESCRIPTOR_HANDLE clear{cpu.ptr + i * size_t(inc)};
            gpu.device()->CreateUnorderedAccessView(basis, nullptr, &desc, clear);
            gpu.device()->CreateUnorderedAccessView(basis, nullptr, &desc, gpu.srv().cpu(first + i));
            Gpu::barrier(cl, basis, kRead, kWrite);
            cl->ClearUnorderedAccessViewUint(gpu.srv().gpu(first + i), clear, basis, zeros, 0, nullptr);
            e.set_texture(kSampling, 0xB8 + 8 * i, basis, DXGI_FORMAT_R32_UINT);
        }
        synchronize_uavs(cl);
        constexpr auto kPixel = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        if (sky) {
            Gpu::barrier(cl, hemi_.Get(), kPixel, kRead);
            Gpu::barrier(cl, cover, kPixel, kRead);
        }
        try {
            if (sky && (parts & 1)) {
                grid_sky_.dispatch(gpu, cl, e, grid_scopes, kX / 8, kY / 8, kSlices - 1);
                synchronize_uavs(cl);
            }
            for (const CubemapVolume* cv : probes) {
                const auto& probe = cv->volume;
                std::array<float4, 32> constants{};
                constexpr float kHardEdge = 1e-3f;  // as the deferred volumes: an unblended face still cuts off
                for (size_t axis = 0; axis < 3; ++axis) {
                    constants[0][axis] = 1 / std::max(probe.blend_neg[axis], preview ? 0.001f : kHardEdge);
                    constants[1][axis] = 1 / std::max(probe.blend_pos[axis], preview ? 0.001f : kHardEdge);
                    std::copy_n(probe.axes.begin() + 4 * axis, 4, constants[9 + axis].begin());
                    constants[14][axis] = 1 / float(probe.probes[axis]);
                }
                constants[0][3] = preview || cv->diffuse ? 1.0f : 0.0f;  // baked diffuse samples
                constants[2][3] = probe.unk30;
                constants[3][3] = cv->max_mip;
                constants[13] = {1, 1, 1, 1};
                constants[14][3] = 1;
                constants[15] = {probe.probe_params[0], probe.probe_params[1],
                                 preview ? 1.0f : cubemap_alpha_exponent(probe.probe_params[2]), probe.probe_params[3]};
                constants[18] = {0, 0, cv->alpha ? 1.0f : 0.0f, cv->squared ? 0.0f : 1.0f};
                constants[20] = {1, 1, 1, 0};  // no relighting
                if (!preview && probe.use_relighting() && channels.size() > 0x8B) {
                    const auto relit = cubemap_relighting(probe, {channels[0x8B][0], channels[0x8A][0], channels[0x89][0],
                                                                  channels[0x08][0], channels[0x09], channels[0x64]});
                    constants[19] = relit[0];
                    constants[20] = relit[1];
                }
                M4 local;
                for (size_t c = 0; c < 4; ++c) std::copy_n(probe.local_to_world.begin() + c * 4, 4, local.c[c].begin());
                const auto inverse_box = inverse_matrix(local);
                for (size_t c = 0; c < 4; ++c) constants[24 + c] = inverse_box.c[c];
                cb11 = upload(constants);
                for (uint32_t off = 0xD0; off <= 0x160; off += 16) e.set(kSampling, off, float4{});
                const std::pair<uint32_t, ID3D12Resource*> textures[] = {
                    {0, cv->specular.Get()}, {1, cv->alpha.Get()}, {2, cv->diffuse.Get()}};
                for (const auto& [slot, resource] : textures)
                    if (resource) Gpu::barrier(cl, resource, kPixel, kRead);
                try {
                    grid_probe_.dispatch(gpu, cl, e, grid_scopes, (kX * kY * kSlices + 63) / 64, 1, 1, textures);
                } catch (...) {
                    for (const auto& [slot, resource] : textures)
                        if (resource) Gpu::barrier(cl, resource, kRead, kPixel);
                    throw;
                }
                for (const auto& [slot, resource] : textures)
                    if (resource) Gpu::barrier(cl, resource, kRead, kPixel);
                synchronize_uavs(cl);
            }
            if (rebuild_lights) {
                const auto lights = grid_lights(vm, center, kLightRadius);
                e.copy_block_from(1, ext_);
                for (const auto& g : lights) {
                    // +0x00: light to world; the CS takes the position (w column) and the +X axis
                    // (the capsule and cone axis).
                    M4 m;
                    m.c[0] = {g.axis[0], g.axis[1], g.axis[2], 0};
                    m.c[1] = {0, 1, 0, 0};
                    m.c[2] = {0, 0, 1, 0};
                    m.c[3] = {g.position[0], g.position[1], g.position[2], 1};
                    e.set(kSampling, 0x00, m);
                    for (uint32_t off : {0x40u, 0x50u, 0x60u, 0x70u}) e.set(kSampling, off, float4{});  // lobes 2 and 3
                    e.set(kSampling, 0x80, float4{0, 0, 0, g.intensity});
                    e.set(kSampling, 0x90, float4{g.colour[0], g.colour[1], g.colour[2], 0});  // .w: capsule half length
                    e.set(kSampling, 0xA0, float4{g.start, g.end, g.inner, g.outer});
                    grid_points_.dispatch(gpu, cl, e, grid_scopes, kX / 8, kY / 8, kSlices - 1);
                }
                if (!lights.empty()) synchronize_uavs(cl);
                grid_light_count_ = lights.size();
            }
            for (uint32_t cascade = kCascades - 1; cascade-- > 0;) {
                cb[11][0] = float(cascade);
                cb3 = upload(cb);
                grid_blend_.dispatch(gpu, cl, e, grid_scopes, kX * 6 / 8, kY / 8, kZ);
                synchronize_uavs(cl);
            }
        } catch (...) {
            for (auto& basis : grid_basis_) Gpu::barrier(cl, basis.Get(), kWrite, kRead);
            if (sky) {
                Gpu::barrier(cl, hemi_.Get(), kRead, kPixel);
                Gpu::barrier(cl, cover, kRead, kPixel);
            }
            throw;
        }
        if (sky) {
            Gpu::barrier(cl, hemi_.Get(), kRead, kPixel);
            Gpu::barrier(cl, cover, kRead, kPixel);
        }
        cb[11] = {};
        cb3 = upload(cb);
        for (uint32_t i = 0; i < 3; ++i) {
            Gpu::barrier(cl, grid_basis_[i].Get(), kWrite, kRead);
            Gpu::barrier(cl, grid_sh_[i].Get(), kRead, kWrite);
            e.set_texture(ext::Postprocess, 8 * i, grid_basis_[i].Get(), DXGI_FORMAT_R32_FLOAT);
            e.set_texture(kSampling, 0xB8 + 8 * i, grid_sh_[i].Get());
        }
        try {
            grid_encode_.dispatch(gpu, cl, e, grid_scopes, kX / 8, kY / 8, kSlices);
        } catch (...) {
            for (auto& sh : grid_sh_) Gpu::barrier(cl, sh.Get(), kWrite, kRead);
            throw;
        }
        for (auto& sh : grid_sh_) Gpu::barrier(cl, sh.Get(), kWrite, kRead);
        grid_key_ = key;
    }
    for (uint32_t i = 0; i < 3; ++i) ext_.set_texture(ext::Transparent, 0x28 + 8 * i, grid_sh_[i].Get());
    bool sun_grid = false;
    if (!preview && shadow && shadow->valid && shadow->depth && grid_sun_.loaded() && grid_sun_vis_[0]) {
        const auto cb3 = [&] {
            const auto a = gpu.dynamic(cb.size() * sizeof(float4));
            std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
            return a.gpu;
        }();
        const ScopeBufferFn grid_scopes = [&](ShaderStage stage, uint32_t reg) { return reg == 3 ? cb3 : scopes(stage, reg); };
        try {
            render_grid_sun(gpu, cl, grid_scopes, *shadow);
            sun_grid = true;
        } catch (const std::exception& ex) {
            ext_.note_estimate("transparent grid sun visibility unavailable", ex.what());
        }
    }
    if (sun_grid) {
        ext_.set_texture(ext::Transparent, 0x40, grid_sun_vis_[0].Get());
        ext_.note_estimate("transparent grid sun visibility: light_sample_points_shadow_compute on nim's sun shadow map, every cell replaced each frame, 1 outside the map",
                           "extern 6 +0x30 compare slope = -map size (as the froxels); extern 0x43 +0x50 = (1, 0, 1, 1) with a constant in-range matrix at +0x60 (camera range test and temporal blend off)");
    } else {
        ext_.estimate_texture(ext::Transparent, 0x40, no_atmosphere_.Get(), "sun visibility on transparent grid = 1");
    }
    ext_.set_scope_constants(ShaderStage::Pixel, 3, cb);
    ext_.set_scope_constants(ShaderStage::Vertex, 3, cb);
    if (preview) {
        ext_.note_estimate("transparent preview: orbit_goliath_013b probe SH and reflection cube",
                           "the fixed preview uses the room probe for all materials; variable per-object reflection selection is unresolved");
    } else {
        ext_.note_estimate("transparent lighting: sky SH, then the map cubemap volumes within 64 m (largest first), on six 16x16x8 cascades, base cell 2 m",
                           "grid size, probe radius and update cadence are estimates; object samples are not in the grid");
        if (grid_light_count_ || grid_light_misses_)
            ext_.note_estimate(std::format("transparent local lights: {} lights within 64 m into light_sample_points_instance_grid ({} without readable registers)",
                                           grid_light_count_, grid_light_misses_),
                               "light PS registers by pattern: falloff = the (-a, b, 0, 0) init, colour = falloff - 1, cone = falloff - 3, weight = the +0x150 product; "
                               "point / spot from the node +X axis, cone exponent and clip planes dropped, no capsule length, one SH lobe; values taken when the grid is rebuilt");
    }
    grid_ready_ = true;
}

void GameLighting::prepare_transparent_reflection(const ViewMatrices& vm, const CubemapVolume* preview) {
    std::array<float4, 37> advanced{};
    advanced[6][3] = 1;
    const CubemapVolume* reflect = preview;
    if (!preview) {  // the smallest map volume whose box holds the eye
        const float q[3] = {vm.eye.x, vm.eye.y, vm.eye.z};
        float best = 0;
        for (const auto& cv : cubemaps_) {
            if (!cv.specular) continue;
            const auto& placement = cv.volume.local_to_world;
            bool within_bounds = true;
            for (size_t r = 0; r < 3; ++r) {
                const float bound = std::abs(placement[r]) + std::abs(placement[r + 4]) + std::abs(placement[r + 8]);
                within_bounds &= std::abs(q[r] - placement[r + 12]) <= bound;
            }
            if (!within_bounds) continue;
            M4 local;
            for (size_t c = 0; c < 4; ++c) std::copy_n(placement.begin() + c * 4, 4, local.c[c].begin());
            const M4 inv = inverse_matrix(local);
            bool inside = true;
            for (size_t r = 0; r < 3 && inside; ++r) {
                const float x = inv.c[0][r] * q[0] + inv.c[1][r] * q[1] + inv.c[2][r] * q[2] + inv.c[3][r];
                inside = std::abs(x) <= 1;
            }
            const float size = cv.volume.extents[0] * cv.volume.extents[1] * cv.volume.extents[2];
            if (inside && (!reflect || size < best)) reflect = &cv, best = size;
        }
    }
    if (reflect) {
        advanced[10][3] = reflect->volume.unk30;
        advanced[11][3] = reflect->max_mip;
        advanced[14][3] = 1;
        advanced[15][3] = reflect->squared ? 0.0f : 1.0f;
        advanced[16] = {1, 1, 1, 0};
        ext_.set_texture(ext::Transparent, 0xE0, reflect->specular.Get());
    }
    ext_.note_estimate(reflect ? "transparent_advanced: reflection cube of the smallest map volume holding the eye, object occlusion one"
                                   : "transparent_advanced: sky reflection (no map volume holds the eye), object occlusion one",
                           "the engine selects the cube per object; nim uses one per frame");
    ext_.set_scope_constants(ShaderStage::Pixel, 8, advanced);
}

void GameLighting::render_grid_sun(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& grid_scopes, const SunShadow& shadow) {
    constexpr uint8_t kSun = 6, kMask = 0x43;  // extern 0x43 (67): the grid sample shadow block
    auto* out = grid_sun_vis_[0].Get();
    auto* one = grid_sun_vis_[1].Get();
    if (!grid_sun_cleared_) {  // the "previous" input stays 1: cells outside the shadow map are lit
        const auto inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto first = gpu.transient_descriptors(2);
        D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        desc.Texture3D.WSize = kSlices;
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        constexpr float ones[4]{1, 1, 1, 1};
        for (uint32_t i = 0; i < 2; ++i) {
            auto* v = grid_sun_vis_[i].Get();
            const D3D12_CPU_DESCRIPTOR_HANDLE clear{grid_clear_heap_->GetCPUDescriptorHandleForHeapStart().ptr + (3 + i) * size_t(inc)};
            gpu.device()->CreateUnorderedAccessView(v, nullptr, &desc, clear);
            gpu.device()->CreateUnorderedAccessView(v, nullptr, &desc, gpu.srv().cpu(first + i));
            Gpu::barrier(cl, v, kRead, kWrite);
            cl->ClearUnorderedAccessViewFloat(gpu.srv().gpu(first + i), clear, v, ones, 0, nullptr);
            Gpu::barrier(cl, v, kWrite, kRead);
        }
        grid_sun_cleared_ = true;
    }
    auto& e = grid_ext_;
    const float size = float(shadow.size);
    e.set(kSun, 0x100, shadow.vm.vp_abs);  // absolute world -> sun clip (cb0[9..12])
    e.set(kSun, 0x18, size);
    e.set(kSun, 0x1C, size);
    e.set(kSun, 0x30, float4{-size, 0, 0, 0});  // reversed depth: a negative slope on (receiver - map)
    e.set_texture(kSun, 0x00, shadow.depth.Get(), DXGI_FORMAT_R32_FLOAT);
    M4 in_range{};
    in_range.c[3] = {0, 0, 0.5f, 1};
    e.set(kMask, 0x60, in_range);
    e.set(kMask, 0x50, float4{1, 0, 1, 1});
    e.set_texture(kMask, 0x10, one);
    e.set_texture(kMask, 0xA0, out, DXGI_FORMAT_R16_FLOAT);
    Gpu::barrier(cl, shadow.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
    Gpu::barrier(cl, out, kRead, kWrite);
    try {
        grid_sun_.dispatch(gpu, cl, e, grid_scopes, kX / 8, kY / 8, kSlices);
    } catch (...) {
        Gpu::barrier(cl, out, kWrite, kRead);
        Gpu::barrier(cl, shadow.depth.Get(), kRead, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        throw;
    }
    Gpu::barrier(cl, out, kWrite, kRead);
    Gpu::barrier(cl, shadow.depth.Get(), kRead, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}
}
