#include "nim/render/scene.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/object_channels.hpp"

#include <d3dcompiler.h>

#include <algorithm>
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
    proj.c[2] = {0, 0, 0, 1};
    proj.c[3] = {0, 0, v.near_z, 0};
    m.vp_rel = mul(proj, rot);
    M4 tr;
    tr.c[0] = {1, 0, 0, 0};
    tr.c[1] = {0, 1, 0, 0};
    tr.c[2] = {0, 0, 1, 0};
    tr.c[3] = {-v.eye.x, -v.eye.y, -v.eye.z, 1};
    m.vp_abs = mul(m.vp_rel, tr);
    return m;
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
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5};
        check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "rtv heap");
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
        check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)), "dsv heap");
    }
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_first = gpu.srv().alloc(6);
    for (int i = 0; i < 4; ++i) {
        D3D12_CLEAR_VALUE cv{kFormat, {0, 0, 0, 0}};
        rt[i] = gpu.texture2d(w, h, kFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
        rtv[i] = {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * inc};
        dev->CreateRenderTargetView(rt[i].Get(), nullptr, rtv[i]);
        dev->CreateShaderResourceView(rt[i].Get(), nullptr, gpu.srv().cpu(srv_first + i));
    }
    D3D12_CLEAR_VALUE dc{DXGI_FORMAT_D32_FLOAT};
    dc.DepthStencil = {0.0f, 0};
    depth = gpu.texture2d(w, h, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc);
    dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
    dev->CreateDepthStencilView(depth.Get(), &dd, dsv);
    dsv_ro = {dsv.ptr + dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
    dd.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    dev->CreateDepthStencilView(depth.Get(), &dd, dsv_ro);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(depth.Get(), &sd, gpu.srv().cpu(srv_first + 4));

    D3D12_CLEAR_VALUE lc{DXGI_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 1}};
    lit = gpu.texture2d(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &lc);
    lit_rtv = {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(4) * inc};
    dev->CreateRenderTargetView(lit.Get(), nullptr, lit_rtv);
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

// The engine's blend states, render target 0 (from goriath's dump of the state table; indexed by
// the technique's blend state byte): enable, src, dst, op, src alpha, dst alpha, op alpha, write mask.
struct BlendRow { uint8_t enable, src, dst, op, src_a, dst_a, op_a, mask; };
constexpr BlendRow kBlendStates[90] = {
    {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {1,2,2,1,2,2,1,15}, {1,9,1,1,7,1,1,15}, {1,9,3,1,7,5,1,15}, {1,2,5,1,2,5,1,15},
    {1,2,5,1,7,1,1,15}, {1,7,2,1,7,1,1,15}, {1,2,6,1,2,6,1,15}, {1,2,2,4,2,2,4,15}, {1,2,2,5,2,2,5,15}, {1,9,2,1,7,2,1,15},
    {1,5,6,1,5,6,1,15}, {1,14,15,1,14,15,1,15}, {1,6,5,1,6,5,1,15}, {1,7,8,1,2,1,1,15}, {1,2,6,1,1,6,1,15}, {0,2,1,1,2,1,1,1},
    {0,2,1,1,2,1,1,2}, {0,2,1,1,2,1,1,4}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,6}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7},
    {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7}, {1,2,5,1,1,2,1,7},
    {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0}, {1,2,5,1,1,2,1,0},
    {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7},
    {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7},
    {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,8}, {0,2,1,1,2,1,1,8},
    {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,15}, {0,2,1,1,2,1,1,0},
    {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0},
    {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,0}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7},
    {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {0,2,1,1,2,1,1,7}, {1,9,1,1,1,2,1,7}, {1,9,3,1,1,2,1,7},
    {0,2,1,1,2,1,1,0}, {1,7,2,1,1,2,1,7}, {1,8,2,1,1,2,1,7}, {1,1,7,1,1,2,1,7}, {1,1,8,1,1,2,1,7}, {1,1,6,1,2,6,1,15},
    {1,2,6,1,2,6,1,7}, {1,2,6,1,2,6,1,8}, {1,16,17,1,18,19,1,15}, {1,2,6,1,1,6,1,15}, {1,2,6,1,1,6,1,15}, {1,2,2,3,2,2,3,15},
};

