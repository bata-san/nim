#include "nim/render/scene.hpp"
#include "nim/core/profile.hpp"
#include "nim/content/map.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/object_channels.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>
#include <tuple>

namespace nim {

// ---- math -----------------------------------------------------------------------------------------
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : a;
}
M4 mul(const M4& a, const M4& b) {
    M4 r;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r.c[j][i] = a.c[0][i] * b.c[j][0] + a.c[1][i] * b.c[j][1] + a.c[2][i] * b.c[j][2] + a.c[3][i] * b.c[j][3];
    return r;
}

ViewMatrices make_view(const View& v) {
    ViewMatrices m;
    m.eye = v.eye;
    m.fwd = normalize(v.target - v.eye);
    V3 world_up{0, 0, 1};
    if (std::fabs(dot(m.fwd, world_up)) > 0.999f) world_up = {0, 1, 0};
    m.right = normalize(cross(m.fwd, world_up));
    m.up = cross(m.right, m.fwd);
    M4 rot;  // rows: right, up, fwd (view space: x right, y up, z forward)
    rot.c[0] = {m.right.x, m.up.x, m.fwd.x, 0};
    rot.c[1] = {m.right.y, m.up.y, m.fwd.y, 0};
    rot.c[2] = {m.right.z, m.up.z, m.fwd.z, 0};
    rot.c[3] = {0, 0, 0, 1};
    const float aspect = float(v.width) / float(std::max(1u, v.height));
    m.fy = 1.0f / std::tan(v.fov_y * 0.5f);
    m.fx = m.fy / aspect;
    M4 proj;  // reversed-Z, infinite far plane: depth = near / z
    proj.c[0] = {m.fx, 0, 0, 0};
    proj.c[1] = {0, m.fy, 0, 0};
    // The jitter moves the image by whole-NDC offsets: clip.w is the view depth.
    proj.c[2] = {2 * v.jitter_x / float(std::max(1u, v.width)), -2 * v.jitter_y / float(std::max(1u, v.height)), 0, 1};
    proj.c[3] = {0, 0, v.near_z, 0};
    m.proj = proj;
    m.vp_rel = mul(proj, rot);
    M4 tr;
    tr.c[0] = {1, 0, 0, 0};
    tr.c[1] = {0, 1, 0, 0};
    tr.c[2] = {0, 0, 1, 0};
    tr.c[3] = {-v.eye.x, -v.eye.y, -v.eye.z, 1};
    m.vp_abs = mul(m.vp_rel, tr);
    m.vp_prev = m.vp_abs;
    return m;
}

void SunShadow::resize(Gpu& gpu, uint32_t n) {
    if (n == size && depth) return;
    size = n;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = n;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_D32_FLOAT};
    cv.DepthStencil.Depth = 0;
    depth.Reset();
    check_hr(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                                   IID_PPV_ARGS(&depth)),
             "sun shadow map");
    if (!dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap)), "sun shadow DSV heap");
    }
    dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    gpu.device()->CreateDepthStencilView(depth.Get(), &dd, dsv);
}

void SunShadow::fit(V3 sun_dir, V3 center, float radius) {
    vm = {};
    vm.fwd = normalize(V3{-sun_dir.x, -sun_dir.y, -sun_dir.z});
    V3 world_up{0, 0, 1};
    if (std::fabs(dot(vm.fwd, world_up)) > 0.999f) world_up = {0, 1, 0};
    vm.right = normalize(cross(vm.fwd, world_up));
    vm.up = cross(vm.right, vm.fwd);
    half = std::max(0.05f, radius);
    // Casters well outside the covered sphere (towers, overhangs) still reach it.
    const float back = radius + std::max(50.0f, radius * 4);
    range = back + radius;
    const float texel = 2 * half / float(std::max(1u, size));
    const float u = std::floor(dot(center, vm.right) / texel) * texel, v = std::floor(dot(center, vm.up) / texel) * texel;
    const float w = dot(center, vm.fwd);
    vm.eye = vm.right * u + vm.up * v + vm.fwd * (w - back);
    vm.ortho = half;
    vm.fx = vm.fy = 1 / half;
    M4 rot;
    rot.c[0] = {vm.right.x, vm.up.x, vm.fwd.x, 0};
    rot.c[1] = {vm.right.y, vm.up.y, vm.fwd.y, 0};
    rot.c[2] = {vm.right.z, vm.up.z, vm.fwd.z, 0};
    rot.c[3] = {0, 0, 0, 1};
    M4 proj;  // reversed depth: 1 at the sun's eye, 0 at `range`
    proj.c[0] = {1 / half, 0, 0, 0};
    proj.c[1] = {0, 1 / half, 0, 0};
    proj.c[2] = {0, 0, -1 / range, 0};
    proj.c[3] = {0, 0, 1, 1};
    vm.proj = proj;
    vm.vp_rel = mul(proj, rot);
    M4 tr;
    tr.c[0] = {1, 0, 0, 0};
    tr.c[1] = {0, 1, 0, 0};
    tr.c[2] = {0, 0, 1, 0};
    tr.c[3] = {-vm.eye.x, -vm.eye.y, -vm.eye.z, 1};
    vm.vp_abs = mul(vm.vp_rel, tr);
}

void SunShadow::fit_view(V3 sun_dir, V3 eye, V3 focus, V3 scene_center, float scene_radius) {
    const V3 d = focus - eye;
    const float reach = std::clamp(std::sqrt(dot(d, d)) * 0.9f, 40.0f, 300.0f);
    if (scene_radius <= reach) fit(sun_dir, scene_center, scene_radius * 1.05f);
    else fit(sun_dir, focus, reach);
}

void RainOcclusion::resize(Gpu& gpu, uint32_t n) {
    if (n == top.size && mask) return;
    top.resize(gpu, n);
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = rd.Height = n;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    mask.Reset();
    check_hr(gpu.device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                   nullptr, IID_PPV_ARGS(&mask)),
             "rain occlusion mask");
    if (srv_ == UINT32_MAX) srv_ = gpu.srv().alloc(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    gpu.device()->CreateShaderResourceView(top.depth.Get(), &sd, gpu.srv().cpu(srv_));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_FLOAT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    gpu.device()->CreateUnorderedAccessView(mask.Get(), nullptr, &ud, gpu.srv().cpu(srv_ + 1));
    valid = false;
}

void RainOcclusion::release(Gpu& gpu) {
    if (srv_ != UINT32_MAX) gpu.srv().free(srv_, 2);
    srv_ = UINT32_MAX;
    mask.Reset();
    top = SunShadow{};
    valid = false;
}

void RainOcclusion::resolve(Gpu& gpu, ID3D12GraphicsCommandList* cl) {
    ID3D12Device* dev = gpu.device();
    if (!pso_) {
        static constexpr const char* kHlsl = R"(
Texture2D<float> top : register(t0);
RWTexture2D<float> mask : register(u0);
[numthreads(8, 8, 1)] void main(uint2 id : SV_DispatchThreadID) { mask[id] = 1 - top[id]; }
)";
        ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), "nim_rain_occlusion.hlsl", nullptr, nullptr, "main", "cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
            throw std::runtime_error(std::format("rain occlusion shader: {}", err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed"));
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable = {2, ranges};
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rs{1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob;
        check_hr(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), "rain occlusion root signature");
        check_hr(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_)), "rain occlusion root signature");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = root_.Get();
        pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        check_hr(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_)), "rain occlusion pipeline");
    }
    Gpu::barrier(cl, top.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, mask.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetComputeRootSignature(root_.Get());
    cl->SetPipelineState(pso_.Get());
    cl->SetComputeRootDescriptorTable(0, gpu.srv().gpu(srv_));
    cl->Dispatch((top.size + 7) / 8, (top.size + 7) / 8, 1);
    Gpu::barrier(cl, mask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, top.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

View frame_sphere(V3 center, float radius, uint32_t w, uint32_t h, float yaw, float pitch) {
    View v;
    v.width = w;
    v.height = h;
    v.target = center;
    v.fov_y = 40.0f * 3.14159265f / 180.0f;
    const float aspect = float(w) / float(std::max(1u, h));
    const float half = std::min(v.fov_y, 2.0f * std::atan(std::tan(v.fov_y * 0.5f) * aspect)) * 0.5f;
    const float dist = radius / std::sin(half) * 0.9f;
    V3 dir{std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), std::sin(pitch)};
    v.eye = center + dir * dist;
    v.near_z = std::max(0.001f, radius * 0.01f);
    return v;
}

// ---- GBuffer --------------------------------------------------------------------------------------
void GBuffer::release(Gpu& gpu) {
    if (srv_first != UINT32_MAX) gpu.srv().free(srv_first, 6);
    srv_first = UINT32_MAX;
    for (auto& r : rt) r.Reset();
    depth.Reset();
    lit.Reset();
    w_ = h_ = 0;
}

void GBuffer::resize(Gpu& gpu, uint32_t w, uint32_t h) {
    w = std::max(1u, w);
    h = std::max(1u, h);
    if (w == w_ && h == h_ && lit) return;
    release(gpu);
    w_ = w;
    h_ = h;
    ID3D12Device* dev = gpu.device();
    if (!rtv_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 6};
        check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "rtv heap");
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
        check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)), "dsv heap");
    }
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_first = gpu.srv().alloc(6);
    for (int i = 0; i < 4; ++i) {
        D3D12_CLEAR_VALUE cv{kFormats[i], {0, 0, 0, 0}};
        rt[i] = gpu.texture2d(w, h, kFormats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
        rtv[i] = {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * inc};
        dev->CreateRenderTargetView(rt[i].Get(), nullptr, rtv[i]);
        dev->CreateShaderResourceView(rt[i].Get(), nullptr, gpu.srv().cpu(srv_first + i));
    }
    D3D12_CLEAR_VALUE dc{kDepthFormat};
    dc.DepthStencil = {0.0f, 0};
    depth = gpu.texture2d(w, h, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc);
    dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{kDepthFormat, D3D12_DSV_DIMENSION_TEXTURE2D};
    dev->CreateDepthStencilView(depth.Get(), &dd, dsv);
    dsv_ro = {dsv.ptr + dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
    dd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    dev->CreateDepthStencilView(depth.Get(), &dd, dsv_ro);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = kDepthSrvFormat;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(depth.Get(), &sd, gpu.srv().cpu(srv_first + 4));

    D3D12_CLEAR_VALUE lc{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
    lit = gpu.texture2d(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &lc);
    lit_rtv = {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(4) * inc};
    dev->CreateRenderTargetView(lit.Get(), nullptr, lit_rtv);
    null_rtv = {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(5) * inc};
    D3D12_RENDER_TARGET_VIEW_DESC unused{};
    unused.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    unused.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    dev->CreateRenderTargetView(nullptr, &unused, null_rtv);  // unused MRT slots, no backing texture
    dev->CreateShaderResourceView(lit.Get(), nullptr, gpu.srv().cpu(srv_first + 5));
}

// ---- StaticMeshScene ------------------------------------------------------------------------------
namespace {

float snorm16(const uint8_t* p) {
    int16_t v;
    std::memcpy(&v, p, 2);
    return std::max(float(v) / 32767.0f, -1.0f);
}

// G-buffer root signature (the same parameter order for static meshes and dynamic models):
//   0 PS b0 (material cbuffer)     1 VS b12 view       2 VS t2 instance data (static, root SRV)
//                                                        or VS t0-t31 table (dynamic: colour, skin, VS textures)
//   3 PS t0-t31 table              4 b1 rigid_model    5 b13 frame
//   6 VS b0 (the VS stage cbuffer) 7 VS t0-t1 + t3-t31 table (static meshes only)
// Shared cbuffers have distinct VS/PS root parameters. Static VS textures use parameter 30.
UINT scope_root_parameter(ShaderStage stage, uint32_t reg) {
    const bool vs = stage == ShaderStage::Vertex;
    if (reg == 0) return vs ? 6 : 0;
    if (reg == 12) return vs ? 1 : 7;
    if (reg == 1) return vs ? 4 : 8;
    if (reg == 13) return vs ? 5 : 9;
    if (reg >= 2 && reg <= 11) return (vs ? 10 : 20) + reg - 2;
    throw std::runtime_error("Original material scope register exceeds supported root layout");
}

ComPtr<ID3D12RootSignature> make_gbuffer_root_signature(ID3D12Device* dev, bool dynamic = false,
    const std::map<uint32_t, SamplerDesc>& vs_samplers = {}, const std::map<uint32_t, SamplerDesc>& ps_samplers = {}) {
    D3D12_DESCRIPTOR_RANGE ps_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE vs_dynamic{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE vs_static[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0},
                                         {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 29, 3, 0, 3}};
    std::array<D3D12_ROOT_PARAMETER, 31> params{};
    for (auto stage : {ShaderStage::Vertex, ShaderStage::Pixel})
        for (UINT reg = 0; reg <= 13; ++reg) {
            auto& param = params[scope_root_parameter(stage, reg)];
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor = {reg, 0};
            param.ShaderVisibility = stage == ShaderStage::Vertex ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
        }
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    if (dynamic) {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &vs_dynamic};
    } else {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = {2, 0};
        params[30].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[30].DescriptorTable = {2, vs_static};
        params[30].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &ps_range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    for (bool vs : {true, false}) {
        for (const auto& [reg, source] : vs ? vs_samplers : ps_samplers) {
            D3D12_STATIC_SAMPLER_DESC desc{};
            desc.Filter = D3D12_FILTER(source.filter);
            desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(source.address_u);
            desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(source.address_v);
            desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE(source.address_w);
            desc.MipLODBias = source.mip_lod_bias;
            desc.MaxAnisotropy = source.max_anisotropy;
            desc.ComparisonFunc = D3D12_COMPARISON_FUNC(source.comparison_func);
            desc.MinLOD = source.min_lod;
            desc.MaxLOD = source.max_lod;
            desc.ShaderRegister = reg;
            desc.ShaderVisibility = vs ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;
            const float4 border{source.border_color[0], source.border_color[1], source.border_color[2], source.border_color[3]};
            if (border == float4{0, 0, 0, 0}) desc.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            else if (border == float4{0, 0, 0, 1}) desc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
            else if (border == float4{1, 1, 1, 1}) desc.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
            else throw std::runtime_error("Original sampler requires dynamic border-color support");
            samplers.push_back(desc);
        }
    }
    D3D12_ROOT_SIGNATURE_DESC desc{dynamic ? 30u : 31u, params.data(), UINT(samplers.size()), samplers.data(),
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, errors;
    check_hr(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "Original material root signature");
    ComPtr<ID3D12RootSignature> signature;
    check_hr(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)), "Original material root signature");
    return signature;
}

// The engine's blend states for render targets 0..4 (from goriath's dump of the state table; indexed
// by the technique's blend state byte): enable, src, dst, op, src alpha, dst alpha, op alpha, write mask.
struct BlendRow { uint8_t enable, src, dst, op, src_a, dst_a, op_a, mask; };
constexpr BlendRow kBlendStates[90][5] = {
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}},
    {{1,2,2,1,2,2,1,15}, {1,2,2,1,2,2,1,15}, {1,2,2,1,2,2,1,15}, {1,2,2,1,2,2,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,9,1,1,7,1,1,15}, {1,9,1,1,7,1,1,15}, {1,9,1,1,7,1,1,15}, {1,9,1,1,7,1,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,9,3,1,7,5,1,15}, {1,9,3,1,7,5,1,15}, {1,9,3,1,7,5,1,15}, {1,9,3,1,7,5,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,2,5,1,15}, {1,2,5,1,2,5,1,15}, {1,2,5,1,2,5,1,15}, {1,2,5,1,2,5,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,7,1,1,15}, {1,2,5,1,7,1,1,15}, {1,2,5,1,7,1,1,15}, {1,2,5,1,7,1,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,7,2,1,7,1,1,15}, {1,7,2,1,7,1,1,15}, {1,7,2,1,7,1,1,15}, {1,7,2,1,7,1,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,6,1,2,6,1,15}, {1,2,6,1,2,6,1,15}, {1,2,6,1,2,6,1,15}, {1,2,6,1,2,6,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,2,4,2,2,4,15}, {1,2,2,4,2,2,4,15}, {1,2,2,4,2,2,4,15}, {1,2,2,4,2,2,4,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,2,5,2,2,5,15}, {1,2,2,5,2,2,5,15}, {1,2,2,5,2,2,5,15}, {1,2,2,5,2,2,5,15}, {1,5,6,1,5,6,1,15}},
    {{1,9,2,1,7,2,1,15}, {1,9,2,1,7,2,1,15}, {1,9,2,1,7,2,1,15}, {1,9,2,1,7,2,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,5,6,1,5,6,1,15}, {1,5,6,1,5,6,1,15}, {1,5,6,1,5,6,1,15}, {1,5,6,1,5,6,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,14,15,1,14,15,1,15}, {1,14,15,1,14,15,1,15}, {1,14,15,1,14,15,1,15}, {1,14,15,1,14,15,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,6,5,1,6,5,1,15}, {1,6,5,1,6,5,1,15}, {1,6,5,1,6,5,1,15}, {1,6,5,1,6,5,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,7,8,1,2,1,1,15}, {1,7,8,1,2,1,1,15}, {1,7,8,1,2,1,1,15}, {1,7,8,1,2,1,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,5,6,1,5,6,1,15}},
    {{0,2,1,1,2,1,1,1}, {0,2,1,1,2,1,1,1}, {0,2,1,1,2,1,1,1}, {0,2,1,1,2,1,1,1}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,2}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,1}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,5}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,2}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,1}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,3}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,1}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,2}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,1}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,2}, {1,2,5,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,5}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,5}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,5}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,5}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,1}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,5}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,6}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{1,9,1,1,1,2,1,7}, {1,9,1,1,1,2,1,0}, {1,9,1,1,1,2,1,0}, {1,9,1,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{1,9,3,1,1,2,1,7}, {1,9,3,1,1,2,1,0}, {1,9,3,1,1,2,1,0}, {1,9,3,1,1,2,1,0}, {1,5,6,1,5,6,1,15}},
    {{0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{1,7,2,1,1,2,1,7}, {1,7,2,1,1,2,1,7}, {1,7,2,1,1,2,1,7}, {1,7,2,1,1,2,1,7}, {1,5,6,1,5,6,1,15}},
    {{1,8,2,1,1,2,1,7}, {1,8,2,1,1,2,1,7}, {1,8,2,1,1,2,1,7}, {1,8,2,1,1,2,1,7}, {1,5,6,1,5,6,1,15}},
    {{1,1,7,1,1,2,1,7}, {1,1,7,1,1,2,1,7}, {1,1,7,1,1,2,1,7}, {1,1,7,1,1,2,1,7}, {1,5,6,1,5,6,1,15}},
    {{1,1,8,1,1,2,1,7}, {1,1,8,1,1,2,1,7}, {1,1,8,1,1,2,1,7}, {1,1,8,1,1,2,1,7}, {1,5,6,1,5,6,1,15}},
    {{1,1,6,1,2,6,1,15}, {1,1,6,1,2,6,1,15}, {1,1,6,1,2,6,1,15}, {1,1,6,1,2,6,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,6,1,2,6,1,7}, {1,2,6,1,2,6,1,7}, {1,2,6,1,2,6,1,7}, {1,2,6,1,2,6,1,7}, {1,5,6,1,5,6,1,15}},
    {{1,2,6,1,2,6,1,8}, {1,2,6,1,2,6,1,8}, {1,2,6,1,2,6,1,8}, {1,2,6,1,2,6,1,8}, {1,5,6,1,5,6,1,8}},
    {{1,16,17,1,18,19,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,15}},
    {{1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,2,2,1,2,2,1,15}, {1,2,2,1,2,2,1,15}, {0,2,1,1,2,1,1,15}},
    {{1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,1,6,1,1,6,1,15}, {1,1,6,1,1,6,1,15}, {1,5,6,1,5,6,1,15}},
    {{1,2,2,3,2,2,3,15}, {1,2,2,3,2,2,3,15}, {1,2,2,3,2,2,3,15}, {1,2,2,3,2,2,3,15}, {1,5,6,1,5,6,1,15}},
};

