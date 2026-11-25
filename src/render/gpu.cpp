#include "nim/render/gpu.hpp"
#include "nim/core/profile.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <format>
#include <limits>
#include <share.h>
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

namespace {
FILE* g_debug_log = nullptr;
bool g_debug_names = false;
void CALLBACK on_d3d_message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR text, void*) {
    if (!g_debug_log || sev > D3D12_MESSAGE_SEVERITY_WARNING) return;
    std::fprintf(g_debug_log, "[%d] #%d %s\n", int(sev), int(id), text);
    std::fflush(g_debug_log);
}
void debug_name(ID3D12Resource* r, const char* kind, uint64_t a, uint64_t b) {
    if (!g_debug_names || !r) return;
    static std::atomic<uint64_t> n{0};
    r->SetName(std::format(L"{} #{} {}x{}", std::wstring(kind, kind + std::strlen(kind)), ++n, a, b).c_str());
}
}  // namespace

void Gpu::init(bool warp, QueueDeviceHook queue_hook, bool log_device_removal) {
    // The viewer records DRED without turning on the more expensive D3D12 validation layer.
    char flag[8] = {};
    const bool debug = GetEnvironmentVariableA("NIM_D3D_DEBUG", flag, sizeof(flag)) > 0 && flag[0] == '1';
    if (debug) {
        g_debug_names = true;
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            // NIM_D3D_GBV=1: GPU-based validation (descriptors of freed resources, out-of-range reads).
            char gbv[4] = {};
            ComPtr<ID3D12Debug1> dbg1;
            if (GetEnvironmentVariableA("NIM_D3D_GBV", gbv, sizeof(gbv)) > 0 && gbv[0] == '1' && SUCCEEDED(dbg.As(&dbg1)))
                dbg1->SetEnableGPUBasedValidation(TRUE);
        }
    }
    if (debug || log_device_removal) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        if (!g_debug_log) {
            wchar_t exe[MAX_PATH]{};
            if (const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH); n > 0 && n < MAX_PATH) {
                std::wstring path(exe, n);
                if (const size_t sep = path.find_last_of(L"\\/"); sep != std::wstring::npos) path.resize(sep + 1);
                path += L"nim_d3d.log";
                g_debug_log = _wfsopen(path.c_str(), L"a", _SH_DENYNO);
            }
            if (!g_debug_log) g_debug_log = _fsopen("nim_d3d.log", "a", _SH_DENYNO);
        }
        if (g_debug_log) {
            std::fprintf(g_debug_log, "nim DRED active; validation %s\n", debug ? "on" : "off");
            std::fflush(g_debug_log);
        }
    }
    check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> ad;
    if (warp) check_hr(factory_->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "EnumWarpAdapter");
    else
        check_hr(factory_->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)),
                 "EnumAdapterByGpuPreference");
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    for (wchar_t* p = desc.Description; *p; ++p) adapter_ += char(*p < 128 ? *p : '?');
    ad.As(&adapter3_);
    check_hr(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)), "D3D12CreateDevice");
    if (debug) {
        ComPtr<ID3D12InfoQueue1> iq;
        DWORD cookie = 0;
        if (SUCCEEDED(device_.As(&iq)))
            iq->RegisterMessageCallback(on_d3d_message, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        throw std::runtime_error("GPU/driver lacks Shader Model 6.6, which the game's shaders need (try --warp)");

    const auto queue_device = queue_hook ? queue_hook(device_.Get()) : ComPtr<ID3D12Device>{};
    init_queue(1000000, queue_device.Get());  // shader-visible CBV/SRV/UAV (tier 1 limit 1M); maps need many
}

Gpu::~Gpu() {
    if (event_) CloseHandle(event_);
}

void Gpu::init_preview(ID3D12Device* device) {
    device_ = device;
    transient_per_frame_ = 8192;
    dynamic_per_frame_ = 8 << 20;
    init_queue(1 << 17);
}

void Gpu::init_queue(uint32_t descriptors, ID3D12Device* queue_device) {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check_hr((queue_device ? queue_device : device_.Get())->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)), "CreateCommandQueue");
    queue_->SetName(L"nim GPU work queue");
    check_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)), "CreateCommandAllocator");
    check_hr(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_)),
             "CreateCommandList");
    list_->SetName(L"nim upload and auxiliary render");
    check_hr(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) check_hr(HRESULT_FROM_WIN32(GetLastError()), "CreateEventW");
    srv_.init(device_.Get(), descriptors);
    transient_first_ = srv_.alloc(transient_per_frame_ * kFramesInFlight);

    dynamic_ = buffer(dynamic_per_frame_ * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    check_hr(dynamic_->Map(0, nullptr, reinterpret_cast<void**>(&dynamic_cpu_)), "Map(dynamic)");
    begin_frame(0);
}

