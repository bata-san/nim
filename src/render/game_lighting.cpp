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

bool kind_is_texture(BindingKind k) {
    return k == BindingKind::Texture2D || k == BindingKind::Texture3D || k == BindingKind::TextureCube ||
           k == BindingKind::Texture2DArray || k == BindingKind::TextureCubeArray || k == BindingKind::Texture1D ||
           k == BindingKind::Texture1DArray || k == BindingKind::Texture2DMS || k == BindingKind::Texture2DMSArray;
}

bool resource_fits(ID3D12Resource* r, BindingKind k) {
    D3D12_RESOURCE_DESC d = r->GetDesc();
    switch (k) {
    case BindingKind::Texture1D: case BindingKind::Texture1DArray:
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    case BindingKind::Texture2DMS: case BindingKind::Texture2DMSArray:
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count > 1;
    case BindingKind::Texture3D: return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    case BindingKind::TextureCube:
    case BindingKind::TextureCubeArray:
        return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.DepthOrArraySize % 6 == 0;
    case BindingKind::Texture2D:
    case BindingKind::Texture2DArray: return d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count == 1;
    default: return false;
    }
}

void write_srv(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, BindingKind k, D3D12_CPU_DESCRIPTOR_HANDLE h, uint32_t stride = 0, uint32_t elements = 0, uint64_t first = 0) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (!r) throw std::runtime_error("Missing game texture (null resource)");
    D3D12_RESOURCE_DESC d = r->GetDesc();
    sd.Format = fmt != DXGI_FORMAT_UNKNOWN ? fmt : d.Format;
    switch (k) {
    case BindingKind::RawBuffer:
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        sd.Buffer.FirstElement = first;
        sd.Buffer.NumElements = elements ? elements : UINT(d.Width / 4 - first);
        break;
    case BindingKind::StructuredBuffer:
        if (!stride) throw std::runtime_error("Original structured SRV stride is required");
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.StructureByteStride = stride;
        sd.Buffer.FirstElement = first;
        sd.Buffer.NumElements = elements ? elements : UINT(d.Width / stride - first);
        break;
    case BindingKind::TypedBuffer:
        if (fmt == DXGI_FORMAT_UNKNOWN || !elements) throw std::runtime_error("Original typed SRV format/element count is required");
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.FirstElement = first;
        sd.Buffer.NumElements = elements;
        break;
    case BindingKind::Texture1D:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
        sd.Texture1D.MipLevels = UINT(-1);
        break;
    case BindingKind::Texture1DArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
        sd.Texture1DArray.MipLevels = UINT(-1);
        sd.Texture1DArray.ArraySize = d.DepthOrArraySize;
        break;
    case BindingKind::Texture2DMS:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        break;
    case BindingKind::Texture2DMSArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
        sd.Texture2DMSArray.ArraySize = d.DepthOrArraySize;
        break;
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

void validate_resource_view(const ExternStore::Tex& view, BindingKind kind, bool uav) {
    if (!view.resource) throw std::runtime_error("Missing original resource view");
    const auto desc = view.resource->GetDesc();
    if (kind == BindingKind::StructuredBuffer && (!view.stride || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER))
        throw std::runtime_error("Original structured-buffer stride is required");
    if (kind == BindingKind::TypedBuffer && (view.format == DXGI_FORMAT_UNKNOWN || !view.elements || desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER))
        throw std::runtime_error("Original typed-buffer format and element count are required");
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        const uint32_t stride = kind == BindingKind::RawBuffer ? 4 : view.stride;
        if (stride && (view.first_element > desc.Width / stride || uint64_t(view.elements) > desc.Width / stride - view.first_element))
            throw std::runtime_error("Original buffer view is outside the resource");
    }
    if (kind == BindingKind::RawBuffer && desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        throw std::runtime_error("Raw view requires an original buffer resource");
    if (kind_is_texture(kind) && !resource_fits(view.resource, kind)) throw std::runtime_error("Original resource dimension mismatch");
    if (uav && (kind == BindingKind::TextureCube || kind == BindingKind::TextureCubeArray || kind == BindingKind::Texture2DMS || kind == BindingKind::Texture2DMSArray))
        throw std::runtime_error("Cube UAV requires the original array view");
}

void write_uav(ID3D12Device* dev, const ExternStore::Tex& view, BindingKind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    validate_resource_view(view, kind, true);
    const auto resource = view.resource->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = view.format == DXGI_FORMAT_UNKNOWN ? resource.Format : view.format;
    if (kind == BindingKind::Texture1D) desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
    else if (kind == BindingKind::Texture1DArray) {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        desc.Texture1DArray.ArraySize = resource.DepthOrArraySize;
    } else if (kind == BindingKind::Texture2D) desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    else if (kind == BindingKind::Texture3D) {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        desc.Texture3D.WSize = resource.DepthOrArraySize;
    } else if (kind == BindingKind::Texture2DArray) {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.ArraySize = resource.DepthOrArraySize;
    } else if (kind == BindingKind::RawBuffer || kind == BindingKind::StructuredBuffer || kind == BindingKind::TypedBuffer) {
        desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        desc.Buffer.FirstElement = view.first_element;
        if (kind == BindingKind::RawBuffer) {
            desc.Format = DXGI_FORMAT_R32_TYPELESS;
            desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            desc.Buffer.NumElements = view.elements ? view.elements : UINT(resource.Width / 4 - view.first_element);
        } else if (kind == BindingKind::StructuredBuffer) {
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.Buffer.StructureByteStride = view.stride;
            desc.Buffer.NumElements = view.elements ? view.elements : UINT(resource.Width / view.stride - view.first_element);
        } else desc.Buffer.NumElements = view.elements;
    } else throw std::runtime_error("Unsupported original UAV view dimension");
    dev->CreateUnorderedAccessView(view.resource, nullptr, &desc, handle);
}

D3D12_STATIC_SAMPLER_DESC static_sampler(const SamplerDesc& s, uint32_t reg, D3D12_SHADER_VISIBILITY vis) {
    D3D12_STATIC_SAMPLER_DESC d{};
    d.Filter = D3D12_FILTER(s.filter);
    if (s.address_u < 1 || s.address_u > 5 || s.address_v < 1 || s.address_v > 5 || s.address_w < 1 || s.address_w > 5 ||
        s.max_anisotropy > 16 || s.comparison_func < 1 || s.comparison_func > 8)
        throw std::runtime_error("Invalid original sampler state");
    d.AddressU = D3D12_TEXTURE_ADDRESS_MODE(s.address_u);
    d.AddressV = D3D12_TEXTURE_ADDRESS_MODE(s.address_v);
    d.AddressW = D3D12_TEXTURE_ADDRESS_MODE(s.address_w);
    d.MipLODBias = s.mip_lod_bias;
    d.MaxAnisotropy = s.max_anisotropy;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC(s.comparison_func);
    const std::array<float, 4> border{s.border_color[0], s.border_color[1], s.border_color[2], s.border_color[3]};
    if (border == float4{0, 0, 0, 0}) d.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    else if (border == float4{0, 0, 0, 1}) d.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    else if (border == float4{1, 1, 1, 1}) d.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    else throw std::runtime_error("Original sampler requires a dynamic border-color sampler");
    d.MinLOD = s.min_lod;
    d.MaxLOD = s.max_lod;
    d.ShaderRegister = reg;
    d.ShaderVisibility = vis;
    return d;
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

void write_null_srv(ID3D12Device* device, BindingKind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (kind) {
    case BindingKind::RawBuffer:
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        sd.Buffer.NumElements = 1;
        break;
    case BindingKind::StructuredBuffer:
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.StructureByteStride = 16;
        sd.Buffer.NumElements = 1;
        break;
    case BindingKind::TypedBuffer:
        sd.Format = DXGI_FORMAT_R32_UINT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = 1;
        break;
    case BindingKind::Texture1D: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D; sd.Texture1D.MipLevels = 1; break;
    case BindingKind::Texture1DArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY; sd.Texture1DArray.MipLevels = 1; sd.Texture1DArray.ArraySize = 1;
        break;
    case BindingKind::Texture2DMS: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; break;
    case BindingKind::Texture2DMSArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY; sd.Texture2DMSArray.ArraySize = 1;
        break;
    case BindingKind::Texture3D: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; sd.Texture3D.MipLevels = 1; break;
    case BindingKind::TextureCube: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = 1; break;
    case BindingKind::TextureCubeArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY; sd.TextureCubeArray.MipLevels = 1; sd.TextureCubeArray.NumCubes = 1;
        break;
    case BindingKind::Texture2DArray:
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = 1;
        break;
    default: sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; break;
    }
    device->CreateShaderResourceView(nullptr, &sd, handle);
}

void write_game_srv(ID3D12Device* device, const ExternStore::Tex& view, BindingKind kind,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    validate_resource_view(view, kind, false);
    write_srv(device, view.resource, view.format, kind, handle, view.stride, view.elements, view.first_element);
}

// ---- ExternStore ------------------------------------------------------------------------------
ExternStore::ExternStore() : data_(size_t(kBlockSize) * kBlocks, 0), present_(data_.size(), 0) {}

void ExternStore::require(uint8_t id, uint32_t off, uint32_t size) const {
    if (id >= kBlocks || off > kBlockSize || size > kBlockSize - off)
        throw std::out_of_range("extern offset");
    const size_t base = size_t(id) * kBlockSize + off;
    if (!std::all_of(present_.begin() + base, present_.begin() + base + size, [](uint8_t v) { return v != 0; })) {
        if (auto it = zero_policy_.find(id); it != zero_policy_.end()) {  // data_ is zero there
            note_estimate(std::format("extern {} +0x{:X} = 0 ({} bytes)", id, off, size), it->second);
            return;
        }
        throw std::runtime_error(std::format("Missing game extern {} at byte 0x{:X} ({} bytes); no replacement is permitted", id, off, size));
    }
}

bool ExternStore::has(uint8_t id, uint32_t off, uint32_t size) const {
    if (id >= kBlocks || off > kBlockSize || size > kBlockSize - off) return false;
    const size_t base = size_t(id) * kBlockSize + off;
    return std::all_of(present_.begin() + base, present_.begin() + base + size, [](uint8_t v) { return v != 0; });
}

void ExternStore::note_estimate(std::string what, std::string_view why) const { estimates_.try_emplace(std::move(what), why); }

bool ExternStore::estimate_null_texture(uint8_t id, uint32_t off) const {
    auto it = zero_policy_.find(id);
    if (it == zero_policy_.end()) return false;
    note_estimate(std::format("extern {} +0x{:X} texture (null view)", id, off), it->second);
    return true;
}

void ExternStore::estimate(uint8_t id, uint32_t off, float4 v, std::string_view why) {
    set(id, off, v);
    note_estimate(std::format("extern {} +0x{:X} = ({:g}, {:g}, {:g}, {:g})", id, off, v[0], v[1], v[2], v[3]), why);
}

void ExternStore::estimate(uint8_t id, uint32_t off, const M4& m, std::string_view why) {
    set(id, off, m);
    note_estimate(std::format("extern {} +0x{:X} mat4", id, off), why);
}

void ExternStore::estimate_texture(uint8_t id, uint32_t off, ID3D12Resource* r, std::string_view why) {
    set_texture(id, off, r);
    note_estimate(std::format("extern {} +0x{:X} texture", id, off), why);
}

void ExternStore::estimate_frame_constants(const std::array<float4, 16>& constants, std::string_view why) {
    set_frame_constants(constants);
    note_estimate("frame cb13 (exposure, temporal seeds)", why);
}

const std::array<float4, 16>& ExternStore::frame_constants() const {
    if (!has_frame_) throw std::runtime_error("Missing engine-produced frame scope cb13 (exposure / temporal state)");
    return frame_;
}

void ExternStore::set_scope_constants(uint32_t reg, std::span<const float4> values) {
    if (values.empty()) throw std::invalid_argument("Empty engine scope");
    std::vector<float4> constants(values.begin(), values.end());
    scopes_[reg] = std::move(constants);
}

const std::vector<float4>& ExternStore::scope_constants(uint32_t reg) const {
    const auto it = scopes_.find(reg);
    if (it == scopes_.end()) throw std::runtime_error(std::format("Missing engine-produced shared scope b{}", reg));
    return it->second;
}

void ExternStore::set_scope_constants(ShaderStage stage, uint32_t reg, std::span<const float4> values) {
    if (values.empty()) throw std::invalid_argument("Empty engine scope");
    std::vector<float4> constants(values.begin(), values.end());
    stage_scopes_[{stage, reg}] = std::move(constants);
}

const std::vector<float4>& ExternStore::scope_constants(ShaderStage stage, uint32_t reg) const {
    if (auto it = stage_scopes_.find({stage, reg}); it != stage_scopes_.end()) return it->second;
    return scope_constants(reg);
}

void ExternStore::set(uint8_t id, uint32_t off, float v) {
    if (id >= kBlocks || off + 4 > kBlockSize) throw std::out_of_range("extern offset");
    std::memcpy(data_.data() + size_t(id) * kBlockSize + off, &v, 4);
    std::fill_n(present_.begin() + size_t(id) * kBlockSize + off, 4, uint8_t{1});
}
void ExternStore::set(uint8_t id, uint32_t off, float4 v) {
    if (id >= kBlocks || off + 16 > kBlockSize) throw std::out_of_range("extern offset");
    std::memcpy(data_.data() + size_t(id) * kBlockSize + off, v.data(), 16);
    std::fill_n(present_.begin() + size_t(id) * kBlockSize + off, 16, uint8_t{1});
}
void ExternStore::set(uint8_t id, uint32_t off, const M4& m) {
    for (int c = 0; c < 4; ++c) set(id, off + uint32_t(c) * 16, m.c[c]);
}
float ExternStore::get_f(uint8_t id, uint32_t off) const {
    require(id, off, 4);
    float v;
    std::memcpy(&v, data_.data() + size_t(id) * kBlockSize + off, 4);
    return v;
}
float4 ExternStore::get_v(uint8_t id, uint32_t off) const {
    require(id, off, 16);
    float4 v;
    std::memcpy(v.data(), data_.data() + size_t(id) * kBlockSize + off, 16);
    return v;
}
void ExternStore::set_texture(uint8_t id, uint32_t off, ID3D12Resource* r, DXGI_FORMAT f, uint32_t stride, uint32_t elements, uint64_t first_element) {
    if (r) textures_[tex_key(id, off)] = {r, f, stride, elements, first_element};
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
    env.global_channel = [this](uint8_t ch) {
        if (ch >= channels.size()) throw std::runtime_error(std::format("Missing game global channel {}", ch));
        return channels[ch];
    };
    // nim loads every mip of a texture: the streaming state is "fully resident" (min mip 0).
    env.texture_tiling = [this](uint8_t texture) {
        note_estimate(std::format("texture {} tiling params = 0", texture), "nim loads all mips: fully resident (min resident mip 0)");
        return float4{0, 0, 0, 0};
    };
    // nim binds every texture as a plain (single-layer, untiled) resource.
    env.texture_layer_count = [this](uint8_t texture) {
        note_estimate(std::format("texture {} layer count = 1", texture), "nim binds textures untiled with a single layer");
        return float4{1, 1, 1, 1};
    };
    return env;
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
    compute_ = t.bind_mode == 6;
    if (!compute_ && t.bind_mode != 1) throw std::runtime_error("Unsupported original pipeline bind mode");
    auto authored_scopes = load_technique_scopes(pm, load_render_globals(pm), t);
    std::vector<const Scope*> scope_ptrs;
    for (const auto& scope : authored_scopes) scope_ptrs.push_back(&scope);
    scopes = scope_ptrs;
    ID3D12Device* dev = gpu.device();

    std::vector<D3D12_ROOT_PARAMETER> params;
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    ranges.reserve(8);
    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
    std::vector<uint8_t> vs_code, ps_code, cs_code;
    const std::vector<ShaderStage> active_stages = compute_ ? std::vector{ShaderStage::Compute} : std::vector{ShaderStage::Vertex, ShaderStage::Pixel};

    for (ShaderStage which : active_stages) {
        const auto& ts = t.stage(which);
        if (ts.shader.is_none()) throw std::runtime_error(std::format("{}: missing {} shader", technique.str(),
                                                                      which == ShaderStage::Vertex ? "vertex" : "pixel"));
        Stage st;
        st.which = which;
        st.tfx = ts;
        auto code = load_shader_bytecode(pm, ts.shader);
        st.bindings = shader_bindings(code);
        (which == ShaderStage::Vertex ? vs_code : which == ShaderStage::Compute ? cs_code : ps_code) = std::move(code);
        const auto vis = which == ShaderStage::Vertex ? D3D12_SHADER_VISIBILITY_VERTEX : which == ShaderStage::Compute ? D3D12_SHADER_VISIBILITY_ALL : D3D12_SHADER_VISIBILITY_PIXEL;

        st.sources = resolve_stage_resources(t, which, scopes);
        for (const auto& [slot, source] : st.sources.extern_textures) st.extern_textures.push_back(source);
        for (const auto& [slot, source] : st.sources.extern_uavs) st.extern_uavs.push_back(source);
        const auto& sampler_tags = st.sources.samplers;

        for (const auto& b : st.bindings) {
            if (b.space != 0) {
                throw std::runtime_error(std::format("{}: unsupported register space {}", ts.shader.str(), b.space));
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
                    const auto it = sampler_tags.find(r);
                    if (it == sampler_tags.end())
                        throw std::runtime_error(std::format("{}: missing authored sampler s{}", ts.shader.str(), r));
                    const auto sd = load_sampler(pm, it->second);
                    samplers.push_back(static_sampler(sd, r, vis));
                }
                break;
            case BindingType::Uav:
                if (b.upper >= 64) throw std::runtime_error("UAV range exceeds original pipeline descriptor capacity");
                for (uint32_t reg = b.lower; reg <= b.upper; ++reg) {
                    st.uav_kinds[reg] = b.kind;
                    st.uav_count = std::max(st.uav_count, reg + 1);
                }
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

        if (st.uav_count) {
            ranges.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, st.uav_count, 0, 0, 0});
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable = {1, &ranges.back()};
            param.ShaderVisibility = vis;
            st.uav_table_param = int(params.size());
            params.push_back(param);
        }
        for (const auto& [slot, tag] : st.sources.textures)
            st.static_textures[slot] = gpu.texture(load_texture(pm, tag));
        stages_.push_back(std::move(st));
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{UINT(params.size()), params.data(), UINT(samplers.size()), samplers.data(),
                                  opt.volume ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE};
    root_sig_ = serialize(dev, rsd, "game pipeline root signature");

    if (compute_) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_sig_.Get();
        desc.CS = {cs_code.data(), cs_code.size()};
        check_hr(dev->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso_)), "Original game compute pipeline");
    } else {
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
    const D3D12_INPUT_ELEMENT_DESC position{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    if (opt.volume) {
        pd.InputLayout = {&position, 1};
        pd.RasterizerState.CullMode = opt.cull_front ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK;
        if (std::getenv("NIM_LIGHT_NOCULL")) pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    cbuffer_floor_ = opt.cbuffer_floor;
    pd.NumRenderTargets = UINT(1 + opt.additional_rtv_formats.size());
    if (pd.NumRenderTargets > 8) throw std::runtime_error("Too many original render targets");
    pd.RTVFormats[0] = opt.rtv_format;
    for (size_t i = 0; i < opt.additional_rtv_formats.size(); ++i) pd.RTVFormats[i + 1] = opt.additional_rtv_formats[i];
    for (const auto& output : shader_outputs(ps_code))
        if (_stricmp(output.semantic.c_str(), "SV_Target") == 0 && output.index >= pd.NumRenderTargets)
            throw std::runtime_error(std::format("{}: original PS requires SV_Target{}; attachment missing", technique.str(), output.index));
    pd.SampleDesc.Count = 1;
    check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState (game pipeline)");

    }
    for (const auto& st : stages_) srv_total_ += st.srv_count + st.uav_count;
    if (srv_total_) srv_first_ = gpu.srv().alloc(srv_total_ * kRing);
}

void GamePipeline::draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        bool validated) {
    if (compute_) throw std::runtime_error("Compute pipeline requires original dispatch dimensions");
    execute(gpu, cl, ext, scopes, validated, 0, 0, 0);
}

