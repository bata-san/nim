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

// A range of a shared default-heap buffer (BufferArena). Holding it keeps its chunk alive.
struct GpuBuffer {
    ComPtr<ID3D12Resource> resource;
    uint64_t offset = 0, size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    explicit operator bool() const { return resource != nullptr; }
};
class Gpu;
// Sub-allocates read-only GPU buffers (geometry, constants) from 64 MB default-heap chunks: one
// committed resource per buffer costs a kernel call and 64 KB alignment each, which dominated
// map loading. Chunks stay in COMMON and are promoted implicitly (copy, then vertex / index / SRV
// / CBV reads). Contents are copied on the immediate list: valid after Gpu::flush().
class BufferArena {
public:
    GpuBuffer upload(Gpu& gpu, const void* data, size_t size, size_t align = 256);
    void reset() { chunk_.Reset(); used_ = capacity_ = 0; }

private:
    ComPtr<ID3D12Resource> chunk_;
    uint64_t used_ = 0, capacity_ = 0;
};

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
    // `n` consecutive shader-visible descriptors (srv() indices), valid for the current frame:
    // per-draw tables, so draws recorded into one list never overwrite each other's views.
    uint32_t transient_descriptors(uint32_t n);
    // Work recorded on the immediate list only and flushed can hand its descriptors back.
    uint32_t transient_mark() const { return transient_offset_; }
    void transient_rewind(uint32_t mark) { transient_offset_ = mark; }

    ComPtr<ID3D12Resource> buffer(size_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> upload(const void* data, size_t size);  // upload-heap buffer with contents
    // Records a copy of `data` into dst at dst_offset on the immediate list (staged in a shared
    // upload ring that is recycled by flush()).
    void stage_copy(ID3D12Resource* dst, uint64_t dst_offset, const void* data, size_t size);
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
    ComPtr<ID3D12Resource> staging_;
    uint8_t* staging_cpu_ = nullptr;
    size_t staging_used_ = 0;
    static constexpr size_t kStagingChunk = 32 << 20;
    DescriptorAllocator srv_;
    ComPtr<ID3D12Resource> dynamic_;
    uint8_t* dynamic_cpu_ = nullptr;
    size_t dynamic_offset_ = 0, dynamic_end_ = 0;
    uint32_t transient_first_ = 0, transient_offset_ = 0, transient_end_ = 0;
    static constexpr uint32_t kTransientPerFrame = 1 << 15;
    std::string adapter_;
    static constexpr size_t kDynamicPerFrame = 64 << 20;
};

} // namespace nim