D3D12_RENDER_TARGET_BLEND_DESC blend_desc(int index) {
    D3D12_RENDER_TARGET_BLEND_DESC d{};
    d.RenderTargetWriteMask = 0xF;
    d.SrcBlend = d.SrcBlendAlpha = D3D12_BLEND_ONE;
    d.DestBlend = d.DestBlendAlpha = D3D12_BLEND_ZERO;
    d.BlendOp = d.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d.LogicOp = D3D12_LOGIC_OP_NOOP;
    if (index < 0 || index >= int(std::size(kBlendStates)))
        throw std::runtime_error(std::format("Missing original blend state {}", index));
    const BlendRow& b = kBlendStates[index];
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

void create_buffer_srv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, UINT elements, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = std::max<UINT>(1, elements);
    dev->CreateShaderResourceView(r, &sd, h);
}


} // namespace

StaticMeshScene::StaticMeshScene() = default;
StaticMeshScene::~StaticMeshScene() = default;

const std::map<std::string, std::string>& StaticMeshScene::estimates() const {
    static const std::map<std::string, std::string> none;
    return material_externs_ ? material_externs_->estimates() : none;
}

void StaticMeshScene::set_render_inputs(const ExternStore& inputs) {
    if (!material_externs_) throw std::runtime_error("No scene is loaded");
    *material_externs_ = inputs;
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
    for (auto& it : items_) gpu.srv().free(it.srv_first, kItemSrvs * kSrvCopies);
    items_.clear();
    info_.clear();
    transparent_count_ = 0;
    channels_.clear();
    channel_overrides_.clear();
    channel_defaults_.clear();
    material_addresses_.clear();
    material_time_ = material_delta_ = 0;
    material_externs_.reset();
    sets_.clear();
    notes_.clear();
    inst_vb_.Reset(); t2_.Reset();
    dynamic_ = false;
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
};
TfxEnvironment with_channels(const TfxEnvironment& base, const ChannelContext& cc, bool vs) {
    TfxEnvironment env = base;
    env.object_channel = [cc, vs](uint32_t h) {
        const float4* value = nullptr;
        if (cc.defaults)
            if (auto it = cc.defaults->find(h); it != cc.defaults->end()) value = &it->second;
        if (cc.overrides)
            if (auto it = cc.overrides->find(h); it != cc.overrides->end()) value = &it->second;
        static const float4 unset{1, 1, 1, 1};
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
    mesh.global_channels = std::move(globals.channel_defaults);
    for (const auto& [name, tag] : globals.scopes) mesh.scopes.push_back(load_scope(pm, tag));

}

using TextureCache = std::unordered_map<uint32_t, std::shared_ptr<const Texture>>;
std::shared_ptr<const Texture> cached_texture(PackageManager& pm, TagHash tag, TextureCache& cache) {
    if (auto it = cache.find(tag.value); it != cache.end()) return it->second;
    auto texture = std::make_shared<const Texture>(load_texture(pm, tag));
    cache.emplace(tag.value, texture);
    return texture;
}

PreparedMesh::Group build_group(PackageManager& pm, const Technique& tech, const StaticMeshPart& part,
                                uint16_t part_index, uint8_t input_layout,
                                TextureCache& texture_cache, std::span<const Scope> available_scopes) {
    PreparedMesh::Group g;
    g.part = part;
    g.input_layout = input_layout;
    g.blend = tech.blend_state();
    const auto& vs_stage = tech.stage(ShaderStage::Vertex);
    const auto& ps_stage = tech.stage(ShaderStage::Pixel);
    g.vs_material = vs_stage;
    g.ps_material = ps_stage;
    std::vector<const Scope*> scope_ptrs;
    for (size_t bit = 0; bit < 64; ++bit) {
        if (!(tech.used_scopes & (uint64_t{1} << bit))) continue;
        if (bit >= available_scopes.size()) throw std::runtime_error("Unknown original scope index");
        g.scopes.push_back(available_scopes[bit]);
    }
    for (const auto& scope : g.scopes) scope_ptrs.push_back(&scope);
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
    if (sum <= 1e-12f) return {0, 0};
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
                                   int permutation, const ChannelValues* channels) {
    PreparedMesh out;
    prepare_material_inputs(pm, out, channels);
    out.tag = tag;
    out.dynamic = true;
    TextureCache texture_cache;
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
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const DynamicMesh& mesh = model.meshes[mi];
        bool any_stage = false;
        for (uint8_t st : kDrawnStages) any_stage |= mesh.part_range[st] < std::min<size_t>(mesh.part_range[st + 1], mesh.parts.size());
        if (!any_stage) continue;
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
    // c8-c11 skinning transform (identity), c12/c13 packed-position offset + 1D scale, c15-c18 identity.
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

PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash tag, const ChannelValues* channels) {
    PreparedMesh out;
    prepare_material_inputs(pm, out, channels);
    out.tag = tag;
    StaticMesh mesh = load_static_mesh(pm, tag);
    TextureCache texture_cache;

    struct Pending { StaticMeshGroup group; StaticMeshPart part; Technique tech; };
    auto drawn = [](uint8_t st) { return st == 0 || st == kStageTransparent; };
    std::vector<Pending> pending;
    uint8_t lod = 0xFF;
    for (const auto& g : mesh.groups)
        if (drawn(g.render_stage)) lod = std::min(lod, mesh.parts.at(g.part_index).lod_category);
    for (size_t g = 0; g < mesh.groups.size(); ++g) {
        const auto& gr = mesh.groups[g];
        if (!drawn(gr.render_stage) || g >= mesh.techniques.size()) continue;
        const auto& part = mesh.parts.at(gr.part_index);
        if (part.lod_category != lod) continue;
        Technique t = load_technique(pm, mesh.techniques[g]);
        if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
        pending.push_back({gr, part, std::move(t)});
    }
    if (pending.empty()) throw std::runtime_error("mesh has no G-buffer or transparent groups with a vertex + pixel shader");
    out.lod = lod;

    const auto& bufs = mesh.buffers.at(pending[0].part.buffer_index);
    out.buffers.emplace_back();
    auto& set = out.buffers.back();
    set.ib = load_index_buffer(pm, bufs.index);
    set.vb0 = load_vertex_buffer(pm, bufs.vertex0);
    if (!bufs.vertex1.is_none()) set.vb1 = load_vertex_buffer(pm, bufs.vertex1);
    const IndexBuffer& ib = set.ib;
    const VertexBuffer& vb0 = set.vb0;

    // Bounds of the drawn geometry, in the space the VS produces (pos * scale + offset).
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const auto& it : pending)
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

    for (auto& p : pending) {
        out.groups.push_back(build_group(pm, p.tech, p.part, p.group.part_index, p.group.input_layout, texture_cache, out.scopes));
        out.groups.back().stage = p.group.render_stage;
    }
    return out;
}