void GamePipeline::draw_volume(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                               const Technique& materials, const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv,
                               uint32_t indices) {
    const Geometry g{&materials, &vbv, &ibv, indices};
    execute(gpu, cl, ext, scopes, true, 0, 0, 0, &g);
}

void GamePipeline::dispatch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                            uint32_t x, uint32_t y, uint32_t z) {
    if (!compute_ || !x || !y || !z || x > 65535 || y > 65535 || z > 65535)
        throw std::invalid_argument("Invalid original compute dispatch");
    execute(gpu, cl, ext, scopes, false, x, y, z);
}

void GamePipeline::execute(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                           bool validated, uint32_t x, uint32_t y, uint32_t z, const Geometry* geometry) {
    if (!validated) validate(ext, scopes);
    if (!pso_) return;
    ID3D12Device* dev = gpu.device();
    const TfxEnvironment env = ext.environment();
    const uint32_t base = srv_first_ + ring_ * srv_total_;
    ring_ = (ring_ + 1) % kRing;

    if (compute_) cl->SetComputeRootSignature(root_sig_.Get());
    else cl->SetGraphicsRootSignature(root_sig_.Get());
    cl->SetPipelineState(pso_.Get());
    ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
    cl->SetDescriptorHeaps(1, heaps);
    if (geometry) {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 1, geometry->vbv);
        cl->IASetIndexBuffer(geometry->ibv);
    } else if (!compute_) {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    }

    uint32_t off = 0;
    for (auto& st : stages_) {
        const TechniqueStage& tfx = geometry && geometry->materials ? geometry->materials->stage(st.which) : st.tfx;
        auto& cb = st.constants;
        cb.assign(tfx.cbuffer_init.begin(), tfx.cbuffer_init.end());
        cb.resize(std::max<size_t>(cb.size(), cbuffer_floor_));
        std::vector<TfxExternTexture> textures = st.extern_textures;
        if (!tfx.bytecode.empty()) run_tfx(tfx.bytecode, tfx.constants, cb, env);
        DynamicAlloc a = gpu.dynamic(cb.size() * sizeof(float4));
        std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
        for (auto [reg, param] : st.cbvs) {
            D3D12_GPU_VIRTUAL_ADDRESS va = int32_t(reg) == tfx.cbuffer_slot ? a.gpu : scopes ? scopes(st.which, reg) : 0;
            if (compute_) cl->SetComputeRootConstantBufferView(param, va);
            else cl->SetGraphicsRootConstantBufferView(param, va);
        }
        auto& cached = st.bound_views[(ring_ + kRing - 1) % kRing];
        cached.resize(st.srv_count);
        for (const auto& [s, kind] : st.srv_kinds) {
            ID3D12Resource* r = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            uint32_t stride = 0, elements = 0;
            uint64_t first_element = 0;
            if (auto it = st.static_textures.find(s); it != st.static_textures.end()) r = it->second.Get();
            for (const auto& et : textures) {
                if (et.slot != s) continue;
                const uint32_t bo = uint32_t(et.offset) * 8;
                auto t = ext.texture(et.extern_id, bo);
                r = t.resource;
                fmt = t.format;
                stride = t.stride;
                elements = t.elements;
                first_element = t.first_element;
            }
            if (r && kind_is_texture(kind) && !resource_fits(r, kind)) r = nullptr, fmt = DXGI_FORMAT_UNKNOWN;
            bool null_view = false;
            if (!r)
                for (const auto& et : textures)
                    if (et.slot == s && ext.estimate_null_texture(et.extern_id, uint32_t(et.offset) * 8)) null_view = true;
            if (null_view) {
                write_null_srv(dev, kind, gpu.srv().cpu(base + off + s));
                cached[s].valid = false;
                continue;
            }
            auto& previous = cached[s];
            if (!cache_bindings || !previous.valid || previous.resource.Get() != r || previous.format != fmt || previous.stride != stride || previous.elements != elements || previous.first_element != first_element) {
                write_game_srv(dev, {r, fmt, stride, elements, first_element}, kind, gpu.srv().cpu(base + off + s));
                previous.resource = r;
                previous.format = fmt;
                previous.stride = stride;
                previous.elements = elements;
                previous.first_element = first_element;
                previous.valid = true;
            }
        }
        if (st.table_param >= 0) {
            if (compute_) cl->SetComputeRootDescriptorTable(UINT(st.table_param), gpu.srv().gpu(base + off));
            else cl->SetGraphicsRootDescriptorTable(UINT(st.table_param), gpu.srv().gpu(base + off));
        }
        for (const auto& [slot, kind] : st.uav_kinds) {
            const auto& source = st.sources.extern_uavs.at(slot);
            auto view = ext.texture(source.extern_id, uint32_t(source.offset) * 8);
            write_uav(dev, view, kind, gpu.srv().cpu(base + off + st.srv_count + slot));
        }
        if (st.uav_table_param >= 0) {
            const auto handle = gpu.srv().gpu(base + off + st.srv_count);
            if (compute_) cl->SetComputeRootDescriptorTable(UINT(st.uav_table_param), handle);
            else cl->SetGraphicsRootDescriptorTable(UINT(st.uav_table_param), handle);
        }
        off += st.srv_count + st.uav_count;
    }
    if (compute_) cl->Dispatch(x, y, z);
    else if (geometry) cl->DrawIndexedInstanced(geometry->indices, 1, 0, 0, 0);
    else cl->DrawInstanced(4, 1, 0, 0);
}