D3D12_RENDER_TARGET_BLEND_DESC blend_desc(int index, int rt = 0) {
    D3D12_RENDER_TARGET_BLEND_DESC d{};
    d.RenderTargetWriteMask = 0xF;
    d.SrcBlend = d.SrcBlendAlpha = D3D12_BLEND_ONE;
    d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_ZERO;
    d.BlendOp = d.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.LogicOp = D3D12_LOGIC_OP_NOOP;
    if (index < 0 || index >= int(std::size(kBlendStates)))
        throw std::runtime_error(std::format("Missing original blend state {}", index));
    const BlendRow& b = kBlendStates[index][rt];
    d.BlendEnable = b.enable;
    d.SrcBlend = D3D12_BLEND(b.src);
    d.DestBlend = D3D12_BLEND(b.dst);
    d.BlendOp = D3D12_BLEND_OP(b.op);
    d.SrcBlendAlpha = D3D12_BLEND(b.src_a);
    d.DestBlendAlpha = D3D12_BLEND(b.dst_a);
    d.BlendOpAlpha = D3D12_BLEND_OP(b.op_a);
    d.RenderTargetWriteMask = b.mask;
    return d;
}

// A texture SRV matching what the shader declares at that register.
void create_texture_srv(ID3D12Device* dev, ID3D12Resource* r, BindingKind kind, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    if (!r) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &sd, h);
        return;
    }
    write_game_srv(dev, {r}, kind, h);
}

std::map<uint32_t, BindingKind> srv_kinds(std::span<const uint8_t> code) {
    std::map<uint32_t, BindingKind> out;
    for (const auto& b : shader_bindings(code))
        if (b.type == BindingType::Srv && b.space == 0)
            for (uint32_t r = b.lower; r <= b.upper && r < 32; ++r) out[r] = b.kind;
    return out;
}

void create_buffer_srv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, UINT elements, D3D12_CPU_DESCRIPTOR_HANDLE h,
                       uint64_t first_element = 0) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.FirstElement = first_element;
    sd.Buffer.NumElements = std::max<UINT>(1, elements);
    dev->CreateShaderResourceView(r, &sd, h);
}
// An RGBA8 / R32 typed view of an arena range (offsets are 256-aligned, so element-aligned).
void create_buffer_srv(ID3D12Device* dev, const GpuBuffer& b, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    create_buffer_srv(dev, b.resource.Get(), fmt, UINT(b.size / 4), h, b.offset / 4);
}


} // namespace

D3D12_RENDER_TARGET_BLEND_DESC engine_blend_desc(int index, int rt) { return blend_desc(index, rt); }

StaticMeshScene::StaticMeshScene() = default;
StaticMeshScene::~StaticMeshScene() = default;

const std::map<std::string, std::string>& StaticMeshScene::estimates() const {
    static const std::map<std::string, std::string> none;
    return material_externs_ ? material_externs_->estimates() : none;
}

void StaticMeshScene::set_render_inputs(const ExternStore& inputs) {
    if (!material_externs_) throw std::runtime_error("No scene is loaded");
    *material_externs_ = inputs;
    global_base_ = inputs.channels;
    if (inputs.has(ext::Frame, 0x10, 4)) material_time_of_day_ = inputs.get_f(ext::Frame, 0x10);
}

void StaticMeshScene::set_material_frame(float seconds, float delta_seconds) {
    material_time_ = std::isfinite(seconds) ? std::max(0.0f, seconds) : 0;
    material_delta_ = std::isfinite(delta_seconds) ? std::max(0.0f, delta_seconds) : 0;
}

void StaticMeshScene::release(Gpu& gpu) {
    // A failed upload leaves texture copies recorded on the immediate list; run them while their
    // resources are still alive, or the next flush page-faults the GPU (device removed).
    if (!items_.empty() || !sets_.empty()) {
        try {
            gpu.flush();
        } catch (...) {
        }
    }
    for (auto& it : items_)
        if (it.owns_srv) gpu.srv().free(it.srv_first, kItemSrvs * it.srv_copies);
    items_.clear();
    table_cache_.clear();
    scopes_resolved_ = false;
    scope_defs_.clear();
    cull_groups_.clear();
    instance_total_ = 0;
    texture_cache_.clear();
    root_cache_.clear();
    pipeline_cache_.clear();
    info_.clear();
    transparent_count_ = 0;
    channels_.clear();
    channel_overrides_.clear();
    channel_defaults_.clear();
    material_addresses_.clear();
    material_time_ = material_delta_ = 0;
    material_time_of_day_ = 0.5f;
    white_color_ = {};
    arena_.reset();
    material_externs_.reset();
    sets_.clear();
    notes_.clear();
    dynamic_ = dynamic_parts_ = false;
    skinned_ = skin_dirty_ = false;
    skin_pose_.clear();
    tag_ = TagHash();
}

