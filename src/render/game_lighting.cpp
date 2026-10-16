#include "nim/render/game_lighting.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nim {
namespace {

uint32_t fnv1(std::string_view s) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : s) h = (h * 0x01000193u) ^ c;
    return h;
}

uint64_t tex_key(uint8_t id, uint32_t off) { return uint64_t(id) << 32 | off; }

// What an engine texture nobody rendered should read as.
float4 filler_value(uint8_t id, uint32_t byte_offset) {
    switch (id) {
    case ext::ShadowMask: return {1, 1, 1, 1};  // unshadowed, unoccluded
    case ext::Atmosphere:
        // 0x108: the froxel volume (x = transmittance, y = sky visibility, z = in-scatter amount):
        // clear air. Everything else: black, opaque.
        if (byte_offset == 0x108) return {1, 1, 0, 1};
        if (byte_offset == 0x80) return {1, 1, 1, 1};  // fog density noise: none
        return {0, 0, 0, 1};
    case ext::Transparent: return byte_offset == 0x50 ? float4{1, 1, 1, 1} : float4{0, 0, 0, 0};
    default: return {0, 0, 0, 0};
    }
}

bool kind_is_texture(BindingKind k) {
    return k == BindingKind::Texture2D || k == BindingKind::Texture3D || k == BindingKind::TextureCube ||
           k == BindingKind::Texture2DArray || k == BindingKind::TextureCubeArray || k == BindingKind::Texture1D;
}

bool resource_fits(ID3D12Resource* r, BindingKind k) {
    D3D12_RESOURCE_DESC d = r->GetDesc();
    switch (k) {
    case BindingKind::Texture3D: return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    case BindingKind::TextureCube:
    case BindingKind::TextureCubeArray:
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize % 6 == 0;
    case BindingKind::Texture2D:
    case BindingKind::Texture2DArray: return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    default: return false;
    }
}

void write_srv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, BindingKind k, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (!r) {  // null descriptor: reads return zero
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        switch (k) {
        case BindingKind::Texture3D: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1; break;
        case BindingKind::TextureCube: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1; break;
        case BindingKind::Texture2DArray:
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = 1;
            break;
        case BindingKind::TextureCubeArray:
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            sd.TextureCubeArray.MipLevels = 1;
            sd.TextureCubeArray.NumCubes = 1;
            break;
        case BindingKind::TypedBuffer: case BindingKind::RawBuffer: case BindingKind::StructuredBuffer:
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = 1;
            break;
        default: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; break;
        }
        dev->CreateShaderResourceView(nullptr, &sd, h);
        return;
    }
    D3D12_RESOURCE_DESC d = r->GetDesc();
    sd.Format = fmt != DXGI_FORMAT_UNKNOWN ? fmt : d.Format;
    switch (k) {
    case BindingKind::Texture3D:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = UINT(-1);
        break;
    case BindingKind::TextureCube:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MipLevels = UINT(-1);
        break;
    case BindingKind::TextureCubeArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
        sd.TextureCubeArray.MipLevels = UINT(-1);
        sd.TextureCubeArray.NumCubes = d.DepthOrArraySize / 6;
        break;
    case BindingKind::Texture2DArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Texture2DArray.MipLevels = UINT(-1);
        sd.Texture2DArray.ArraySize = d.DepthOrArraySize;
        break;
    default:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = UINT(-1);
        break;
    }
    dev->CreateShaderResourceView(r, &sd, h);
}

D3D12_STATIC_SAMPLER_DESC static_sampler(const SamplerDesc& s, uint32_t reg, D3D12_SHADER_VISIBILITY vis) {
    D3D12_STATIC_SAMPLER_DESC d{};
    d.Filter = D3D12_FILTER(s.filter);
    d.AddressU = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<uint32_t>(s.address_u, 1, 5));
    d.AddressV = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<uint32_t>(s.address_v, 1, 5));
    d.AddressW = D3D12_TEXTURE_ADDRESS_MODE(std::clamp<uint32_t>(s.address_w, 1, 5));
    d.MipLODBias = std::min(s.mip_lod_bias, 0.0f);
    d.MaxAnisotropy = std::clamp<uint32_t>(s.max_anisotropy, 1, 16);
    d.ComparisonFunc = D3D12_COMPARISON_FUNC(std::clamp<uint32_t>(s.comparison_func, 1, 8));
    d.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    d.MinLOD = s.min_lod;
    d.MaxLOD = s.max_lod;
    d.ShaderRegister = reg;
    d.ShaderVisibility = vis;
    return d;
}

SamplerDesc linear_clamp() {
    SamplerDesc s;
    s.filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    s.address_u = s.address_v = s.address_w = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    return s;
}

ScopeStage scope_stage(ShaderStage s) {
    switch (s) {
    case ShaderStage::Vertex: return ScopeStage::Vertex;
    case ShaderStage::Geometry: return ScopeStage::Geometry;
    case ShaderStage::Hull: return ScopeStage::Hull;
    case ShaderStage::Domain: return ScopeStage::Domain;
    case ShaderStage::Compute: return ScopeStage::Compute;
    default: return ScopeStage::Pixel;
    }
}

void sanitize(std::vector<float4>& cb) {
    for (auto& v : cb)
        for (auto& c : v) {
            if (std::isnan(c)) c = 0;
            else if (std::isinf(c)) c = std::copysign(3.0e38f, c);
        }
}