void GamePipeline::validate(const ExternStore& ext, const ScopeBufferFn& scopes) const {
    if (!loaded()) throw std::runtime_error("Game pipeline is not loaded");
    for (const auto& st : stages_) {
        std::vector<float4> cb(st.tfx.cbuffer_init.begin(), st.tfx.cbuffer_init.end());
        cb.resize(std::max<size_t>(cb.size(), 256));
        auto textures = st.extern_textures;
        if (!st.tfx.bytecode.empty()) run_tfx(st.tfx.bytecode, st.tfx.constants, cb, ext.environment());
        for (auto [reg, param] : st.cbvs)
            if (int32_t(reg) != st.tfx.cbuffer_slot && (!scopes || !scopes(st.which, reg)))
                throw std::runtime_error(std::format("{}: missing shared cbuffer b{}", st.tfx.shader.str(), reg));
        for (const auto& [slot, kind] : st.uav_kinds) {
            const auto it = st.sources.extern_uavs.find(slot);
            if (it == st.sources.extern_uavs.end()) throw std::runtime_error(std::format("{}: missing original UAV source u{}", st.tfx.shader.str(), slot));
            const auto view = ext.texture(it->second.extern_id, uint32_t(it->second.offset) * 8);
            if (!view.resource || !(view.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                throw std::runtime_error(std::format("{}: missing/incompatible original UAV u{}", st.tfx.shader.str(), slot));
            validate_resource_view(view, kind, true);
        }
        for (const auto& [slot, kind] : st.srv_kinds) {
            ID3D12Resource* resource = nullptr;
            if (auto it = st.static_textures.find(slot); it != st.static_textures.end()) resource = it->second.Get();
            uint32_t stride = 0, elements = 0;
            uint64_t first_element = 0;
            DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
            std::string source;
            bool null_view = false;
            for (const auto& et : textures) if (et.slot == slot) {
                const auto offset = uint32_t(et.offset) * 8;
                const auto view = ext.texture(et.extern_id, offset);
                resource = view.resource;
                stride = view.stride;
                elements = view.elements;
                first_element = view.first_element;
                format = view.format;
                source = std::format(" extern {} byte 0x{:X}", et.extern_id, offset);
                null_view = !resource && ext.estimate_null_texture(et.extern_id, offset);
            }
            if (null_view) continue;
            if (!resource || (kind_is_texture(kind) && !resource_fits(resource, kind)))
                throw std::runtime_error(std::format("{}: missing/incompatible t{}{}; no replacement is permitted", st.tfx.shader.str(), slot, source));
            validate_resource_view({resource, format, stride, elements, first_element}, kind, false);
        }
    }
}

std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext) {
    const auto& ss = scope.stages[size_t(stage)];
    std::vector<float4> cb(ss.cbuffer_init.begin(), ss.cbuffer_init.end());
    cb.resize(std::max<size_t>(cb.size(), 64));
    if (!ss.bytecode.empty()) run_tfx(ss.bytecode, ss.constants, cb, ext.environment());
    return cb;
}

// ---- GameLighting -----------------------------------------------------------------------------
namespace {
float4 v4(V3 v, float w) { return {v.x, v.y, v.z, w}; }
}

// ---- Viewer environment ------------------------------------------------------------------------
// Not game data: a map's reflection probes / sky are not loaded, so nim draws its own sky into the
// shading pass's specular-IBL light buffer and behind the model (the user asked for a readable
// environment here). Everything is reported as an estimate.
namespace {
constexpr const char* kViewerEnvHlsl = R"(
cbuffer P : register(b0) {
    float3 fwd;         float fx;
    float3 right;       float fy;
    float3 up;          float inv_w;
    float3 sky_top;     float inv_h;
    float3 sky_horizon; float sky_intensity;
    float3 ground;      float pad0;
    float3 bg_top;      float pad1;
    float3 bg_bottom;   float pad2;
};
Texture2D<float>  depth : register(t0);
Texture2D<float4> g1 : register(t1);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 sky(float3 d) {
    float t = d.z;
    float3 c = t > 0 ? lerp(sky_horizon, sky_top, pow(saturate(t), 0.6)) : lerp(sky_horizon, ground, saturate(-t * 4));
    return c * sky_intensity;
}
float3 view_ray(float4 pos) {
    float2 ndc = float2(pos.x * inv_w * 2 - 1, 1 - pos.y * inv_h * 2);
    return normalize(fwd + right * (ndc.x / fx) + up * (ndc.y / fy));
}
// Sky reflection along the G-buffer normal, blurred toward the hemisphere average when rough.
float4 ps_ibl(float4 pos : SV_Position) : SV_Target {
    int3 p = int3(pos.xy, 0);
    if (depth.Load(p) <= 0) return 0;
    float3 v = g1.Load(p).rgb * 2 - 1;
    float len = length(v);
    float3 n = len > 0 ? v / len : float3(0, 0, 1);
    float smooth = saturate(len * 4 - 3);
    float3 r = reflect(view_ray(pos), n);
    float3 blurred = lerp(sky(-n) * 0.25 + ground * 0.25, sky(n), saturate(n.z * 0.5 + 0.5));
    return float4(lerp(blurred, sky(r), smooth * smooth), 1);
}
// Background where nothing was drawn (linear, before the game's final_combine).
float4 ps_background(float4 pos : SV_Position) : SV_Target {
    if (depth.Load(int3(pos.xy, 0)) > 0) discard;
    return float4(lerp(bg_top, bg_bottom, pos.y * inv_h), 1);
}
)";

// Sun shadow resolve (viewer estimate): each pixel's world position from the depth buffer,
// compared with nim's sun shadow map (5x5 PCF), faded out at the map's edge.
constexpr const char* kSunShadowHlsl = R"(
cbuffer P : register(b0) {
    float3 fwd;     float fx;
    float3 right;   float fy;
    float3 up;      float near_z;
    float3 eye;     float inv_w;
    float3 s_eye;   float inv_h;
    float3 s_right; float s_half;
    float3 s_up;    float s_range;
    float3 s_fwd;   float s_texel;
    float bias;     float3 pad;
};
Texture2D<float> depth : register(t0);
Texture2D<float> smap : register(t1);
SamplerComparisonState cmp : register(s0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps_main(float4 pos : SV_Position) : SV_Target {
    float d = depth.Load(int3(pos.xy, 0));
    if (d <= 0) return 1;
    float z = near_z / d;
    float2 ndc = float2(pos.x * inv_w * 2 - 1, 1 - pos.y * inv_h * 2);
    float3 w = eye + fwd * z + right * (ndc.x * z / fx) + up * (ndc.y * z / fy);
    float3 rel = w - s_eye;
    float2 suv = float2(dot(rel, s_right), dot(rel, s_up)) / s_half;
    float sz = dot(rel, s_fwd);
    float edge = max(abs(suv.x), abs(suv.y));
    if (edge >= 1 || sz <= 0 || sz >= s_range) return 1;
    // Depth bias grows with distance from the camera (the depth buffer's precision).
    float ref = 1 - sz / s_range + bias * (1 + z * 0.02);
    float2 tc = suv * float2(0.5, -0.5) + 0.5;
    float sum = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
            sum += smap.SampleCmpLevelZero(cmp, tc + float2(x, y) * s_texel, ref);
    return lerp(sum / 25, 1, saturate((edge - 0.85) / 0.14));
}
)";

struct SunShadowConstants {
    V3 fwd; float fx;
    V3 right; float fy;
    V3 up; float near_z;
    V3 eye; float inv_w;
    V3 s_eye; float inv_h;
    V3 s_right; float s_half;
    V3 s_up; float s_range;
    V3 s_fwd; float s_texel;
    float bias; float pad[3];
};

struct ViewerEnvConstants {
    V3 fwd; float fx;
    V3 right; float fy;
    V3 up; float inv_w;
    V3 sky_top; float inv_h;
    V3 sky_horizon; float sky_intensity;
    V3 ground; float pad0;
    V3 bg_top; float pad1;
    V3 bg_bottom; float pad2;
};

ComPtr<ID3DBlob> compile_viewer_env(const char* entry, const char* target, const char* source = kViewerEnvHlsl) {
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(source, std::strlen(source), "nim_viewer_env.hlsl", nullptr, nullptr, entry,
                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr))
        throw std::runtime_error(std::format("viewer environment shader: {}",
                                             err ? static_cast<const char*>(err->GetBufferPointer()) : "compile failed"));
    return code;
}
} // namespace

