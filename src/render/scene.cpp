#include "nim/render/scene.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

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
        hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
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

// G-buffer root signature: b0/b12 root CBVs, t2 root SRV for the VS instance buffer, t0-t19 table
// for the PS, static samplers s0-s15.
// Static meshes read their instance data from a raw t2 (root SRV); dynamic models read the
// skinning streams t2-t4 as typed buffers (a table).
ComPtr<ID3D12RootSignature> make_gbuffer_root_signature(ID3D12Device* dev, bool dynamic = false) {
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 0, 0, 0};
    D3D12_DESCRIPTOR_RANGE skin_range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 2, 0, 0};
    D3D12_ROOT_PARAMETER params[6]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {12, 0};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    if (dynamic) {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable = {1, &skin_range};
    } else {
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor = {2, 0};
    }
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {1, &range};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // rigid_model scope
    params[4].Descriptor = {1, 0};
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;  // frame scope
    params[5].Descriptor = {13, 0};
    params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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
    D3D12_ROOT_SIGNATURE_DESC rsd{6, params, 16, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> blob, err;
    check_hr(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err), "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> rs;
    check_hr(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)),
             "CreateRootSignature");
    return rs;
}

// Externs the material bytecode reads that only the engine provides. Extern 0x61 (97) feeds
// world-space detail-layer transforms with distance fades; push its fade ranges out of reach so the
// shader takes its base-material path, and keep the divisions finite.
TfxEnvironment default_environment() {
    TfxEnvironment env;
    env.extern_vec4 = [](uint8_t id, uint8_t off) -> float4 {
        if (id == 0x61 && (off == 0x06 || off == 0x10 || off == 0x1A)) return {1e9f, 2e9f, 0, 0};
        return {0, 0, 0, 0};
    };
    return env;
}

} // namespace

void StaticMeshScene::release(Gpu& gpu) {
    for (auto& it : items_) gpu.srv().free(it.srv_first, 20);
    for (auto& st : sets_)
        if (st.srv_first != UINT32_MAX) gpu.srv().free(st.srv_first, 3);
    items_.clear();
    info_.clear();
    sets_.clear();
    notes_.clear();
    inst_vb_.Reset(); zero_vb_.Reset(); t2_.Reset(); fallback_.Reset();
    dynamic_ = false;
    tag_ = TagHash();
}

namespace {

// Shaders, material cbuffer (the PS stage's TFX over its init values) and textures of one draw.
PreparedMesh::Group build_group(PackageManager& pm, const Technique& tech, const StaticMeshPart& part,
                                uint16_t part_index, uint8_t input_layout, const TfxEnvironment& env) {
    PreparedMesh::Group g;
    g.part = part;
    g.input_layout = input_layout;
    const auto& vs_stage = tech.stage(ShaderStage::Vertex);
    const auto& ps_stage = tech.stage(ShaderStage::Pixel);
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
        TfxResult r = run_tfx(ps_stage.bytecode, ps_stage.constants, g.cb0, env);
        info.vm_instructions = r.instructions;
        info.stubbed_ops = r.unsupported;
        info.extern_textures = r.extern_textures;
    }
    for (auto& v : g.cb0)
        for (auto& c : v)
            if (!std::isfinite(c)) c = 0;