ComPtr<ID3D12RootSignature> serialize(ID3D12Device* dev, const D3D12_ROOT_SIGNATURE_DESC& rsd, const char* what) {
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr))
        throw std::runtime_error(std::format("{}: {}", what, err ? static_cast<const char*>(err->GetBufferPointer()) : "serialize failed"));
    ComPtr<ID3D12RootSignature> rs;
    check_hr(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)), what);
    return rs;
}

} // namespace

// ---- ExternStore ------------------------------------------------------------------------------
ExternStore::ExternStore() : data_(size_t(kBlockSize) * kBlocks, 0) {
    // Engine defaults (cross-checked with goriath's extern definitions).
    set(ext::Frame, 0x18, 1.0f / 60.0f);  // exposure_time
    set(ext::Frame, 0x1C, 1.0f);          // exposure_scale
    set(ext::Frame, 0x28, 1.0f);          // exposure_illum_relative
    set(ext::Frame, 0x70, 0.5f);
    set(ext::Frame, 0x160, float4{0.1f, 0.1f, 0.1f, 1.0f});  // wind
    set(ext::Frame, 0x180, float4{0.1f, 0.1f, 0.1f, 0.0f});
    set(ext::Frame, 0x1B0, float4{0, 0, 0, 1});
    set(ext::Frame, 0x1C0, float4{0, 0, 1, 0});
    set(ext::Frame, 0x250, float4{1, 0, 1, 0});
    set(ext::Frame, 0x280, float4{1, 1, 1, 1});
    set(ext::View, 0x40, float4{-1, -1, -1, -1});
    set(ext::Deferred, 0x00, float4{1.0f / 50000.0f, 1.0f / 0.02f, 0, 0});
    set(ext::Deferred, 0x40, float4{1920, 1080, 0, 0});
    for (uint32_t o = 0x100; o <= 0x130; o += 0x10) set(ext::DeferredLight, o, float4{0, 0, 0, 1});
    set(ext::DeferredLight, 0x154, 7500.0f);
    set(ext::RigidModel, 0x40, float4{1, 1, 1, 1});
    set(ext::RigidModel, 0x60, float4{1, 1, 0, 0});
    set(ext::GlobalLighting, 0x30, float4{0, 0, -1, 0});
}

void ExternStore::set(uint8_t id, uint32_t off, float v) {
    if (id >= kBlocks || off + 4 > kBlockSize) throw std::out_of_range("extern offset");
    std::memcpy(data_.data() + size_t(id) * kBlockSize + off, &v, 4);
}
void ExternStore::set(uint8_t id, uint32_t off, float4 v) {
    if (id >= kBlocks || off + 16 > kBlockSize) throw std::out_of_range("extern offset");
    std::memcpy(data_.data() + size_t(id) * kBlockSize + off, v.data(), 16);
}
void ExternStore::set(uint8_t id, uint32_t off, const M4& m) {
    for (int c = 0; c < 4; ++c) set(id, off + uint32_t(c) * 16, m.c[c]);
}
float ExternStore::get_f(uint8_t id, uint32_t off) const {
    if (id >= kBlocks || off + 4 > kBlockSize) return 0;
    float v;
    std::memcpy(&v, data_.data() + size_t(id) * kBlockSize + off, 4);
    return v;
}
float4 ExternStore::get_v(uint8_t id, uint32_t off) const {
    if (id >= kBlocks || off + 16 > kBlockSize) return {};
    float4 v;
    std::memcpy(v.data(), data_.data() + size_t(id) * kBlockSize + off, 16);
    return v;
}
void ExternStore::set_texture(uint8_t id, uint32_t off, ID3D12Resource* r, DXGI_FORMAT f) {
    if (r) textures_[tex_key(id, off)] = {r, f};
    else textures_.erase(tex_key(id, off));
}
ExternStore::Tex ExternStore::texture(uint8_t id, uint32_t off) const {
    auto it = textures_.find(tex_key(id, off));
    return it == textures_.end() ? Tex{} : it->second;
}
void ExternStore::set_channel(std::string_view name, float4 v) {
    const uint32_t h = fnv1(name);
    for (size_t i = 0; i < channel_ids.size() && i < channels.size(); ++i)
        if (channel_ids[i] == h) channels[i] = v;
}
TfxEnvironment ExternStore::environment() const {
    TfxEnvironment env;
    env.extern_vec4 = [this](uint8_t id, uint8_t off) { return get_v(id, uint32_t(off) * 16); };
    env.extern_float = [this](uint8_t id, uint8_t off) { return get_f(id, uint32_t(off) * 4); };
    env.global_channel = [this](uint8_t ch) { return ch < channels.size() ? channels[ch] : float4{1, 1, 1, 1}; };
    return env;
}

// ---- FillerTextures -----------------------------------------------------------------------------
ID3D12Resource* FillerTextures::get(Gpu& gpu, BindingKind kind, float4 v) {
    const int k = kind == BindingKind::Texture3D ? 3
                  : (kind == BindingKind::TextureCube || kind == BindingKind::TextureCubeArray) ? 6
                  : kind == BindingKind::Texture2DArray ? 1 : 2;
    auto key = std::make_pair(k, v);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.Get();
    Texture t;
    t.dxgi_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    t.width = t.height = 1;
    t.depth = k == 3 ? 1 : 1;
    t.array_size = k == 6 ? 6 : 1;
    const int texels = k == 6 ? 6 : 1;
    t.data.resize(size_t(texels) * 16);
    for (int i = 0; i < texels; ++i) std::memcpy(t.data.data() + i * 16, v.data(), 16);
    ComPtr<ID3D12Resource> r;
    if (k == 3) {  // Gpu::texture makes a 3D resource for depth > 1 only; build the 1x1x1 volume here
        t.depth = 2;
        t.data.resize(32);
        std::memcpy(t.data.data() + 16, v.data(), 16);
    }
    r = gpu.texture(t);
    cache_[key] = r;
    return r.Get();
}