void GameLighting::release(Gpu& gpu) {
    shading_.release(gpu);
    final_combine_.release(gpu);
    if (env_srv_ != UINT32_MAX) gpu.srv().free(env_srv_, 4);
    env_srv_ = UINT32_MAX;
    env_.Reset();
    shadow_mask_.Reset();
    light_diffuse_.Reset();
    light_specular_.Reset();
    cube_.Reset();
    lights_.clear();
    for (auto& lp : light_pipelines_) lp->release(gpu);
    light_pipelines_.clear();
    hdr_.Reset();
    rtv_heap_.Reset();
    w_ = h_ = 0;
}

void GameLighting::init(PackageManager& pm, Gpu& gpu) {
    globals_ = load_render_globals(pm);
    frame_scope_ = load_scope(pm, globals_.scope("frame"));
    view_scope_ = load_scope(pm, globals_.scope("view"));
    ext_.channels.assign(globals_.channel_defaults.begin(), globals_.channel_defaults.end());
    ext_.channel_ids = globals_.channel_ids;
    const Scope* scopes[] = {&frame_scope_, &view_scope_};
    const auto shading = globals_.pipeline("global_lighting_and_shading");
    const auto final = globals_.pipeline("final_combine");
    if (shading.is_none() || final.is_none()) throw std::runtime_error("Missing required game shading/final_combine pipeline");
    shading_.load(pm, gpu, shading, scopes, {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
    final_combine_.load(pm, gpu, final, scopes, {DXGI_FORMAT_R8G8B8A8_UNORM, false});
    auto lookup = [&](TagHash tag) {
        if (tag.is_none()) throw std::runtime_error("Missing render_globals BRDF lookup");
        return gpu.texture(load_texture(pm, tag));
    };
    tint_lookup_ = lookup(globals_.specular_tint_lookup);
    lobe_lookup_ = lookup(globals_.specular_lobe_lookup);
    lobe3d_lookup_ = lookup(globals_.specular_lobe_3d_lookup);
    iridescence_lookup_ = lookup(globals_.iridescence_lookup);
    {
        Texture white;
        white.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        white.width = white.height = 1;
        white.data = {255, 255, 255, 255};
        unshadowed_ = gpu.texture(white);
        // The shading pass multiplies the lit colour by the atmosphere volume's .x (transmittance)
        // and .y (fog transmittance) and adds .z-weighted in-scatter: (1, 1, 0) is "no atmosphere".
        Texture clear;
        clear.dxgi_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        clear.width = clear.height = 1;
        clear.depth = 2;
        clear.data = {255, 255, 0, 0, 255, 255, 0, 0};
        no_atmosphere_ = gpu.texture(clear);
    }
    {
        ID3D12Device* dev = gpu.device();
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor = {0, 0};
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        env_rs_ = serialize(dev, rsd, "viewer environment root signature");
        auto vs = compile_viewer_env("vs_main", "vs_5_0");
        for (auto [entry, pso] : {std::pair{"ps_ibl", &env_ibl_pso_}, std::pair{"ps_background", &env_bg_pso_}}) {
            auto ps = compile_viewer_env(entry, "ps_5_0");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = env_rs_.Get();
            pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1;
            pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pd.SampleDesc.Count = 1;
            check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(pso)), "viewer environment PSO");
        }
        // Sun shadow resolve: b0, (scene depth, shadow map), a comparison sampler.
        D3D12_STATIC_SAMPLER_DESC ss{};
        ss.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.ComparisonFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC srsd{2, params, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        shadow_rs_ = serialize(dev, srsd, "sun shadow resolve root signature");
        auto svs = compile_viewer_env("vs_main", "vs_5_0", kSunShadowHlsl);
        auto sps = compile_viewer_env("ps_main", "ps_5_0", kSunShadowHlsl);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = shadow_rs_.Get();
        pd.VS = {svs->GetBufferPointer(), svs->GetBufferSize()};
        pd.PS = {sps->GetBufferPointer(), sps->GetBufferSize()};
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        pd.SampleDesc.Count = 1;
        check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadow_pso_)), "sun shadow resolve PSO");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5};
    check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "game HDR RTV heap");
    gpu.flush();
}