    for (const auto& tb : ps_stage.textures) {
        SceneTextureInfo ti{tb.slot, tb.texture, 0, 0, 0, 0, {}};
        try {
            Texture tx = load_texture(pm, tb.texture);
            ti.format = tx.dxgi_format;
            ti.width = tx.width;
            ti.height = tx.height;
            ti.mips = tx.mip_count;
            if (tx.depth != 1 || tx.array_size != 1) ti.error = "not a 2D texture (fallback bound)";
            else g.textures[tb.slot] = std::move(tx);
        } catch (const std::exception& ex) {
            ti.error = ex.what();
        }
        info.textures.push_back(std::move(ti));
    }
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

// The bind pose in the layout the game's compute skinning writes (24-byte layout-7 vertices).
bool pack_skin_streams(const VertexBuffer& vb0, V3 scale, float scale_1d, std::vector<uint32_t>& pos,
                       std::vector<uint32_t>& nrm) {
    if (vb0.stride != 24 || scale_1d <= 1e-8f || vb0.data.size() % 24) return false;
    const size_t n = vb0.data.size() / 24;
    pos.resize(n * 2);
    nrm.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* v = vb0.data.data() + i * 24;
        const int32_t x = quant_snorm(s16(v) * scale.x / scale_1d, 21);
        const int32_t y = quant_snorm(s16(v + 2) * scale.y / scale_1d, 21);
        const int32_t z = quant_snorm(s16(v + 4) * scale.z / scale_1d, 22);
        pos[i * 2] = (uint32_t(x) & 0x1FFFFF) | ((uint32_t(y) & 0x7FF) << 21);
        pos[i * 2 + 1] = ((uint32_t(y) >> 11) & 0x3FF) | ((uint32_t(z) & 0x3FFFFF) << 10);
        auto [nu, nv] = oct_encode({s16(v + 8), s16(v + 10), s16(v + 12)});
        auto [tu, tv] = oct_encode({s16(v + 16), s16(v + 18), s16(v + 20)});
        nrm[i * 2] = (uint32_t(quant_snorm(nv, 16)) << 16) | (uint32_t(quant_snorm(nu, 16)) & 0xFFFF);
        nrm[i * 2 + 1] = (uint32_t(quant_snorm(tv, 16)) << 16) | (uint32_t(quant_snorm(tu, 16)) & 0xFFFF);
    }
    return true;
}

bool vs_reads_typed_buffers(const std::vector<uint8_t>& vs) {
    for (const auto& b : shader_bindings(vs))
        if (b.type == BindingType::Srv && (b.kind == BindingKind::TypedBuffer || b.kind == BindingKind::RawBuffer) &&
            b.lower >= 2 && b.lower <= 4)
            return true;
    return false;
}

} // namespace

PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials,
                                   int permutation) {
    PreparedMesh out;
    out.tag = tag;
    out.dynamic = true;
    uint32_t perm = 0;
    if (materials) {
        perm = permutation < 0 ? materials->default_permutation : uint32_t(permutation);
        out.component = materials->component;
        out.permutation = perm;
        out.permutation_count = materials->permutation_count;
    }
    DynamicModel model = load_dynamic_model(pm, tag);
    const V3 scale{model.model_scale[0], model.model_scale[1], model.model_scale[2]};
    const V3 offset{model.model_offset[0], model.model_offset[1], model.model_offset[2]};
    const TfxEnvironment env = default_environment();
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    uint8_t lod = 0xFF;
    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const DynamicMesh& mesh = model.meshes[mi];
        const size_t first = mesh.part_range[0], last = std::min<size_t>(mesh.part_range[1], mesh.parts.size());
        if (first >= last) continue;
        // Highest detail: lod_category bit 0 (goriath); fall back to every part when none has it.
        bool any_high = false;
        for (size_t k = first; k < last; ++k) any_high |= (mesh.parts[k].lod_category & 1) != 0;
        PreparedMesh::Buffers bufs;
        try {
            bufs.ib = load_index_buffer(pm, mesh.index);
            bufs.vb0 = load_vertex_buffer(pm, mesh.vertex0);
            if (!mesh.vertex1.is_none()) bufs.vb1 = load_vertex_buffer(pm, mesh.vertex1);
        } catch (const std::exception& ex) {
            out.notes.push_back(std::format("mesh {}: buffers: {}", mi, ex.what()));
            continue;
        }
        const float s1 = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
        const bool packed = !mesh.skinning.is_none() && pack_skin_streams(bufs.vb0, scale, s1, bufs.skin_pos, bufs.skin_normal);
        const uint32_t set = uint32_t(out.buffers.size());
        bool used = false;
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
                PreparedMesh::Group g = build_group(pm, t, part, uint16_t(k), mesh.input_layout[0], env);
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
        if (used) out.buffers.push_back(std::move(bufs));
    }
    if (out.groups.empty())
        throw std::runtime_error(out.notes.empty() ? "dynamic model has no drawable G-buffer parts" : out.notes.front());
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
    cb[7] = {0, 0, 0, 0.8f};  // dynamic SH / AO
    // c8-c11 skinning transform (identity), c12/c13 packed-position offset + 1D scale, c15-c18 identity.
    const float s1 = std::max({std::fabs(scale.x), std::fabs(scale.y), std::fabs(scale.z)});
    cb[8] = {1, 0, 0, 0}; cb[9] = {0, 1, 0, 0}; cb[10] = {0, 0, 1, 0}; cb[11] = {0, 0, 0, 1};
    cb[12] = cb[13] = {offset.x, offset.y, offset.z, s1};
    cb[15] = {1, 0, 0, 0}; cb[16] = {0, 1, 0, 0}; cb[17] = {0, 0, 1, 0}; cb[18] = {0, 0, 0, 1};
    return out;
}

