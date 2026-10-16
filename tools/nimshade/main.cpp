// nimshade — headless shading test.
//
// Renders a static mesh with the game's own compiled shaders (DXIL VS + PS, untouched), material
// constants evaluated by nim's TFX VM, and the game's BC textures. Writes the G-buffer the PS
// produces plus a simple lit composite as PNGs.
//
//   nimshade [--install DIR] STATIC_TAG [OUT_DIR] [--size WxH] [--warp]
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/core/package_manager.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <print>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nim;
namespace fs = std::filesystem;

namespace {

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::format("{} failed (hr=0x{:08X})", what, uint32_t(hr)));
}

// ---- small math (column vectors, matrices stored as 4 columns) -------------------------------
struct V3 { float x, y, z; };
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) { float l = std::sqrt(dot(a, a)); return l > 0 ? a * (1.0f / l) : a; }
struct M4 { float4 c[4]{}; };  // c[j] = column j
M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r.c[j][i] = a.c[0][i] * b.c[j][0] + a.c[1][i] * b.c[j][1] + a.c[2][i] * b.c[j][2] + a.c[3][i] * b.c[j][3];
    return r;
}

float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 0x400), int(e) - 25);
    return s ? -v : v;
}
float srgb_encode(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// ---- D3D12 helpers --------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    uint64_t fence_value = 0;
    HANDLE event = nullptr;
    std::vector<ComPtr<ID3D12Resource>> keep;  // uploads kept alive until the frame is done
    std::string adapter;

    void init(bool warp) {
        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> ad;
        if (warp) {
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "EnumWarpAdapter");
        } else {
            check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)),
                  "EnumAdapterByGpuPreference");
        }
        DXGI_ADAPTER_DESC1 desc{};
        ad->GetDesc1(&desc);
        for (wchar_t* p = desc.Description; *p; ++p) adapter += char(*p < 128 ? *p : '?');
        check(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
              "CreateCommandList");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    void flush() {
        check(list->Close(), "Close");
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++fence_value), "Signal");
        check(fence->SetEventOnCompletion(fence_value, event), "SetEventOnCompletion");
        WaitForSingleObject(event, INFINITE);
        check(device->GetDeviceRemovedReason(), "GPU execution (device removed)");
        keep.clear();
        check(alloc->Reset(), "alloc Reset");
        check(list->Reset(alloc.Get(), nullptr), "list Reset");
    }

    ComPtr<ID3D12Resource> buffer(size_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{heap};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<size_t>(size, 256);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
              "CreateCommittedResource(buffer)");
        return r;
    }

    // Upload-heap buffer with contents; fine for a one-shot headless frame.
    ComPtr<ID3D12Resource> upload(const void* data, size_t size) {
        auto r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        check(r->Map(0, nullptr, &p), "Map");
        std::memcpy(p, data, size);
        r->Unmap(0, nullptr);
        return r;
    }

    void barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        list->ResourceBarrier(1, &b);
    }

    ComPtr<ID3D12Resource> texture(const Texture& t) {
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
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&r)),
              "CreateCommittedResource(texture)");

        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(t.mip_count);
        std::vector<UINT> rows(t.mip_count);
        std::vector<UINT64> row_bytes(t.mip_count);
        UINT64 total = 0;
        device->GetCopyableFootprints(&rd, 0, t.mip_count, 0, fp.data(), rows.data(), row_bytes.data(), &total);
        auto up = buffer(size_t(total), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uint8_t* p = nullptr;
        check(up->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map");
        size_t src = 0;
        for (uint32_t m = 0; m < t.mip_count; ++m) {
            for (UINT y = 0; y < rows[m]; ++y) {
                std::memcpy(p + fp[m].Offset + size_t(y) * fp[m].Footprint.RowPitch, t.data.data() + src, size_t(row_bytes[m]));
                src += size_t(row_bytes[m]);
            }
        }
        up->Unmap(0, nullptr);
        for (uint32_t m = 0; m < t.mip_count; ++m) {
            D3D12_TEXTURE_COPY_LOCATION dst{r.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION s{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            s.PlacedFootprint = fp[m];
            list->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
        }
        barrier(r.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        keep.push_back(up);
        return r;
    }
};

// ---- PNG output via WIC -----------------------------------------------------------------------
void write_png(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    ComPtr<IWICImagingFactory> f;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC factory");
    ComPtr<IWICStream> stream;
    check(f->CreateStream(&stream), "CreateStream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> enc;
    check(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "CreateEncoder");
    check(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(enc->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    check(frame->Initialize(nullptr), "frame Initialize");
    check(frame->SetSize(w, h), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    check(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    check(frame->WritePixels(h, w * 4, UINT(rgba.size()), const_cast<uint8_t*>(rgba.data())), "WritePixels");
    check(frame->Commit(), "frame Commit");
    check(enc->Commit(), "encoder Commit");
}

struct Image {
    uint32_t w = 0, h = 0;
    std::vector<float4> px;
    float4 at(uint32_t x, uint32_t y) const { return px[size_t(y) * w + x]; }
};

std::vector<uint8_t> to_rgba8(const Image& img, auto&& fn) {
    std::vector<uint8_t> out(size_t(img.w) * img.h * 4);
    for (size_t i = 0; i < img.px.size(); ++i) {
        float4 c = fn(img.px[i], i);
        for (int k = 0; k < 4; ++k) out[i * 4 + k] = uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
    }
    return out;
}

// ---- Mesh preparation -------------------------------------------------------------------------
struct DrawItem {
    StaticMeshGroup group;
    StaticMeshPart part;
    Technique tech;
};

struct Material {
    std::vector<uint8_t> vs, ps;
    std::vector<float4> cb0;
    std::map<uint32_t, ComPtr<ID3D12Resource>> textures;
    TfxResult vm;
};

float snorm16(const uint8_t* p) { int16_t v; std::memcpy(&v, p, 2); return std::max(float(v) / 32767.0f, -1.0f); }

int run(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path install;
    uint32_t width = 1600, height = 1000;
    bool warp = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf_s(args[++i].c_str(), "%ux%u", &width, &height);
        else if (args[i] == "--warp") warp = true;
        else pos.push_back(args[i]);
    }
    if (pos.empty()) {
        std::println(stderr, "usage: nimshade [--install DIR] STATIC_TAG [OUT_DIR] [--size WxH] [--warp]");
        return 2;
    }
    if (install.empty()) {
        auto found = PackageManager::find_install();
        if (!found) { std::println(stderr, "Marathon install not found; pass --install DIR"); return 2; }
        install = *found;
    }
    TagHash mesh_tag(uint32_t(std::stoul(pos[0], nullptr, 16)));
    fs::path out_dir = pos.size() >= 2 ? fs::path(pos[1]) : fs::path("shade_" + mesh_tag.str());
    fs::create_directories(out_dir);

    auto t0 = std::chrono::steady_clock::now();
    PackageManager pm(install);
    pm.build_hash64_index();
    StaticMesh mesh = load_static_mesh(pm, mesh_tag);
    auto layouts = load_input_layouts(pm);

    // G-buffer groups at the highest detail LOD present.
    std::vector<DrawItem> items;
    uint8_t best_lod = 0xFF;
    for (const auto& g : mesh.groups)
        if (g.render_stage == 0) best_lod = std::min(best_lod, mesh.parts.at(g.part_index).lod_category);
    for (size_t g = 0; g < mesh.groups.size(); ++g) {
        const auto& gr = mesh.groups[g];
        if (gr.render_stage != 0 || g >= mesh.techniques.size()) continue;
        const auto& part = mesh.parts.at(gr.part_index);
        if (part.lod_category != best_lod) continue;
        Technique t = load_technique(pm, mesh.techniques[g]);
        if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
        items.push_back({gr, part, std::move(t)});
    }
    if (items.empty()) throw std::runtime_error("mesh has no G-buffer groups with VS+PS");
    std::println("mesh {}: {} G-buffer group(s) at lod {}", mesh_tag.str(), items.size(), best_lod);

    const auto& bufs = mesh.buffers.at(items[0].part.buffer_index);
    IndexBuffer ib = load_index_buffer(pm, bufs.index);
    VertexBuffer vb0 = load_vertex_buffer(pm, bufs.vertex0);
    VertexBuffer vb1 = bufs.vertex1.is_none() ? VertexBuffer{} : load_vertex_buffer(pm, bufs.vertex1);

    // Bounds of the drawn geometry, in the same space the VS produces (pos * scale + offset).
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& it : items) {
        for (uint32_t i = 0; i < it.part.index_count; ++i) {
            size_t ii = it.part.index_start + i;
            uint32_t v = ib.is_32bit ? reinterpret_cast<const uint32_t*>(ib.data.data())[ii]
                                     : reinterpret_cast<const uint16_t*>(ib.data.data())[ii];
            if (!ib.is_32bit && v == 0xFFFF) continue;
            const uint8_t* p = vb0.data.data() + size_t(v) * vb0.stride;
            V3 q{snorm16(p) * mesh.mesh_scale + mesh.mesh_offset[0], snorm16(p + 2) * mesh.mesh_scale + mesh.mesh_offset[1],
                 snorm16(p + 4) * mesh.mesh_scale + mesh.mesh_offset[2]};
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
    }
    V3 center = (lo + hi) * 0.5f;
    float radius = std::sqrt(dot(hi - lo, hi - lo)) * 0.5f;
    std::println("bounds ({:.2f},{:.2f},{:.2f}) - ({:.2f},{:.2f},{:.2f}), radius {:.2f}", lo.x, lo.y, lo.z, hi.x, hi.y,
                 hi.z, radius);

    Gpu gpu;
    gpu.init(warp);
    std::println("adapter: {}", gpu.adapter);

    // ---- Camera (Tiger is Z-up). Reversed-Z infinite perspective, depth cleared to 0. ----
    const float fov_y = 40.0f * 3.14159265f / 180.0f, aspect = float(width) / float(height);
    const V3 view_dir = normalize(V3{-0.75f, 1.0f, -0.55f});  // looking from front-right, slightly above
    float dist = radius / std::sin(fov_y * 0.5f) * 0.82f;
    V3 eye = center - view_dir * dist;
    V3 fwd = view_dir, right = normalize(cross(fwd, V3{0, 0, 1})), up = cross(right, fwd);
    const float near_z = std::max(0.01f, dist - radius * 2.0f) * 0.5f;
    M4 view_rot;  // rows: right, up, fwd
    view_rot.c[0] = {right.x, up.x, fwd.x, 0};
    view_rot.c[1] = {right.y, up.y, fwd.y, 0};
    view_rot.c[2] = {right.z, up.z, fwd.z, 0};
    view_rot.c[3] = {0, 0, 0, 1};
    const float fy = 1.0f / std::tan(fov_y * 0.5f), fx = fy / aspect;
    M4 proj;
    proj.c[0] = {fx, 0, 0, 0};
    proj.c[1] = {0, fy, 0, 0};
    proj.c[2] = {0, 0, 0, 1};
    proj.c[3] = {0, 0, near_z, 0};
    M4 vp_rel = mul(proj, view_rot);  // camera-relative positions
    M4 tr;
    tr.c[0] = {1, 0, 0, 0}; tr.c[1] = {0, 1, 0, 0}; tr.c[2] = {0, 0, 1, 0}; tr.c[3] = {-eye.x, -eye.y, -eye.z, 1};
    M4 vp_abs = mul(vp_rel, tr);      // absolute world positions

    // cb12 "scope_view" (464 bytes): regs 0-3 = view-projection columns (absolute), 7 = camera
    // position, 19 = projection translation for camera-relative positions, 20-23 = previous frame.
    std::vector<float4> cb12(32);
    for (int j = 0; j < 4; ++j) cb12[j] = vp_abs.c[j];
    cb12[7] = {eye.x, eye.y, eye.z, 1};
    cb12[19] = vp_rel.c[3];
    for (int j = 0; j < 4; ++j) cb12[20 + j] = vp_abs.c[j];

    // t2 instance data: one 80-byte record (3x4 rows, translation in w; +60 = mesh block offset),
    // then the mesh block: (offset.xyz, scale), (uv scale, uv offset).
    std::vector<uint8_t> inst(128, 0);
    float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(inst.data(), rows, sizeof(rows));
    uint32_t block = 80;
    std::memcpy(inst.data() + 60, &block, 4);
    float mb[8] = {mesh.mesh_offset[0], mesh.mesh_offset[1], mesh.mesh_offset[2], mesh.mesh_scale,
                   mesh.texcoord_scale, mesh.texcoord_offset[0], mesh.texcoord_offset[1], 0};
    std::memcpy(inst.data() + 80, mb, sizeof(mb));

    // ---- Root signature: b0/b12 root CBVs, t2 root SRV (VS), t0-t19 table (PS), static samplers.
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {12, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor = {2, 0};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[16]{};
    for (UINT s = 0; s < 16; ++s) {
        auto& d = samplers[s];
        d.Filter = D3D12_FILTER_ANISOTROPIC;
        d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        d.MaxAnisotropy = 8;
        d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        d.MaxLOD = D3D12_FLOAT32_MAX;
        d.ShaderRegister = s;
        d.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{4, params, 16, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> rs_blob, rs_err;
    check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err), "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root_sig;
    check(gpu.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(), IID_PPV_ARGS(&root_sig)),
          "CreateRootSignature");

    // ---- Render targets: 4 x RGBA16F G-buffer + D32 depth.
    const DXGI_FORMAT rt_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 4};
    check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "rtv heap");
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
    check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)), "dsv heap");
    const UINT rtv_inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const UINT srv_inc = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ComPtr<ID3D12Resource> rts[4], depth;
    for (int i = 0; i < 4; ++i) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = rt_format; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{rt_format, {0, 0, 0, 0}};
        check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                  IID_PPV_ARGS(&rts[i])),
              "render target");
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * rtv_inc;
        gpu.device->CreateRenderTargetView(rts[i].Get(), nullptr, h);
    }
    {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{DXGI_FORMAT_D32_FLOAT};
        cv.DepthStencil = {0.0f, 0};
        check(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                  IID_PPV_ARGS(&depth)),
              "depth");
        gpu.device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
    }
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(20 * items.size()), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    check(gpu.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)), "srv heap");

    // Fallback for slots the material leaves empty (engine-provided textures we do not have).
    Texture grey;
    grey.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    grey.width = grey.height = 1;
    grey.data = {128, 128, 128, 255};
    auto fallback = gpu.texture(grey);

    // ---- Geometry buffers.
    auto ib_res = gpu.upload(ib.data.data(), ib.data.size());
    auto vb0_res = gpu.upload(vb0.data.data(), vb0.data.size());
    auto vb1_res = vb1.data.empty() ? nullptr : gpu.upload(vb1.data.data(), vb1.data.size());
    uint32_t instance_index = 0;
    auto inst_vb = gpu.upload(&instance_index, 4);
    auto t2_res = gpu.upload(inst.data(), inst.size());
    auto cb12_res = gpu.upload(cb12.data(), cb12.size() * sizeof(float4));

    // Externs the material bytecode reads that only the engine provides. Extern 0x61 (97) feeds
    // world-space detail-layer transforms with distance fades; push its fade ranges out of reach so
    // the shader takes its base-material path, and keep the divisions finite.
    TfxEnvironment env;
    env.extern_vec4 = [](uint8_t id, uint8_t off) -> float4 {
        if (id == 0x61 && (off == 0x06 || off == 0x10 || off == 0x1A)) return {1e9f, 2e9f, 0, 0};
        return {0, 0, 0, 0};
    };

    std::vector<Material> mats(items.size());
    std::vector<ComPtr<ID3D12PipelineState>> psos(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        auto& m = mats[i];
        const auto& vs_stage = it.tech.stage(ShaderStage::Vertex);
        const auto& ps_stage = it.tech.stage(ShaderStage::Pixel);
        m.vs = load_shader_bytecode(pm, vs_stage.shader);
        m.ps = load_shader_bytecode(pm, ps_stage.shader);
        m.cb0.assign(ps_stage.cbuffer_init.begin(), ps_stage.cbuffer_init.end());
        m.cb0.resize(std::max<size_t>(m.cb0.size(), 128));
        if (!ps_stage.bytecode.empty()) m.vm = run_tfx(ps_stage.bytecode, ps_stage.constants, m.cb0, env);
        for (auto& v : m.cb0)
            for (auto& c : v)
                if (!std::isfinite(c)) c = 0;
        std::println("group part {}: technique {} VS {} PS {}  cb0 {} regs, vm {} instr, {} material textures",
                     it.group.part_index, it.tech.tag.str(), vs_stage.shader.str(), ps_stage.shader.str(),
                     ps_stage.cbuffer_init.size(), m.vm.instructions, ps_stage.textures.size());
        for (const auto& u : m.vm.unsupported) std::println("  stubbed TFX op: {}", u);

        for (const auto& tb : ps_stage.textures) {
            try {
                Texture tx = load_texture(pm, tb.texture);
                if (tx.depth != 1 || tx.array_size != 1) { std::println("  t{} {}: not 2D, using fallback", tb.slot, tb.texture.str()); continue; }
                m.textures[tb.slot] = gpu.texture(tx);
                std::println("  t{:<2} {} format {} {}x{} mips {}", tb.slot, tb.texture.str(), tx.dxgi_format, tx.width,
                             tx.height, tx.mip_count);
            } catch (const std::exception& ex) {
                std::println("  t{} {}: {}", tb.slot, tb.texture.str(), ex.what());
            }
        }
        for (UINT s = 0; s < 20; ++s) {
            auto itx = m.textures.find(s);
            ID3D12Resource* r = itx != m.textures.end() ? itx->second.Get() : fallback.Get();
            D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = rd.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = rd.MipLevels;
            D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += (i * 20 + s) * srv_inc;
            gpu.device->CreateShaderResourceView(r, &sd, h);
        }

        // Input layout straight from the game's vertex layout table.
        const auto& layout = layouts.at(it.group.input_layout);
        std::vector<D3D12_INPUT_ELEMENT_DESC> ied;
        for (const auto& e : layout) {
            D3D12_INPUT_ELEMENT_DESC d{};
            d.SemanticName = e.semantic;
            d.SemanticIndex = e.semantic_index;
            d.Format = DXGI_FORMAT(e.dxgi_format);
            d.InputSlot = e.buffer;
            d.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
            d.InputSlotClass = e.instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d.InstanceDataStepRate = e.instanced ? 1 : 0;
            ied.push_back(d);
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_sig.Get();
        pd.VS = {m.vs.data(), m.vs.size()};
        pd.PS = {m.ps.data(), m.ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        for (int r = 1; r < 4; ++r) pd.BlendState.RenderTarget[r].RenderTargetWriteMask = 0xF;
        pd.BlendState.IndependentBlendEnable = FALSE;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        pd.InputLayout = {ied.data(), UINT(ied.size())};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 4;
        for (int r = 0; r < 4; ++r) pd.RTVFormats[r] = rt_format;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        check(gpu.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&psos[i])), "CreateGraphicsPipelineState (game DXIL)");
    }

    std::vector<ComPtr<ID3D12Resource>> cb0_res;
    for (auto& m : mats) cb0_res.push_back(gpu.upload(m.cb0.data(), m.cb0.size() * sizeof(float4)));

    // ---- Draw.
    auto& cl = gpu.list;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    for (int i = 0; i < 4; ++i) {
        rtv[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv[i].ptr += size_t(i) * rtv_inc;
        const float clear[4] = {0, 0, 0, 0};
        cl->ClearRenderTargetView(rtv[i], clear, 0, nullptr);
    }
    auto dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    cl->OMSetRenderTargets(4, rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp{0, 0, float(width), float(height), 0, 1};
    D3D12_RECT sc{0, 0, LONG(width), LONG(height)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(root_sig.Get());
    ID3D12DescriptorHeap* heaps[] = {srv_heap.Get()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(1, cb12_res->GetGPUVirtualAddress());
    cl->SetGraphicsRootShaderResourceView(2, t2_res->GetGPUVirtualAddress());
    D3D12_INDEX_BUFFER_VIEW ibv{ib_res->GetGPUVirtualAddress(), UINT(ib.data.size()),
                                ib.is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
    cl->IASetIndexBuffer(&ibv);
    D3D12_VERTEX_BUFFER_VIEW vbv[3]{};
    vbv[0] = {vb0_res->GetGPUVirtualAddress(), UINT(vb0.data.size()), vb0.stride};
    if (vb1_res) vbv[1] = {vb1_res->GetGPUVirtualAddress(), UINT(vb1.data.size()), vb1.stride};
    vbv[2] = {inst_vb->GetGPUVirtualAddress(), 4, 4};
    cl->IASetVertexBuffers(0, 3, vbv);
    for (size_t i = 0; i < items.size(); ++i) {
        const auto& part = items[i].part;
        cl->SetPipelineState(psos[i].Get());
        cl->SetGraphicsRootConstantBufferView(0, cb0_res[i]->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE table = srv_heap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += i * 20 * srv_inc;
        cl->SetGraphicsRootDescriptorTable(3, table);
        cl->IASetPrimitiveTopology(part.primitive_type == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawIndexedInstanced(part.index_count, 1, part.index_start, 0, 0);
    }

    // ---- Read back.
    D3D12_RESOURCE_DESC rd = rts[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback[4];
    for (int i = 0; i < 4; ++i) {
        readback[i] = gpu.buffer(size_t(total), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu.barrier(rts[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{readback[i].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{rts[i].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    gpu.flush();
    auto t1 = std::chrono::steady_clock::now();

    Image gb[4];
    for (int i = 0; i < 4; ++i) {
        gb[i].w = width;
        gb[i].h = height;
        gb[i].px.resize(size_t(width) * height);
        uint8_t* p = nullptr;
        check(readback[i]->Map(0, nullptr, reinterpret_cast<void**>(&p)), "Map readback");
        for (uint32_t y = 0; y < height; ++y) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(p + size_t(y) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < width; ++x)
                for (int c = 0; c < 4; ++c) gb[i].px[size_t(y) * width + x][c] = half_to_float(row[x * 4 + c]);
        }
        readback[i]->Unmap(0, nullptr);
    }

    // Coverage: RT1 (packed normal) is never all-zero where the PS ran.
    auto covered = [&](size_t i) { const auto& n = gb[1].px[i]; return n[0] + n[1] + n[2] > 0.0f; };
    size_t pixels = 0;
    for (size_t i = 0; i < gb[1].px.size(); ++i) pixels += covered(i);
    std::println("rendered {} pixels covered ({:.1f}%) in {:.2f}s", pixels, 100.0 * pixels / gb[1].px.size(),
                 std::chrono::duration<double>(t1 - t0).count());
    if (!pixels) throw std::runtime_error("nothing was rendered");

    auto bg = [&](size_t i) {
        float t = float(i / width) / float(height);
        return float4{0.16f + 0.10f * t, 0.17f + 0.10f * t, 0.19f + 0.10f * t, 1};
    };
    // RT0: albedo (linear) in rgb.
    write_png(out_dir / "gbuffer0_albedo.png", width, height, to_rgba8(gb[0], [&](float4 c, size_t i) {
        return covered(i) ? float4{srgb_encode(c[0]), srgb_encode(c[1]), srgb_encode(c[2]), 1} : bg(i);
    }));
    // RT1: normal * (0.375 + smoothness / 8) + 0.5.
    write_png(out_dir / "gbuffer1_normal.png", width, height, to_rgba8(gb[1], [&](float4 c, size_t i) {
        return covered(i) ? float4{c[0], c[1], c[2], 1} : bg(i);
    }));
    write_png(out_dir / "gbuffer2.png", width, height, to_rgba8(gb[2], [&](float4 c, size_t i) {
        return covered(i) ? float4{c[0], c[1], c[2], 1} : bg(i);
    }));

    // Composite: decode the packed normal and smoothness, light with a sun + sky term.
    const V3 sun = normalize(V3{0.35f, -0.55f, 0.75f});
    double smooth_sum = 0;
    write_png(out_dir / "composite.png", width, height, to_rgba8(gb[0], [&](float4 a, size_t i) {
        if (!covered(i)) return bg(i);
        const float4 e = gb[1].px[i];
        V3 v{e[0] - 0.5f, e[1] - 0.5f, e[2] - 0.5f};
        float len = std::sqrt(dot(v, v));
        V3 n = len > 0 ? v * (1.0f / len) : V3{0, 0, 1};
        float smooth = std::clamp((len - 0.375f) * 8.0f, 0.0f, 1.0f);
        smooth_sum += smooth;
        uint32_t px = uint32_t(i % width), py = uint32_t(i / width);
        // view ray for this pixel
        float ndc_x = (px + 0.5f) / width * 2 - 1, ndc_y = 1 - (py + 0.5f) / height * 2;
        V3 ray = normalize(fwd + right * (ndc_x / fx) + up * (ndc_y / fy));
        V3 h = normalize(sun - ray);
        float ndl = std::max(dot(n, sun), 0.0f);
        float sky = 0.5f + 0.5f * n.z;
        float power = std::exp2(smooth * 10.0f + 1.0f);
        float spec = (power + 8.0f) / 25.0f * std::pow(std::max(dot(n, h), 0.0f), power) * ndl;
        float fres = 0.04f + 0.96f * std::pow(1.0f - std::max(dot(n, ray * -1.0f), 0.0f), 5.0f);
        float4 out{};
        for (int c = 0; c < 3; ++c) {
            float lin = a[c] * (ndl * 2.6f + sky * 0.45f + 0.05f) + spec * fres * smooth * 0.6f;
            lin = lin / (1.0f + lin);  // Reinhard
            out[c] = srgb_encode(lin * 1.25f);
        }
        out[3] = 1;
        return out;
    }));
    std::println("mean smoothness {:.3f}", smooth_sum / double(pixels));
    std::println("wrote {}", fs::absolute(out_dir).string());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        return run(argc, argv);
    } catch (const std::exception& ex) {
        std::println(stderr, "error: {}", ex.what());
        return 1;
    }
}