void GameLighting::resize(Gpu& gpu, uint32_t w, uint32_t h) {
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R16G16B16A16_FLOAT, {0, 0, 0, 0}};
    hdr_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    gpu.device()->CreateRenderTargetView(hdr_.Get(), nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    env_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    gpu.device()->CreateRenderTargetView(env_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + inc});
    D3D12_CLEAR_VALUE mv{DXGI_FORMAT_R8_UNORM, {1, 1, 1, 1}};
    shadow_mask_ = gpu.texture2d(w, h, mv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &mv);
    gpu.device()->CreateRenderTargetView(shadow_mask_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 2 * inc});
    light_diffuse_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    light_specular_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    gpu.device()->CreateRenderTargetView(light_diffuse_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 3 * inc});
    gpu.device()->CreateRenderTargetView(light_specular_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 4 * inc});
    if (env_srv_ == UINT32_MAX) env_srv_ = gpu.srv().alloc(4);  // env: depth, rt1; shadow: depth, map
    w_ = w;
    h_ = h;
}

std::array<float4, 16> frame_constants(float, float, const ExternStore& ext) {
    return ext.frame_constants();
}

void estimate_engine_frame(ExternStore& ext, float t, float dt) {
    constexpr std::string_view why = "engine run-time state (exposure, wind); goriath FrameScope default";
    auto est = [&](uint32_t off, float4 v) {
        if (!ext.has(ext::Frame, off, 16)) ext.estimate(ext::Frame, off, v, why);
    };
    auto est1 = [&](uint32_t off, float v) {
        if (!ext.has(ext::Frame, off, 4)) {
            ext.set(ext::Frame, off, v);
            ext.note_estimate(std::format("extern 1 +0x{:X} = {:g}", off, v), why);
        }
    };
    est1(0x18, 1.0f / 60.0f);  // exposure_time
    est1(0x1C, 1.0f);          // exposure_scale
    est1(0x28, 1.0f);          // exposure_illum_relative
    est1(0x70, 0.5f);
    est(0x160, {0.1f, 0.1f, 0.1f, 1.0f});  // wind
    est(0x180, {0.1f, 0.1f, 0.1f, 0.0f});
    est(0x1B0, {0, 0, 0, 1});
    est(0x1C0, {0, 0, 1, 0});
    est(0x1E0, {0, 0, 0, 0});
    est(0x250, {1, 0, 1, 0});
    est(0x280, {1, 1, 1, 1});
    est(0x1F0, {0, 0, 1, 0});  // .z scales light_specular_ibl
    if (!ext.zero_policy(ext::Frame)) ext.zero_when_missing(ext::Frame, "engine frame value with no known meaning: zero");
    if (ext.has_frame_constants()) return;
    std::array<float4, 16> f{};
    const float es = ext.get_f(ext::Frame, 0x1C), ir = ext.get_f(ext::Frame, 0x28);
    f[0] = {t, t, dt > 0 ? dt : 1.0f / 60.0f, ext.get_f(ext::Frame, 0x18)};
    f[1] = {es, ir * 16.0f, es, ir};
    f[2] = {(t * 60 + 33.75f) * 1.258699f, (t * 60 + 60) * 0.9583125f, (t * 60 + 60) * 8.789123f, (t * 60 + 33.75f) * 2.311535f};
    f[3] = {0.5f, 0.5f, 0, 0};
    f[4] = {1, 1, 0, 1};
    f[5] = {0, 0, 512, 0};
    f[6] = {1, 1, 1, 1};
    ext.estimate_frame_constants(f, why);
}