namespace {

// Shaders, material cbuffer (the PS stage's TFX over its init values) and textures of one draw.
// Channel inputs must come from component-authored defaults or an explicitly supplied object state.
struct ChannelContext {
    const ChannelValues* overrides = nullptr;
    std::map<uint32_t, ChannelUse>* uses = nullptr;
    const ChannelValues* defaults = nullptr;
    const ExternStore* estimates = nullptr;  // where an unauthored channel's estimate is reported
    // The object's world position (its placement's translation), for interpolated_world_position.
    const float4* world_position = nullptr;
};
// fnv1("interpolated_world_position"): where the object is. Materials rotate / scale their
// vertices about it; unset, the pivot is somewhere else and the object swings around it.
constexpr uint32_t kInterpolatedWorldPosition = 0xA7A7FE43;
bool reads_object_position(const TechniqueStage& stage) {
    return disassemble_tfx(stage.bytecode).find("push_object_channel A7 A7 FE 43") != std::string::npos;
}
TfxEnvironment with_channels(const TfxEnvironment& base, const ChannelContext& cc, bool vs) {
    TfxEnvironment env = base;
    env.object_channel = [cc, vs](uint32_t h) {
        const float4* value = nullptr;
        if (cc.defaults)
            if (auto it = cc.defaults->find(h); it != cc.defaults->end()) value = &it->second;
        if (cc.overrides)
            if (auto it = cc.overrides->find(h); it != cc.overrides->end()) value = &it->second;
        static const float4 unset{1, 1, 1, 1};
        if (!value && h == kInterpolatedWorldPosition && cc.world_position) value = cc.world_position;
        if (!value) {
            // No component default and no override: the channel is set at run time by gameplay.
            if (!cc.estimates) throw std::runtime_error(std::format("Missing engine object channel {:08X}", h));
            cc.estimates->note_estimate(std::format("object channel {:08X} ({}) = 1", h, object_channel_name(h) ? object_channel_name(h) : "unnamed"),
                                        "run-time object state with no authored default: 1 (goriath's unset channel)");
            value = &unset;
        }
        const float4 v = *value;
        if (cc.uses) {
            ChannelUse& u = (*cc.uses)[h];
            if (vs) u.vs = true;
            else u.ps = true;
            if (!vs || !u.ps) u.value = v;
        }
        return v;
    };
    return env;
}

void prepare_material_inputs(PackageManager& pm, PreparedMesh& mesh, const ChannelValues* channels) {
    if (channels) mesh.channel_overrides = *channels;
    auto globals = load_render_globals(pm);
    auto global_channels = std::make_shared<std::vector<float4>>(std::move(globals.channel_defaults));
    auto scopes = std::make_shared<std::vector<Scope>>();
    scopes->reserve(globals.scopes.size());
    for (const auto& [name, tag] : globals.scopes) scopes->push_back(load_scope(pm, tag));
    mesh.global_channels = std::move(global_channels);
    mesh.scopes = std::move(scopes);

}

struct TextureCache {
    std::unordered_map<uint32_t, std::shared_ptr<const Texture>> local;
    PrepareCache* shared = nullptr;
};
std::shared_ptr<const Texture> cached_texture(PackageManager& pm, TagHash tag, TextureCache& cache) {
    ProfileScope profile("prepare.texture_cache");
    if (cache.shared) {
        auto& map = cache.shared->textures;
        std::shared_lock lk(*cache.shared->texture_mutex);
        if (auto it = map.find(tag.value); it != map.end()) {
            Profile::instance().count("prepare.texture_cache_hits");
            return it->second;
        }
        lk.unlock();
        Profile::instance().count("prepare.texture_cache_misses");
        auto texture = std::make_shared<const Texture>(load_texture(pm, tag, cache.shared->max_texture_dim));
        std::unique_lock write(*cache.shared->texture_mutex);
        return map.emplace(tag.value, texture).first->second;
    }
    if (auto it = cache.local.find(tag.value); it != cache.local.end()) return it->second;
    auto texture = std::make_shared<const Texture>(load_texture(pm, tag));
    return cache.local.emplace(tag.value, texture).first->second;
}

PreparedMesh::Group build_group(PackageManager& pm, const Technique& tech, const StaticMeshPart& part,
                                uint16_t part_index, uint8_t input_layout,
                                TextureCache& texture_cache, std::shared_ptr<const std::vector<Scope>> available_scopes) {
    PreparedMesh::Group g;
    g.part = part;
    g.input_layout = input_layout;
    g.blend = tech.blend_state();
    const auto& vs_stage = tech.stage(ShaderStage::Vertex);
    const auto& ps_stage = tech.stage(ShaderStage::Pixel);
    g.vs_material = vs_stage;
    g.ps_material = ps_stage;
    std::vector<const Scope*> scope_ptrs;
    const auto& scopes = *available_scopes;
    for (size_t bit = 0; bit < 64; ++bit) {
        if (!(tech.used_scopes & (uint64_t{1} << bit))) continue;
        if (bit >= scopes.size()) throw std::runtime_error("Unknown original scope index");
        scope_ptrs.push_back(&scopes[bit]);
    }
    g.scopes = std::move(available_scopes);
    g.vs_sources = resolve_stage_resources(tech, ShaderStage::Vertex, scope_ptrs);
    g.ps_sources = resolve_stage_resources(tech, ShaderStage::Pixel, scope_ptrs);
    for (const auto& [slot, tag] : g.vs_sources.samplers) g.vs_samplers[slot] = load_sampler(pm, tag);
    for (const auto& [slot, tag] : g.ps_sources.samplers) g.ps_samplers[slot] = load_sampler(pm, tag);
    auto& info = g.info;
    info.part = part_index;
    info.index_count = part.index_count;
    info.technique = tech.tag;
    info.vs = vs_stage.shader;
    info.ps = ps_stage.shader;
    g.vs = load_shader_bytecode(pm, vs_stage.shader);
    g.ps = load_shader_bytecode(pm, ps_stage.shader);

    g.cb0.assign(ps_stage.cbuffer_init.begin(), ps_stage.cbuffer_init.end());
    info.cbuffer_regs = g.cb0.size();
    g.cb0.resize(std::max<size_t>(g.cb0.size(), 128));
    if (!ps_stage.bytecode.empty()) {
        TfxResult r = inspect_tfx_bindings(ps_stage.bytecode);
        info.vm_instructions = r.instructions;
        info.stubbed_ops = r.unsupported;
        info.extern_textures = r.extern_textures;
        g.ps_live = true;
    }

    for (const auto& [slot, tag] : g.ps_sources.textures) {
        const TextureBinding tb{slot, tag};
        SceneTextureInfo ti{tb.slot, tb.texture, 0, 0, 0, 0, {}};
        auto tx = cached_texture(pm, tb.texture, texture_cache);
        ti.format = tx->dxgi_format;
        ti.width = tx->width;
        ti.height = tx->height;
        ti.mips = tx->mip_count;
        g.textures[tb.slot] = std::move(tx);
        info.textures.push_back(std::move(ti));
    }

    // The VS stage has its own cbuffer and textures (vertex animation: wind, noise volumes).
    g.vs_cb0.assign(vs_stage.cbuffer_init.begin(), vs_stage.cbuffer_init.end());
    g.vs_cb0.resize(std::max<size_t>(g.vs_cb0.size(), 128));
    if (!vs_stage.bytecode.empty()) {
        const auto result = inspect_tfx_bindings(vs_stage.bytecode);
        g.vs_live = true;
        for (const auto& note : result.unsupported)
            info.stubbed_ops.push_back("VS: " + note);
    }
    for (const auto& [slot, tag] : g.vs_sources.textures)
        g.vs_textures[slot] = cached_texture(pm, tag, texture_cache);
    // A missing sampler must not silently select an anisotropic or linear preset.
    for (auto [code, samplers] : {std::pair{&g.vs, &g.vs_samplers}, std::pair{&g.ps, &g.ps_samplers}})
        for (const auto& binding : shader_bindings(*code))
            if (binding.type == BindingType::Sampler)
                for (uint32_t reg = binding.lower; reg <= binding.upper; ++reg)
                    if (!samplers->contains(reg))
                        throw std::runtime_error(std::format("{}: missing original sampler s{}", tech.tag.str(), reg));
    return g;
}

// Grows [lo, hi] by the snorm16 positions (x scale + offset) the part's indices reach.
void grow_bounds(const IndexBuffer& ib, const VertexBuffer& vb0, const StaticMeshPart& part, V3 scale, V3 offset,
                 V3& lo, V3& hi) {
    for (uint32_t i = 0; i < part.index_count; ++i) {
        size_t ii = part.index_start + i;
        if ((ii + 1) * (ib.is_32bit ? 4 : 2) > ib.data.size()) break;
        uint32_t v = ib.is_32bit ? reinterpret_cast<const uint32_t*>(ib.data.data())[ii]
                                 : reinterpret_cast<const uint16_t*>(ib.data.data())[ii];
        if (!ib.is_32bit && v == 0xFFFF) continue;
        if ((size_t(v) + 1) * vb0.stride > vb0.data.size()) continue;
        const uint8_t* p = vb0.data.data() + size_t(v) * vb0.stride;
        V3 q{snorm16(p) * scale.x + offset.x, snorm16(p + 2) * scale.y + offset.y, snorm16(p + 4) * scale.z + offset.z};
        lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
        hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
    }
}

int32_t quant_snorm(float v, int bits) {
    const int32_t denom = 1 << (bits - 1);
    v = std::clamp(v, -1.0f, 0.999999f);
    return std::clamp(int32_t(std::lround(v * float(denom))), -denom, denom - 1);
}
std::pair<float, float> oct_encode(V3 v) {
    const float sum = std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z);
    if (sum <= 1e-12f) return {0.0f, 0.0f};
    float x = v.x / sum, y = v.y / sum;
    if (v.z / sum < 0) {
        const float ox = x;
        x = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f);
        y = (1 - std::fabs(ox)) * (y >= 0 ? 1.0f : -1.0f);
    }
    return {x, y};
}
float s16(const uint8_t* p) { return float(*reinterpret_cast<const int16_t*>(p)) / 32767.0f; }

// One vertex in the layout the game's compute skinning writes: `p` is the packed position
// ((position - offset) / range, in [-1, 1]); normal and tangent go octahedral.
void pack_skin_vertex(V3 p, V3 normal, V3 tangent, uint32_t* pos, uint32_t* nrm) {
    const int32_t x = quant_snorm(p.x, 21), y = quant_snorm(p.y, 21), z = quant_snorm(p.z, 22);
    pos[0] = (uint32_t(x) & 0x1FFFFF) | ((uint32_t(y) & 0x7FF) << 21);
    pos[1] = ((uint32_t(y) >> 11) & 0x3FF) | ((uint32_t(z) & 0x3FFFFF) << 10);
    auto [nu, nv] = oct_encode(normal);
    auto [tu, tv] = oct_encode(tangent);
    nrm[0] = (uint32_t(quant_snorm(nv, 16)) << 16) | (uint32_t(quant_snorm(nu, 16)) & 0xFFFF);
    nrm[1] = (uint32_t(quant_snorm(tv, 16)) << 16) | (uint32_t(quant_snorm(tu, 16)) & 0xFFFF);
}

// Packed positions of skinned meshes span twice the model's half-extent, so posed vertices that
// leave the bind-pose box still fit.
constexpr float kSkinRange = 2.0f;

// The bind pose in the layout the game's compute skinning writes (24-byte layout-7 vertices).
bool pack_skin_streams(const VertexBuffer& vb0, V3 scale, float range, std::vector<uint32_t>& pos,
                       std::vector<uint32_t>& nrm) {
    if (vb0.stride != 24 || range <= 1e-8f || vb0.data.size() % 24) return false;
    const size_t n = vb0.data.size() / 24;
    pos.resize(n * 2);
    nrm.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* v = vb0.data.data() + i * 24;
        pack_skin_vertex({s16(v) * scale.x / range, s16(v + 2) * scale.y / range, s16(v + 4) * scale.z / range},
                         {s16(v + 8), s16(v + 10), s16(v + 12)}, {s16(v + 16), s16(v + 18), s16(v + 20)}, &pos[i * 2], &nrm[i * 2]);
    }
    return true;
}

// Render stages nim draws: 0 GenerateGbuffer, 8 Transparents.
constexpr uint8_t kDrawnStages[] = {0, 8};
constexpr uint8_t kStageTransparent = 8;

bool vs_reads_typed_buffers(const std::vector<uint8_t>& vs) {
    for (const auto& b : shader_bindings(vs))
        if (b.type == BindingType::Srv && (b.kind == BindingKind::TypedBuffer || b.kind == BindingKind::RawBuffer) &&
            b.lower >= 2 && b.lower <= 4)
            return true;
    return false;
}

} // namespace

PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials,
                                   int permutation, const ChannelValues* channels, PrepareCache* cache) {
    PreparedMesh out;
    if (cache) {
        std::lock_guard lk(*cache->mutex);
        if (!cache->ready) {
            PreparedMesh first;
            prepare_material_inputs(pm, first, nullptr);
            cache->global_channels = std::move(first.global_channels);
            cache->scopes = std::move(first.scopes);
            cache->ready = true;
        }
        if (channels) out.channel_overrides = *channels;
        out.global_channels = cache->global_channels;
        out.scopes = cache->scopes;
    } else {
        prepare_material_inputs(pm, out, channels);
    }
    out.tag = tag;
    out.dynamic = true;
    TextureCache texture_cache;
    texture_cache.shared = cache;
    uint32_t perm = 0;
    if (materials) {
        out.channel_defaults = materials->object_defaults;
        perm = permutation < 0 ? materials->default_permutation : uint32_t(permutation);
        out.component = materials->component;
        out.permutation = perm;
        out.permutation_count = materials->permutation_count;
    }
    DynamicModel model = load_dynamic_model(pm, tag);
    const V3 scale{model.model_scale[0], model.model_scale[1], model.model_scale[2]};
    const V3 offset{model.model_offset[0], model.model_offset[1], model.model_offset[2]};
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    uint8_t lod = 0xFF;
    bool has_gbuffer_parts = false;
    // Use the same selected render stages as static meshes. In particular the map's
    // opaque path must not accidentally enable entity death/spawn/energy FX.
    const bool gbuffer_only = cache && cache->gbuffer_only;
    auto drawn = [gbuffer_only](uint8_t st) { return st == 0 || (!gbuffer_only && st == kStageTransparent); };
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const DynamicMesh& mesh = model.meshes[mi];
        bool any_stage = false;
        for (uint8_t st : kDrawnStages) if (drawn(st)) any_stage |= mesh.part_range[st] < std::min<size_t>(mesh.part_range[st + 1], mesh.parts.size());
        if (!any_stage) continue;
        has_gbuffer_parts = true;
        PreparedMesh::Buffers bufs;
        try {
            bufs.ib = load_index_buffer(pm, mesh.index);
            bufs.vb0 = load_vertex_buffer(pm, mesh.vertex0);
            if (!mesh.vertex1.is_none()) bufs.vb1 = load_vertex_buffer(pm, mesh.vertex1);
            if (!mesh.color.is_none()) {
                try {
                    bufs.color = load_vertex_buffer(pm, mesh.color).data;
                } catch (...) {
                }
            }
        } catch (const std::exception& ex) {
            out.notes.push_back(std::format("mesh {}: buffers: {}", mi, ex.what()));
            continue;
        }
        const float s1 = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
        const bool packed =
            !mesh.skinning.is_none() && pack_skin_streams(bufs.vb0, scale, s1 * kSkinRange, bufs.skin_pos, bufs.skin_normal);
        if (packed) {
            try {
                bufs.skin = decode_skin_weights(bufs.vb0, load_vertex_buffer(pm, mesh.skinning));
            } catch (const std::exception& ex) {
                out.notes.push_back(std::format("mesh {}: skin weights: {}", mi, ex.what()));
            }
        }
        const uint32_t set = uint32_t(out.buffers.size());
        bool used = false;
        for (uint8_t stage : kDrawnStages) {
        if (!drawn(stage)) continue;
        const size_t first = mesh.part_range[stage], last = std::min<size_t>(mesh.part_range[stage + 1], mesh.parts.size());
        // Highest detail: lod_category bit 0 (goriath); fall back to every part when none has it.
        bool any_high = false;
        for (size_t k = first; k < last; ++k) any_high |= (mesh.parts[k].lod_category & 1) != 0;
        for (size_t k = first; k < last; ++k) {
            const DynamicMeshPart& dp = mesh.parts[k];
            if (any_high && !(dp.lod_category & 1)) continue;
            // The entity's material variant wins over the part's own technique (goriath binds both,
            // the variant last).
            TagHash tech = materials ? materials->technique(dp.variant_shader_index, perm) : TagHash();
            if (tech.is_none()) tech = dp.technique;
            if (tech.is_none()) {
                if (dp.variant_shader_index != 0xFFFF)
                    out.notes.push_back(std::format("mesh {} part {}: material variant {} needs the entity component", mi, k,
                                                    dp.variant_shader_index));
                continue;
            }
            if (dp.primitive_type != 3 && dp.primitive_type != 5) continue;
            StaticMeshPart part{dp.index_start, dp.index_count, uint8_t(mi), dp.lod_category, dp.primitive_type};
            try {
                Technique t = load_technique(pm, tech);
                if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
                PreparedMesh::Group g = build_group(pm, t, part, uint16_t(k), mesh.input_layout[stage], texture_cache, out.scopes);
                g.stage = stage;
                if (vs_reads_typed_buffers(g.vs) && !packed) {
                    out.notes.push_back(std::format("mesh {} part {}: skinning shader, but no 24-byte vertex stream to pack", mi, k));
                    continue;
                }
                g.buffers = set;
                grow_bounds(bufs.ib, bufs.vb0, part, scale, offset, lo, hi);
                lod = std::min(lod, dp.lod_category);
                out.groups.push_back(std::move(g));
                used = true;
            } catch (const std::exception& ex) {
                out.notes.push_back(std::format("mesh {} part {}: {}", mi, k, ex.what()));
            }
        }
        }
        if (used) out.buffers.push_back(std::move(bufs));
    }
    if (out.groups.empty() && !has_gbuffer_parts) {
        out.no_drawable = true;
        out.notes.push_back("no G-buffer or transparent parts; other render stages are not drawable in this viewer");
        return out;
    }
    if (!out.notes.empty()) throw std::runtime_error(out.notes.front());
    if (out.groups.empty())
        throw std::runtime_error(out.notes.empty() ? "dynamic model has no G-buffer or transparent parts (only FX / shadow / other stages)"
                                                   : out.notes.front());
    out.lod = lod;
    out.center = (lo + hi) * 0.5f;
    out.radius = std::max(0.01f, std::sqrt(dot(hi - lo, hi - lo)) * 0.5f);

    auto& cb = out.model_cb;
    cb[0] = {1, 0, 0, 0};
    cb[1] = {0, 1, 0, 0};
    cb[2] = {0, 0, 1, 0};
    cb[3] = {0, 0, 0, 1};
    cb[4] = {model.model_scale[0], model.model_scale[1], model.model_scale[2], model.model_scale[3]};
    cb[5] = {model.model_offset[0], model.model_offset[1], model.model_offset[2], model.model_offset[3]};
    cb[6] = {model.texcoord_scale[0], model.texcoord_scale[1], model.texcoord_offset[0], model.texcoord_offset[1]};
    // Dynamic SH / AO must come from the real object lighting scope, never a guessed 0.8.
    // c8-c11 previous placement (identity before placement), c12/c13 packed-position offset + 1D scale, c15-c18 identity.
    const float s1 = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
    cb[8] = {1, 0, 0, 0}; cb[9] = {0, 1, 0, 0}; cb[10] = {0, 0, 1, 0}; cb[11] = {0, 0, 0, 1};
    cb[12] = cb[13] = {offset.x, offset.y, offset.z, s1 * kSkinRange};
    cb[15] = {1, 0, 0, 0}; cb[16] = {0, 1, 0, 0}; cb[17] = {0, 0, 1, 0}; cb[18] = {0, 0, 0, 1};
    return out;
}

