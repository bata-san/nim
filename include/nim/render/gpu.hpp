#pragma once
#include "nim/content/tfx.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nim {

using Microsoft::WRL::ComPtr;

void check_hr(HRESULT hr, const char* what);  // throws std::runtime_error on failure

// First-fit allocator over one shader-visible CBV/SRV/UAV heap. Everything that binds SRVs
// (scenes, the lighting pass, the UI) shares this heap, since only one can be bound at a time.
class DescriptorAllocator {
public:
    void init(ID3D12Device* device, uint32_t count);
    uint32_t alloc(uint32_t n);  // throws when the heap is exhausted
    void free(uint32_t first, uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t i) const { return {cpu_.ptr + size_t(i) * inc_}; }
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t i) const { return {gpu_.ptr + uint64_t(i) * inc_}; }
    ID3D12DescriptorHeap* heap() const { return heap_.Get(); }

private:
    struct Range { uint32_t first, count; };
    ComPtr<ID3D12DescriptorHeap> heap_;
    std::vector<Range> free_;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_{};
    uint32_t inc_ = 0;
};

// Per-frame constant data, from a persistently mapped upload buffer split into frame regions.
struct DynamicAlloc { void* cpu; D3D12_GPU_VIRTUAL_ADDRESS gpu; };

class Gpu {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    void init(bool warp = false);
    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* queue() const { return queue_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }
    const std::string& adapter_name() const { return adapter_; }
    uint64_t vram_usage() const;   // bytes of local video memory this process uses
    uint64_t vram_budget() const;
    DescriptorAllocator& srv() { return srv_; }

    // Command list for uploads and headless work; flush() submits it and waits for the GPU.
    ID3D12GraphicsCommandList* immediate() { return list_.Get(); }
    void flush();
    uint64_t signal();               // signals the queue fence, returns the value
    void wait(uint64_t fence_value); // blocks until the fence reaches it
    void wait_idle() { wait(signal()); }
    // Throws when the device was removed (a GPU fault); with NIM_D3D_DEBUG=1 logs DRED first.
    void check_device();
    bool removed() const { return removed_; }

    void begin_frame(uint32_t frame_index);  // recycles that frame's dynamic region
    DynamicAlloc dynamic(size_t size);       // 256-byte aligned, valid for the current frame

    ComPtr<ID3D12Resource> buffer(size_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> upload(const void* data, size_t size);  // upload-heap buffer with contents
    ComPtr<ID3D12Resource> texture(const Texture& t);  // records the copy on the immediate list
    ComPtr<ID3D12Resource> texture2d(uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear);
    // Copies a 2D texture back to the CPU (tightly packed rows). Flushes.
    std::vector<uint8_t> read_texture(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, uint32_t bytes_per_pixel);

    static void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES from,
                        D3D12_RESOURCE_STATES to);

private:
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter3> adapter3_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> alloc_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    uint64_t fence_value_ = 0;
    bool removed_ = false;
    HANDLE event_ = nullptr;
    std::vector<ComPtr<ID3D12Resource>> keep_;  // staging kept alive until the next flush
    DescriptorAllocator srv_;
    ComPtr<ID3D12Resource> dynamic_;
    uint8_t* dynamic_cpu_ = nullptr;
    size_t dynamic_offset_ = 0, dynamic_end_ = 0;
    std::string adapter_;
    static constexpr size_t kDynamicPerFrame = 4 << 20;
};

} // namespace nim