// ---- GamePipeline -----------------------------------------------------------------------------------
void GamePipeline::release(Gpu& gpu) {
    if (srv_first_ != UINT32_MAX) gpu.srv().free(srv_first_, srv_total_ * kRing);
    srv_first_ = UINT32_MAX;
    srv_total_ = 0;
    stages_.clear();
    pso_.Reset();
    root_sig_.Reset();
    notes.clear();
}

void GamePipeline::load(PackageManager& pm, Gpu& gpu, TagHash technique, std::span<const Scope* const> scopes,
                        Options opt) {
    release(gpu);
    tag_ = technique;
    Technique t = load_technique(pm, technique);
    ID3D12Device* dev = gpu.device();

    std::vector<D3D12_ROOT_PARAMETER> params;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    ranges.reserve(8);
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    std::vector<uint8_t> vs_code, ps_code;

    for (ShaderStage which : {ShaderStage::Vertex, ShaderStage::Pixel}) {
        const auto& ts = t.stage(which);
        if (ts.shader.is_none()) throw std::runtime_error(std::format("{}: missing {} shader", technique.str(),
                                                                      which == ShaderStage::Vertex ? "vertex" : "pixel"));
        Stage st;
        st.which = which;
        st.tfx = ts;
        auto code = load_shader_bytecode(pm, ts.shader);
        st.bindings = shader_bindings(code);
        (which == ShaderStage::Vertex ? vs_code : ps_code) = std::move(code);
        const auto vis = which == ShaderStage::Vertex ? D3D12_SHADER_VISIBILITY_VERTEX : D3D12_SHADER_VISIBILITY_PIXEL;

        // Dry run for the texture / sampler assignments the bytecode makes.
        TfxResult dry;
        if (!ts.bytecode.empty()) {
            std::vector<float4> cb(ts.cbuffer_init.begin(), ts.cbuffer_init.end());
            cb.resize(std::max<size_t>(cb.size(), 256));
            dry = run_tfx(ts.bytecode, ts.constants, cb, ExternStore().environment());
        }
        st.extern_textures = dry.extern_textures;
        for (const auto& u : dry.unsupported) notes.push_back(std::format("{}: stubbed TFX op {}", ts.shader.str(), u));

        std::map<uint32_t, TagHash> sampler_tags;
        for (const Scope* sc : scopes) {
            const auto& ss = sc->stages[size_t(scope_stage(which))];
            if (ss.bytecode.empty()) continue;
            std::vector<float4> cb(256);
            TfxResult r = run_tfx(ss.bytecode, ss.constants, cb, ExternStore().environment());
            for (const auto& s : r.samplers)
                if (s.index < ss.samplers.size()) sampler_tags[s.slot] = ss.samplers[s.index];
        }
        for (const auto& s : dry.samplers)
            if (s.index < ts.samplers.size()) sampler_tags[s.slot] = ts.samplers[s.index];

        for (const auto& b : st.bindings) {
            if (b.space != 0) {
                notes.push_back(std::format("{}: binding in space {} ignored", ts.shader.str(), b.space));
                continue;
            }
            switch (b.type) {
            case BindingType::Cbv: {
                D3D12_ROOT_PARAMETER p{};
                p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
                p.Descriptor = {b.lower, 0};
                p.ShaderVisibility = vis;
                st.cbvs.push_back({b.lower, uint32_t(params.size())});
                params.push_back(p);
                break;
            }
            case BindingType::Srv:
                for (uint32_t r = b.lower; r <= std::min(b.upper, 63u); ++r) {
                    st.srv_kinds[r] = b.kind;
                    st.srv_count = std::max(st.srv_count, r + 1);
                }
                break;
            case BindingType::Sampler:
                for (uint32_t r = b.lower; r <= std::min(b.upper, 15u); ++r) {
                    SamplerDesc sd = linear_clamp();
                    auto it = sampler_tags.find(r);
                    if (it != sampler_tags.end()) {
                        try {
                            sd = load_sampler(pm, it->second);
                        } catch (const std::exception& ex) {
                            notes.push_back(std::format("s{}: {}", r, ex.what()));
                        }
                    } else {
                        notes.push_back(std::format("{}: s{} has no sampler tag (linear clamp)", ts.shader.str(), r));
                    }
                    samplers.push_back(static_sampler(sd, r, vis));
                }
                break;
            case BindingType::Uav:
                notes.push_back(std::format("{}: UAV u{} not supported", ts.shader.str(), b.lower));
                break;
            default: break;
            }
        }
        if (st.srv_count) {
            ranges.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, st.srv_count, 0, 0, 0});
            D3D12_ROOT_PARAMETER p{};
            p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            p.DescriptorTable = {1, &ranges.back()};
            p.ShaderVisibility = vis;
            st.table_param = int(params.size());
            params.push_back(p);
        }

        // Textures the technique itself carries.
        for (const auto& tb : ts.textures) {
            try {
                st.static_textures[tb.slot] = gpu.texture(load_texture(pm, tb.texture));
            } catch (const std::exception& ex) {
                notes.push_back(std::format("t{} {}: {}", tb.slot, tb.texture.str(), ex.what()));
            }
        }
        stages_.push_back(std::move(st));
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{UINT(params.size()), params.data(), UINT(samplers.size()), samplers.data(),
                                  D3D12_ROOT_SIGNATURE_FLAG_NONE};
    root_sig_ = serialize(dev, rsd, "game pipeline root signature");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root_sig_.Get();
    pd.VS = {vs_code.data(), vs_code.size()};
    pd.PS = {ps_code.data(), ps_code.size()};
    auto& bl = pd.BlendState.RenderTarget[0];
    bl.RenderTargetWriteMask = 0xF;
    if (opt.additive) {
        bl.BlendEnable = TRUE;
        bl.SrcBlend = bl.DestBlend = D3D12_BLEND_ONE;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = bl.DestBlendAlpha = D3D12_BLEND_ONE;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = opt.rtv_format;
    pd.SampleDesc.Count = 1;
    check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState (game pipeline)");

    for (const auto& st : stages_) srv_total_ += st.srv_count;
    if (srv_total_) srv_first_ = gpu.srv().alloc(srv_total_ * kRing);
}