PreparedMesh prepare_mesh(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials,
                          const ChannelValues* channels) {
    const Entry* e = pm.entry(tag);
    if (e && e->reference == kClassDynamicModel) return prepare_dynamic_model(pm, tag, materials, -1, channels);
    return prepare_static_mesh(pm, tag, channels);
}

PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash tag, const ChannelValues* channels, PrepareCache* cache) {
    ProfileScope profile_total("prepare.static.total");
    PreparedMesh out;
    if (cache) {
        std::lock_guard lk(*cache->mutex);
        if (!cache->ready) {
            PreparedMesh first;
            prepare_material_inputs(pm, first, nullptr);
            cache->global_channels = std::move(first.global_channels);
            cache->scopes = std::move(first.scopes);
            cache->ready = true;
        }
        if (channels) out.channel_overrides = *channels;
        out.global_channels = cache->global_channels;
        out.scopes = cache->scopes;
    } else {
        prepare_material_inputs(pm, out, channels);
    }
    out.tag = tag;
    ProfileScope profile_mesh("prepare.static.mesh_decode");
    StaticMesh mesh = load_static_mesh(pm, tag);
    profile_mesh.stop();
    TextureCache texture_cache;
    texture_cache.shared = cache;

    struct Pending { StaticMeshGroup group; StaticMeshPart part; Technique tech; };
    const bool gbuffer_only = cache && cache->gbuffer_only;
    auto drawn = [gbuffer_only](uint8_t st) { return st == 0 || (!gbuffer_only && st == kStageTransparent); };
    std::vector<Pending> pending;
    uint8_t lod = 0xFF;
    for (const auto& g : mesh.groups)
        if (drawn(g.render_stage)) lod = std::min(lod, mesh.parts.at(g.part_index).lod_category);
    // lod_category is a mask of LOD levels (1 = LOD0, 2 = LOD1, 4 = LOD2 ...). Every level shares
    // the mesh's buffers; with all_lods each part keeps its mask.
    const bool all_lods = cache && cache->all_lods;
    int buffer = -1;
    for (size_t g = 0; g < mesh.groups.size(); ++g) {
        const auto& gr = mesh.groups[g];
        if (!drawn(gr.render_stage) || g >= mesh.techniques.size()) continue;
        const auto& part = mesh.parts.at(gr.part_index);
        if (all_lods ? (part.lod_category & 0x0F) == 0 : part.lod_category != lod) continue;
        if (buffer >= 0 && part.buffer_index != buffer) continue;
        Technique t = load_technique(pm, mesh.techniques[g]);
        if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
        buffer = part.buffer_index;
        pending.push_back({gr, part, std::move(t)});
    }
    if (pending.empty()) throw std::runtime_error("mesh has no G-buffer or transparent groups with a vertex + pixel shader");
    out.lod = lod;
    if (all_lods) out.lod_levels = 0;

    ProfileScope profile_buffers("prepare.static.buffers_bounds");
    const auto& bufs = mesh.buffers.at(pending[0].part.buffer_index);
    out.buffers.emplace_back();
    auto& set = out.buffers.back();
    set.ib = load_index_buffer(pm, bufs.index);
    set.vb0 = load_vertex_buffer(pm, bufs.vertex0);
    if (!bufs.vertex1.is_none()) set.vb1 = load_vertex_buffer(pm, bufs.vertex1);
    if (!bufs.color.is_none()) set.color = load_vertex_buffer(pm, bufs.color).data;  // per-vertex RGBA8 (VS t0)
    const IndexBuffer& ib = set.ib;
    const VertexBuffer& vb0 = set.vb0;

    // Bounds of the drawn geometry, in the space the VS produces (pos * scale + offset).
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    const uint8_t detail = all_lods ? uint8_t(pending[0].part.lod_category & 0x0F) : 0;
    uint8_t lowest = 0x0F;
    for (const auto& it : pending) lowest = std::min<uint8_t>(lowest, it.part.lod_category & 0x0F);
    const uint8_t lowest_bit = uint8_t(lowest & (~lowest + 1));
    (void)detail;
    for (const auto& it : pending)
        if (!all_lods || (it.part.lod_category & lowest_bit))
        for (uint32_t i = 0; i < it.part.index_count; ++i) {
            size_t ii = it.part.index_start + i;
            if ((ii + 1) * (ib.is_32bit ? 4 : 2) > ib.data.size()) break;
            uint32_t v = ib.is_32bit ? reinterpret_cast<const uint32_t*>(ib.data.data())[ii]
                                     : reinterpret_cast<const uint16_t*>(ib.data.data())[ii];
            if (!ib.is_32bit && v == 0xFFFF) continue;
            if ((size_t(v) + 1) * vb0.stride > vb0.data.size()) continue;
            const uint8_t* p = vb0.data.data() + size_t(v) * vb0.stride;
            V3 q{snorm16(p) * mesh.mesh_scale + mesh.mesh_offset[0], snorm16(p + 2) * mesh.mesh_scale + mesh.mesh_offset[1],
                 snorm16(p + 4) * mesh.mesh_scale + mesh.mesh_offset[2]};
            lo = {std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)};
            hi = {std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z)};
        }
    out.center = (lo + hi) * 0.5f;
    out.radius = std::max(0.01f, std::sqrt(dot(hi - lo, hi - lo)) * 0.5f);

    // t2 instance data: one 80-byte record (3x4 rows, translation in w; +60 = mesh block offset),
    // then the mesh block: (offset.xyz, scale), (uv scale, uv offset).
    out.instance.assign(128, 0);
    const float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(out.instance.data(), rows, sizeof(rows));
    const uint32_t block = 80;
    std::memcpy(out.instance.data() + 60, &block, 4);
    const float mb[8] = {mesh.mesh_offset[0], mesh.mesh_offset[1], mesh.mesh_offset[2], mesh.mesh_scale,
                         mesh.texcoord_scale, mesh.texcoord_offset[0], mesh.texcoord_offset[1], 0};
    std::memcpy(out.instance.data() + 80, mb, sizeof(mb));

    profile_buffers.stop();
    ProfileScope profile_groups("prepare.static.build_groups");
    for (auto& p : pending) {
        out.groups.push_back(build_group(pm, p.tech, p.part, p.group.part_index, p.group.input_layout, texture_cache, out.scopes));
        out.groups.back().stage = p.group.render_stage;
        out.groups.back().lod_mask = all_lods ? uint8_t(p.part.lod_category & 0x0F) : 1;
        if (all_lods) out.lod_levels |= out.groups.back().lod_mask;
    }
    return out;
}

PreparedMesh prepare_terrain(PackageManager& pm, const Terrain& terrain, const std::vector<std::vector<InputElement>>& layouts,
                             PrepareCache* cache) {
    ProfileScope profile_total("prepare.terrain.total");
    PreparedMesh out;
    if (cache) {
        std::lock_guard lk(*cache->mutex);
        if (!cache->ready) {
            PreparedMesh first;
            prepare_material_inputs(pm, first, nullptr);
            cache->global_channels = std::move(first.global_channels);
            cache->scopes = std::move(first.scopes);
            cache->ready = true;
        }
        out.global_channels = cache->global_channels;
        out.scopes = cache->scopes;
    } else {
        prepare_material_inputs(pm, out, nullptr);
    }
    out.tag = terrain.tag;
    out.buffers.emplace_back();
    auto& set = out.buffers.back();
    set.ib = load_index_buffer(pm, terrain.index);
    set.vb0 = load_vertex_buffer(pm, terrain.vertex0);
    set.vb1 = load_vertex_buffer(pm, terrain.vertex1);
    // The game layout whose streams have exactly these strides (int16x4 position; normal + uv).
    int layout = -1;
    for (size_t l = 0; l < layouts.size() && layout < 0; ++l) {
        uint32_t stride[4]{};
        bool position = false;
        for (const auto& e : layouts[l]) {
            if (e.buffer < 4) stride[e.buffer] += e.size;
            position |= _stricmp(e.semantic, "POSITION") == 0 && e.buffer == 0;
            if (e.instanced) stride[3] += 1000;
        }
        if (position && stride[0] == set.vb0.stride && stride[1] == set.vb1.stride && !stride[2] && !stride[3] &&
            layouts[l].size() == 3)
            layout = int(l);
    }
    if (layout < 0) throw std::runtime_error(std::format("terrain {}: no game input layout for strides {}/{}", terrain.tag.str(),
                                                         set.vb0.stride, set.vb1.stride));
    TextureCache texture_cache;
    texture_cache.shared = cache;
    for (size_t i = 0; i < terrain.parts.size(); ++i) {
        const auto& tp = terrain.parts[i];
        if (tp.detail != 0 || tp.group >= terrain.groups.size() || tp.technique.is_none()) continue;
        Technique t = load_technique(pm, tp.technique);
        if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
        StaticMeshPart part{tp.index_start, tp.index_count, 0, 1, 5};  // strips
        out.groups.push_back(build_group(pm, t, part, uint16_t(i), uint8_t(layout), texture_cache, out.scopes));
        auto& g = out.groups.back();
        const auto& group = terrain.groups[tp.group];
        // PS t14: the group's dyemap (goriath binds it there; the PS declares t14).
        if (!group.dyemap.is_none()) {
            g.textures[14] = cached_texture(pm, group.dyemap, texture_cache);
            g.ps_sources.textures[14] = group.dyemap;
        }
        g.own_cbuffers[{ShaderStage::Vertex, 11}] = {
            float4{terrain.offset[0], terrain.offset[1], terrain.offset[2], terrain.offset[3]},
            float4{group.texcoord[0], group.texcoord[1], group.texcoord[2], group.texcoord[3]}, float4{}, float4{}};
    }
    if (out.groups.empty()) throw std::runtime_error(std::format("terrain {}: no high-detail parts with shaders", terrain.tag.str()));
    // The VS: world = ((pos.xy + offset.xy) / 64, (pos.z + offset.z + (pos.w + offset.w) * 65536) / 8192).
    out.center = {(terrain.lo[0] + terrain.hi[0]) * 0.5f, (terrain.lo[1] + terrain.hi[1]) * 0.5f, (terrain.lo[2] + terrain.hi[2]) * 0.5f};
    const V3 ext{terrain.hi[0] - terrain.lo[0], terrain.hi[1] - terrain.lo[1], terrain.hi[2] - terrain.lo[2]};
    out.radius = std::max(0.01f, std::sqrt(dot(ext, ext)) * 0.5f);
    out.instance.assign(128, 0);
    const float rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(out.instance.data(), rows, sizeof(rows));
    const uint32_t block = 80;
    std::memcpy(out.instance.data() + 60, &block, 4);
    return out;
}

void StaticMeshScene::upload(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts) {
    begin(gpu);
    const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const TagHash tag = mesh.tag;
    const V3 center = mesh.center;
    const float radius = mesh.radius;
    add_mesh(gpu, std::move(mesh), layouts, std::span(&identity, 1));
    finish(gpu, tag, center, radius);
}

void StaticMeshScene::begin(Gpu& gpu) {
    release(gpu);
    ID3D12Device* dev = gpu.device();
    if (!root_sig_) root_sig_ = make_gbuffer_root_signature(dev);
    if (!root_sig_dynamic_) root_sig_dynamic_ = make_gbuffer_root_signature(dev, true);
}

void StaticMeshScene::finish(Gpu& gpu, TagHash tag, V3 center, float radius) {
    gpu.flush();  // texture uploads
    tag_ = tag;
    center_ = center;
    radius_ = radius;
}

void StaticMeshScene::add_mesh(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts,
                               std::span<const std::array<float, 12>> placements, std::span<const float> sky_order) {
    if (!sky_order.empty() && (!mesh.dynamic || sky_order.size() != placements.size()))
        throw std::invalid_argument("Sky order requires one key per dynamic placement");
    scopes_resolved_ = false;
    // All or nothing: a mesh that fails validation leaves no partial parts behind.
    const size_t items = items_.size(), sets = sets_.size(), infos = info_.size(), transparent = transparent_count_;
    const size_t instances = instance_total_, culls = cull_groups_.size();
    try {
        add_mesh_parts(gpu, std::move(mesh), layouts, placements, sky_order);
    } catch (...) {
        try { gpu.flush(); } catch (...) {}
        for (size_t k = items; k < items_.size(); ++k) {
            if (!items_[k].owns_srv) continue;
            gpu.srv().free(items_[k].srv_first, kItemSrvs * items_[k].srv_copies);
            std::erase_if(table_cache_, [&](const auto& e) { return e.second == items_[k].srv_first; });
        }
        items_.resize(items);
        sets_.resize(sets);
        info_.resize(infos);
        transparent_count_ = transparent;
        instance_total_ = instances;
        cull_groups_.resize(culls);
        throw;
    }
}