void fill_view_externs(ExternStore& ext, uint32_t width, uint32_t height, const ViewMatrices& vm) {
    const float w = float(width), h = float(height);

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
    const M4 cam_to_proj = vm.proj;  // perspective, or the sun's orthographic shadow view
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
    ext.set(ext::View, 0x00, w);
    ext.set(ext::View, 0x04, h);
    ext.set(ext::View, 0x10, float4{0, 0, 0, 0});
    ext.set(ext::View, 0x20, v4(vm.eye, 1));
    ext.set(ext::View, 0x40, v4(vm.eye * -1.0f, 0));
    // Engine view state with no known meaning yet (goriath leaves these zero as well).
    constexpr std::string_view unknown_view = "unknown engine view value; zero as in goriath";
    ext.estimate(ext::View, 0x30, float4{0, 0, 0, 0}, unknown_view);  // view_miscellaneous
    ext.estimate(ext::View, 0x50, float4{0, 0, 0, 0}, unknown_view);
    ext.estimate(ext::View, 0x260, M4{}, unknown_view);
    ext.estimate(ext::View, 0x420, M4{}, unknown_view);
    ext.estimate(ext::View, 0x460, float4{0, 0, 0, 0}, unknown_view);
    if (!ext.zero_policy(ext::View)) ext.zero_when_missing(ext::View, std::string(unknown_view));
    ext.set(ext::View, 0x60, world_to_cam);
    ext.set(ext::View, 0xA0, cam_to_proj);
    ext.set(ext::View, 0xE0, cam_to_world);
    ext.set(ext::View, 0x120, pixel_to_ray);
    ext.set(ext::View, 0x160, proj_to_ray);
    ext.set(ext::View, 0x1A0, vm.vp_abs);
    ext.set(ext::View, 0x3E0, vm.vp_abs);

}

V3 sun_direction(const LightingParams& p) {
    return normalize(V3{std::cos(p.sun_azimuth) * std::cos(p.sun_elevation), std::sin(p.sun_azimuth) * std::cos(p.sun_elevation),
                        std::sin(p.sun_elevation)});
}

