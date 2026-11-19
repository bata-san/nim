#include "nim/render/game_lighting.hpp"

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
    for (auto* pipeline : {&grid_sky_, &grid_blend_, &grid_encode_}) pipeline->release(gpu);
    for (auto& resource : grid_basis_) resource.Reset();
    for (auto& resource : grid_sh_) resource.Reset();
    grid_clear_heap_.Reset();
    grid_key_.reset();
    grid_ready_ = false;
}

void GameLighting::render_light_grid(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm,
                                     const ScopeBufferFn& scopes, bool overhead) {
    if (!grid_clear_heap_ || !hemi_inputs_) return;
    const float4 center{std::floor(vm.eye.x / kCell) * kCell, std::floor(vm.eye.y / kCell) * kCell,
                        std::floor(vm.eye.z / kCell) * kCell, 0};
    const float4 cover_scale = overhead ? overhead_scale_ : float4{};
    const float4 cover_offset = overhead ? overhead_offset_ : float4{};
    const auto& channels = ext_.channels;
    const std::array key{center, cover_scale, cover_offset,
                         float4{float(overhead_items_), float(overhead), channels.at(0x85)[0], channels.at(0x86)[0]},
                         channels.at(0x54), channels.at(0xB7), channels.at(0x8C), channels.at(0x83)};

    // The grid scope's constants are laid out by 80A740DC. The shaders used here never read
    // register 12 (the undecoded operation on the cascade count).
    std::array<float4, 16> cb{};
    cb[0] = cb[1] = center;
    cb[2] = {float(kX), float(kY), float(kZ), 0};
    cb[3] = {kX * 0.5f, kY * 0.5f, kZ * 0.5f, 0};
    cb[4] = {float(kX), float(kY), float(kSlices), 0};
    cb[5] = {kX + 0.5f, kY + 0.5f, kSlices + 0.5f, 0};
    cb[6] = {kCell, kCell, kCell, 0};
    cb[7][0] = kCell;
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
        const ScopeBufferFn grid_scopes = [&](ShaderStage stage, uint32_t reg) {
            if (stage == ShaderStage::Compute && reg == 12) return view_gpu;
            if (reg == 3) return cb3;
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
        Gpu::barrier(cl, hemi_.Get(), kPixel, kRead);
        // The white texture is uploaded to GENERIC_READ, which already includes compute access.
        if (overhead) Gpu::barrier(cl, cover, kPixel, kRead);
        try {
            grid_sky_.dispatch(gpu, cl, e, grid_scopes, kX / 8, kY / 8, kSlices - 1);
            synchronize_uavs(cl);
            for (uint32_t cascade = kCascades - 1; cascade-- > 0;) {
                cb[11][0] = float(cascade);
                cb3 = upload(cb);
                grid_blend_.dispatch(gpu, cl, e, grid_scopes, kX * 6 / 8, kY / 8, kZ);
                synchronize_uavs(cl);
            }
        } catch (...) {
            for (auto& basis : grid_basis_) Gpu::barrier(cl, basis.Get(), kWrite, kRead);
            Gpu::barrier(cl, hemi_.Get(), kRead, kPixel);
            if (overhead) Gpu::barrier(cl, cover, kRead, kPixel);
            throw;
        }
        Gpu::barrier(cl, hemi_.Get(), kRead, kPixel);
        if (overhead) Gpu::barrier(cl, cover, kRead, kPixel);
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
    ext_.set_scope_constants(ShaderStage::Pixel, 8, advanced);
    ext_.note_estimate("transparent lighting: sky SH on six 16x16x8 cascades, base cell 2 m, current overhead occlusion when available",
                       "grid size, all-cell visibility and update cadence are estimates; local light, local probe and per-object producers are pending");
    ext_.note_estimate("transparent_advanced: sky reflection, t26 volume weight zero, object occlusion one",
                       "object-level specular cubemap selection is unavailable");
    grid_ready_ = true;
}
}