void StaticMeshScene::add_mesh_parts(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts,
                                     std::span<const std::array<float, 12>> placements, std::span<const float> sky_order) {
    ProfileScope profile_total("add.mesh_parts.total");
    ID3D12Device* dev = gpu.device();
    const bool first_mesh = sets_.empty();
    if (dynamic_ && !first_mesh) throw std::runtime_error("a dynamic model scene holds that model alone");
    if (mesh.dynamic && first_mesh && placements.size() != 1) throw std::runtime_error("a dynamic model view places it once");
    if (placements.empty()) throw std::runtime_error("mesh has no placements");
    const uint32_t set_base = uint32_t(sets_.size());
    if (first_mesh) {
        center_ = mesh.center;
        radius_ = mesh.radius;
        lod_ = mesh.lod;
        dynamic_ = mesh.dynamic;
        model_cb_ = mesh.model_cb;
        skin_scale_ = {mesh.model_cb[4][0], mesh.model_cb[4][1], mesh.model_cb[4][2]};
        skin_offset_ = {mesh.model_cb[13][0], mesh.model_cb[13][1], mesh.model_cb[13][2]};
        skin_range_ = mesh.model_cb[13][3] > 1e-8f ? mesh.model_cb[13][3] : 1.0f;
        notes_ = mesh.notes;
        channels_ = mesh.channels;
        channel_overrides_ = mesh.channel_overrides;
        channel_defaults_ = std::move(mesh.channel_defaults);
        material_externs_ = std::make_unique<ExternStore>();
        if (mesh.global_channels) material_externs_->channels = *mesh.global_channels;
        global_base_ = material_externs_->channels;
    } else {
        dynamic_parts_ |= mesh.dynamic;
        for (const auto& n : mesh.notes) notes_.push_back(n);
        for (auto& [h, v] : mesh.channel_defaults) channel_defaults_.emplace(h, v);
        for (auto& [h, u] : mesh.channels) channels_.emplace(h, u);
    }

    // Instance index stream (0..n-1) and t2: n 80-byte records (3x4 rows, translation in w;
    // +60 = byte offset of the mesh block), then the mesh block from the prepared single record.
    const uint32_t n = uint32_t(placements.size());
    std::vector<uint32_t> iota(n);
    for (uint32_t i = 0; i < n; ++i) iota[i] = i;
    ProfileScope profile_geometry("add.geometry_upload");
    const GpuBuffer inst = arena_.upload(gpu, iota.data(), iota.size() * 4);
    GpuBuffer t2;
    if (!mesh.dynamic) {
        if (mesh.instance.size() < 112) throw std::runtime_error("Missing original mesh instance data");
        std::vector<uint8_t> data(size_t(n) * 80 + 48, 0);
        const uint32_t block = n * 80;
        for (uint32_t i = 0; i < n; ++i) {
            std::memcpy(data.data() + size_t(i) * 80, placements[i].data(), 48);
            std::memcpy(data.data() + size_t(i) * 80 + 60, &block, 4);
        }
        std::memcpy(data.data() + block, mesh.instance.data() + 80, 32);
        t2 = arena_.upload(gpu, data.data(), data.size());
    }
    uint32_t cull = UINT32_MAX;
    if (!mesh.dynamic) {
        CullGroup group;
        group.spheres.reserve(n);
        for (const auto& m : placements) {
            const V3 c = mesh.center;
            const float scale = std::sqrt(std::max({m[0] * m[0] + m[1] * m[1] + m[2] * m[2], m[4] * m[4] + m[5] * m[5] + m[6] * m[6],
                                                    m[8] * m[8] + m[9] * m[9] + m[10] * m[10]}));
            group.spheres.push_back({m[0] * c.x + m[1] * c.y + m[2] * c.z + m[3], m[4] * c.x + m[5] * c.y + m[6] * c.z + m[7],
                                     m[8] * c.x + m[9] * c.y + m[10] * c.z + m[11], mesh.radius * scale});
        }
        group.levels = mesh.lod_levels ? mesh.lod_levels : 1;
        if (std::popcount(group.levels) > 1 && material_externs_)
            material_externs_->note_estimate("static mesh LOD per instance by screen radius (LOD1 < 160 px, LOD2 < 64 px, LOD3 < 24 px)",
                                             "engine LOD distances not decoded");
        cull = uint32_t(cull_groups_.size());
        cull_groups_.push_back(std::move(group));
    }
    instance_total_ += n;
    for (const auto& b : mesh.buffers) {
        BufferSet set;
        set.ib = arena_.upload(gpu, b.ib.data.data(), b.ib.data.size());
        set.vb0 = arena_.upload(gpu, b.vb0.data.data(), b.vb0.data.size());
        if (!b.vb1.data.empty()) set.vb1 = arena_.upload(gpu, b.vb1.data.data(), b.vb1.data.size());
        set.ibv = {set.ib.gpu, UINT(b.ib.data.size()), b.ib.is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        set.vbv[0] = {set.vb0.gpu, UINT(b.vb0.data.size()), b.vb0.stride};
        if (set.vb1) set.vbv[1] = {set.vb1.gpu, UINT(b.vb1.data.size()), b.vb1.stride};
        set.inst = inst;
        set.t2 = t2;
        set.instances = n;
        set.cull = cull;
        set.vbv[2] = {inst.gpu, n * 4, 4};
        set.dynamic = mesh.dynamic;
        if (mesh.dynamic) {
            set.model_cb = mesh.model_cb;
            set.model_center = mesh.center;
            set.placements.assign(placements.begin(), placements.end());
            set.sky_order.assign(sky_order.begin(), sky_order.end());
        }
        if (mesh.dynamic) {
            auto up = [&](const std::vector<uint32_t>& v) -> ComPtr<ID3D12Resource> {
                return v.empty() ? ComPtr<ID3D12Resource>{} : gpu.upload(v.data(), v.size() * 4);
            };
            if (first_mesh && !b.skin.bones.empty() && b.skin_pos.size() == b.skin.bones.size() * 2 && b.vb0.stride == 24) {
                // Posed on the CPU (update_skin); the first render uploads the bind pose.
                const size_t bytes = b.skin_pos.size() * 4;
                set.skin_pos = gpu.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
                set.skin_normal = gpu.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
                for (auto& u : set.skin_upload) u = gpu.buffer(bytes * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                set.vb0_cpu = b.vb0.data;
                set.skin = b.skin;
                skinned_ = skin_dirty_ = true;
            } else {
                set.skin_pos = up(b.skin_pos);
                set.skin_normal = up(b.skin_normal);
            }
        }
        if (b.color.size() >= 4) set.color = arena_.upload(gpu, b.color.data(), b.color.size() & ~size_t(3));
        sets_.push_back(std::move(set));
    }
    profile_geometry.stop();

    auto& shared_textures = texture_cache_;
    auto& shared_roots = root_cache_;
    auto& shared_pipelines = pipeline_cache_;
    auto upload_textures = [&](const auto& source, const StageResourceSources& sources, auto& destination) {
        ProfileScope profile_textures("add.texture_upload");
        for (const auto& [slot, tx] : source) {
            const TagHash tag = sources.textures.at(slot);
            auto found = shared_textures.find(tag.value);
            if (!tag.is_none() && found != shared_textures.end()) destination[slot] = found->second;
            else {
                destination[slot] = gpu.texture(*tx);
                if (!tag.is_none()) shared_textures.emplace(tag.value, destination[slot]);
            }
        }
    };
    for (auto& g : mesh.groups) {
        Item item;
        // Finish recorded copies before a failed strict validation destroys this item's assets.
        struct FlushFailedUpload {
            Gpu& gpu;
            Item& item;
            bool committed = false;
            ~FlushFailedUpload() {
                if (committed) return;
                try { gpu.flush(); } catch (...) {}
                if (item.srv_first != UINT32_MAX && item.owns_srv) gpu.srv().free(item.srv_first, kItemSrvs * item.srv_copies);
            }
        } upload_guard{gpu, item};
        item.part = g.part;
        item.dynamic = mesh.dynamic;
        item.buffers = set_base + g.buffers;
        for (const auto& [key, values] : g.own_cbuffers)
            item.own_cbvs.emplace_back(scope_root_parameter(key.first, key.second),
                                       arena_.upload(gpu, values.data(), values.size() * sizeof(float4)));
        item.own_regs = g.own_cbuffers;
        item.cb0 = arena_.upload(gpu, g.cb0.data(), g.cb0.size() * sizeof(float4));
        item.vs_cb0 = arena_.upload(gpu, g.vs_cb0.data(), g.vs_cb0.size() * sizeof(float4));
        item.vs_material = std::move(g.vs_material);
        item.ps_material = std::move(g.ps_material);
        if (const auto texture = g.textures.find(0); texture != g.textures.end() &&
            texture->second->tile_count > 1 && tfx_animated_atlas_operand(item.ps_material.bytecode)) {
            const auto& atlas = *texture->second;
            if (!std::all_of(atlas.tile_uv.begin(), atlas.tile_uv.end(), [](float v) { return v > 0 && v <= 1; }))
                throw std::runtime_error("Invalid animated texture atlas UVs");
            item.ps_atlas = Item::AnimatedAtlas{atlas.tile_uv, atlas.tile_count};
        }
        item.vs_live = g.vs_live;
        item.ps_live = g.ps_live;
        item.technique = g.info.technique;
        item.scopes = std::move(g.scopes);
        item.vs_sources = std::move(g.vs_sources);
        item.ps_sources = std::move(g.ps_sources);
        ProfileScope profile_root("add.root_signature");
        // Keyed apart for dynamic parts (another root signature layout); tags never set bit 30.
        const uint32_t root_key = g.info.technique.value ^ (mesh.dynamic ? 0x40000000u : 0u);
        auto& root = shared_roots[root_key];
        if (!root) root = make_gbuffer_root_signature(dev, mesh.dynamic, g.vs_samplers, g.ps_samplers);
        item.root_sig = root;
        profile_root.stop();
        upload_textures(g.textures, item.ps_sources, item.textures);
        upload_textures(g.vs_textures, item.vs_sources, item.vs_textures);
        for (auto [code, registers] : {std::pair{&g.vs, &item.vs_cbvs}, std::pair{&g.ps, &item.ps_cbvs}})
            for (const auto& binding : shader_bindings(*code)) {
                if (binding.space != 0) throw std::runtime_error("Original material register space is unsupported");
                if (binding.type == BindingType::Cbv) {
                    if (binding.upper != binding.lower || binding.upper > 13) throw std::runtime_error("Original material cbuffer range is unsupported");
                    registers->push_back(binding.lower);
                }
            }
        const auto ps_kinds = srv_kinds(g.ps), vs_kinds = srv_kinds(g.vs);
        item.ps_kinds = ps_kinds;
        item.vs_kinds = vs_kinds;
        auto kind_of = [](const std::map<uint32_t, BindingKind>& m, uint32_t r) {
            auto f = m.find(r);
            return f == m.end() ? BindingKind::Texture2D : f->second;
        };
        // Reject missing resources before allocating per-draw descriptor rings.
        for (const auto& [slot, kind] : ps_kinds)
            if (!item.textures.contains(slot) && !item.ps_sources.extern_textures.contains(slot))
                throw std::runtime_error(std::format("{}: missing original PS texture t{} (shared game producer not ported)", item.technique.str(), slot));
        const auto& source_buffers = sets_.at(item.buffers);
        for (const auto& [slot, kind] : vs_kinds) {
            const bool color = (slot == 0 || slot == 1) && (source_buffers.color || !mesh.dynamic);
            const bool skin = mesh.dynamic && slot >= 2 && slot <= 4 && source_buffers.skin_pos;
            const bool instance_data = !mesh.dynamic && slot == 2;  // the mesh's own t2, a root SRV
            if (!item.vs_textures.contains(slot) && !item.vs_sources.extern_textures.contains(slot) && !color && !skin &&
                !instance_data)
                throw std::runtime_error(std::format("{}: missing original VS resource t{}", item.technique.str(), slot));
        }
        // A descriptor referenced by an in-flight command list must not be rewritten.
        const bool rebinds = std::any_of(item.ps_sources.extern_textures.begin(), item.ps_sources.extern_textures.end(),
                                         [&](const auto& e) { return ps_kinds.contains(e.first); }) ||
                             std::any_of(item.vs_sources.extern_textures.begin(), item.vs_sources.extern_textures.end(),
                                         [&](const auto& e) { return vs_kinds.contains(e.first); });
        // Extern views use a ring even when a static part owns the table.
        item.srv_copies = rebinds ? kSrvCopies : 1;
        // Static parts with the same views and extern sources share one table or ring.
        std::vector<uint64_t> table_key;
        if (!mesh.dynamic) {
            for (const auto* src : {&item.ps_sources.extern_textures, &item.vs_sources.extern_textures}) {
                table_key.push_back(0xFFFFFFFFull);
                for (const auto& [slot, e] : *src) table_key.push_back(uint64_t(slot) << 16 | uint64_t(e.extern_id) << 8 | e.offset);
            }
            for (UINT s = 0; s < kPsSrvs; ++s) {
                auto f = item.textures.find(s);
                table_key.push_back(uint64_t(reinterpret_cast<uintptr_t>(f != item.textures.end() ? f->second.Get() : nullptr)));
                table_key.push_back(uint64_t(kind_of(ps_kinds, s)));
            }
            for (UINT s = 0; s < kVsSrvs; ++s) {
                auto f = item.vs_textures.find(s);
                table_key.push_back(uint64_t(reinterpret_cast<uintptr_t>(f != item.vs_textures.end() ? f->second.Get() : nullptr)));
                table_key.push_back(uint64_t(kind_of(vs_kinds, s)));
            }
            if (auto shared = table_cache_.find(table_key); shared != table_cache_.end()) {
                item.srv_first = shared->second;
                item.owns_srv = false;
            }
        }
        ProfileScope profile_descriptors("add.descriptors");
        if (item.owns_srv) {
            item.srv_first = gpu.srv().alloc(kItemSrvs * item.srv_copies);
            if (!table_key.empty()) table_cache_.emplace(table_key, item.srv_first);
        }
        for (uint32_t copy = 0; item.owns_srv && copy < item.srv_copies; ++copy) {
            const uint32_t srv_first = item.srv_first + copy * kItemSrvs;
            for (UINT s = 0; s < kPsSrvs; ++s) {
                auto f = item.textures.find(s);
                const BindingKind k = kind_of(ps_kinds, s);
                if (f == item.textures.end() && ps_kinds.contains(s) && !item.ps_sources.extern_textures.contains(s))
                    throw std::runtime_error(std::format("{}: missing original PS texture t{} (shared game producer not ported)", item.technique.str(), s));
                create_texture_srv(dev, f != item.textures.end() ? f->second.Get() : nullptr, k, gpu.srv().cpu(srv_first + s));
            }
            // VS t0-t15: the stage's own textures, else (dynamic models) t0 vertex colour and t2-t4 the
            // skinning streams. Undeclared table holes have null descriptors.
            const BufferSet& bs = sets_.at(item.buffers);
            for (UINT s = 0; s < kVsSrvs; ++s) {
                const auto h = gpu.srv().cpu(srv_first + kPsSrvs + s);
                const BindingKind k = kind_of(vs_kinds, s);
                if (auto f = item.vs_textures.find(s); f != item.vs_textures.end()) {
                    create_texture_srv(dev, f->second.Get(), k, h);
                } else if ((s == 0 || s == 1) && bs.color) {
                    create_buffer_srv(dev, bs.color, DXGI_FORMAT_R8G8B8A8_UNORM, h);
                } else if ((s == 0 || s == 1) && !mesh.dynamic && vs_kinds.contains(s) && !item.vs_sources.extern_textures.contains(s)) {
                    // The VS reads a colour stream this mesh does not have (its colour comes from
                    // map-baked per-instance data not decoded yet): white.
                    if (!white_color_) {
                        const uint32_t white = 0xFFFFFFFFu;
                        white_color_ = arena_.upload(gpu, &white, 4);
                    }
                    create_buffer_srv(dev, white_color_.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, h, white_color_.offset / 4);
                    if (material_externs_)
                        material_externs_->note_estimate(std::format("{} VS t{} vertex colour = white", item.technique.str(), s),
                                                         "mesh has no colour stream (map-baked per-instance colour not decoded)");
                } else if (mesh.dynamic && s >= 2 && s <= 4 && bs.skin_pos) {
                    ID3D12Resource* r = s == 3 ? bs.skin_normal.Get() : bs.skin_pos.Get();
                    create_buffer_srv(dev, r, DXGI_FORMAT_R32_UINT, UINT(r->GetDesc().Width / 4), h);
                } else {
                    if (vs_kinds.contains(s) && !item.vs_sources.extern_textures.contains(s) && (mesh.dynamic || s != 2))
                        throw std::runtime_error(std::format("{}: missing original VS resource t{}", item.technique.str(), s));
                    create_texture_srv(dev, nullptr, k, h);
                }
            }

        }
        profile_descriptors.stop();


        // Input layout straight from the game's vertex layout table.
        std::vector<D3D12_INPUT_ELEMENT_DESC> ied;
        for (const auto& e : layouts.at(g.input_layout)) {
            D3D12_INPUT_ELEMENT_DESC d{};
            if (e.buffer >= 4 || sets_.at(item.buffers).vbv[e.buffer].BufferLocation == 0)
                throw std::runtime_error(std::format("{}: missing original vertex stream {}", item.technique.str(), e.buffer));
            d.SemanticName = e.semantic;
            d.SemanticIndex = e.semantic_index;
            d.Format = DXGI_FORMAT(e.dxgi_format);
            d.InputSlot = e.buffer;
            d.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT;
            d.InputSlotClass = e.instanced ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            d.InstanceDataStepRate = e.instanced ? 1 : 0;
            ied.push_back(d);
        }
        for (const auto& input : shader_inputs(g.vs)) {
            const bool found = std::any_of(ied.begin(), ied.end(), [&](const auto& desc) {
                return desc.SemanticIndex == input.index && _stricmp(desc.SemanticName, input.semantic.c_str()) == 0;
            });
            if (!found) throw std::runtime_error(std::format("{}: original vertex layout {} lacks {}{}; no zero stream is substituted",
                item.technique.str(), g.input_layout, input.semantic, input.index));
        }
        item.stage = g.stage;
        item.lod_mask = g.lod_mask;
        const bool transparent = g.stage == kStageTransparent;
        const auto outputs = shader_outputs(g.ps);
        const bool motion = std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
            return _stricmp(output.semantic.c_str(), "SV_Target") == 0 && output.index == 4 && !output.never_written;
        });
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = item.root_sig.Get();
        pd.VS = {g.vs.data(), g.vs.size()};
        pd.PS = {g.ps.data(), g.ps.size()};
        // G-buffer decals blend into some targets and leave the others alone (their state masks them).
        bool blended = false;
        if (transparent) {
            pd.BlendState.IndependentBlendEnable = TRUE;
            pd.BlendState.RenderTarget[0] = blend_desc(g.blend);
            pd.BlendState.RenderTarget[4] = blend_desc(g.blend, 4);  // original motion blend / coverage
            if (!motion) pd.BlendState.RenderTarget[4].RenderTargetWriteMask = 0;
        }
        else if (g.blend >= 0) {
            pd.BlendState.IndependentBlendEnable = TRUE;
            for (int r = 0; r < 4; ++r) {
                pd.BlendState.RenderTarget[r] = blend_desc(g.blend, r);
                blended |= pd.BlendState.RenderTarget[r].BlendEnable || pd.BlendState.RenderTarget[r].RenderTargetWriteMask != 0xF;
            }
        } else pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        item.blended = blended;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = transparent || blended ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = transparent || blended ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
        pd.InputLayout = {ied.data(), UINT(ied.size())};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // Strips restart at the all-ones index.
        if (g.part.primitive_type == 5)
            pd.IBStripCutValue = mesh.buffers.at(g.buffers).ib.is_32bit ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                       : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
        // SV_Target4 is the motion vector. Slots 1..3 are null RTVs with writes disabled.
        pd.NumRenderTargets = transparent ? 5 : 4;
        for (UINT r = 0; r < pd.NumRenderTargets; ++r) pd.RTVFormats[r] = transparent ? DXGI_FORMAT_R16G16B16A16_FLOAT : GBuffer::kFormats[r];
        for (const auto& output : outputs)
            if (_stricmp(output.semantic.c_str(), "SV_Target") == 0 && output.index >= pd.NumRenderTargets)
                throw std::runtime_error(std::format("{}: original PS writes SV_Target{}; original pass attachments must be reconstructed",
                    item.technique.str(), output.index));
        pd.DSVFormat = GBuffer::kDepthFormat;
        pd.SampleDesc.Count = 1;
        const auto pipeline_key = std::tuple{root_key, g.input_layout, g.stage, g.part.primitive_type,
                                             mesh.buffers.at(g.buffers).ib.is_32bit};
        ProfileScope profile_pso("add.pso");
        HRESULT pipeline_hr = S_OK;
        if (auto existing = shared_pipelines.find(pipeline_key); existing != shared_pipelines.end()) item.pso = existing->second;
        else {
            pipeline_hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&item.pso));
            if (SUCCEEDED(pipeline_hr)) shared_pipelines.emplace(pipeline_key, item.pso);
        }
        if (const HRESULT hr = pipeline_hr; FAILED(hr)) {
            std::string vs_in, layout;
            for (const auto& in : shader_inputs(g.vs)) vs_in += std::format(" {}{}", in.semantic, in.index);
            for (const auto& d : ied) layout += std::format(" {}{}@{}", d.SemanticName, d.SemanticIndex, d.InputSlot);
            auto binds = [](std::span<const uint8_t> code) {
                std::string o;
                for (const auto& b : shader_bindings(code))
                    o += std::format(" {}{}-{}/s{}k{}", "?scbu"[int(b.type)], b.lower, b.upper, b.space, int(b.kind));
                return o;
            };
            layout += "; VS binds" + binds(g.vs) + "; PS binds" + binds(g.ps);
            throw std::runtime_error(std::format("CreateGraphicsPipelineState (game DXIL) failed (hr=0x{:08X}); VS in:{}; layout {}:{}",
                                                 uint32_t(hr), vs_in, g.input_layout, layout));
        }
        profile_pso.stop();

        if (!transparent) {
            // Sun shadow: the same VS / PS (alpha-tested parts still clip) with no colour targets,
            // and a slope bias against acne (reversed depth: toward 0).
            const auto shadow_key = std::tuple{root_key, g.input_layout, uint8_t(0xF0), g.part.primitive_type,
                                               mesh.buffers.at(g.buffers).ib.is_32bit};
            if (auto existing = shared_pipelines.find(shadow_key); existing != shared_pipelines.end()) item.shadow_pso = existing->second;
            else {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC sd = pd;
                sd.DSVFormat = DXGI_FORMAT_D32_FLOAT;  // the sun's separate depth-only target
                sd.NumRenderTargets = 0;
                for (auto& f : sd.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
                sd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
                sd.RasterizerState.DepthBias = -16;
                sd.RasterizerState.SlopeScaledDepthBias = -2.0f;
                sd.RasterizerState.DepthBiasClamp = -0.01f;
                if (SUCCEEDED(dev->CreateGraphicsPipelineState(&sd, IID_PPV_ARGS(&item.shadow_pso))))
                    shared_pipelines.emplace(shadow_key, item.shadow_pso);
            }
        }
        transparent_count_ += transparent;
        if (transparent && cull != UINT32_MAX) cull_groups_[cull].has_transparency = true;
        items_.push_back(std::move(item));
        upload_guard.committed = true;
        info_.push_back(std::move(g.info));
    }
}