void GameLighting::fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p, bool shadowed) {
    fill_view_externs(ext_, gb.width(), gb.height(), vm);
    ext_.set(ext::Deferred, 0x00, float4{0, 1 / vm.vp_rel.c[3][2], 0, 0});
    ext_.set(ext::Deferred, 0x40, float4{float(gb.width()), float(gb.height()), 0, 0});
    ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), DXGI_FORMAT_R32_FLOAT);
    ext_.set_texture(ext::Deferred, 0xA8, gb.rt[0].Get());
    ext_.set_texture(ext::Deferred, 0xB0, gb.rt[1].Get());
    ext_.set_texture(ext::Deferred, 0xB8, gb.rt[2].Get());
    ext_.set(ext::Frame, 0x00, p.time);
    ext_.set(ext::Frame, 0x04, p.time);
    ext_.set(ext::Frame, 0x14, p.delta_time);
    estimate_engine_frame(ext_, p.time, p.delta_time);
    if (!ext_.texture(ext::Atmosphere, 0x108).resource)
        ext_.estimate_texture(ext::Atmosphere, 0x108, no_atmosphere_.Get(),
                              "no map atmosphere: transmittance 1, in-scatter 0 (clear air)");
    for (uint8_t id : {ext::Atmosphere, uint8_t(80), uint8_t(81), uint8_t(82)})
        if (!ext_.zero_policy(id)) ext_.zero_when_missing(id, "world state from a map (not loaded): zero");
    if (!lights_.empty()) {
        ext_.set_texture(ext::Deferred, 0xC0, light_diffuse_.Get());
        ext_.set_texture(ext::Deferred, 0xC8, light_specular_.Get());
    } else if (ext_.texture(ext::Deferred, 0xC0).resource == light_diffuse_.Get()) {
        ext_.set_texture(ext::Deferred, 0xC0, nullptr);
        ext_.set_texture(ext::Deferred, 0xC8, nullptr);
    }
    if (!ext_.texture(ext::Deferred, 0xD0).resource)
        ext_.estimate_texture(ext::Deferred, 0xD0, env_.Get(), "viewer environment: nim sky reflection (no map reflection probes)");
    if (!ext_.zero_policy(ext::Deferred)) ext_.zero_when_missing(ext::Deferred, "deferred-pass value with no known meaning: zero");
    // No shadow pass yet: the shadow / occlusion masks say "unshadowed" (white), full strength.
    constexpr std::string_view unshadowed = "no shadow pass yet: unshadowed (white) mask";
    // t7 is the sun shadow (the PS reads .r at the pixel while +0x34 = 0, its upsampling filter off).
    if (shadowed)
        ext_.estimate_texture(ext::ShadowMask, 0x00, shadow_mask_.Get(),
                              "nim sun shadow map: the scene's own pipelines drawn from the sun, 5x5 PCF (engine cascades not decoded)");
    else if (ext_.texture(ext::ShadowMask, 0x00).resource == shadow_mask_.Get())
        ext_.estimate_texture(ext::ShadowMask, 0x00, unshadowed_.Get(), unshadowed);
    for (uint32_t off : {0x00u, 0x08u, 0x10u})
        if (!ext_.texture(ext::ShadowMask, off).resource) ext_.estimate_texture(ext::ShadowMask, off, unshadowed_.Get(), unshadowed);
    if (!ext_.has(ext::ShadowMask, 0x30, 4)) {
        ext_.set(ext::ShadowMask, 0x30, 1.0f);
        ext_.set(ext::ShadowMask, 0x34, 0.0f);
        ext_.note_estimate("extern 43 +0x30 = 1, +0x34 = 0 (shadow strength / filter)", unshadowed);
    }
    if (!ext_.zero_policy(ext::ShadowMask)) ext_.zero_when_missing(ext::ShadowMask, "no shadow pass yet (sun shadow mask not rendered): zero");
    if (!ext_.zero_policy(ext::VolumeFog)) ext_.zero_when_missing(ext::VolumeFog, "no volumetric fog pass yet: zero");
    ext_.set_texture(ext::Frame, 0xC0, lobe_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xC8, lobe3d_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD0, tint_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD8, iridescence_lookup_.Get());

    // GlobalLighting: a map's sun and hemisphere ambient. Without a map it is nim's light set-up
    // (the panel's sun / sky), laid out as the shading reads it: the sun is multiplied by
    // 1/(4 pi) and the ambient by 1/4 there, so nim's values are scaled to match.
    constexpr std::string_view viewer_light = "viewer light set-up (no map lighting loaded)";
    const V3 sun_dir = sun_direction(p);
    const V3 sun = p.sun_color * (p.sun_intensity * 4.0f);
    const float ka = 4.0f * p.sky_intensity;
    ext_.estimate(ext::GlobalLighting, 0x10, v4(sun, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x30, v4(sun_dir, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x70, v4(p.sky_top * ka, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x80, v4(p.ground * ka, 0), viewer_light);
    constexpr std::string_view chosen = "meaning reverse-engineered, value chosen (v0.3.1)";
    ext_.estimate(ext::GlobalLighting, 0x50, float4{0, 0, 1, 0}, chosen);  // hemisphere axis
    ext_.estimate(ext::GlobalLighting, 0x90, float4{0.35f, 0.35f, 0, 0}, chosen);  // up / down sharpness
    ext_.estimate(ext::GlobalLighting, 0xD0, float4{1, 1, 0, 0}, chosen);  // light-buffer occlusion
    ext_.set_channel("skybox_sun_color", v4(p.sun_color, 1));
    ext_.set_channel("skybox_sun_intensity", float4{p.sun_intensity * 4.0f, 0, 0, 0});
    ext_.set_channel("sun_atmosphere_direction", v4(sun_dir, 0));
    ext_.set_channel("skybox_up_ambient_color", v4(p.sky_top, 1));
    ext_.set_channel("skybox_up_ambient_intensity", float4{ka, 0, 0, 0});
    ext_.set_channel("skybox_down_ambient_color", v4(p.ground, 1));
    ext_.set_channel("skybox_down_ambient_intensity", float4{ka, 0, 0, 0});
    ext_.note_estimate("global channels skybox_sun_* / skybox_*_ambient_*", viewer_light);
    if (ext_.channels.size() > 130) {
        ext_.channels[130] = {1, 0, 0, 0};  // scales the hemisphere ambient (cb0 60 / 62) in the shading pass
        ext_.note_estimate("global channel 130 = 1 (near-field ambient strength)", chosen);
    }
    if (!ext_.zero_policy(ext::GlobalLighting))
        ext_.zero_when_missing(ext::GlobalLighting, "global lighting value with no known meaning: zero");
}

void GameLighting::render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                          const LightingParams& p, const TransparentFn& transparents, SunShadow* shadow) {
    if (!ready()) throw std::runtime_error("Original game lighting is unavailable");
    if (gb.width() != w_ || gb.height() != h_) resize(gpu, gb.width(), gb.height());
    const bool shadowed = shadow && shadow->valid && shadow->depth;
    fill_externs(gb, vm, p, shadowed);
    // Validate every dependency before recording any render-target transitions.
    const auto frame = ext_.frame_constants();
    auto upload = [&](const auto& values) {
        auto a = gpu.dynamic(values.size() * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    const auto cb13 = upload(frame);
    const auto cb12_vs = upload(evaluate_scope(view_scope_, ScopeStage::Vertex, ext_));
    const auto cb12_ps = upload(evaluate_scope(view_scope_, ScopeStage::Pixel, ext_));
    ScopeBufferFn scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 13) return cb13;
        if (reg == 12) return stage == ShaderStage::Vertex ? cb12_vs : cb12_ps;
        return 0;
    };
    ext_.set_texture(ext::Postprocess, 0x00, hdr_.Get());
    shading_.validate(ext_, scopes);
    final_combine_.validate(ext_, scopes);
    // The complete pass graph (atmosphere, lighting volumes, exposure, grading and temporal
    // histories) is not yet decoded. Its outputs must be supplied by their real producers.
    // Transparent parts are left out (not substituted): the original pass's shared inputs and MRT
    // attachments are not reconstructed yet.
    if (transparents)
        ext_.note_estimate("transparent parts are NOT drawn", "original transparent pass inputs / MRTs not reconstructed");
    for (auto& rt : gb.rt)
        Gpu::barrier(cl, rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto depth_read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    Gpu::barrier(cl, gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, depth_read);
    Gpu::barrier(cl, hdr_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    D3D12_RECT sc{0, 0, LONG(w_), LONG(h_)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    const auto hdr = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const float clear[4]{};
    cl->ClearRenderTargetView(hdr, clear, 0, nullptr);

    // Viewer environment (estimate): sky reflection into the IBL buffer, background after shading.
    const bool own_env = ext_.texture(ext::Deferred, 0xD0).resource == env_.Get();
    const D3D12_CPU_DESCRIPTOR_HANDLE env_rtv{hdr.ptr + gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    gpu.device()->CreateShaderResourceView(gb.depth.Get(), &sd, gpu.srv().cpu(env_srv_));
    sd.Format = gb.rt[1]->GetDesc().Format;
    gpu.device()->CreateShaderResourceView(gb.rt[1].Get(), &sd, gpu.srv().cpu(env_srv_ + 1));
    ViewerEnvConstants ec{};
    ec.fwd = vm.fwd; ec.fx = vm.fx;
    ec.right = vm.right; ec.fy = vm.fy;
    ec.up = vm.up; ec.inv_w = 1.0f / float(w_);
    ec.sky_top = p.sky_top; ec.inv_h = 1.0f / float(h_);
    ec.sky_horizon = p.sky_horizon; ec.sky_intensity = p.sky_intensity;
    ec.ground = p.ground;
    ec.bg_top = p.background_top;
    ec.bg_bottom = p.background_bottom;
    const auto ecb = gpu.dynamic(sizeof(ec));
    std::memcpy(ecb.cpu, &ec, sizeof(ec));
    auto env_pass = [&](ID3D12PipelineState* pso, D3D12_CPU_DESCRIPTOR_HANDLE target) {
        cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
        cl->SetGraphicsRootSignature(env_rs_.Get());
        cl->SetPipelineState(pso);
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootConstantBufferView(0, ecb.gpu);
        cl->SetGraphicsRootDescriptorTable(1, gpu.srv().gpu(env_srv_));
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    };
    if (shadowed) {
        D3D12_SHADER_RESOURCE_VIEW_DESC dsd{};
        dsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dsd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dsd.Texture2D.MipLevels = 1;
        dsd.Format = DXGI_FORMAT_R32_FLOAT;
        gpu.device()->CreateShaderResourceView(gb.depth.Get(), &dsd, gpu.srv().cpu(env_srv_ + 2));
        gpu.device()->CreateShaderResourceView(shadow->depth.Get(), &dsd, gpu.srv().cpu(env_srv_ + 3));
        SunShadowConstants sc{};
        sc.fwd = vm.fwd; sc.fx = vm.fx;
        sc.right = vm.right; sc.fy = vm.fy;
        sc.up = vm.up; sc.near_z = vm.vp_rel.c[3][2];
        sc.eye = vm.eye; sc.inv_w = 1.0f / float(w_);
        sc.s_eye = shadow->vm.eye; sc.inv_h = 1.0f / float(h_);
        sc.s_right = shadow->vm.right; sc.s_half = shadow->half;
        sc.s_up = shadow->vm.up; sc.s_range = shadow->range;
        sc.s_fwd = shadow->vm.fwd; sc.s_texel = 1.0f / float(std::max(1u, shadow->size));
        sc.bias = 0.03f / shadow->range;
        const auto scb = gpu.dynamic(sizeof(sc));
        std::memcpy(scb.cpu, &sc, sizeof(sc));
        Gpu::barrier(cl, shadow->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Gpu::barrier(cl, shadow_mask_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE mask_rtv{hdr.ptr + 2 * gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV)};
        cl->OMSetRenderTargets(1, &mask_rtv, FALSE, nullptr);
        cl->SetGraphicsRootSignature(shadow_rs_.Get());
        cl->SetPipelineState(shadow_pso_.Get());
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootConstantBufferView(0, scb.gpu);
        cl->SetGraphicsRootDescriptorTable(1, gpu.srv().gpu(env_srv_ + 2));
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        Gpu::barrier(cl, shadow_mask_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Gpu::barrier(cl, shadow->depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }
    lights_drawn_ = 0;
    if (!lights_.empty()) draw_lights(gpu, cl, vm, scopes);
    if (own_env) {
        Gpu::barrier(cl, env_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        env_pass(env_ibl_pso_.Get(), env_rtv);
        Gpu::barrier(cl, env_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    cl->OMSetRenderTargets(1, &hdr, FALSE, nullptr);
    cl->SetGraphicsRootSignature(nullptr);
    shading_.draw(gpu, cl, ext_, scopes, true);
    env_pass(env_bg_pso_.Get(), hdr);
    ext_.note_estimate("background gradient", "viewer environment (no map sky)");
    Gpu::barrier(cl, hdr_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
    final_combine_.draw(gpu, cl, ext_, scopes, true);
    Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    for (auto& rt : gb.rt)
        Gpu::barrier(cl, rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, gb.depth.Get(), depth_read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ---- Local lights ------------------------------------------------------------------------------
namespace {

M4 m4(const std::array<float, 16>& a) {
    M4 m;
    for (int c = 0; c < 4; ++c) m.c[c] = {a[size_t(c * 4)], a[size_t(c * 4 + 1)], a[size_t(c * 4 + 2)], a[size_t(c * 4 + 3)]};
    return m;
}
float det3(const M4& m) {
    const auto& a = m.c[0]; const auto& b = m.c[1]; const auto& c = m.c[2];
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - b[0] * (a[1] * c[2] - a[2] * c[1]) + c[0] * (a[1] * b[2] - a[2] * b[1]);
}
M4 inverse(const M4& m) {
    float a[16], inv[16];
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) a[c * 4 + r] = m.c[c][size_t(r)];
    inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
    inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
    inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
    inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
    inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
    inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
    inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
    inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
    inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
    inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
    inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
    inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
    inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
    inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
    inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
    inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    det = std::fabs(det) > 1e-30f ? 1.0f / det : 0.0f;
    M4 r;
    for (int c = 0; c < 4; ++c) for (int rr = 0; rr < 4; ++rr) r.c[c][size_t(rr)] = inv[c * 4 + rr] * det;
    return r;
}
// goriath compute_light_bounds + compute_light_local_to_world: the light volume's box (in light
// space) as a transform of the unit cube [0,1]^3 with x and z swapped.
M4 light_box_to_world(const M4& node, const M4& light_space) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float4 p{c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, c & 4 ? 1.0f : -1.0f, 1.0f};
        float4 q{};
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) q[size_t(r)] += light_space.c[k][size_t(r)] * p[size_t(k)];
        for (int a = 0; a < 3; ++a) {
            const float v = std::fabs(q[3]) >= 1e-4f ? q[size_t(a)] / q[3] : 0.0f;
            lo[a] = std::min(lo[a], v);
            hi[a] = std::max(hi[a], v);
        }
    }
    float4 axis[3], w = node.c[3];
    for (int a = 0; a < 3; ++a) {
        const float half = (hi[a] - lo[a]) * 0.5f, centre = (hi[a] + lo[a]) * 0.5f;
        for (int k = 0; k < 4; ++k) {
            axis[a][size_t(k)] = node.c[a][size_t(k)] * half;
            w[size_t(k)] += node.c[a][size_t(k)] * centre;
        }
    }
    M4 m;
    for (int k = 0; k < 4; ++k) {
        m.c[2][size_t(k)] = axis[0][size_t(k)] * 2;
        m.c[1][size_t(k)] = axis[1][size_t(k)] * 2;
        m.c[0][size_t(k)] = axis[2][size_t(k)] * 2;
        m.c[3][size_t(k)] = w[size_t(k)] - axis[0][size_t(k)] - axis[1][size_t(k)] - axis[2][size_t(k)];
    }
    return m;
}

}  // namespace

void GameLighting::set_lights(PackageManager& pm, Gpu& gpu, std::span<const MapLight* const> lights) {
    lights_.clear();
    light_notes_.clear();
    if (!cube_) {
        // Unit cube, outward-facing counter-clockwise triangles.
        const float v[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
        const uint16_t idx[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
        std::vector<uint8_t> bytes(sizeof(v) + sizeof(idx));
        std::memcpy(bytes.data(), v, sizeof(v));
        std::memcpy(bytes.data() + sizeof(v), idx, sizeof(idx));
        cube_ = gpu.upload(bytes.data(), bytes.size());
        cube_vbv_ = {cube_->GetGPUVirtualAddress(), UINT(sizeof(v)), 12};
        cube_ibv_ = {cube_->GetGPUVirtualAddress() + sizeof(v), UINT(sizeof(idx)), DXGI_FORMAT_R16_UINT};
    }
    // One pipeline pair per (shaders, textures, samplers); techniques are cached by tag.
    std::map<std::string, uint32_t> keys;
    std::unordered_map<uint32_t, std::shared_ptr<const Technique>> techniques;
    std::map<std::string, size_t> failures;
    const Scope* scopes[] = {&frame_scope_, &view_scope_};
    for (const MapLight* l : lights) {
        try {
            auto& tech = techniques[l->technique.value];
            if (!tech) tech = std::make_shared<const Technique>(load_technique(pm, l->technique));
            std::string key;
            for (auto which : {ShaderStage::Vertex, ShaderStage::Pixel}) {
                const auto& st = tech->stage(which);
                key += std::format("{:08X}:", st.shader.value);
                for (const auto& t : st.textures) key += std::format("t{}={:08X},", t.slot, t.texture.value);
                for (const auto& sm : st.samplers) key += std::format("s{:08X},", sm.value);
                key += '|';
            }
            auto it = keys.find(key);
            if (it == keys.end()) {
                const uint32_t index = uint32_t(light_pipelines_.size());
                for (bool front : {true, false}) {
                    auto lp = std::make_unique<GamePipeline>();
                    GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, true, {DXGI_FORMAT_R16G16B16A16_FLOAT}};
                    opt.volume = true;
                    opt.cull_front = front;
                    opt.cbuffer_floor = 64;
                    lp->load(pm, gpu, l->technique, scopes, opt);
                    light_pipelines_.push_back(std::move(lp));
                }
                it = keys.emplace(key, index).first;
            }
            lights_.push_back({it->second, tech, *l});
        } catch (const std::exception& ex) {
            ++failures[std::string(ex.what()).substr(0, 120)];
        }
    }
    for (const auto& [why, n] : failures) light_notes_.push_back(std::format("{} lights left out: {}", n, why));
    if (!lights_.empty())
        ext_.note_estimate("local lights: deferred_light +0x100..+0x130 = (0,0,0,1), +0x150 = 1, +0x154 = +0x158 = 0",
                           "goriath's DeferredLight defaults (plane masks off, full strength)");
}

void GameLighting::draw_lights(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, const ScopeBufferFn& scopes) {
    Gpu::barrier(cl, light_diffuse_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, light_specular_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{base.ptr + 3 * inc}, {base.ptr + 4 * inc}};
    const float zero[4]{};
    cl->ClearRenderTargetView(rtvs[0], zero, 0, nullptr);
    cl->ClearRenderTargetView(rtvs[1], zero, 0, nullptr);
    cl->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    if (!ext_.zero_policy(ext::DeferredLight)) ext_.zero_when_missing(ext::DeferredLight, "deferred light value with no known meaning: zero");
    if (!ext_.zero_policy(15)) ext_.zero_when_missing(15, "simple geometry value with no known meaning: zero");
    M4 to_rel;  // world -> camera-relative world
    to_rel.c[0] = {1, 0, 0, 0}; to_rel.c[1] = {0, 1, 0, 0}; to_rel.c[2] = {0, 0, 1, 0};
    to_rel.c[3] = {-vm.eye.x, -vm.eye.y, -vm.eye.z, 1};
    for (const auto& ll : lights_) {
        const MapLight& l = ll.light;
        // Off-screen volumes: all eight box corners outside one clip plane.
        bool outside = false;
        {
            int out[5]{};
            for (int c = 0; c < 8; ++c) {
                const float4 p{c & 1 ? l.hi[0] - vm.eye.x : l.lo[0] - vm.eye.x, c & 2 ? l.hi[1] - vm.eye.y : l.lo[1] - vm.eye.y,
                               c & 4 ? l.hi[2] - vm.eye.z : l.lo[2] - vm.eye.z, 1};
                float4 q{};
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) q[size_t(r)] += vm.vp_rel.c[k][size_t(r)] * p[size_t(k)];
                out[0] += q[0] < -q[3]; out[1] += q[0] > q[3]; out[2] += q[1] < -q[3]; out[3] += q[1] > q[3]; out[4] += q[3] <= 0;
            }
            for (int i = 0; i < 5; ++i) outside |= out[i] == 8;
        }
        if (outside) continue;
        const M4 node = m4(l.node), space = m4(l.light_space);
        const M4 node_rel = mul(to_rel, node);
        const M4 volume = mul(node_rel, space);  // unit cube -> camera-relative world
        ext_.set(15, 0x00, mul(vm.vp_rel, volume));
        ext_.set(ext::DeferredLight, 0x00, 0.0f);
        ext_.set(ext::DeferredLight, 0x40, inverse(mul(to_rel, light_box_to_world(node, space))));
        ext_.set(ext::DeferredLight, 0xC0, node_rel);
        for (uint32_t off : {0x100u, 0x110u, 0x120u, 0x130u}) ext_.set(ext::DeferredLight, off, float4{0, 0, 0, 1});
        ext_.set(ext::DeferredLight, 0x150, 1.0f);
        ext_.set(ext::DeferredLight, 0x154, 0.0f);
        ext_.set(ext::DeferredLight, 0x158, 0.0f);
        // Only the far side of the volume is drawn (the camera may be inside it); the cube's
        // winding on screen flips with the handedness of its transform.
        const bool mirrored = det3(volume) * (space.c[3][3] + space.c[2][3] < 0 ? -1.0f : 1.0f) < 0;
        GamePipeline& gp = *light_pipelines_[ll.pipeline + (mirrored ? 1 : 0)];
        try {
            gp.draw_volume(gpu, cl, ext_, scopes, *ll.technique, cube_vbv_, cube_ibv_, 36);
            ++lights_drawn_;
        } catch (const std::exception& ex) {
            if (light_notes_.size() < 32) light_notes_.push_back(std::format("{}: {}", ll.technique->tag.str(), ex.what()));
        }
    }
    Gpu::barrier(cl, light_diffuse_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, light_specular_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

} // namespace nim