void GamePipeline::draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        FillerTextures& fillers) {
    if (!pso_) return;
    ID3D12Device* dev = gpu.device();
    const TfxEnvironment env = ext.environment();
    const uint32_t base = srv_first_ + ring_ * srv_total_;
    ring_ = (ring_ + 1) % kRing;

    cl->SetGraphicsRootSignature(root_sig_.Get());
    cl->SetPipelineState(pso_.Get());
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    DynamicAlloc zero = gpu.dynamic(4096);
    std::memset(zero.cpu, 0, 4096);
    uint32_t off = 0;
    for (auto& st : stages_) {
        std::vector<float4> cb(st.tfx.cbuffer_init.begin(), st.tfx.cbuffer_init.end());
        cb.resize(std::max<size_t>(cb.size(), 256));
        std::vector<TfxExternTexture> textures = st.extern_textures;
        if (!st.tfx.bytecode.empty()) {
            try {
                textures = run_tfx(st.tfx.bytecode, st.tfx.constants, cb, env).extern_textures;
            } catch (const std::exception& ex) {
                notes.push_back(std::format("{}: TFX error {}", st.tfx.shader.str(), ex.what()));
            }
        }
        if (st.which == ShaderStage::Pixel)
            for (auto [reg, v] : ps_overrides) {
                if (reg >= cb.size()) cb.resize(reg + 1);
                cb[reg] = v;
            }
        sanitize(cb);
        DynamicAlloc a = gpu.dynamic(cb.size() * sizeof(float4));
        std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
        for (auto [reg, param] : st.cbvs) {
            D3D12_GPU_VIRTUAL_ADDRESS va = reg == 0 ? a.gpu : scopes ? scopes(st.which, reg) : 0;
            cl->SetGraphicsRootConstantBufferView(param, va ? va : zero.gpu);
        }
        for (uint32_t s = 0; s < st.srv_count; ++s) {
            auto kit = st.srv_kinds.find(s);
            const BindingKind kind = kit == st.srv_kinds.end() ? BindingKind::Texture2D : kit->second;
            ID3D12Resource* r = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            if (auto it = st.static_textures.find(s); it != st.static_textures.end()) r = it->second.Get();
            float4 fill{0, 0, 0, 0};
            for (const auto& et : textures) {
                if (et.slot != s) continue;
                const uint32_t bo = uint32_t(et.offset) * 8;
                auto t = ext.texture(et.extern_id, bo);
                r = t.resource;
                fmt = t.format;
                fill = filler_value(et.extern_id, bo);
            }
            if (r && kind_is_texture(kind) && !resource_fits(r, kind)) r = nullptr, fmt = DXGI_FORMAT_UNKNOWN;
            if (!r && kind_is_texture(kind)) r = fillers.get(gpu, kind, fill);
            write_srv(dev, r, fmt, kind, gpu.srv().cpu(base + off + s));
        }
        if (st.table_param >= 0) cl->SetGraphicsRootDescriptorTable(UINT(st.table_param), gpu.srv().gpu(base + off));
        off += st.srv_count;
    }
    cl->DrawInstanced(4, 1, 0, 0);
}

void GamePipeline::prewarm(Gpu& gpu, FillerTextures& fillers) {
    for (const auto& st : stages_)
        for (const auto& [slot, kind] : st.srv_kinds) {
            if (!kind_is_texture(kind) || st.static_textures.count(slot)) continue;
            float4 fill{0, 0, 0, 0};
            for (const auto& et : st.extern_textures)
                if (et.slot == slot) fill = filler_value(et.extern_id, uint32_t(et.offset) * 8);
            fillers.get(gpu, kind, fill);
        }
}

std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext) {
    const auto& ss = scope.stages[size_t(stage)];
    std::vector<float4> cb(ss.cbuffer_init.begin(), ss.cbuffer_init.end());
    cb.resize(std::max<size_t>(cb.size(), 64));
    if (!ss.bytecode.empty()) run_tfx(ss.bytecode, ss.constants, cb, ext.environment());
    sanitize(cb);
    return cb;
}