void StaticMeshScene::bind_common(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h, const ViewMatrices&,
                                  ID3D12RootSignature* root) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(root ? root : dynamic_ ? root_sig_dynamic_.Get() : root_sig_.Get());
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
}

void StaticMeshScene::update_materials(Gpu& gpu, uint32_t w, uint32_t h, const ViewMatrices& vm, uint8_t render_stage,
                                       const ExternStore* frame_inputs) {
    if (!material_externs_) return;
    auto& ext = *material_externs_;
    fill_view_externs(ext, w, h, vm);
    ext.set(ext::Frame, 0x00, material_time_);
    ext.set(ext::Frame, 0x04, material_time_);
    // Map materials (including sky layers) evaluate authored day/night curves from Frame +0x10.
    ext.set(ext::Frame, 0x10, frame_inputs ? frame_inputs->get_f(ext::Frame, 0x10) : material_time_of_day_);
    ext.set(ext::Frame, 0x14, material_delta_);
    ext.set(ext::Frame, 0x1C, material_exposure_);
    estimate_engine_frame(ext, material_time_, material_delta_);
    if (global_values_.size() == global_base_.size() && !global_values_.empty()) {
        ext.channels = global_values_;
    } else if (ext.channels.size() != global_base_.size() || ext.channels != global_base_) {
        if (!global_base_.empty()) ext.channels = global_base_;
    }
    ext.world_channels = global_overrides_;
    if (rain_ && rain_->valid) {
        ext.set_texture(80, 0x00, rain_->mask.Get(), DXGI_FORMAT_R32_FLOAT);
        ext.set(80, 0x10, rain_->scale);
        ext.set(80, 0x20, rain_->offset);
        ext.note_estimate("extern 80 overhead visibility (rain occlusion)",
                          "nim draws the scene's depth from straight above (engine pass not decoded)");
    }
    // A single model has no map around it: no atmosphere, no overhead visibility map.
    for (uint8_t id : {ext::Atmosphere, uint8_t(80), uint8_t(81), uint8_t(82)})
        if (!ext.zero_policy(id)) ext.zero_when_missing(id, "world state from a map (not loaded): zero");
    // PatternBlending (97): an engine-produced pattern texture whose producer is not decoded.
    if (!ext.zero_policy(97)) ext.zero_when_missing(97, "pattern-blending engine input (producer not decoded): zero");
    if (!frame_inputs)
        ext.set(ext::Deferred, 0x00, float4{0, 1.0f / vm.vp_rel.c[3][2], 0, 0});
    if (dynamic_parts_ && !ext.zero_policy(ext::RigidModel))
        ext.zero_when_missing(ext::RigidModel, "per-object engine value of a placed entity (object lighting / SH) not produced: zero");
    if (dynamic_) {
        // RigidModel (goriath layout): local_to_world, position scale / offset, texcoord0
        // scale-offset, all from the model itself. Beyond 0x70 (dynamic SH / AO, skinning
        // constants) the values belong to the engine's object lighting: reported when read.
        for (uint32_t r = 0; r < 7; ++r) ext.set(ext::RigidModel, r * 16, model_cb_[r]);
        if (!ext.zero_policy(ext::RigidModel))
            ext.zero_when_missing(ext::RigidModel, "per-object engine value (object lighting / SH) not produced: zero");
    }
    auto base = ext.environment();
    const ChannelContext cc{&channel_overrides_, &channels_, &channel_defaults_, &ext};
    material_addresses_.clear();
    // Shared scope bindings are fixed per item: resolved once to slots of scope_defs_, whose
    // constants are produced at most once per frame.
    if (!scopes_resolved_) {
        scope_defs_.clear();
        std::map<std::tuple<uint32_t, ShaderStage, uint32_t>, uint32_t> slots;
        for (auto& it : items_) {
            it.scope_slots.clear();
            for (auto [stage, sources, required] : {std::tuple{ShaderStage::Vertex, &it.vs_sources, &it.vs_cbvs},
                                                    std::tuple{ShaderStage::Pixel, &it.ps_sources, &it.ps_cbvs}})
                for (const auto reg : *required) {
                    if (reg == 0 || it.own_regs.contains({stage, reg})) continue;
                    const auto source = sources->cbuffer_scopes.find(reg);
                    if (source == sources->cbuffer_scopes.end())
                        throw std::runtime_error(std::format("{}: no original shared scope b{}", it.technique.str(), reg));
                    const auto scope = std::find_if(it.scopes->begin(), it.scopes->end(), [&](const Scope& value) { return value.tag == source->second; });
                    if (scope == it.scopes->end()) throw std::runtime_error("Original shared scope was not loaded");
                    // A dynamic model's object scopes carry its own constants, per placement.
                    if (it.dynamic && (scope->name == "rigid_model" || scope->name == "skinning")) {
                        it.object_params.push_back(scope_root_parameter(stage, reg));
                        continue;
                    }
                    const auto key = std::tuple{source->second.value, stage, reg};
                    auto slot = slots.find(key);
                    if (slot == slots.end()) {
                        slot = slots.emplace(key, uint32_t(scope_defs_.size())).first;
                        scope_defs_.push_back({&*scope, stage, reg});
                    }
                    it.scope_slots.emplace_back(scope_root_parameter(stage, reg), slot->second);
                }
        }
        scopes_resolved_ = true;
    }
    scope_frame_.assign(scope_defs_.size(), 0);
    auto scope_address = [&](uint32_t slot) {
        if (scope_frame_[slot]) return scope_frame_[slot];
        const auto& def = scope_defs_[slot];
        const Scope* scope = def.scope;
        const ShaderStage stage = def.stage;
        const uint32_t reg = def.reg;
        std::vector<float4> constants;
        if (ext.has_scope_constants(stage, reg)) constants = ext.scope_constants(stage, reg);
        else if (scope->name == "frame") {
            const auto& frame = frame_inputs ? frame_inputs->frame_constants() : ext.frame_constants();
            constants.assign(frame.begin(), frame.end());
        } else {
            const auto& core = scope->stages[size_t(scope_stage(stage))];
            const auto disassembly = disassemble_tfx(core.bytecode);
            const bool writes = disassembly.find("pop_output") != std::string::npos;
            if (!writes && std::all_of(core.cbuffer_init.begin(), core.cbuffer_init.end(), [](const float4& value) { return value == float4{}; })) {
                if (!ext.has_scope_constants(stage, reg))
                    throw std::runtime_error(std::format("Missing engine-produced scope \"{}\" (b{})", scope->name, reg));
                constants = ext.scope_constants(stage, reg);
            }
            else {
                try {
                    constants = evaluate_scope(*scope, scope_stage(stage), ext);
                } catch (const std::exception& ex) {
                    throw std::runtime_error(std::format("scope \"{}\" (b{}): {}", scope->name, reg, ex.what()));
                }
            }
        }
        const auto upload = gpu.dynamic(constants.size() * sizeof(float4));
        std::memcpy(upload.cpu, constants.data(), constants.size() * sizeof(float4));
        return scope_frame_[slot] = upload.gpu;
    };
    // Dynamic models: the object constants of every placement (local_to_world = the placement).
    for (auto& set : sets_) {
        if (!set.dynamic) continue;
        set.object_cb.clear();
        for (const auto& p : set.placements) {
            auto cb = set.model_cb;
            // Column-major: the translation is the fourth row.
            cb[0] = {p[0], p[4], p[8], 0};
            cb[1] = {p[1], p[5], p[9], 0};
            cb[2] = {p[2], p[6], p[10], 0};
            cb[3] = {p[3], p[7], p[11], 1};
            // Original rigid/skinned VS emit previous world positions through c8..c11.
            // Map placements are static: previous placement equals current placement.
            for (size_t c = 0; c < 4; ++c) cb[8 + c] = cb[c];
            const auto upload = gpu.dynamic(cb.size() * sizeof(float4));
            std::memcpy(upload.cpu, cb.data(), cb.size() * sizeof(float4));
            set.object_cb.push_back(upload.gpu);
        }
    }
    for (auto& it : items_) {
        if (it.stage != render_stage) continue;  // transparent inputs are produced after the G-buffer
        if (!item_instances(it)) continue;  // culled this frame: not drawn, nothing to evaluate
      try {
        it.scope_addresses.resize(it.scope_slots.size());
        for (size_t k = 0; k < it.scope_slots.size(); ++k)
            it.scope_addresses[k] = {it.scope_slots[k].first, scope_address(it.scope_slots[k].second)};
        auto material_env = [&](const ChannelContext& channels, bool vs) {
            auto env = with_channels(base, channels, vs);
            if (!vs && it.ps_atlas) {
                const auto& atlas = *it.ps_atlas;
                env.texture_layer_count = [&atlas](uint8_t) { return atlas.uv; };
                env.atlas_frame_count = [&atlas](uint8_t, uint8_t) { return float(atlas.frames); };
                ext.note_estimate("animated texture atlas: TFX 0x68 UVs and 0x69 frame count",
                                  "operand pairs and shader sampling match atlas metadata and successive frames in the game texture");
            }
            return env;
        };
        auto evaluate = [&](const TechniqueStage& material, std::vector<float4>& cb, const GpuBuffer& fixed, bool vs) {
            const uint64_t key = (uint64_t(it.technique.value) << 1) | uint64_t(vs);
            if (auto shared = material_addresses_.find(key); shared != material_addresses_.end()) return shared->second;
            if (!(vs ? it.vs_live : it.ps_live)) return fixed.gpu;
            // A placed object's position is filled in per placement below: not reported as unset here.
            static const float4 origin{0, 0, 0, 1};
            ChannelContext ec = cc;
            if ((vs ? it.vs_reads_position : it.ps_reads_position) > 0 && (dynamic_ || sets_[it.buffers].dynamic)) ec.world_position = &origin;
            evaluate_tfx(material.bytecode, material.constants, material.cbuffer_init, cb, material_env(ec, vs));
            const auto a = gpu.dynamic(cb.size() * sizeof(float4));
            std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
            material_addresses_.emplace(key, a.gpu);
            return a.gpu;
        };
        auto bind_shared = [&](const StageResourceSources& sources, const auto& kinds, bool vs) {
            for (const auto& [slot, source] : sources.extern_textures) {
                const auto kind = kinds.find(slot);
                if (kind == kinds.end()) continue;
                const auto view = ext.texture(source.extern_id, uint32_t(source.offset) * 8);
                const uint32_t table_slot = (vs ? kPsSrvs : 0) + slot;
                if (it.srv_copies == 1) {
                    const std::pair<const void*, uint32_t> now{view.resource ? static_cast<const void*>(view.resource) : &it, uint32_t(view.format)};
                    auto& last = it.written[table_slot];
                    if (last == now) continue;  // unchanged since it was written (and reported)
                    last = now;
                }
                if (!view.resource && ext.estimate_null_texture(source.extern_id, uint32_t(source.offset) * 8)) {
                    create_texture_srv(gpu.device(), nullptr, kind->second,
                        gpu.srv().cpu(it.srv_first + (srv_frame_ % it.srv_copies) * kItemSrvs + (vs ? kPsSrvs : 0) + slot));
                    continue;
                }
                if (!view.resource) throw std::runtime_error(std::format("{}: missing game {} t{} from extern {} byte 0x{:X}",
                    it.technique.str(), vs ? "VS" : "PS", slot, source.extern_id, uint32_t(source.offset) * 8));
                write_game_srv(gpu.device(), view, kind->second,
                    gpu.srv().cpu(it.srv_first + (srv_frame_ % it.srv_copies) * kItemSrvs + (vs ? kPsSrvs : 0) + slot));
            }
        };
        bind_shared(it.vs_sources, it.vs_kinds, true);
        bind_shared(it.ps_sources, it.ps_kinds, false);
        if (it.vs_reads_position < 0) it.vs_reads_position = reads_object_position(it.vs_material);
        if (it.ps_reads_position < 0) it.ps_reads_position = reads_object_position(it.ps_material);
        it.vs_address = evaluate(it.vs_material, it.vs_constants, it.vs_cb0, true);
        it.ps_address = evaluate(it.ps_material, it.ps_constants, it.cb0, false);
        // The object's position is per placement: one set of constants for each.
        it.vs_placement_addresses.clear();
        it.ps_placement_addresses.clear();
        if (it.vs_reads_position || it.ps_reads_position) {
            const auto& set = sets_[it.buffers];
            auto at = [&](const TechniqueStage& material, std::vector<float4>& cb, float4 position, bool vs) {
                ChannelContext pc = cc;
                pc.world_position = &position;
                evaluate_tfx(material.bytecode, material.constants, material.cbuffer_init, cb, material_env(pc, vs));
                const auto a = gpu.dynamic(cb.size() * sizeof(float4));
                std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
                return a.gpu;
            };
            if (set.dynamic) {
                for (const auto& p : set.placements) {
                    const float4 position{p[3], p[7], p[11], 1};
                    if (it.vs_reads_position) it.vs_placement_addresses.push_back(at(it.vs_material, it.vs_constants, position, true));
                    if (it.ps_reads_position) it.ps_placement_addresses.push_back(at(it.ps_material, it.ps_constants, position, false));
                }
            } else if (dynamic_) {
                const float4 position{model_cb_[3][0], model_cb_[3][1], model_cb_[3][2], 1};
                if (it.vs_reads_position) it.vs_address = at(it.vs_material, it.vs_constants, position, true);
                if (it.ps_reads_position) it.ps_address = at(it.ps_material, it.ps_constants, position, false);
            }
        }
      } catch (const std::exception& ex) {
        // One part without its game inputs is left out (never drawn with invented values);
        // the rest of the scene still draws.
        it.failed = true;
        notes_.push_back(std::format("{}: not drawn: {}", it.technique.str(), ex.what()));
      }
    }
}