uint64_t Gpu::signal() {
    const HRESULT hr = queue_->Signal(fence_.Get(), ++fence_value_);
    if (FAILED(hr)) check_device();
    check_hr(hr, "Signal");
    return fence_value_;
}

void Gpu::wait(uint64_t v) {
    const auto complete = fence_->GetCompletedValue();
    if (complete == std::numeric_limits<uint64_t>::max()) check_device();
    if (complete >= v) return;
    const HRESULT hr = fence_->SetEventOnCompletion(v, event_);
    if (FAILED(hr)) check_device();
    check_hr(hr, "SetEventOnCompletion");
    if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0)
        check_hr(HRESULT_FROM_WIN32(GetLastError()), "GPU fence wait");
    if (fence_->GetCompletedValue() == std::numeric_limits<uint64_t>::max()) check_device();
}

void Gpu::flush() {
    ProfileScope profile_flush("gpu.flush.total");
    Profile::instance().count("gpu.flushes");
    check_hr(list_->Close(), "Close");
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    {
        ProfileScope profile_wait("gpu.flush.wait");
        wait(signal());
    }
    check_device();
    keep_.clear();
    staging_used_ = 0;
    for (auto& c : staging_full_) staging_free_.push_back(std::move(c));
    staging_full_.clear();
    reclaim();
    check_hr(alloc_->Reset(), "allocator Reset");
    check_hr(list_->Reset(alloc_.Get(), nullptr), "list Reset");
}

void Gpu::submit() {
    ProfileScope profile_submit("gpu.submit");
    check_hr(list_->Close(), "Close");
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    InFlight f;
    f.fence = signal();
    f.alloc = std::move(alloc_);
    f.keep = std::move(keep_);
    keep_.clear();
    f.staging = std::move(staging_full_);
    staging_full_.clear();
    if (staging_) f.staging.push_back({std::move(staging_), staging_cpu_});
    staging_ = nullptr;
    staging_cpu_ = nullptr;
    staging_used_ = 0;
    in_flight_.push_back(std::move(f));
    // Back-pressure: at most a few submissions' staging in flight.
    if (in_flight_.size() > 4) wait(in_flight_[in_flight_.size() - 5].fence);
    reclaim();
    if (!alloc_free_.empty()) {
        alloc_ = std::move(alloc_free_.back());
        alloc_free_.pop_back();
        check_hr(alloc_->Reset(), "allocator Reset");
    } else {
        check_hr(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_)), "CreateCommandAllocator");
    }
    check_hr(list_->Reset(alloc_.Get(), nullptr), "list Reset");
}

void Gpu::reclaim() {
    const uint64_t done = fence_->GetCompletedValue();
    if (done == std::numeric_limits<uint64_t>::max()) check_device();
    while (!in_flight_.empty() && in_flight_.front().fence <= done) {
        auto& f = in_flight_.front();
        alloc_free_.push_back(std::move(f.alloc));
        for (auto& c : f.staging) staging_free_.push_back(std::move(c));
        in_flight_.pop_front();
    }
    // Bound the idle upload memory (a whole map streams ~1 GB through it).
    while (staging_free_.size() > 8) staging_free_.pop_back();
}