// ---- GameLighting ---------------------------------------------------------------------------------
namespace {

// nim's own helpers around the game pass: a sky reflection for light_specular_ibl (the game fills
// it from cubemap volumes we do not render yet) and the tonemap to the 8-bit view target.
constexpr const char* kUtilHlsl = R"(
cbuffer P : register(b0) {
    float3 eye;         float exposure;
    float3 fwd;         float fx;
    float3 right;       float fy;
    float3 up;          float inv_w;
    float3 sky_top;     float inv_h;
    float3 sky_horizon; float sky_intensity;
    float3 ground;      float pad0;
    float3 bg_top;      float pad1;
    float3 bg_bottom;   float pad2;
    float3 fog_color;   float fog_on;
    float3 sun_dir;     float sun_glow;
    float3 sun_col;     float pad3;
};
Texture2D<float4> hdr : register(t0);
Texture2D<float>  depth : register(t1);
Texture2D<float4> g1 : register(t2);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 sky(float3 d) {
    float t = d.z;
    float3 c = t > 0 ? lerp(sky_horizon, sky_top, pow(saturate(t), 0.6)) : lerp(sky_horizon, ground, saturate(-t * 4));
    return c * sky_intensity;
}
float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }
float3 srgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }

float4 ps_ibl(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    if (depth.Load(p) <= 0) return 0;
    float2 ndc = float2(pos.x * inv_w * 2 - 1, 1 - pos.y * inv_h * 2);
    float3 ray = normalize(fwd + right * (ndc.x / fx) + up * (ndc.y / fy));
    float3 v = g1.Load(p).rgb * 2 - 1;
    float len = length(v);
    float3 n = len > 0 ? v / len : float3(0, 0, 1);
    float smooth = saturate(len * 4 - 3);
    float3 r = reflect(ray, n);
    float3 blurred = lerp(sky(-n) * 0.25 + ground * 0.25, sky(n), saturate(n.z * 0.5 + 0.5));
    return float4(lerp(blurred, sky(r), smooth * smooth), 1);
}
float3 view_ray(float4 pos) {
    float2 ndc = float2(pos.x * inv_w * 2 - 1, 1 - pos.y * inv_h * 2);
    return normalize(fwd + right * (ndc.x / fx) + up * (ndc.y / fy));
}
// What the game's sky lookups would hold along this ray: the fog colour, brighter toward the sun.
float3 inscatter(float3 r) {
    float mu = saturate(dot(r, sun_dir));
    return fog_color * sky_intensity + sun_col * (sun_glow * pow(mu, 8) * 0.25);
}
float4 ps_fog(float4 pos : SV_Position) : SV_Target { return float4(inscatter(view_ray(pos)), 1); }
// Exposed linear HDR for final_combine; the background where nothing was drawn.
float4 ps_post(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    if (depth.Load(p) <= 0) {
        if (fog_on > 0) return float4(inscatter(view_ray(pos)) * exposure, 1);
        return float4(lerp(bg_top, bg_bottom, pos.y * inv_h), 1);
    }
    return float4(max(hdr.Load(p).rgb, 0) * exposure, 1);
}
float4 ps_tonemap(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    if (depth.Load(p) <= 0) return float4(srgb(lerp(bg_top, bg_bottom, pos.y * inv_h)), 1);
    return float4(srgb(aces(max(hdr.Load(p).rgb, 0) * exposure)), 1);
}
)";

ComPtr<ID3DBlob> compile_util(const char* entry, const char* target) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(kUtilHlsl, std::strlen(kUtilHlsl), "nim_game_util.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
        throw std::runtime_error(std::format("game util shader: {}",
                                             err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed"));
    return code;
}

struct UtilConstants {
    V3 eye; float exposure;
    V3 fwd; float fx;
    V3 right; float fy;
    V3 up; float inv_w;
    V3 sky_top; float inv_h;
    V3 sky_horizon; float sky_intensity;
    V3 ground; float pad0;
    V3 bg_top; float pad1;
    V3 bg_bottom; float pad2;
    V3 fog_color; float fog_on;
    V3 sun_dir; float sun_glow;
    V3 sun_col; float pad3;
};

ComPtr<ID3D12Resource> load_lookup(PackageManager& pm, Gpu& gpu, TagHash tag, std::vector<std::string>& notes) {
    if (tag.is_none()) return nullptr;
    try {
        return gpu.texture(load_texture(pm, tag));
    } catch (const std::exception& ex) {
        notes.push_back(std::format("lookup texture {}: {}", tag.str(), ex.what()));
        return nullptr;
    }
}

float4 v4(V3 v, float w) { return {v.x, v.y, v.z, w}; }

} // namespace

void GameLighting::release(Gpu& gpu) {
    shading_.release(gpu);
    final_combine_.release(gpu);
    fillers_.release();
    for (auto& t : targets_) t.Reset();
    if (srv_first_ != UINT32_MAX) gpu.srv().free(srv_first_, 3);
    srv_first_ = UINT32_MAX;
    w_ = h_ = 0;
}

void GameLighting::init(PackageManager& pm, Gpu& gpu) {
    globals_ = load_render_globals(pm);
    frame_scope_ = load_scope(pm, globals_.scope("frame"));
    view_scope_ = load_scope(pm, globals_.scope("view"));
    ext_.channels.assign(globals_.channel_defaults.begin(), globals_.channel_defaults.end());
    ext_.channels.resize(256, float4{1, 1, 1, 1});
    ext_.channel_ids = globals_.channel_ids;

    const Scope* scopes[] = {&frame_scope_, &view_scope_};
    const TagHash shading = globals_.pipeline("global_lighting_and_shading");
    if (shading.is_none()) throw std::runtime_error("render globals have no global_lighting_and_shading pipeline");
    shading_.load(pm, gpu, shading, scopes, {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
    shading_.prewarm(gpu, fillers_);
    if (TagHash fc = globals_.pipeline("final_combine"); !fc.is_none()) {
        try {
            final_combine_.load(pm, gpu, fc, scopes, {DXGI_FORMAT_R8G8B8A8_UNORM, false});
            final_combine_.prewarm(gpu, fillers_);
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("final_combine: {} (nim tonemap used)", ex.what()));
        }
    }
    tint_lookup_ = load_lookup(pm, gpu, globals_.specular_tint_lookup, shading_.notes);
    lobe_lookup_ = load_lookup(pm, gpu, globals_.specular_lobe_lookup, shading_.notes);
    lobe3d_lookup_ = load_lookup(pm, gpu, globals_.specular_lobe_3d_lookup, shading_.notes);
    iridescence_lookup_ = load_lookup(pm, gpu, globals_.iridescence_lookup, shading_.notes);

    ID3D12Device* dev = gpu.device();
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    util_rs_ = serialize(dev, rsd, "game util root signature");
    auto vs = compile_util("vs_main", "vs_5_0");
    for (auto [entry, pso, fmt] : {std::tuple{"ps_ibl", &ibl_pso_, DXGI_FORMAT_R16G16B16A16_FLOAT},
                                   std::tuple{"ps_fog", &fog_pso_, DXGI_FORMAT_R16G16B16A16_FLOAT},
                                   std::tuple{"ps_post", &post_pso_, DXGI_FORMAT_R16G16B16A16_FLOAT},
                                   std::tuple{"ps_tonemap", &tonemap_pso_, DXGI_FORMAT_R8G8B8A8_UNORM}}) {
        auto ps = compile_util(entry, "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = util_rs_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = fmt;
        pd.SampleDesc.Count = 1;
        check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pso)), "game util PSO");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTargets};
    check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "game lighting rtv heap");
    gpu.flush();  // lookup / filler / static texture uploads
}