PreparedMesh prepare_mesh(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials) {
    const Entry* e = pm.entry(tag);
    if (e && e->reference == kClassDynamicModel) return prepare_dynamic_model(pm, tag, materials);
    return prepare_static_mesh(pm, tag);
}

PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash tag) {
    PreparedMesh out;
    out.tag = tag;
    StaticMesh mesh = load_static_mesh(pm, tag);

    struct Pending { StaticMeshGroup group; StaticMeshPart part; Technique tech; };
    std::vector<Pending> pending;
    uint8_t lod = 0xFF;
    for (const auto& g : mesh.groups)
        if (g.render_stage == 0) lod = std::min(lod, mesh.parts.at(g.part_index).lod_category);
    for (size_t g = 0; g < mesh.groups.size(); ++g) {
        const auto& gr = mesh.groups[g];
        if (gr.render_stage != 0 || g >= mesh.techniques.size()) continue;
        const auto& part = mesh.parts.at(gr.part_index);
        if (part.lod_category != lod) continue;
        Technique t = load_technique(pm, mesh.techniques[g]);
        if (t.stage(ShaderStage::Vertex).shader.is_none() || t.stage(ShaderStage::Pixel).shader.is_none()) continue;
        pending.push_back({gr, part, std::move(t)});
    }
    if (pending.empty()) throw std::runtime_error("mesh has no G-buffer groups with a vertex + pixel shader");
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

    const TfxEnvironment env = default_environment();
    for (auto& p : pending)
        out.groups.push_back(build_group(pm, p.tech, p.part, p.group.part_index, p.group.input_layout, env));
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
    notes_ = mesh.notes;

    const uint32_t instance_index = 0;
    inst_vb_ = gpu.upload(&instance_index, 4);
    const float zeros[4] = {0, 0, 0, 0};
    zero_vb_ = gpu.upload(zeros, sizeof(zeros));
    if (mesh.instance.empty()) mesh.instance.assign(128, 0);
    t2_ = gpu.upload(mesh.instance.data(), mesh.instance.size());
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
        set.vbv[3] = {zero_vb_->GetGPUVirtualAddress(), 16, 0};
        if (mesh.dynamic) {
            const uint32_t zero[2] = {0, 0};
            auto up = [&](const std::vector<uint32_t>& v) {
                return v.empty() ? gpu.upload(zero, sizeof(zero)) : gpu.upload(v.data(), v.size() * 4);
            };
            set.skin_pos = up(b.skin_pos);
            set.skin_normal = up(b.skin_normal);
            set.srv_first = gpu.srv().alloc(3);
            ID3D12Resource* res[3] = {set.skin_pos.Get(), set.skin_normal.Get(), set.skin_pos.Get()};
            for (UINT k = 0; k < 3; ++k) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R32_UINT;
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Buffer.NumElements = UINT(res[k]->GetDesc().Width / 4);
                dev->CreateShaderResourceView(res[k], &sd, gpu.srv().cpu(set.srv_first + k));
            }
        }
        sets_.push_back(std::move(set));
    }

    Texture grey;  // engine-provided textures we do not have yet
    grey.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    grey.width = grey.height = 1;
    grey.data = {128, 128, 128, 255};
    fallback_ = gpu.texture(grey);

    for (auto& g : mesh.groups) {
        Item item;
        item.part = g.part;
        item.buffers = g.buffers;
        item.cb0 = gpu.upload(g.cb0.data(), g.cb0.size() * sizeof(float4));
        for (auto& [slot, tx] : g.textures) item.textures[slot] = gpu.texture(tx);
        item.srv_first = gpu.srv().alloc(20);
        for (UINT s = 0; s < 20; ++s) {
            auto f = item.textures.find(s);
            ID3D12Resource* r = f != item.textures.end() ? f->second.Get() : fallback_.Get();
            D3D12_RESOURCE_DESC rd = r->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = rd.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = rd.MipLevels;
            dev->CreateShaderResourceView(r, &sd, gpu.srv().cpu(item.srv_first + s));
        }

        // Input layout straight from the game's vertex layout table.
        std::vector<D3D12_INPUT_ELEMENT_DESC> ied;
        for (const auto& e : layouts.at(g.input_layout)) {
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
        // Inputs the VS declares but the layout lacks read zeros from slot 3 (stride 0).
        for (const auto& in : shader_inputs(g.vs)) {
            bool found = false;
            for (const auto& d : ied)
                found |= d.SemanticIndex == in.index && _stricmp(d.SemanticName, in.semantic.c_str()) == 0;
            if (found) continue;
            const char* name = nullptr;
            for (const char* known : {"POSITION", "NORMAL", "TANGENT", "TEXCOORD", "COLOR", "BLENDWEIGHT", "BLENDINDICES", "BINORMAL"})
                if (_stricmp(known, in.semantic.c_str()) == 0) name = known;
            if (!name) continue;
            D3D12_INPUT_ELEMENT_DESC d{};
            d.SemanticName = name;
            d.SemanticIndex = in.index;
            d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            d.InputSlot = 3;
            d.AlignedByteOffset = 0;
            d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            ied.push_back(d);
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = mesh.dynamic ? root_sig_dynamic_.Get() : root_sig_.Get();
        pd.VS = {g.vs.data(), g.vs.size()};
        pd.PS = {g.ps.data(), g.ps.size()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
        pd.InputLayout = {ied.data(), UINT(ied.size())};
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // Strips restart at the all-ones index.
        if (g.part.primitive_type == 5)
            pd.IBStripCutValue = mesh.buffers.at(g.buffers).ib.is_32bit ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                                                       : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
        pd.NumRenderTargets = 4;
        for (int r = 0; r < 4; ++r) pd.RTVFormats[r] = GBuffer::kFormat;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&item.pso)), "CreateGraphicsPipelineState (game DXIL)");

        items_.push_back(std::move(item));
        info_.push_back(std::move(g.info));
    }
    gpu.flush();  // texture uploads
    tag_ = mesh.tag;
}