void StaticMeshScene::draw_items(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint8_t stage, uint32_t w, uint32_t h, const ViewMatrices& vm,
                                 bool shadow) {
    uint32_t bound = UINT32_MAX;
    ID3D12RootSignature* bound_root = nullptr;
    bool bound_root_changed = false;
    // Redundant root arguments are skipped (maps issue tens of thousands of draws).
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 31> bound_cbv{};
    ID3D12PipelineState* bound_pso = nullptr;
    uint32_t bound_table = UINT32_MAX;
    D3D_PRIMITIVE_TOPOLOGY bound_topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    auto set_cbv = [&](uint32_t param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        if (bound_cbv[param] == address) return;
        bound_cbv[param] = address;
        cl->SetGraphicsRootConstantBufferView(param, address);
    };
    auto draw = [&](const Item& it, uint32_t instances, uint32_t first_instance) {
        if (bound_root != it.root_sig.Get()) {
            bind_common(gpu, cl, w, h, vm, it.root_sig.Get());
            bound_root = it.root_sig.Get();
            bound_root_changed = true;
            bound_cbv = {};
            bound_table = UINT32_MAX;
        }
        const auto& set = sets_[it.buffers];
        if (it.buffers != bound || bound_root_changed) {
            bound = it.buffers;
            cl->IASetIndexBuffer(&sets_[bound].ibv);
            cl->IASetVertexBuffers(0, 4, sets_[bound].vbv);
            if (!it.dynamic) cl->SetGraphicsRootShaderResourceView(2, sets_[bound].t2.gpu);
        }
        bound_root_changed = false;
        ID3D12PipelineState* pso = shadow ? it.shadow_pso.Get() : it.pso.Get();
        if (bound_pso != pso) cl->SetPipelineState(bound_pso = pso);
        set_cbv(0, it.ps_address);
        set_cbv(6, it.vs_address);
        for (const auto& [param, address] : it.scope_addresses) set_cbv(param, address);
        for (const auto& [param, buffer] : it.own_cbvs) set_cbv(param, buffer.gpu);
        const uint32_t first = it.srv_first + (srv_frame_ % it.srv_copies) * kItemSrvs;
        if (bound_table != first) {
            bound_table = first;
            cl->SetGraphicsRootDescriptorTable(3, gpu.srv().gpu(first));
            cl->SetGraphicsRootDescriptorTable(it.dynamic ? 2 : 30, gpu.srv().gpu(first + kPsSrvs));
        }
        const auto topology = it.part.primitive_type == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (bound_topology != topology) cl->IASetPrimitiveTopology(bound_topology = topology);
        if (it.dynamic) {
            for (size_t p = first_instance; p < size_t(first_instance) + instances; ++p) {
                for (uint32_t param : it.object_params) set_cbv(param, set.object_cb[p]);
                if (p < it.ps_placement_addresses.size()) set_cbv(0, it.ps_placement_addresses[p]);
                if (p < it.vs_placement_addresses.size()) set_cbv(6, it.vs_placement_addresses[p]);
                cl->DrawIndexedInstanced(it.part.index_count, 1, it.part.index_start, 0, 0);
            }
        } else cl->DrawIndexedInstanced(it.part.index_count, instances, it.part.index_start, 0, first_instance);
    };
    if (stage == kStageTransparent) {
        struct Draw { const Item* item; uint32_t instance; float depth; bool sky; };
        std::vector<Draw> ordered;
        auto enqueue = [&](const Item& item, uint32_t instance, float depth, bool sky = false) {
            ordered.push_back({&item, instance, std::isfinite(depth) ? depth : 0, sky});
        };
        for (const auto& it : items_) {
            if (it.stage != stage || !item_instances(it)) continue;
            const auto& set = sets_[it.buffers];
            if (it.dynamic) {
                const auto c = set.model_center;
                for (uint32_t i = 0; i < set.placements.size(); ++i) {
                    if (!set.sky_order.empty()) {
                        enqueue(it, i, set.sky_order[i], true);
                        continue;
                    }
                    const auto& p = set.placements[i];
                    const V3 pos{p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3],
                                 p[4] * c.x + p[5] * c.y + p[6] * c.z + p[7],
                                 p[8] * c.x + p[9] * c.y + p[10] * c.z + p[11]};
                    enqueue(it, i, dot(pos - vm.eye, vm.fwd));
                }
            } else {
                const auto& group = cull_groups_[set.cull];
                for (size_t l = 0; l < 4; ++l) {
                    if (!((it.lod_mask >> l) & 1)) continue;
                    for (uint32_t i = group.first[l]; i < group.first[l] + group.count[l]; ++i)
                        enqueue(it, i, group.visible_depths[i]);
                }
            }
        }
        // Preserve the authored part order for equal depths, including parts of one placement.
        std::stable_sort(ordered.begin(), ordered.end(), [](const Draw& a, const Draw& b) {
            if (a.sky != b.sky) return a.sky;
            return a.sky ? a.depth < b.depth : a.depth > b.depth;
        });
        for (const auto& entry : ordered) draw(*entry.item, 1, entry.instance);
        return;
    }
    // Blended G-buffer items (decals) go over opaque surfaces.
    for (int pass = 0; pass < 2; ++pass)
    for (const auto& it : items_) {
        if (it.stage != stage || it.blended != (pass == 1) || (shadow && (!it.shadow_pso || it.blended))) continue;
        const uint32_t instances = item_instances(it);
        if (!instances) continue;
        const auto& set = sets_[it.buffers];
        if (set.cull == UINT32_MAX) draw(it, instances, 0);
        else {
            const auto& group = cull_groups_[set.cull];
            for (size_t l = 0; l < 4; ++l)
                if (((it.lod_mask >> l) & 1) && group.count[l])
                    draw(it, group.count[l], group.first[l]);
        }
    }
}

void StaticMeshScene::set_skin_pose(std::vector<BoneTransform> bones) {
    skin_pose_ = std::move(bones);
    skin_dirty_ = skinned_;
}