void GameLighting::resize(Gpu& gpu, uint32_t w, uint32_t h) {
    ID3D12Device* dev = gpu.device();
    w_ = w;
    h_ = h;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int i = 0; i < kTargets; ++i) {
        D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R16G16B16A16_FLOAT, {0, 0, 0, 0}};
        targets_[i] = gpu.texture2d(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
        dev->CreateRenderTargetView(targets_[i].Get(), nullptr,
                                    {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * inc});
    }
    if (srv_first_ == UINT32_MAX) srv_first_ = gpu.srv().alloc(3);
}

void GameLighting::fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p) {
    const float w = float(gb.width()), h = float(gb.height());
    const float near_z = vm.vp_rel.c[3][2];

    // View, in nim's camera space (x right, y up, z forward). The lighting VS reconstructs a
    // per-pixel ray at unit depth from 0x160 and moves it to world space with 0xE0.
    M4 cam_to_world;
    cam_to_world.c[0] = v4(vm.right, 0);
    cam_to_world.c[1] = v4(vm.up, 0);
    cam_to_world.c[2] = v4(vm.fwd, 0);
    cam_to_world.c[3] = v4(vm.eye, 1);
    M4 world_to_cam;
    world_to_cam.c[0] = {vm.right.x, vm.up.x, vm.fwd.x, 0};
    world_to_cam.c[1] = {vm.right.y, vm.up.y, vm.fwd.y, 0};
    world_to_cam.c[2] = {vm.right.z, vm.up.z, vm.fwd.z, 0};
    world_to_cam.c[3] = {-dot(vm.right, vm.eye), -dot(vm.up, vm.eye), -dot(vm.fwd, vm.eye), 1};
    M4 cam_to_proj;
    cam_to_proj.c[0] = {vm.fx, 0, 0, 0};
    cam_to_proj.c[1] = {0, vm.fy, 0, 0};
    cam_to_proj.c[2] = {0, 0, 0, 1};
    cam_to_proj.c[3] = {0, 0, near_z, 0};
    M4 proj_to_ray;  // (ndc.x, ndc.y, any, 1) -> camera-space ray with z = 1, w = 0
    proj_to_ray.c[0] = {1 / vm.fx, 0, 0, 0};
    proj_to_ray.c[1] = {0, 1 / vm.fy, 0, 0};
    proj_to_ray.c[2] = {0, 0, 0, 0};
    proj_to_ray.c[3] = {0, 0, 1, 0};
    M4 pixel_to_ray;
    pixel_to_ray.c[0] = {2 / (w * vm.fx), 0, 0, 0};
    pixel_to_ray.c[1] = {0, -2 / (h * vm.fy), 0, 0};
    pixel_to_ray.c[2] = {0, 0, 0, 0};
    pixel_to_ray.c[3] = {-1 / vm.fx, 1 / vm.fy, 1, 0};
    ext_.set(ext::View, 0x00, w);
    ext_.set(ext::View, 0x04, h);
    ext_.set(ext::View, 0x10, float4{0, 0, 0, 0});
    ext_.set(ext::View, 0x20, v4(vm.eye, 1));
    ext_.set(ext::View, 0x60, world_to_cam);
    ext_.set(ext::View, 0xA0, cam_to_proj);
    ext_.set(ext::View, 0xE0, cam_to_world);
    ext_.set(ext::View, 0x120, pixel_to_ray);
    ext_.set(ext::View, 0x160, proj_to_ray);
    ext_.set(ext::View, 0x1A0, vm.vp_abs);

    // Deferred: the G-buffer the game's material shaders wrote, and nim's light buffers.
    ext_.set(ext::Deferred, 0x00, float4{0, 1 / near_z, 0, 0});  // reversed infinite Z: 1/z = d / near
    ext_.set(ext::Deferred, 0x30, float4{0, 0, 0, 0});           // light buffers at full resolution
    ext_.set(ext::Deferred, 0x40, float4{w, h, 0, 0});
    ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), DXGI_FORMAT_R32_FLOAT);
    ext_.set_texture(ext::Deferred, 0xA8, gb.rt[0].Get());
    ext_.set_texture(ext::Deferred, 0xB0, gb.rt[1].Get());
    ext_.set_texture(ext::Deferred, 0xB8, gb.rt[2].Get());
    ext_.set_texture(ext::Deferred, 0xC0, targets_[0].Get());
    ext_.set_texture(ext::Deferred, 0xC8, targets_[1].Get());
    ext_.set_texture(ext::Deferred, 0xD0, targets_[2].Get());

    // Frame: time and the BRDF lookups from the render globals.
    ext_.set(ext::Frame, 0x00, time_);
    ext_.set(ext::Frame, 0x04, time_);
    ext_.set_texture(ext::Frame, 0xC0, lobe_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xC8, lobe3d_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD0, tint_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD8, iridescence_lookup_.Get());
    ext_.set(ext::Frame, 0x1F0, float4{0, 0, 1, 0});  // .z scales light_specular_ibl

    // GlobalLighting: the map's sun and hemisphere ambient. The shading multiplies the sun by
    // 1/(4 pi) and the ambient by 1/4 (and the hemisphere weight), so scale nim's values to match.
    const V3 sun_dir = normalize(V3{std::cos(p.sun_azimuth) * std::cos(p.sun_elevation),
                                    std::sin(p.sun_azimuth) * std::cos(p.sun_elevation), std::sin(p.sun_elevation)});
    const V3 sun = p.sun_color * (p.sun_intensity * 4.0f);
    const float ka = 4.0f * p.sky_intensity;
    ext_.set(ext::GlobalLighting, 0x10, v4(sun, 0));
    ext_.set(ext::GlobalLighting, 0x30, v4(sun_dir, 0));
    ext_.set(ext::GlobalLighting, 0x50, float4{0, 0, 1, 0});
    ext_.set(ext::GlobalLighting, 0x70, v4(p.sky_top * ka, 0));
    ext_.set(ext::GlobalLighting, 0x80, v4(p.ground * ka, 0));
    ext_.set(ext::GlobalLighting, 0x90, 0.35f);  // up / down hemisphere sharpness
    ext_.set(ext::GlobalLighting, 0x94, 0.35f);
    ext_.set(ext::GlobalLighting, 0xD0, 1.0f);   // light-buffer occlusion strength
    ext_.set(ext::GlobalLighting, 0xD4, 1.0f);
    ext_.set_channel("skybox_sun_color", v4(p.sun_color, 1));
    ext_.set_channel("skybox_sun_intensity", float4{p.sun_intensity * 4.0f, 0, 0, 0});
    ext_.set_channel("sun_atmosphere_direction", v4(sun_dir, 0));
    ext_.set_channel("skybox_up_ambient_color", v4(p.sky_top, 1));
    ext_.set_channel("skybox_up_ambient_intensity", float4{ka, 0, 0, 0});
    ext_.set_channel("skybox_down_ambient_color", v4(p.ground, 1));
    ext_.set_channel("skybox_down_ambient_intensity", float4{ka, 0, 0, 0});
    if (ext_.channels.size() > 130) ext_.channels[130] = {1, 0, 0, 0};  // near-field ambient strength

    // Atmosphere: the near / far sky in-scatter lookups the fog blends between (nim's stand-in).
    ext_.set_texture(ext::Atmosphere, 0xE0, targets_[4].Get());
    ext_.set_texture(ext::Atmosphere, 0xF0, targets_[4].Get());

    // ShadowMask: no sun shadow / AO rendered yet (fillers are white); full strength, simple filter.
    ext_.set(ext::ShadowMask, 0x30, 1.0f);
    ext_.set(ext::ShadowMask, 0x34, 0.0f);
}

