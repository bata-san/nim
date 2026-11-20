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
                                      std::pair{&grid_encode_, "light_sample_encode_from_rgba_to_rgb"}}) {
            const auto tag = globals_.pipeline(name);
            if (tag.is_none()) throw std::runtime_error(std::format("Missing {}", name));
            pipeline->load(pm, gpu, tag, scopes, {});
        }
        for (auto& basis : grid_basis_) basis = volume(gpu, kX * 6, DXGI_FORMAT_R32_TYPELESS);
        for (auto& sh : grid_sh_) sh = volume(gpu, kX, DXGI_FORMAT_R16G16B16A16_FLOAT);
        D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3};
        check_hr(gpu.device()->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&grid_clear_heap_)), "light grid UAV descriptors");
    } catch (const std::exception& ex) {
        release_light_grid(gpu);
        shading_.notes.push_back(std::format("transparent light grid: {}", ex.what()));
    }
}

void GameLighting::release_light_grid(Gpu& gpu) {
    for (auto* pipeline : {&grid_sky_, &grid_probe_, &grid_blend_, &grid_encode_}) pipeline->release(gpu);
    for (auto& resource : grid_basis_) resource.Reset();
    for (auto& resource : grid_sh_) resource.Reset();
    grid_clear_heap_.Reset();
    grid_key_.reset();
    grid_ready_ = false;
}

void GameLighting::render_light_grid(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm,
                                     const ScopeBufferFn& scopes, bool sky, bool overhead, const CubemapVolume* preview) {
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
    if (!grid_clear_heap_ || (sky && !hemi_inputs_) || (!sky && probes.empty())) return;
    const float4 cover_scale = overhead ? overhead_scale_ : float4{};
    const float4 cover_offset = overhead ? overhead_offset_ : float4{};
    const auto& channels = ext_.channels;
    const std::array key{center, cover_scale, cover_offset,
                         float4{float(overhead_items_), float(overhead), channels.at(0x85)[0], channels.at(0x86)[0]},
                         float4{float(probes.size()), float(cubemaps_.size()), 0, 0},
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
            if (sky) {
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
    ext_.estimate_texture(ext::Transparent, 0x40, no_atmosphere_.Get(), "sun visibility on transparent grid = 1");
    ext_.set_scope_constants(ShaderStage::Pixel, 3, cb);
    ext_.set_scope_constants(ShaderStage::Vertex, 3, cb);
    std::array<float4, 37> advanced{};
    advanced[6][3] = 1;
    const CubemapVolume* reflect = preview;
    if (!preview) {  // the smallest map volume whose box holds the eye
        float best = 0;
        for (const auto& cv : cubemaps_) {
            if (!cv.specular) continue;
            M4 local;
            for (size_t c = 0; c < 4; ++c) std::copy_n(cv.volume.local_to_world.begin() + c * 4, 4, local.c[c].begin());
            const M4 inv = inverse_matrix(local);
            const float q[3] = {vm.eye.x, vm.eye.y, vm.eye.z};
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
        const CubemapVolume* preview = reflect;
        advanced[10][3] = preview->volume.unk30;
        advanced[11][3] = preview->max_mip;
        advanced[14][3] = 1;
        advanced[15][3] = preview->squared ? 0.0f : 1.0f;
        advanced[16] = {1, 1, 1, 0};
        ext_.set_texture(ext::Transparent, 0xE0, preview->specular.Get());
    }
    if (preview) {
        ext_.note_estimate("transparent preview: orbit_goliath_013b probe SH and reflection cube",
                           "the fixed preview uses the room probe for all materials; variable per-object reflection selection is unresolved");
    } else {
        ext_.note_estimate("transparent lighting: sky SH, then the map cubemap volumes within 64 m (largest first), on six 16x16x8 cascades, base cell 2 m",
                           "grid size, probe radius and update cadence are estimates; local lights and object samples are not in the grid");
        ext_.note_estimate(reflect ? "transparent_advanced: reflection cube of the smallest map volume holding the eye, object occlusion one"
                                   : "transparent_advanced: sky reflection (no map volume holds the eye), object occlusion one",
                           "the engine selects the cube per object; nim uses one per frame");
    }
    ext_.set_scope_constants(ShaderStage::Pixel, 8, advanced);
    grid_ready_ = true;
}
}