void StaticMeshScene::render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm) {
    const float clear[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) cl->ClearRenderTargetView(gb.rtv[i], clear, 0, nullptr);
    cl->ClearDepthStencilView(gb.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    if (items_.empty()) return;

    // cb12 "scope_view" (464 bytes): regs 0-3 = view-projection columns (absolute), 7 = camera
    // position, 19 = projection translation for camera-relative positions, 20-23 = previous frame.
    auto cb12 = gpu.dynamic(32 * sizeof(float4));
    auto* r = static_cast<float4*>(cb12.cpu);
    std::memset(r, 0, 32 * sizeof(float4));
    for (int j = 0; j < 4; ++j) r[j] = vm.vp_abs.c[j];
    r[7] = {vm.eye.x, vm.eye.y, vm.eye.z, 1};
    r[19] = vm.vp_rel.c[3];
    for (int j = 0; j < 4; ++j) r[20 + j] = vm.vp_abs.c[j];
    // VS reg 20 is View 0x40 in the game's view scope: the world -> camera-relative offset the
    // skinned vertex shaders add to the mesh translation.
    r[20] = {-vm.eye.x, -vm.eye.y, -vm.eye.z, 0};

    cl->OMSetRenderTargets(4, gb.rtv, FALSE, &gb.dsv);
    D3D12_VIEWPORT vp{0, 0, float(gb.width()), float(gb.height()), 0, 1};
    D3D12_RECT sc{0, 0, LONG(gb.width()), LONG(gb.height())};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(dynamic_ ? root_sig_dynamic_.Get() : root_sig_.Get());
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetGraphicsRootConstantBufferView(1, cb12.gpu);
    if (!dynamic_) cl->SetGraphicsRootShaderResourceView(2, t2_->GetGPUVirtualAddress());
    // cb1 rigid_model: the mesh sits at the world origin (the VS subtracts cb12[7] itself).
    auto cb1 = gpu.dynamic(sizeof(model_cb_));
    std::memcpy(cb1.cpu, model_cb_.data(), sizeof(model_cb_));
    cl->SetGraphicsRootConstantBufferView(4, cb1.gpu);
    // cb13 frame: time 0, unit exposure (see GameLighting for the full layout).
    auto cb13 = gpu.dynamic(16 * sizeof(float4));
    auto* f = static_cast<float4*>(cb13.cpu);
    std::memset(f, 0, 16 * sizeof(float4));
    f[1] = {1, 16, 1, 1};
    f[3] = {0.5f, 0.5f, 0, 0};
    f[4] = {1, 1, 0, 1};
    f[6] = {1, 1, 1, 1};
    cl->SetGraphicsRootConstantBufferView(5, cb13.gpu);
    uint32_t bound = UINT32_MAX;
    for (const auto& it : items_) {
        if (it.buffers != bound) {
            bound = it.buffers;
            cl->IASetIndexBuffer(&sets_[bound].ibv);
            cl->IASetVertexBuffers(0, 4, sets_[bound].vbv);
            if (dynamic_) cl->SetGraphicsRootDescriptorTable(2, gpu.srv().gpu(sets_[bound].srv_first));
        }
        cl->SetPipelineState(it.pso.Get());
        cl->SetGraphicsRootConstantBufferView(0, it.cb0->GetGPUVirtualAddress());
        cl->SetGraphicsRootDescriptorTable(3, gpu.srv().gpu(it.srv_first));
        cl->IASetPrimitiveTopology(it.part.primitive_type == 5 ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                               : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawIndexedInstanced(it.part.index_count, 1, it.part.index_start, 0, 0);
    }
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
// Lighting is nim's own (GGX sun + sky) until the game's deferred lighting shaders are run.
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

float3 sky(float3 d) {
    float t = d.z;
    float3 c = t > 0 ? lerp(sky_horizon, sky_top, pow(saturate(t), 0.6)) : lerp(sky_horizon, ground, saturate(-t * 4));
    return c * sky_intensity;
}

float3 env_brdf_approx(float3 f0, float rough, float ndv) {  // Karis, mobile split-sum fit
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float3 aces(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
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

    float rough = max(0.03, 1 - smooth);
    float alpha = rough * rough;
    float3 V = -ray;
    float3 L = sun_dir;
    float3 H = normalize(V + L);
    float ndl = saturate(dot(n, L)), ndv = max(dot(n, V), 1e-4), ndh = saturate(dot(n, H)), vdh = saturate(dot(V, H));
    float3 f0 = lerp(0.04, albedo, metal);
    float3 F = f0 + (1 - f0) * pow(1 - vdh, 5);
    float a2 = alpha * alpha;
    float D = a2 / (PI * pow(ndh * ndh * (a2 - 1) + 1, 2));
    float k = alpha * 0.5;
    float G = (ndl / (ndl * (1 - k) + k)) * (ndv / (ndv * (1 - k) + k));
    float3 spec = D * G * F / max(4 * ndl * ndv, 1e-4);
    float3 diffuse = albedo * (1 - metal) * (1 - F) / PI;
    float3 sun = sun_color * sun_intensity;
    float3 color = (diffuse + spec) * sun * ndl;

    float3 irradiance = lerp(sky(-n) * 0.15 + ground * 0.2, sky(n), saturate(n.z * 0.5 + 0.5));
    color += albedo * (1 - metal) * irradiance * occl;
    float3 R = reflect(ray, n);
    color += env_brdf_approx(f0, rough, ndv) * lerp(sky(R) * 0.35, sky(R), smooth) * occl;
    color += albedo * emission;

    return float4(srgb(aces(color * exposure)), 1);
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