void Gpu::check_device() {
    const HRESULT hr = device_->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) return;
    std::string dred_summary;
    if (g_debug_log) {
        SYSTEMTIME time{};
        GetLocalTime(&time);
        std::fprintf(g_debug_log, "%04u-%02u-%02u %02u:%02u:%02u pid %lu: Device removed reason: 0x%08X\n",
                     unsigned(time.wYear), unsigned(time.wMonth), unsigned(time.wDay), unsigned(time.wHour),
                     unsigned(time.wMinute), unsigned(time.wSecond), GetCurrentProcessId(), unsigned(hr));
        std::fprintf(g_debug_log, "Local video memory: %llu MiB used / %llu MiB budget\n",
                     static_cast<unsigned long long>(vram_usage() >> 20), static_cast<unsigned long long>(vram_budget() >> 20));
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
        if (SUCCEEDED(device_.As(&dred)) && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
            for (auto* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
                const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                const char* lname = n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "?";
                std::fprintf(g_debug_log, "DRED list %s: %u of %u ops done\n", lname, done, n->BreadcrumbCount);
                if (done < n->BreadcrumbCount) {
                    if (dred_summary.empty() && n->pCommandHistory)
                        dred_summary = std::format("\nDRED: list {} op {}/{} (op={})", lname, done, n->BreadcrumbCount,
                                                   int(n->pCommandHistory[done]));
                    for (UINT i = done > 40 ? done - 40 : 0; n->pCommandHistory && i < std::min(n->BreadcrumbCount, done + 4); ++i)
                        std::fprintf(g_debug_log, "   %s op %u = %d\n", i == done ? ">>" : "  ", i, int(n->pCommandHistory[i]));
                }
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        if (dred && SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf))) {
            std::fprintf(g_debug_log, "DRED page fault VA 0x%llx (per-frame upload ring at 0x%llx)\n", (unsigned long long)pf.PageFaultVA,
                         dynamic_ ? (unsigned long long)dynamic_->GetGPUVirtualAddress() : 0ull);
            dred_summary += std::format("\nPageFault VA=0x{:x}", (unsigned long long)pf.PageFaultVA);
            for (auto* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
                std::fprintf(g_debug_log, "  existing: %s type %d\n", a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
            for (auto* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
                std::fprintf(g_debug_log, "  freed: %s type %d\n", a->ObjectNameA ? a->ObjectNameA : "?", int(a->AllocationType));
        }
        std::fflush(g_debug_log);
    }
    removed_ = true;
    throw std::runtime_error(std::format("GPU execution (device removed) failed (hr=0x{:08X}){}", uint32_t(hr), dred_summary));
}

uint64_t Gpu::vram_usage() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
    if (adapter3_ && SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return mi.CurrentUsage;
    return 0;
}
uint64_t Gpu::vram_budget() const {
    DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
    if (adapter3_ && SUCCEEDED(adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi))) return mi.Budget;
    return 0;
}

void Gpu::begin_frame(uint32_t frame_index) {
    // The epoch that ends here was submitted and covered by the last signal (the viewer signals
    // after submitting its frame list; headless work flushes the immediate list).
    epoch_end_fences_[epoch_ % epoch_end_fences_.size()] = fence_value_;
    ++epoch_;
    table_frame_ = frame_index % kFramesInFlight;
    table_cache_[table_frame_].clear();
    table_hits_ = 0;
    dynamic_offset_ = size_t(frame_index % kFramesInFlight) * dynamic_per_frame_;
    dynamic_end_ = dynamic_offset_ + dynamic_per_frame_;
    transient_offset_ = transient_first_ + (frame_index % kFramesInFlight) * transient_per_frame_;
    transient_end_ = transient_offset_ + transient_per_frame_;
}

std::optional<uint32_t> Gpu::find_transient_table(const std::vector<uint64_t>& key) {
    const auto& cache = table_cache_[table_frame_];
    if (auto it = cache.find(key); it != cache.end()) {
        ++table_hits_;
        return it->second.first;
    }
    return std::nullopt;
}

void Gpu::cache_transient_table(std::vector<uint64_t> key, uint32_t first, std::span<ID3D12Resource* const> resources) {
    CachedTable value{first, {}};
    // Keep resource identities valid even if their owner replaces a texture during this frame.
    for (auto* resource : resources) if (resource) value.resources.emplace_back(resource);
    table_cache_[table_frame_].emplace(std::move(key), std::move(value));
}

void Gpu::transient_rewind(uint32_t mark) {
    transient_offset_ = mark;
    // Only permitted after flushing immediate work. Rewound descriptors may be overwritten.
    auto& cache = table_cache_[table_frame_];
    std::erase_if(cache, [mark](const auto& entry) { return entry.second.first >= mark; });
}

uint32_t Gpu::transient_descriptors(uint32_t n) {
    if (n > transient_end_ - transient_offset_)
        throw std::runtime_error(std::format("per-frame descriptor space exhausted: {} used + {} requested / {}",
                                            transient_used(), n, transient_per_frame_));
    const uint32_t first = transient_offset_;
    transient_offset_ += n;
    return first;
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
    debug_name(r.Get(), heap == D3D12_HEAP_TYPE_UPLOAD ? "upload" : heap == D3D12_HEAP_TYPE_READBACK ? "readback" : "buffer", size, 0);
    return r;
}

void Gpu::stage_copy(ID3D12Resource* dst, uint64_t dst_offset, const void* data, size_t size) {
    if (!size) return;
    Profile::instance().count("gpu.stage_copy.calls");
    Profile::instance().count("gpu.stage_copy.bytes", size);
    if (size > kStagingChunk) {
        auto up = upload(data, size);
        list_->CopyBufferRegion(dst, dst_offset, up.Get(), 0, size);
        keep_.push_back(std::move(up));
        return;
    }
    staging_used_ = (staging_used_ + 15) & ~size_t(15);
    if (!staging_ || staging_used_ + size > kStagingChunk) {
        if (staging_) staging_full_.push_back({std::move(staging_), staging_cpu_});  // still read by recorded copies
        if (!staging_free_.empty()) {
            staging_ = std::move(staging_free_.back().resource);
            staging_cpu_ = staging_free_.back().cpu;
            staging_free_.pop_back();
        } else {
            staging_ = buffer(kStagingChunk, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            check_hr(staging_->Map(0, nullptr, reinterpret_cast<void**>(&staging_cpu_)), "Map(staging)");
        }
        staging_used_ = 0;
    }
    std::memcpy(staging_cpu_ + staging_used_, data, size);
    list_->CopyBufferRegion(dst, dst_offset, staging_.Get(), staging_used_, size);
    staging_used_ += size;
}

GpuBuffer BufferArena::upload(Gpu& gpu, const void* data, size_t size, size_t align) {
    constexpr uint64_t kChunk = 64ull << 20;
    const uint64_t bytes = std::max<uint64_t>(size, 4);
    uint64_t at = (used_ + align - 1) / align * align;
    if (!chunk_ || at + bytes > capacity_) {
        capacity_ = std::max(kChunk, (bytes + 0xFFFF) & ~0xFFFFull);
        chunk_ = gpu.buffer(size_t(capacity_), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        at = 0;
    }
    used_ = at + bytes;
    gpu.stage_copy(chunk_.Get(), at, data, size);
    return {chunk_, at, bytes, chunk_->GetGPUVirtualAddress() + at};
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
    const auto hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, clear, IID_PPV_ARGS(&r));
    if (FAILED(hr)) check_device();  // preserve the device-removal reason and DRED breadcrumbs
    check_hr(hr, "CreateCommittedResource(texture2d)");
    debug_name(r.Get(), "tex2d", w, h);
    return r;
}

ComPtr<ID3D12Resource> Gpu::texture(const Texture& t, bool volume) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    const bool is3d = volume || t.depth > 1;
    if (is3d && t.array_size > 1) throw std::invalid_argument("Texture3D cannot have array layers");
    rd.Dimension = is3d ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = t.width;
    rd.Height = t.height;
    rd.DepthOrArraySize = is3d ? t.depth : std::max<uint16_t>(1, t.array_size);
    rd.MipLevels = t.mip_count;
    rd.Format = DXGI_FORMAT(t.dxgi_format);
    rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    check_hr(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&r)),
             "CreateCommittedResource(texture)");
    debug_name(r.Get(), "texture", t.width, t.height);

    const UINT subs = UINT(t.mip_count) * (is3d ? 1u : std::max<UINT>(1, t.array_size));
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> row_bytes(subs);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, 0, subs, 0, fp.data(), rows.data(), row_bytes.data(), &total);
    auto up = buffer(size_t(total), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr;
    check_hr(up->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map");
    size_t src = 0;
    for (uint32_t m = 0; m < subs; ++m)
        for (UINT z = 0; z < fp[m].Footprint.Depth; ++z)
            for (UINT y = 0; y < rows[m]; ++y) {
                size_t n = size_t(row_bytes[m]);
                if (src + n > t.data.size()) break;
                std::memcpy(p + fp[m].Offset + (size_t(z) * rows[m] + y) * fp[m].Footprint.RowPitch, t.data.data() + src, n);
                src += n;
            }
    up->Unmap(0, nullptr);
    for (uint32_t m = 0; m < subs; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION s{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        s.PlacedFootprint = fp[m];
        list_->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
    }
    barrier(list_.Get(), r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // Keep both ends of the copy alive until it has run: a caller that throws before flushing
    // would otherwise free a texture the GPU still copies into.
    keep_.push_back(up);
    keep_.push_back(r);
    return r;
}

std::vector<uint8_t> Gpu::read_texture(ID3D12Resource* tex, D3D12_RESOURCE_STATES state, uint32_t bpp, uint32_t subresource) {
    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device_->GetCopyableFootprints(&rd, subresource, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = buffer(size_t(total), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(list_.Get(), tex, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    src.SubresourceIndex = subresource;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list_.Get(), tex, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    flush();
    const auto width = fp.Footprint.Width, height = fp.Footprint.Height, depth = fp.Footprint.Depth;
    std::vector<uint8_t> out(size_t(width) * height * depth * bpp);
    uint8_t* p = nullptr;
    check_hr(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map(readback)");
    for (uint32_t z = 0; z < depth; ++z) for (uint32_t y = 0; y < height; ++y) {
        const size_t row = size_t(z) * height + y;
        std::memcpy(out.data() + row * width * bpp, p + row * fp.Footprint.RowPitch, size_t(width) * bpp);
    }
    rb->Unmap(0, nullptr);
    return out;
}

} // namespace nim
