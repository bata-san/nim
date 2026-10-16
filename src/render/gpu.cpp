#include "nim/render/gpu.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nim {

void check_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::format("{} failed (hr=0x{:08X})", what, uint32_t(hr)));
}

// ---- DescriptorAllocator ------------------------------------------------------------------------

void DescriptorAllocator::init(ID3D12Device* device, uint32_t count) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check_hr(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "CreateDescriptorHeap(srv)");
    cpu_ = heap_->GetCPUDescriptorHandleForHeapStart();
    gpu_ = heap_->GetGPUDescriptorHandleForHeapStart();
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    free_ = {{0, count}};
}

uint32_t DescriptorAllocator::alloc(uint32_t n) {
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->count < n) continue;
        uint32_t first = it->first;
        it->first += n;
        it->count -= n;
        if (it->count == 0) free_.erase(it);
        return first;
    }
    throw std::runtime_error("descriptor heap exhausted");
}

void DescriptorAllocator::free(uint32_t first, uint32_t n) {
    if (n == 0) return;
    auto it = std::lower_bound(free_.begin(), free_.end(), first, [](const Range& r, uint32_t v) { return r.first < v; });
    it = free_.insert(it, {first, n});
    // Merge with neighbours.
    if (it + 1 != free_.end() && it->first + it->count == (it + 1)->first) {
        it->count += (it + 1)->count;
        free_.erase(it + 1);
    }
    if (it != free_.begin() && (it - 1)->first + (it - 1)->count == it->first) {
        (it - 1)->count += it->count;
        free_.erase(it);
    }
}

// ---- Gpu -----------------------------------------------------------------------------------------

void Gpu::init(bool warp) {
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> ad;
    if (warp) check_hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "EnumWarpAdapter");
    else
        check_hr(factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)),
                 "EnumAdapterByGpuPreference");
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    for (wchar_t* p = desc.Description; *p; ++p) adapter_ += char(*p < 128 ? *p : '?');
    check_hr(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice");

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        throw std::runtime_error("GPU/driver lacks Shader Model 6.6, which the game's shaders need (try --warp)");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check_hr(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
    check_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)), "CreateCommandAllocator");
    check_hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_)),
             "CreateCommandList");
    check_hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    srv_.init(device_.Get(), 16384);

    dynamic_ = buffer(kDynamicPerFrame * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    check_hr(dynamic_->Map(0, nullptr, reinterpret_cast<void**>(&dynamic_cpu_)), "Map(dynamic)");
    begin_frame(0);
}

uint64_t Gpu::signal() {
    check_hr(queue_->Signal(fence_.Get(), ++fence_value_), "Signal");
    return fence_value_;
}

void Gpu::wait(uint64_t v) {
    if (fence_->GetCompletedValue() >= v) return;
    check_hr(fence_->SetEventOnCompletion(v, event_), "SetEventOnCompletion");
    WaitForSingleObject(event_, INFINITE);
}

void Gpu::flush() {
    check_hr(list_->Close(), "Close");
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    wait(signal());
    check_hr(device_->GetDeviceRemovedReason(), "GPU execution (device removed)");
    keep_.clear();
    check_hr(alloc_->Reset(), "allocator Reset");
    check_hr(list_->Reset(alloc_.Get(), nullptr), "list Reset");
}

void Gpu::begin_frame(uint32_t frame_index) {
    dynamic_offset_ = size_t(frame_index % kFramesInFlight) * kDynamicPerFrame;
    dynamic_end_ = dynamic_offset_ + kDynamicPerFrame;
}

DynamicAlloc Gpu::dynamic(size_t size) {
    size = (size + 255) & ~size_t(255);
    if (dynamic_offset_ + size > dynamic_end_) throw std::runtime_error("per-frame upload space exhausted");
    DynamicAlloc a{dynamic_cpu_ + dynamic_offset_, dynamic_->GetGPUVirtualAddress() + dynamic_offset_};
    dynamic_offset_ += size;
    return a;
}

ComPtr<ID3D12Resource> Gpu::buffer(size_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                   D3D12_RESOURCE_FLAGS flags) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<size_t>(size, 256);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check_hr(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
             "CreateCommittedResource(buffer)");
    return r;
}

ComPtr<ID3D12Resource> Gpu::upload(const void* data, size_t size) {
    auto r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* p = nullptr;
    check_hr(r->Map(0, nullptr, &p), "Map");
    std::memcpy(p, data, size);
    r->Unmap(0, nullptr);
    return r;
}

void Gpu::barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                  D3D12_RESOURCE_STATES to) {
    if (from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Gpu::texture2d(uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                      D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check_hr(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&r)),
             "CreateCommittedResource(texture2d)");
    return r;
}

ComPtr<ID3D12Resource> Gpu::texture(const Texture& t) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = t.width;
    rd.Height = t.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = t.mip_count;
    rd.Format = DXGI_FORMAT(t.dxgi_format);
    rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    check_hr(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&r)),
             "CreateCommittedResource(texture)");

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.mip_count);
    std::vector<UINT> rows(t.mip_count);
    std::vector<UINT64> row_bytes(t.mip_count);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, 0, t.mip_count, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    auto up = buffer(size_t(total), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr;
    check_hr(up->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map");
    size_t src = 0;
    for (uint32_t m = 0; m < t.mip_count; ++m)
        for (UINT y = 0; y < rows[m]; ++y) {
            size_t n = size_t(row_bytes[m]);
            if (src + n > t.data.size()) break;
            std::memcpy(p + fp[m].Offset + size_t(y) * fp[m].Footprint.RowPitch, t.data.data() + src, n);
            src += n;
        }
    up->Unmap(0, nullptr);
    for (uint32_t m = 0; m < t.mip_count; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION s{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        s.PlacedFootprint = fp[m];
        list_->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    }
    barrier(list_.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    keep_.push_back(up);
    return r;
}

std::vector<uint8_t> Gpu::read_texture(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, uint32_t bpp) {
    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = buffer(size_t(total), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(list_.Get(), tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list_.Get(), tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    flush();
    std::vector<uint8_t> out(size_t(rd.Width) * rd.Height * bpp);
    uint8_t* p = nullptr;
    check_hr(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map(readback)");
    for (uint32_t y = 0; y < rd.Height; ++y)
        std::memcpy(out.data() + size_t(y) * rd.Width * bpp, p + size_t(y) * fp.Footprint.RowPitch, size_t(rd.Width) * bpp);
    rb->Unmap(0, nullptr);
    return out;
}

} // namespace nim