// Linear blend skinning of every skinned buffer set, packed into this frame's upload buffer and
// copied into the streams the vertex shaders read. Runs at most once per frame (from
// render_gbuffer), so alternating between the kFramesInFlight upload buffers is safe.
void StaticMeshScene::update_skin(ID3D12GraphicsCommandList* cl) {
    skin_dirty_ = false;
    const uint32_t slot = skin_frame_++ % Gpu::kFramesInFlight;
    struct Mat { float m[12]; };  // rows: rotation * scale | translation
    std::vector<Mat> mats(skin_pose_.size());
    for (size_t b = 0; b < mats.size(); ++b) {
        const auto& t = skin_pose_[b];
        const float x = t.rotation[0], y = t.rotation[1], z = t.rotation[2], w = t.rotation[3], s = t.scale;
        mats[b] = {{(1 - 2 * (y * y + z * z)) * s, 2 * (x * y - z * w) * s, 2 * (x * z + y * w) * s, t.translation[0],
                    2 * (x * y + z * w) * s, (1 - 2 * (x * x + z * z)) * s, 2 * (y * z - x * w) * s, t.translation[1],
                    2 * (x * z - y * w) * s, 2 * (y * z + x * w) * s, (1 - 2 * (x * x + y * y)) * s, t.translation[2]}};
    }
    for (auto& set : sets_) {
        if (!set.skin_upload[0]) continue;
        const size_t n = set.skin.bones.size();
        ID3D12Resource* upload = set.skin_upload[slot].Get();
        uint32_t* dst = nullptr;
        const D3D12_RANGE none{0, 0};
        check_hr(upload->Map(0, &none, reinterpret_cast<void**>(&dst)), "map skin upload");
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* v = set.vb0_cpu.data() + i * 24;
            V3 p{s16(v) * skin_scale_.x + skin_offset_.x, s16(v + 2) * skin_scale_.y + skin_offset_.y,
                 s16(v + 4) * skin_scale_.z + skin_offset_.z};
            V3 nr{s16(v + 8), s16(v + 10), s16(v + 12)}, tg{s16(v + 16), s16(v + 18), s16(v + 20)};
            if (!mats.empty()) {
                float m[12]{};
                float total = 0;
                for (int k = 0; k < 4; ++k) {
                    const float wk = set.skin.weights[i][k];
                    const uint16_t bk = set.skin.bones[i][k];
                    if (wk <= 0 || bk >= mats.size()) continue;
                    for (int j = 0; j < 12; ++j) m[j] += mats[bk].m[j] * wk;
                    total += wk;
                }
                if (total > 0) {
                    auto xf = [&](V3 a, float w1) {
                        return V3{m[0] * a.x + m[1] * a.y + m[2] * a.z + m[3] * w1, m[4] * a.x + m[5] * a.y + m[6] * a.z + m[7] * w1,
                                  m[8] * a.x + m[9] * a.y + m[10] * a.z + m[11] * w1};
                    };
                    p = xf(p, 1);
                    nr = xf(nr, 0);
                    tg = xf(tg, 0);
                }
            }
            pack_skin_vertex((p - skin_offset_) * (1.0f / skin_range_), normalize(nr), normalize(tg), dst + i * 2, dst + n * 2 + i * 2);
        }
        upload->Unmap(0, nullptr);
        constexpr auto kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (set.skin_readable) {
            Gpu::barrier(cl, set.skin_pos.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
            Gpu::barrier(cl, set.skin_normal.Get(), kRead, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        cl->CopyBufferRegion(set.skin_pos.Get(), 0, upload, 0, n * 8);
        cl->CopyBufferRegion(set.skin_normal.Get(), 0, upload, n * 8, n * 8);
        Gpu::barrier(cl, set.skin_pos.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        Gpu::barrier(cl, set.skin_normal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kRead);
        set.skin_readable = true;
    }
}

uint32_t StaticMeshScene::item_instances(const Item& it) const {
    if (it.failed) return 0;
    const auto& set = sets_[it.buffers];
    if (set.cull == UINT32_MAX) return set.instances;
    uint32_t n = 0;
    for (size_t l = 0; l < 4; ++l)
        if ((it.lod_mask >> l) & 1) n += cull_groups_[set.cull].count[l];
    return n;
}

void StaticMeshScene::cull(Gpu& gpu, const ViewMatrices& vm, uint32_t height) {
    // Frustum side planes from the projection scales (x' = x * fx / z), plus a size cut: an
    // instance smaller than about a pixel on screen is skipped.
    const float kx = std::sqrt(vm.fx * vm.fx + 1), ky = std::sqrt(vm.fy * vm.fy + 1);
    const float pixel = 2.0f / (vm.fy * float(std::max(1u, height)));
    const float half_h = 0.5f * float(std::max(1u, height)) * vm.fy;
    for (auto& group : cull_groups_) {
        group.visible_depths.clear();
        const auto upload = gpu.dynamic(group.spheres.size() * 4);
        auto* out = static_cast<uint32_t*>(static_cast<void*>(upload.cpu));
        const bool lods = group.levels != 1;
        if (lods)
            for (auto& v : cull_scratch_) v.clear();
        uint32_t visible = 0;
        for (uint32_t i = 0; i < group.spheres.size(); ++i) {
            const auto& sp = group.spheres[i];
            const V3 d{sp[0] - vm.eye.x, sp[1] - vm.eye.y, sp[2] - vm.eye.z};
            const float z = dot(d, vm.fwd), r = sp[3];
            const float x = dot(d, vm.right), y = dot(d, vm.up);
            float px;  // projected radius in pixels
            if (vm.ortho > 0) {
                if (std::abs(x) - r > vm.ortho || std::abs(y) - r > vm.ortho || z < -r) continue;
                px = r / vm.ortho * 0.5f * float(std::max(1u, height));
                if (px < 0.5f) continue;
            } else {
                if (z < -r) continue;
                if (std::abs(x) * vm.fx - z > r * kx || std::abs(y) * vm.fy - z > r * ky) continue;
                if (z > r && r < z * pixel) continue;
                px = z > r ? r * half_h / z : 1e9f;
            }
            if (!lods) {
                out[visible++] = i;
                if (group.has_transparency) group.visible_depths.push_back(z);
                continue;
            }
            int level = px >= 160 ? 0 : px >= 64 ? 1 : px >= 24 ? 2 : 3;
            while (level > 0 && !((group.levels >> level) & 1)) --level;
            while (level < 3 && !((group.levels >> level) & 1)) ++level;
            cull_scratch_[size_t(level)].push_back(i);
        }
        group.first = {};
        group.count = {};
        if (lods)
            for (size_t l = 0; l < 4; ++l) {
                group.first[l] = visible;
                group.count[l] = uint32_t(cull_scratch_[l].size());
                if (!cull_scratch_[l].empty()) std::memcpy(out + visible, cull_scratch_[l].data(), cull_scratch_[l].size() * 4);
                if (group.has_transparency)
                    for (uint32_t i : cull_scratch_[l]) {
                        const auto& s = group.spheres[i];
                        group.visible_depths.push_back(dot(V3{s[0], s[1], s[2]} - vm.eye, vm.fwd));
                    }
                visible += group.count[l];
            }
        else group.count[0] = visible;
        group.visible = visible;
        group.visible_address = upload.gpu;
    }
    for (auto& set : sets_)
        if (set.cull != UINT32_MAX) set.vbv[2] = {cull_groups_[set.cull].visible_address, std::max(4u, cull_groups_[set.cull].visible * 4), 4};
}

void StaticMeshScene::render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm) {
    srv_frame_ = (srv_frame_ + 1) % kSrvCopies;
    cull(gpu, vm, gb.height());
    update_materials(gpu, gb.width(), gb.height(), vm);
    if (skin_dirty_) update_skin(cl);
    const float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) cl->ClearRenderTargetView(gb.rtv[i], clear, 0, nullptr);
    cl->ClearDepthStencilView(gb.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    if (items_.size() == transparent_count_) return;
    cl->OMSetRenderTargets(4, gb.rtv, FALSE, &gb.dsv);
    draw_items(gpu, cl, 0, gb.width(), gb.height(), vm);
}

void StaticMeshScene::render_shadow(Gpu& gpu, ID3D12GraphicsCommandList* cl, SunShadow& shadow) {
    shadow.valid = false;
    if (!shadow.depth || items_.size() == transparent_count_) return;
    cull(gpu, shadow.vm, shadow.size);
    update_materials(gpu, shadow.size, shadow.size, shadow.vm);
    cl->ClearDepthStencilView(shadow.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    cl->OMSetRenderTargets(0, nullptr, FALSE, &shadow.dsv);
    draw_items(gpu, cl, 0, shadow.size, shadow.size, shadow.vm, true);
    shadow.valid = true;
}

void StaticMeshScene::render_rain_occlusion(Gpu& gpu, ID3D12GraphicsCommandList* cl, RainOcclusion& rain, V3 center, float radius) {
    rain.valid = false;
    if (!rain.top.depth) return;
    rain.top.fit(V3{0, 0, 1}, center, radius);
    render_shadow(gpu, cl, rain.top);
    if (!rain.top.valid) return;
    rain.resolve(gpu, cl);
    // Straight down: right = +x, up = +y. u = 0.5 + (x - eye.x) / 2h, v = 0.5 - (y - eye.y) / 2h,
    // depth = (eye.z - z) / range (1 - the reversed depth the pass wrote).
    const float h2 = 2 * rain.top.half;
    const V3 e = rain.top.vm.eye;
    rain.scale = {1 / h2, -1 / h2, -1 / rain.top.range, 0};
    rain.offset = {0.5f - e.x / h2, 0.5f + e.y / h2, e.z / rain.top.range, 0};
    rain.items = items_.size();
    rain.valid = true;
}

void StaticMeshScene::render_transparent(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                         const ViewMatrices& vm, const ExternStore& inputs) {
    if (!transparent_count_) return;
    auto& ext = *material_externs_;
    ext.note_estimate("transparent pass order: authored sky order, then object placement depth",
                      "sky collections are stored in ascending order; engine interleaving and ties are unknown");
    ext.copy_block_from(ext::Atmosphere, inputs);
    ext.copy_block_from(ext::Transparent, inputs);
    ext.copy_block_from(ext::Deferred, inputs);
    material_exposure_ = inputs.get_f(ext::Frame, 0x1C);  // lighting's newly adapted exposure
    // Supply cb13 for this pass without freezing the scene's own frame constants next frame.
    // Reevaluate only transparents: descriptors used by recorded G-buffer/shadow draws stay intact.
    update_materials(gpu, gb.width(), gb.height(), vm, kStageTransparent, &inputs);
    Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {rtv, gb.null_rtv, gb.null_rtv, gb.null_rtv, gb.rtv[3]};
    cl->OMSetRenderTargets(5, targets, FALSE, &gb.dsv_ro);
    draw_items(gpu, cl, kStageTransparent, gb.width(), gb.height(), vm, false);
    Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ---- LightingPass ---------------------------------------------------------------------------------
const char* view_mode_name(ViewMode m) {
    switch (m) {
    case ViewMode::Lit: return "Lit";
    case ViewMode::Albedo: return "Albedo (RT0)";
    case ViewMode::Normal: return "Normal (RT1)";
    case ViewMode::Smoothness: return "Smoothness (RT1 length)";
    case ViewMode::Metalness: return "Metalness (RT2.r)";
    case ViewMode::Material: return "Material params (RT2)";
    case ViewMode::Motion: return "Motion vectors (RT3)";
    default: return "?";
    }
}

namespace {

// G-buffer decode follows the game's G-buffer PS (see docs/ARCHITECTURE.md, section 6):
//   RT0.rgb albedo (linear)
//   RT1.rgb normal * (0.375 + smoothness / 8) + 0.5
//   RT2.r   metalness, RT2.g = 0.5 * (occlusion + log-encoded emission), RT2.b = 0.8 * (unknown)
//   RT3.xy  motion
// Diagnostic visualization only. Lit scenes require the original game pipelines.
constexpr const char* kLightingHlsl = R"(
cbuffer Params : register(b0) {
    float3 eye;        float exposure;
    float3 fwd;        float fx;
    float3 right;      float fy;
    float3 up;         uint mode;
    float3 sun_dir;    float sun_intensity;
    float3 sun_color;  float sky_intensity;
    float3 sky_top;    float inv_w;
    float3 sky_horizon; float inv_h;
    float3 ground;     float pad0;
    float3 bg_top;     float pad1;
    float3 bg_bottom;  float pad2;
};
Texture2D<float4> g0 : register(t0);
Texture2D<float4> g1 : register(t1);
Texture2D<float4> g2 : register(t2);
Texture2D<float4> g3 : register(t3);
Texture2D<float>  depth : register(t4);

static const float PI = 3.14159265;

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055;
}

float4 ps_main(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    float2 ndc = float2(pos.x * inv_w * 2 - 1, 1 - pos.y * inv_h * 2);
    float3 ray = normalize(fwd + right * (ndc.x / fx) + up * (ndc.y / fy));
    if (depth.Load(p) <= 0) {
        float t = pos.y * inv_h;
        return float4(srgb(lerp(bg_top, bg_bottom, t)), 1);
    }
    float4 a = g0.Load(p), e = g1.Load(p), m = g2.Load(p), mv = g3.Load(p);
    float3 v = e.rgb - 0.5;
    float len = length(v);
    float3 n = len > 0 ? v / len : float3(0, 0, 1);
    float smooth = saturate((len - 0.375) * 8);
    float metal = saturate(m.r);
    float occl = m.g >= 0.5 ? 1 : saturate(m.g * 2);
    float emis_enc = m.g >= 0.5 ? m.g * 2 - 1 : 0;
    float emission = max(0, exp2((emis_enc - 0.538) / 0.0769) - 0.0078125);
    float3 albedo = max(a.rgb, 0);

    if (mode == 1) return float4(srgb(albedo), 1);
    if (mode == 2) return float4(n * 0.5 + 0.5, 1);
    if (mode == 3) return float4(smooth.xxx, 1);
    if (mode == 4) return float4(metal.xxx, 1);
    if (mode == 5) return float4(saturate(m.rgb), 1);
    if (mode == 6) return float4(saturate(mv.xy * 20 + 0.5), 0.5, 1);

    return float4(0, 0, 0, 1); // mode 0 is rejected on the CPU; diagnostics only.
}
)";

ComPtr<ID3DBlob> compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kLightingHlsl, std::strlen(kLightingHlsl), "nim_lighting.hlsl", nullptr, nullptr, entry,
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
        throw std::runtime_error(std::format("lighting shader: {}",
                                             err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed"));
    return code;
}

struct LightingConstants {
    V3 eye; float exposure;
    V3 fwd; float fx;
    V3 right; float fy;
    V3 up; uint32_t mode;
    V3 sun_dir; float sun_intensity;
    V3 sun_color; float sky_intensity;
    V3 sky_top; float inv_w;
    V3 sky_horizon; float inv_h;
    V3 ground; float pad0;
    V3 bg_top; float pad1;
    V3 bg_bottom; float pad2;
};

} // namespace

void LightingPass::init(Gpu& gpu) {
    ID3D12Device* dev = gpu.device();
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, err;
    check_hr(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), "serialize lighting root sig");
    check_hr(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_sig_)),
             "lighting root sig");
    auto vs = compile("vs_main", "vs_5_0");
    auto ps = compile("ps_main", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "lighting PSO");
}

void LightingPass::render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                          const LightingParams& p) {
    if (p.mode == ViewMode::Lit)
        throw std::runtime_error("Approximate lighting was removed; original game inputs are required");
    for (auto& r : gb.rt)
        Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    auto cb = gpu.dynamic(sizeof(LightingConstants));
    LightingConstants c{};
    c.eye = vm.eye; c.exposure = p.exposure;
    c.fwd = vm.fwd; c.fx = vm.fx;
    c.right = vm.right; c.fy = vm.fy;
    c.up = vm.up; c.mode = uint32_t(p.mode);
    c.sun_dir = normalize(V3{std::cos(p.sun_azimuth) * std::cos(p.sun_elevation),
                             std::sin(p.sun_azimuth) * std::cos(p.sun_elevation), std::sin(p.sun_elevation)});
    c.sun_intensity = p.sun_intensity;
    c.sun_color = p.sun_color;
    c.sky_intensity = p.sky_intensity;
    c.sky_top = p.sky_top;
    c.sky_horizon = p.sky_horizon;
    c.ground = p.ground;
    c.bg_top = p.background_top;
    c.bg_bottom = p.background_bottom;
    c.inv_w = 1.0f / float(gb.width());
    c.inv_h = 1.0f / float(gb.height());
    std::memcpy(cb.cpu, &c, sizeof(c));

    cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(gb.width()), float(gb.height()), 0, 1};
    D3D12_RECT sc{0, 0, LONG(gb.width()), LONG(gb.height())};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetPipelineState(pso_.Get());
    cl->SetGraphicsRootSignature(root_sig_.Get());
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(0, cb.gpu);
    cl->SetGraphicsRootDescriptorTable(1, gpu.srv().gpu(gb.srv_first));
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(3, 1, 0, 0);

    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (auto& r : gb.rt)
        Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, gb.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

} // namespace nim