void GameLighting::render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                          const LightingParams& p) {
    if (!ready()) return;
    ID3D12Device* dev = gpu.device();
    if (gb.width() != w_ || gb.height() != h_) resize(gpu, gb.width(), gb.height());
    time_ += 1.0f / 60.0f;
    fill_externs(gb, vm, p);

    for (auto& r : gb.rt)
        Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (auto& t : targets_)
        Gpu::barrier(cl, t.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto rtv = [&](int i) { return D3D12_CPU_DESCRIPTOR_HANDLE{rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * inc}; };
    const float zero[4] = {0, 0, 0, 0};
    cl->ClearRenderTargetView(rtv(0), zero, 0, nullptr);
    cl->ClearRenderTargetView(rtv(1), zero, 0, nullptr);
    cl->ClearRenderTargetView(rtv(3), zero, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w_), LONG(h_)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    // nim helper SRVs: hdr, depth, rt1.
    write_srv(dev, targets_[3].Get(), DXGI_FORMAT_UNKNOWN, BindingKind::Texture2D, gpu.srv().cpu(srv_first_));
    write_srv(dev, gb.depth.Get(), DXGI_FORMAT_R32_FLOAT, BindingKind::Texture2D, gpu.srv().cpu(srv_first_ + 1));
    write_srv(dev, gb.rt[1].Get(), DXGI_FORMAT_UNKNOWN, BindingKind::Texture2D, gpu.srv().cpu(srv_first_ + 2));
    UtilConstants uc{};
    uc.eye = vm.eye; uc.exposure = p.exposure;
    uc.fwd = vm.fwd; uc.fx = vm.fx;
    uc.right = vm.right; uc.fy = vm.fy;
    uc.up = vm.up; uc.inv_w = 1.0f / float(w_);
    uc.sky_top = p.sky_top; uc.inv_h = 1.0f / float(h_);
    uc.sky_horizon = p.sky_horizon; uc.sky_intensity = p.sky_intensity;
    uc.ground = p.ground;
    uc.bg_top = p.background_top;
    uc.bg_bottom = p.background_bottom;
    uc.fog_color = p.fog_color;
    uc.fog_on = p.fog ? 1.0f : 0.0f;
    uc.sun_dir = normalize(V3{std::cos(p.sun_azimuth) * std::cos(p.sun_elevation),
                              std::sin(p.sun_azimuth) * std::cos(p.sun_elevation), std::sin(p.sun_elevation)});
    uc.sun_glow = p.fog_sun_glow;
    uc.sun_col = p.sun_color * p.sun_intensity;
    DynamicAlloc ucb = gpu.dynamic(sizeof(uc));
    std::memcpy(ucb.cpu, &uc, sizeof(uc));
    auto util_pass = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE target) {
        cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
        cl->SetGraphicsRootSignature(util_rs_.Get());
        cl->SetPipelineState(pso);
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootConstantBufferView(0, ucb.gpu);
        cl->SetGraphicsRootDescriptorTable(1, gpu.srv().gpu(srv_first_));
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    };
    util_pass(ibl_pso_.Get(), rtv(2));
    util_pass(fog_pso_.Get(), rtv(4));
    Gpu::barrier(cl, targets_[4].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (int i = 0; i < 3; ++i)
        Gpu::barrier(cl, targets_[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Shared cbuffers: cb13 frame (engine-filled, layout per goriath's FrameScope), cb12 view (the
    // view scope's own bytecode over the View extern).
    const float t = time_;
    float4 frame[16]{};
    frame[0] = {t, t, 1.0f / 60.0f, ext_.get_f(ext::Frame, 0x18)};
    const float es = ext_.get_f(ext::Frame, 0x1C), ir = ext_.get_f(ext::Frame, 0x28);
    frame[1] = {es, ir * 16.0f, es, ir};
    frame[2] = {(t * 60 + 33.75f) * 1.258699f, (t * 60 + 60) * 0.9583125f, (t * 60 + 60) * 8.789123f,
                (t * 60 + 33.75f) * 2.311535f};
    frame[3] = {0.5f, 0.5f, 0, 0};
    frame[4] = {1, 1, 0, 1};
    frame[5] = {0, 0, 512, 0};
    frame[6] = {1, 1, 1, 1};
    DynamicAlloc cb13 = gpu.dynamic(sizeof(frame));
    std::memcpy(cb13.cpu, frame, sizeof(frame));
    auto upload = [&](const std::vector<float4>& v) {
        DynamicAlloc a = gpu.dynamic(v.size() * sizeof(float4));
        std::memcpy(a.cpu, v.data(), v.size() * sizeof(float4));
        return a.gpu;
    };
    const D3D12_GPU_VIRTUAL_ADDRESS cb12_ps = upload(evaluate_scope(view_scope_, ScopeStage::Pixel, ext_));
    const D3D12_GPU_VIRTUAL_ADDRESS cb12_vs = upload(evaluate_scope(view_scope_, ScopeStage::Vertex, ext_));
    ScopeBufferFn scopes = [&](ShaderStage s, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 13) return cb13.gpu;
        if (reg == 12) return s == ShaderStage::Vertex ? cb12_vs : cb12_ps;
        return 0;
    };

    // Height fog, evaluated by the shading pass itself (two exponential layers over world height,
    // optical depth = 0.001 * distance * density). In game these registers come from the map's
    // atmosphere settings; nim drives them from LightingParams.
    auto& ov = shading_.ps_overrides;
    ov[8] = {1, 0, 0, 0};                              // in-scatter intensity
    ov[13] = {p.fog_height, 0, 0, 0};                  // layer 0 base height
    ov[18] = {p.fog_height, 0, 0, 0};                  // layer 1 base height
    ov[40] = {p.fog_falloff, p.fog_falloff, 0, 0};     // per-km height falloff
    ov[41] = {p.fog ? p.fog_density : 0.0f, 0, 0, 0};  // per-km density
    ov[42] = {0, 0, 0.5f, 0.5f};                       // density noise uv (constant)
    ov[45] = {0, 1, 0, 0};                             // near/far lookup blend: near
    ov[104] = ov[105] = ov[107] = ov[108] = {0, 0, 0, 0};  // no sky-visibility / froxel terms
    ov[106] = {0, 0, 0, 0};                            // surface fully attenuated
    {
        float4 r109 = ext_.get_v(ext::Atmosphere, 0x130);
        r109[1] = p.fog_start;
        ov[109] = r109;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE hdr = rtv(3);
    cl->OMSetRenderTargets(1, &hdr, FALSE, nullptr);
    shading_.draw(gpu, cl, ext_, scopes, fillers_);
    Gpu::barrier(cl, targets_[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // The game's own film curve (final_combine) over the exposed HDR.
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (final_combine_.loaded()) {
        util_pass(post_pso_.Get(), rtv(5));
        Gpu::barrier(cl, targets_[5].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ext_.set_texture(ext::Postprocess, 0x00, targets_[5].Get());
        cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
        final_combine_.draw(gpu, cl, ext_, scopes, fillers_);
    } else {
        util_pass(tonemap_pso_.Get(), gb.lit_rtv);
    }
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    for (auto& r : gb.rt)
        Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, gb.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

} // namespace nim