void StaticMeshScene::upload(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts) {
    release(gpu);
    ID3D12Device* dev = gpu.device();
    if (!root_sig_) root_sig_ = make_gbuffer_root_signature(dev);
    if (!root_sig_dynamic_) root_sig_dynamic_ = make_gbuffer_root_signature(dev, true);
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
    material_externs_->channels = std::move(mesh.global_channels);

    const uint32_t instance_index = 0;
    inst_vb_ = gpu.upload(&instance_index, 4);
    if (!mesh.dynamic) {
        if (mesh.instance.empty()) throw std::runtime_error("Missing original mesh instance data");
        t2_ = gpu.upload(mesh.instance.data(), mesh.instance.size());
    }
    for (const auto& b : mesh.buffers) {
        BufferSet set;
        set.ib = gpu.upload(b.ib.data.data(), b.ib.data.size());
        set.vb0 = gpu.upload(b.vb0.data.data(), b.vb0.data.size());
        if (!b.vb1.data.empty()) set.vb1 = gpu.upload(b.vb1.data.data(), b.vb1.data.size());
        set.ibv = {set.ib->GetGPUVirtualAddress(), UINT(b.ib.data.size()),
                   b.ib.is_32bit ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
        set.vbv[0] = {set.vb0->GetGPUVirtualAddress(), UINT(b.vb0.data.size()), b.vb0.stride};
        if (set.vb1) set.vbv[1] = {set.vb1->GetGPUVirtualAddress(), UINT(b.vb1.data.size()), b.vb1.stride};
        set.vbv[2] = {inst_vb_->GetGPUVirtualAddress(), 4, 4};
        if (mesh.dynamic) {
            auto up = [&](const std::vector<uint32_t>& v) -> ComPtr<ID3D12Resource> {
                return v.empty() ? ComPtr<ID3D12Resource>{} : gpu.upload(v.data(), v.size() * 4);
            };
            if (!b.skin.bones.empty() && b.skin_pos.size() == b.skin.bones.size() * 2 && b.vb0.stride == 24) {
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
            if (b.color.size() >= 4) set.color = gpu.upload(b.color.data(), b.color.size() & ~size_t(3));
        }
        sets_.push_back(std::move(set));
    }

    std::map<uint32_t, ComPtr<ID3D12Resource>> shared_textures;
    std::map<uint32_t, ComPtr<ID3D12RootSignature>> shared_roots;
    std::map<std::tuple<uint32_t, uint8_t, uint8_t, uint8_t, bool>, ComPtr<ID3D12PipelineState>> shared_pipelines;
    auto upload_textures = [&](const auto& source, const StageResourceSources& sources, auto& destination) {
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
                if (item.srv_first != UINT32_MAX) gpu.srv().free(item.srv_first, kItemSrvs * kSrvCopies);
            }
        } upload_guard{gpu, item};
        item.part = g.part;
        item.buffers = g.buffers;
        item.cb0 = gpu.upload(g.cb0.data(), g.cb0.size() * sizeof(float4));
        item.vs_cb0 = gpu.upload(g.vs_cb0.data(), g.vs_cb0.size() * sizeof(float4));
        item.vs_material = std::move(g.vs_material);
        item.ps_material = std::move(g.ps_material);
        item.vs_live = g.vs_live;
        item.ps_live = g.ps_live;
        item.technique = g.info.technique;
        item.scopes = std::move(g.scopes);
        item.vs_sources = std::move(g.vs_sources);
        item.ps_sources = std::move(g.ps_sources);
        auto& root = shared_roots[g.info.technique.value];
        if (!root) root = make_gbuffer_root_signature(dev, mesh.dynamic, g.vs_samplers, g.ps_samplers);
        item.root_sig = root;
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
        const auto& source_buffers = sets_.at(g.buffers);
        for (const auto& [slot, kind] : vs_kinds) {
            const bool color = mesh.dynamic && (slot == 0 || slot == 1) && source_buffers.color;
            const bool skin = mesh.dynamic && slot >= 2 && slot <= 4 && source_buffers.skin_pos;
            const bool instance_data = !mesh.dynamic && slot == 2;  // the mesh's own t2, a root SRV
            if (!item.vs_textures.contains(slot) && !item.vs_sources.extern_textures.contains(slot) && !color && !skin &&
                !instance_data)
                throw std::runtime_error(std::format("{}: missing original VS resource t{}", item.technique.str(), slot));
        }
        item.srv_first = gpu.srv().alloc(kItemSrvs * kSrvCopies);
        for (uint32_t copy = 0; copy < kSrvCopies; ++copy) {
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
            const BufferSet& bs = sets_.at(g.buffers);
            for (UINT s = 0; s < kVsSrvs; ++s) {
                const auto h = gpu.srv().cpu(srv_first + kPsSrvs + s);
                const BindingKind k = kind_of(vs_kinds, s);
                if (auto f = item.vs_textures.find(s); f != item.vs_textures.end()) {
                    create_texture_srv(dev, f->second.Get(), k, h);
                } else if (mesh.dynamic && (s == 0 || s == 1) && bs.color) {
                    create_buffer_srv(dev, bs.color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, UINT(bs.color->GetDesc().Width / 4), h);
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


        // Input layout straight from the game's vertex layout table.
        std::vector<D3D12_INPUT_ELEMENT_DESC> ied;
        for (const auto& e : layouts.at(g.input_layout)) {
            D3D12_INPUT_ELEMENT_DESC d{};
            if (e.buffer >= 4 || sets_.at(g.buffers).vbv[e.buffer].BufferLocation == 0)
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
        const bool transparent = g.stage == kStageTransparent;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = item.root_sig.Get();
        pd.VS = {g.vs.data(), g.vs.size()};
        pd.PS = {g.ps.data(), g.ps.size()};
        if (transparent) pd.BlendState.RenderTarget[0] = blend_desc(g.blend);
        else pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = transparent ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = transparent ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_GREATER;
        pd.InputLayout = {ied.data(), UINT(ied.size())};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // Strips restart at the all-ones index.
        if (g.part.primitive_type == 5)
            pd.IBStripCutValue = mesh.buffers.at(g.buffers).ib.is_32bit ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                       : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
        pd.NumRenderTargets = transparent ? 1 : 4;
        for (UINT r = 0; r < pd.NumRenderTargets; ++r) pd.RTVFormats[r] = GBuffer::kFormat;
        for (const auto& output : shader_outputs(g.ps))
            if (_stricmp(output.semantic.c_str(), "SV_Target") == 0 && output.index >= pd.NumRenderTargets)
                throw std::runtime_error(std::format("{}: original PS writes SV_Target{}; original pass attachments must be reconstructed",
                    item.technique.str(), output.index));
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        const auto pipeline_key = std::tuple{g.info.technique.value, g.input_layout, g.stage, g.part.primitive_type,
                                             mesh.buffers.at(g.buffers).ib.is_32bit};
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

        transparent_count_ += transparent;
        items_.push_back(std::move(item));
        upload_guard.committed = true;
        info_.push_back(std::move(g.info));
    }
    gpu.flush();  // texture uploads
    tag_ = mesh.tag;
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
    if (!dynamic_) cl->SetGraphicsRootShaderResourceView(2, t2_->GetGPUVirtualAddress());
}

void StaticMeshScene::update_materials(Gpu& gpu, uint32_t w, uint32_t h, const ViewMatrices& vm) {
    if (!material_externs_) return;
    auto& ext = *material_externs_;
    fill_view_externs(ext, w, h, vm);
    ext.set(ext::Frame, 0x00, material_time_);
    ext.set(ext::Frame, 0x04, material_time_);
    ext.set(ext::Frame, 0x14, material_delta_);
    estimate_engine_frame(ext, material_time_, material_delta_);
    // A single model has no map around it: no atmosphere, no overhead visibility map.
    for (uint8_t id : {ext::Atmosphere, uint8_t(80), uint8_t(81), uint8_t(82)})
        if (!ext.zero_policy(id)) ext.zero_when_missing(id, "world state from a map (not loaded): zero");
    ext.set(ext::Deferred, 0x00, float4{0, 1.0f / vm.vp_rel.c[3][2], 0, 0});
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
    std::map<std::pair<uint32_t, ShaderStage>, D3D12_GPU_VIRTUAL_ADDRESS> scope_cache;
    for (auto& it : items_) {
        it.scope_addresses.clear();
        auto bind_scopes = [&](ShaderStage stage, const StageResourceSources& sources, const auto& required) {
            for (const auto reg : required) {
                if (reg == 0) continue;
                const auto source = sources.cbuffer_scopes.find(reg);
                if (source == sources.cbuffer_scopes.end()) throw std::runtime_error(std::format("{}: no original shared scope b{}", it.technique.str(), reg));
                const auto key = std::pair{source->second.value, stage};
                auto cached = scope_cache.find(key);
                if (cached == scope_cache.end()) {
                    const auto scope = std::find_if(it.scopes.begin(), it.scopes.end(), [&](const Scope& value) { return value.tag == source->second; });
                    if (scope == it.scopes.end()) throw std::runtime_error("Original shared scope was not loaded");
                    std::vector<float4> constants;
                    if (ext.has_scope_constants(stage, reg)) constants = ext.scope_constants(stage, reg);
                    else if (dynamic_ && (scope->name == "rigid_model" || scope->name == "skinning")) {
                        // The object's own constants: local_to_world, position / texcoord packing, skinning.
                        constants.assign(model_cb_.begin(), model_cb_.end());
                    } else if (scope->name == "frame") {
                        const auto& frame = ext.frame_constants();
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
                        else constants = evaluate_scope(*scope, scope_stage(stage), ext);
                    }
                    const auto upload = gpu.dynamic(constants.size() * sizeof(float4));
                    std::memcpy(upload.cpu, constants.data(), constants.size() * sizeof(float4));
                    cached = scope_cache.emplace(key, upload.gpu).first;
                }
                it.scope_addresses[{stage, reg}] = cached->second;
            }
        };
        bind_scopes(ShaderStage::Vertex, it.vs_sources, it.vs_cbvs);
        bind_scopes(ShaderStage::Pixel, it.ps_sources, it.ps_cbvs);
        auto evaluate = [&](const TechniqueStage& material, std::vector<float4>& cb, ID3D12Resource* fixed, bool vs) {
            const uint64_t key = (uint64_t(it.technique.value) << 1) | uint64_t(vs);
            if (auto shared = material_addresses_.find(key); shared != material_addresses_.end()) return shared->second;
            if (!(vs ? it.vs_live : it.ps_live)) return fixed->GetGPUVirtualAddress();
            evaluate_tfx(material.bytecode, material.constants, material.cbuffer_init, cb, with_channels(base, cc, vs));
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
                if (!view.resource && ext.estimate_null_texture(source.extern_id, uint32_t(source.offset) * 8)) {
                    create_texture_srv(gpu.device(), nullptr, kind->second,
                        gpu.srv().cpu(it.srv_first + srv_frame_ * kItemSrvs + (vs ? kPsSrvs : 0) + slot));
                    continue;
                }
                if (!view.resource) throw std::runtime_error(std::format("{}: missing game {} t{} from extern {} byte 0x{:X}",
                    it.technique.str(), vs ? "VS" : "PS", slot, source.extern_id, uint32_t(source.offset) * 8));
                write_game_srv(gpu.device(), view, kind->second,
                    gpu.srv().cpu(it.srv_first + srv_frame_ * kItemSrvs + (vs ? kPsSrvs : 0) + slot));
            }
        };
        bind_shared(it.vs_sources, it.vs_kinds, true);
        bind_shared(it.ps_sources, it.ps_kinds, false);
        it.vs_address = evaluate(it.vs_material, it.vs_constants, it.vs_cb0.Get(), true);
        it.ps_address = evaluate(it.ps_material, it.ps_constants, it.cb0.Get(), false);
    }
}

void StaticMeshScene::draw_items(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint8_t stage, uint32_t w, uint32_t h, const ViewMatrices& vm) {
    uint32_t bound = UINT32_MAX;
    ID3D12RootSignature* bound_root = nullptr;
    for (const auto& it : items_) {
        if (it.stage != stage) continue;
        if (bound_root != it.root_sig.Get()) {
            bind_common(gpu, cl, w, h, vm, it.root_sig.Get());
            bound_root = it.root_sig.Get();
        }
        if (it.buffers != bound) {
            bound = it.buffers;
            cl->IASetIndexBuffer(&sets_[bound].ibv);
            cl->IASetVertexBuffers(0, 4, sets_[bound].vbv);
        }
        cl->SetPipelineState(it.pso.Get());
        cl->SetGraphicsRootConstantBufferView(0, it.ps_address);
        cl->SetGraphicsRootConstantBufferView(6, it.vs_address);
        for (const auto& [key, address] : it.scope_addresses)
            cl->SetGraphicsRootConstantBufferView(scope_root_parameter(key.first, key.second), address);
        const uint32_t first = it.srv_first + srv_frame_ * kItemSrvs;
        cl->SetGraphicsRootDescriptorTable(3, gpu.srv().gpu(first));
        cl->SetGraphicsRootDescriptorTable(dynamic_ ? 2 : 30, gpu.srv().gpu(first + kPsSrvs));
        cl->IASetPrimitiveTopology(it.part.primitive_type == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                               : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawIndexedInstanced(it.part.index_count, 1, it.part.index_start, 0, 0);
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

void StaticMeshScene::render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm) {
    srv_frame_ = (srv_frame_ + 1) % kSrvCopies;
    update_materials(gpu, gb.width(), gb.height(), vm);
    if (skin_dirty_) update_skin(cl);
    const float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) cl->ClearRenderTargetView(gb.rtv[i], clear, 0, nullptr);
    cl->ClearDepthStencilView(gb.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    if (items_.size() == transparent_count_) return;
    cl->OMSetRenderTargets(4, gb.rtv, FALSE, &gb.dsv);
    draw_items(gpu, cl, 0, gb.width(), gb.height(), vm);
}

void StaticMeshScene::render_transparent(Gpu&, ID3D12GraphicsCommandList*, GBuffer&, D3D12_CPU_DESCRIPTOR_HANDLE,
                                         const ViewMatrices&) {
    if (!transparent_count_) return;
    throw std::runtime_error("Original transparent pass inputs/MRTs have not been reconstructed; substitution is disabled");
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
