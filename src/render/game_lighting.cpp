#include "nim/render/game_lighting.hpp"
#include "nim/core/profile.hpp"
#include "nim/core/parallel.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/content/weather.hpp"

#include <d3dcompiler.h>
#include <mutex>


#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
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
        sd.TextureCube.MostDetailedMip = UINT(first);  // a cube view's first mip (filter source chains)
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
        // A first mip selects that one mip (a mip chain's source while the next is written).
        sd.Texture2D.MostDetailedMip = UINT(first);
        sd.Texture2D.MipLevels = elements ? elements : first ? 1 : UINT(-1);
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
    } else if (kind == BindingKind::Texture2D) {
        desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = view.mip;
    } else if (kind == BindingKind::Texture3D) {
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

void ExternStore::copy_block_from(uint8_t id, const ExternStore& source) {
    if (this == &source) return;
    const size_t base = size_t(id) * kBlockSize;
    std::copy_n(source.data_.begin() + base, kBlockSize, data_.begin() + base);
    std::copy_n(source.present_.begin() + base, kBlockSize, present_.begin() + base);
    const auto first = tex_key(id, 0), last = first + (uint64_t(1) << 32);
    textures_.erase(textures_.lower_bound(first), textures_.lower_bound(last));
    textures_.insert(source.textures_.lower_bound(first), source.textures_.lower_bound(last));
    zero_policy_.erase(id);
    if (const auto policy = source.zero_policy_.find(id); policy != source.zero_policy_.end())
        zero_policy_[id] = policy->second;
    estimates_.insert(source.estimates_.begin(), source.estimates_.end());
}

void ExternStore::require(uint8_t id, uint32_t off, uint32_t size) const {
    if (id >= kBlocks || off > kBlockSize || size > kBlockSize - off)
        throw std::out_of_range("extern offset");
    const size_t base = size_t(id) * kBlockSize + off;
    if (!std::all_of(present_.begin() + base, present_.begin() + base + size, [](uint8_t v) { return v != 0; })) {
        if (auto it = zero_policy_.find(id); it != zero_policy_.end()) {  // data_ is zero there
            if (first_note(uint64_t(id) << 40 | uint64_t(off) << 16 | size))
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

namespace {
// Material constants evaluate on worker threads and report through the same stores.
std::mutex g_estimate_mutex;
}
void ExternStore::note_estimate(std::string what, std::string_view why) const {
    std::lock_guard lock(g_estimate_mutex);
    estimates_.try_emplace(std::move(what), why);
}
// Repeated per-frame reports of the same missing value skip formatting the same text again.
bool ExternStore::first_note(uint64_t key) const {
    std::lock_guard lock(g_estimate_mutex);
    return noted_.insert(key).second;
}

bool ExternStore::estimate_null_texture(uint8_t id, uint32_t off) const {
    auto it = zero_policy_.find(id);
    if (it == zero_policy_.end()) return false;
    if (first_note(uint64_t(1) << 63 | tex_key(id, off)))
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
    frame_estimated_ = true;
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
void ExternStore::set_texture(uint8_t id, uint32_t off, ID3D12Resource* r, DXGI_FORMAT f, uint32_t stride, uint32_t elements, uint64_t first_element, uint32_t mip) {
    if (r) textures_[tex_key(id, off)] = {r, f, stride, elements, first_element, mip};
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
void ExternOverlay::set(uint8_t id, uint32_t off, const M4& m) {
    for (int c = 0; c < 4; ++c) set(id, off + uint32_t(c) * 16, m.c[c]);
}

TfxEnvironment ExternStore::environment(const std::vector<std::pair<uint32_t, float>>* overlay) const {
    TfxEnvironment env = environment();
    if (!overlay || overlay->empty()) return env;
    auto find = [overlay](uint8_t id, uint32_t off) -> const float* {
        const uint32_t k = ExternOverlay::key(id, off);
        const auto it = std::lower_bound(overlay->begin(), overlay->end(), k, [](const auto& e, uint32_t v) { return e.first < v; });
        return it != overlay->end() && it->first == k ? &it->second : nullptr;
    };
    env.extern_float = [this, find](uint8_t id, uint8_t off) {
        if (const float* v = find(id, uint32_t(off) * 4)) return *v;
        return get_f(id, uint32_t(off) * 4);
    };
    env.extern_vec4 = [this, find](uint8_t id, uint8_t off16) {
        const uint32_t off = uint32_t(off16) * 16;
        float4 r{};
        uint32_t mask = 0;
        for (uint32_t w = 0; w < 4; ++w)
            if (const float* v = find(id, off + w * 4)) r[w] = *v, mask |= 1u << w;
        if (mask == 0) return get_v(id, off);
        if (mask == 0xF) return r;
        // Partly written by the draw: the store's words for the rest, checked as one vec4 read
        // (the same report or error as reading it after the draw's writes).
        bool present = true;
        for (uint32_t w = 0; w < 4; ++w)
            if (!(mask >> w & 1) && !has(id, off + w * 4, 4)) present = false;
        if (!present) require(id, off, 16);
        for (uint32_t w = 0; w < 4; ++w)
            if (!(mask >> w & 1)) std::memcpy(&r[w], data_.data() + size_t(id) * kBlockSize + off + w * 4, 4);
        return r;
    };
    return env;
}

TfxEnvironment ExternStore::environment() const {
    TfxEnvironment env;
    env.extern_vec4 = [this](uint8_t id, uint8_t off) { return get_v(id, uint32_t(off) * 16); };
    env.extern_float = [this](uint8_t id, uint8_t off) { return get_f(id, uint32_t(off) * 4); };
    env.global_channel = [this](uint8_t ch) {
        if (ch >= channels.size()) throw std::runtime_error(std::format("Missing game global channel {}", ch));
        return channels[ch];
    };
    // Reported once each (lock-free check): materials evaluate these on worker threads every frame.
    env.world_channel = [this](uint8_t ch) {
        if (first_bit(0))
            note_estimate("TFX 0x64 = scalar world channel registry",
                          "separate 52-slot script registry; engine opcode implementation is not available");
        float v = 0;
        if (const auto it = world_channels.find(ch); it != world_channels.end()) v = it->second;
        else if (first_bit(256 + ch)) note_estimate(std::format("world channel {} = 0", ch), "no active script state supplied");
        return float4{v, v, v, v};
    };
    // nim loads every mip of a texture: the streaming state is "fully resident" (min mip 0).
    env.texture_tiling = [this](uint8_t texture) {
        if (first_bit(512 + texture))
            note_estimate(std::format("texture {} tiling params = 0", texture), "nim loads all mips: fully resident (min resident mip 0)");
        return float4{0, 0, 0, 0};
    };
    // nim binds every texture as a plain (single-layer, untiled) resource.
    env.texture_layer_count = [this](uint8_t texture) {
        if (first_bit(768 + texture))
            note_estimate(std::format("texture {} layer count = 1", texture), "nim binds textures untiled with a single layer");
        return float4{1, 1, 1, 1};
    };
    return env;
}

// ---- GamePipeline -----------------------------------------------------------------------------------
void GamePipeline::release(Gpu& gpu) {
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
    compute_ = t.bind_mode == 6 || (opt.compute_stage && !t.stage(ShaderStage::Compute).shader.is_none()) || (t.bind_mode == 2 &&
        !t.stage(ShaderStage::Compute).shader.is_none() && t.stage(ShaderStage::Pixel).shader.is_none());
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
    const std::vector<ShaderStage> active_stages = compute_ ? std::vector{ShaderStage::Compute} :
        opt.stencil_volume ? std::vector{ShaderStage::Vertex} : std::vector{ShaderStage::Vertex, ShaderStage::Pixel};

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
                                  opt.volume || opt.instance_float4s ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : D3D12_ROOT_SIGNATURE_FLAG_NONE};
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
    if (opt.blend_state >= 0) {
        pd.BlendState.IndependentBlendEnable = TRUE;
        for (UINT r = 0; r < 1 + opt.additional_rtv_formats.size() && r < 5; ++r)
            pd.BlendState.RenderTarget[r] = engine_blend_desc(opt.blend_state, int(r));
    } else if (opt.additive) {
        bl.BlendEnable = TRUE;
        bl.SrcBlend = bl.DestBlend = D3D12_BLEND_ONE;
        bl.BlendOp = D3D12_BLEND_OP_ADD;
        bl.SrcBlendAlpha = bl.DestBlendAlpha = D3D12_BLEND_ONE;
        bl.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (!opt.write_masks.empty()) {
        pd.BlendState.IndependentBlendEnable = TRUE;
        for (size_t r = 0; r < opt.write_masks.size() && r < 8; ++r) pd.BlendState.RenderTarget[r].RenderTargetWriteMask = opt.write_masks[r];
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
    std::vector<D3D12_INPUT_ELEMENT_DESC> instance_layout;
    for (uint32_t i = 0; i < opt.instance_float4s; ++i)
        instance_layout.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16 * i, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1});
    if (!instance_layout.empty()) {
        pd.InputLayout = {instance_layout.data(), UINT(instance_layout.size())};
        pd.RasterizerState.DepthClipEnable = TRUE;
    }
    cbuffer_floor_ = opt.cbuffer_floor;
    pd.NumRenderTargets = UINT(1 + opt.additional_rtv_formats.size());
    if (pd.NumRenderTargets > 8) throw std::runtime_error("Too many original render targets");
    pd.RTVFormats[0] = opt.rtv_format;
    for (size_t i = 0; i < opt.additional_rtv_formats.size(); ++i) pd.RTVFormats[i + 1] = opt.additional_rtv_formats[i];
    for (const auto& output : opt.stencil_volume ? std::vector<ShaderInput>{} : shader_outputs(ps_code))
        if (_stricmp(output.semantic.c_str(), "SV_Target") == 0 && output.index >= pd.NumRenderTargets && !output.never_written &&
            !opt.drop_extra_targets)
            throw std::runtime_error(std::format("{}: original PS requires SV_Target{}; attachment missing", technique.str(), output.index));
    if (opt.depth_func || opt.stencil_volume || opt.stencil_test) {
        pd.DSVFormat = GBuffer::kDepthFormat;
    }
    if (opt.depth_func || opt.stencil_volume) {
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = opt.stencil_volume ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : opt.depth_func;
    }
    if (opt.stencil_volume || opt.stencil_test) {
        auto& ds = pd.DepthStencilState;
        ds.StencilEnable = TRUE;
        ds.StencilReadMask = 0xFF;
        ds.StencilWriteMask = opt.stencil_volume ? 0xFF : 0;
        ds.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                        opt.stencil_test ? D3D12_COMPARISON_FUNC_NOT_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS};
        ds.BackFace = ds.FrontFace;
        if (opt.stencil_volume) {
            pd.PS = {};
            pd.NumRenderTargets = 0;
            for (auto& f : pd.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            ds.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_INCR;
            ds.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_DECR;
        }
    }
    pd.SampleDesc.Count = 1;
    check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)), "CreateGraphicsPipelineState (game pipeline)");

    }
}

void GamePipeline::draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        bool validated) {
    if (compute_) throw std::runtime_error("Compute pipeline requires original dispatch dimensions");
    execute(gpu, cl, ext, scopes, validated, 0, 0, 0);
}

void GamePipeline::draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures) {
    if (compute_) throw std::runtime_error("Compute pipeline requires original dispatch dimensions");
    const Geometry g{nullptr, nullptr, nullptr, 0, ps_textures};
    execute(gpu, cl, ext, scopes, true, 0, 0, 0, &g);
}

void GamePipeline::draw_instances(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                                  const D3D12_VERTEX_BUFFER_VIEW& vbv, uint32_t instances, VolumeBatch& batch) {
    if (compute_) throw std::runtime_error("Compute pipeline cannot draw instances");
    Geometry g{nullptr, &vbv, nullptr, 4};
    g.instances = instances;
    g.batch = &batch;
    g.stable_views = true;
    execute(gpu, cl, ext, scopes, true, 0, 0, 0, &g);
}

void GamePipeline::dispatch_batch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                                  uint32_t groups_x, VolumeBatch& batch) {
    if (!compute_ || !groups_x || groups_x > 65535) throw std::invalid_argument("Invalid original compute dispatch");
    Geometry g{nullptr, nullptr, nullptr, 0, {}, &batch};
    g.stable_views = true;
    execute(gpu, cl, ext, scopes, batch.pipeline == this, groups_x, 1, 1, &g);
}

void GamePipeline::draw_volume(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                               const Technique* materials, const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv,
                               uint32_t indices, std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures, VolumeBatch* batch,
                               const StageConstants* constants) {
    const Geometry g{materials, &vbv, &ibv, indices, ps_textures, batch, constants};
    execute(gpu, cl, ext, scopes, true, 0, 0, 0, &g);
}

void GamePipeline::evaluate_stages(const TfxEnvironment& env, const Technique* materials, StageConstants& out) const {
    if (stages_.size() > out.size()) throw std::logic_error("evaluate_stages: more stages than StageConstants holds");
    for (size_t i = 0; i < stages_.size(); ++i) {
        const auto& st = stages_[i];
        const TechniqueStage& tfx = materials ? materials->stage(st.which) : st.tfx;
        auto& cb = out[i];
        cb.assign(tfx.cbuffer_init.begin(), tfx.cbuffer_init.end());
        cb.resize(std::max<size_t>(cb.size(), cbuffer_floor_));
        if (!tfx.bytecode.empty()) run_tfx(tfx.bytecode, tfx.constants, cb, env, nullptr, false);
    }
}

void GamePipeline::dispatch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                            uint32_t x, uint32_t y, uint32_t z, std::span<const std::pair<uint32_t, ID3D12Resource*>> textures) {
    if (!compute_ || !x || !y || !z || x > 65535 || y > 65535 || z > 65535)
        throw std::invalid_argument("Invalid original compute dispatch");
    const Geometry bindings{nullptr, nullptr, nullptr, 0, textures};
    execute(gpu, cl, ext, scopes, !textures.empty(), x, y, z, textures.empty() ? nullptr : &bindings);
}

void GamePipeline::dispatch_indirect(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                                     ID3D12CommandSignature* signature, ID3D12Resource* args) {
    if (!compute_ || !signature || !args) throw std::invalid_argument("Invalid original indirect compute dispatch");
    indirect_signature_ = signature;
    indirect_args_ = args;
    try {
        execute(gpu, cl, ext, scopes, false, 1, 1, 1);
    } catch (...) {
        indirect_args_ = nullptr;
        throw;
    }
    indirect_args_ = nullptr;
}

void GamePipeline::execute(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                           bool validated, uint32_t x, uint32_t y, uint32_t z, const Geometry* geometry) {
    if (!validated) validate(ext, scopes);
    if (!pso_) return;
    ID3D12Device* dev = gpu.device();
    std::optional<TfxEnvironment> env;  // only when a stage was not evaluated ahead
    // Identical resolved views share an immutable table for this frame. Per-light constants
    // still change independently; different cubemap textures never alias a mutable table.

    auto* batch = geometry ? geometry->batch : nullptr;
    const bool reuse_views = batch && batch->pipeline == this && geometry->stable_views;
    auto* stable_tables = batch && geometry->stable_views && !reuse_views ? &batch->stable_tables[this] : nullptr;
    if (!batch || batch->pipeline != this) {
        if (compute_) cl->SetComputeRootSignature(root_sig_.Get());
        else cl->SetGraphicsRootSignature(root_sig_.Get());
        cl->SetPipelineState(pso_.Get());
        if (!batch || !batch->pipeline) {
            ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
            cl->SetDescriptorHeaps(1, heaps);
        }
        if (batch) {
            batch->pipeline = this;
            batch->root_bindings.fill(UINT64_MAX);
        }
    }
    if (geometry && geometry->instances) {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cl->IASetVertexBuffers(0, 1, geometry->vbv);
    } else if (geometry && geometry->vbv) {
        if (!batch || batch->vertices != geometry->vbv || batch->indices != geometry->ibv) {
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->IASetVertexBuffers(0, 1, geometry->vbv);
            cl->IASetIndexBuffer(geometry->ibv);
            if (batch) {
                batch->vertices = geometry->vbv;
                batch->indices = geometry->ibv;
            }
        }
    } else if (!compute_) {
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    }
    auto changed = [&](uint32_t param, uint64_t value) {
        if (!batch) return true;
        auto& old = batch->root_bindings[param];
        if (old == value) return false;
        old = value;
        return true;
    };

    for (size_t stage_index = 0; stage_index < stages_.size(); ++stage_index) {
        auto& st = stages_[stage_index];
        const TechniqueStage& tfx = geometry && geometry->materials ? geometry->materials->stage(st.which) : st.tfx;
        const auto& textures = st.extern_textures;
        const bool ahead = geometry && geometry->constants;  // evaluated ahead (evaluate_stages)
        if (!ahead) {
            auto& cb = st.constants;
            cb.assign(tfx.cbuffer_init.begin(), tfx.cbuffer_init.end());
            cb.resize(std::max<size_t>(cb.size(), cbuffer_floor_));
            // The authored resource bindings were decoded at pipeline creation.
            if (!tfx.bytecode.empty()) {
                if (!env) env = ext.environment();
                run_tfx(tfx.bytecode, tfx.constants, cb, *env, nullptr, false);
            }
        }
        const auto& cb = ahead ? (*geometry->constants)[stage_index] : st.constants;
        D3D12_GPU_VIRTUAL_ADDRESS own_cb = 0;
        for (auto [reg, param] : st.cbvs) {
            if (int32_t(reg) == tfx.cbuffer_slot && !own_cb) {
                const auto a = gpu.dynamic(cb.size() * sizeof(float4));
                std::memcpy(a.cpu, cb.data(), cb.size() * sizeof(float4));
                own_cb = a.gpu;
            }
            const auto va = int32_t(reg) == tfx.cbuffer_slot ? own_cb : scopes ? scopes(st.which, reg) : 0;
            if (!changed(param, va)) continue;
            if (compute_) cl->SetComputeRootConstantBufferView(param, va);
            else cl->SetGraphicsRootConstantBufferView(param, va);
        }
        if (reuse_views || (!st.srv_count && !st.uav_count)) continue;
        const auto bind_tables = [&](uint32_t base) {
            if (st.table_param >= 0) {
                const auto handle = gpu.srv().gpu(base);
                if (changed(uint32_t(st.table_param), handle.ptr)) {
                    if (compute_) cl->SetComputeRootDescriptorTable(UINT(st.table_param), handle);
                    else cl->SetGraphicsRootDescriptorTable(UINT(st.table_param), handle);
                }
            }
            if (st.uav_table_param >= 0) {
                const auto handle = gpu.srv().gpu(base + st.srv_count);
                if (changed(uint32_t(st.uav_table_param), handle.ptr)) {
                    if (compute_) cl->SetComputeRootDescriptorTable(UINT(st.uav_table_param), handle);
                    else cl->SetGraphicsRootDescriptorTable(UINT(st.uav_table_param), handle);
                }
            }
        };
        // Draw order can interleave pipelines. Their tables stay valid for the whole phase.
        auto* stable_base = stable_tables ? &stable_tables->at(stage_index) : nullptr;
        if (stable_base && *stable_base) {
            bind_tables(**stable_base);
            continue;
        }
        pending_views_.clear();
        view_resources_.clear();
        view_key_.assign({st.srv_count, st.uav_count});
        auto remember = [&](uint32_t slot, BindingKind kind, ExternStore::Tex view, bool uav = false) {
            pending_views_.push_back({slot, kind, view, uav});
            view_key_.insert(view_key_.end(), {slot, uint64_t(kind), uint64_t(reinterpret_cast<uintptr_t>(view.resource)),
                uint64_t(view.format), view.stride, view.elements, view.first_element, uint64_t(uav), uint64_t(view.mip)});
            view_resources_.push_back(view.resource);
        };
        for (const auto& [s, kind] : st.srv_kinds) {
            ID3D12Resource* r = nullptr;
            DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
            uint32_t stride = 0, elements = 0;
            uint64_t first_element = 0;
            if (auto it = st.static_textures.find(s); it != st.static_textures.end()) r = it->second.Get();
            bool engine_bound = false;  // a view the caller binds wins over the TFX's extern
            if (geometry && (st.which == ShaderStage::Pixel || st.which == ShaderStage::Compute))
                for (const auto& [slot, res] : geometry->ps_textures)
                    if (slot == s) r = res, engine_bound = true;
            for (const auto& et : textures) {
                if (et.slot != s || engine_bound) continue;
                const uint32_t bo = uint32_t(et.offset) * 8;
                auto t = ext.texture(et.extern_id, bo);
                r = t.resource;
                fmt = t.format;
                stride = t.stride;
                elements = t.elements;
                first_element = t.first_element;
            }
            bool null_view = engine_bound && !r;
            if (!r)
                for (const auto& et : textures)
                    if (et.slot == s && ext.estimate_null_texture(et.extern_id, uint32_t(et.offset) * 8)) null_view = true;
            if (null_view) {
                remember(s, kind, {});
                continue;
            }
            if (!r) throw std::runtime_error(std::format("{}: missing t{}; no replacement is permitted", st.tfx.shader.str(), s));
            remember(s, kind, {r, fmt, stride, elements, first_element});
        }
        for (const auto& [slot, kind] : st.uav_kinds) {
            const auto& source = st.sources.extern_uavs.at(slot);
            remember(st.srv_count + slot, kind, ext.texture(source.extern_id, uint32_t(source.offset) * 8), true);
        }
        uint32_t base = 0;
        if (st.srv_count || st.uav_count) {
            if (const auto cached = gpu.find_transient_table(view_key_)) base = *cached;
            else {
                base = gpu.transient_descriptors(st.srv_count + st.uav_count);
                for (const auto& view : pending_views_) {
                    const auto handle = gpu.srv().cpu(base + view.slot);
                    if (view.uav) write_uav(dev, view.view, view.kind, handle);
                    else if (!view.view.resource) write_null_srv(dev, view.kind, handle);
                    else write_game_srv(dev, view.view, view.kind, handle);
                }
                // Publish only after every descriptor is valid; a failed strict binding cannot
                // leave a partially initialized table in the cache.
                gpu.cache_transient_table(view_key_, base, view_resources_);
            }
        }
        if (stable_base) *stable_base = base;
        bind_tables(base);
    }
    if (compute_ && indirect_args_) cl->ExecuteIndirect(indirect_signature_, 1, indirect_args_, 0, nullptr, 0);
    else if (compute_) cl->Dispatch(x, y, z);
    else if (geometry && geometry->instances) cl->DrawInstanced(4, geometry->instances, 0, 0);
    else if (geometry && geometry->vbv && geometry->ibv && geometry->ibv->BufferLocation) cl->DrawIndexedInstanced(geometry->indices, 1, 0, 0, 0);
    else if (geometry && geometry->vbv) cl->DrawInstanced(geometry->indices, 1, 0, 0);
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

std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const TfxEnvironment& env) {
    const auto& ss = scope.stages[size_t(stage)];
    std::vector<float4> cb(ss.cbuffer_init.begin(), ss.cbuffer_init.end());
    cb.resize(std::max<size_t>(cb.size(), 64));
    if (!ss.bytecode.empty()) run_tfx(ss.bytecode, ss.constants, cb, env);
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

// View-space depth from the reversed depth buffer (z = near / d): the hdao pass's second depth
// input, gathered from R on one side of each sample pair and from G on the other (both hold the
// same depth here). The engine's producer of that texture is not identified, so nim makes it.
constexpr float kSkyViewDepth = 1e6f;  // finite view depth at the cleared device depth
constexpr const char* kLinearDepthHlsl = R"(
cbuffer P : register(b0) { float near_z; float sky_depth; float2 pad; };
Texture2D<float> depth : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float2 ps_main(float4 pos : SV_Position) : SV_Target {
    float d = depth.Load(int3(pos.xy, 0));
    return (d > 0 ? near_z / d : sky_depth).xx;
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
    hdao_.release(gpu);
    apply_ssao_.release(gpu);
    sky_near_.release(gpu);
    sky_far_.release(gpu);
    sky_.release(gpu);
    atmo_density_.release(gpu);
    atmo_density_rt_.Reset();
    sky_near_rt_.Reset();
    sky_far_rt_.Reset();
    vol_downsample_.release(gpu);
    vol_blur_.release(gpu);
    vol_upres_.release(gpu);
    for (auto& vp : vol_pipelines_) vp->release(gpu);
    vol_pipelines_.clear();
    vol_lights_.clear();
    vol_minmax_.Reset();
    for (auto& r : vol_layer_) r.Reset();
    for (auto& r : vol_blurred_) r.Reset();
    vol_rtv_heap_.Reset();
    vw_ = vh_ = 0;
    atm_ = {};
    atm_notes_.clear();
    ao_.Reset();
    linear_depth_.Reset();
    final_combine_.release(gpu);
    release_post(gpu);
    release_sky_ambient(gpu);
    release_froxels(gpu);
    release_ssr(gpu);
    release_light_grid(gpu);
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
    cubemaps_.clear();
    for (auto& cp : cubemap_pipelines_) cp->release(gpu);
    cubemap_pipelines_.clear();
    if (cubemap_stencil_) cubemap_stencil_->release(gpu);
    cubemap_stencil_.reset();
    cubemap_variants_.clear();
    probe_textures_.clear();
    probe_shapes_.clear();
    filtered_cubes_.clear();
    if (cube_filter_) cube_filter_->release(gpu);
    cube_filter_.reset();
    hdr_.Reset();
    rtv_heap_.Reset();
    w_ = h_ = 0;
}

void GameLighting::init(PackageManager& pm, Gpu& gpu) {
    globals_ = load_render_globals(pm);
    const auto registry = load_world_channel_registry(pm);
    if (const auto it = registry.slots.find(kWorldDayNightState); it != registry.slots.end())
        day_night_slot_ = int(it->second);
    if (const auto it = registry.slots.find(fnv1("time_of_day")); it != registry.slots.end())
        time_of_day_slot_ = int(it->second);
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
    if (const auto hdao = globals_.pipeline("hdao"); !hdao.is_none()) {
        try {
            hdao_.load(pm, gpu, hdao, scopes, {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("hdao (SSAO) not loaded: {}", ex.what()));
        }
    }
    // apply_ssao_to_light_buffers: 1 + (ao - 1) * strength into both light buffers, multiplied in
    // (engine blend state 3, dest * src).
    if (const auto apply = globals_.pipeline("apply_ssao_to_light_buffers"); !apply.is_none()) {
        try {
            GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, false, {DXGI_FORMAT_R16G16B16A16_FLOAT}};
            opt.blend_state = 3;
            apply_ssao_.load(pm, gpu, apply, scopes, opt);
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("apply_ssao_to_light_buffers not loaded: {}", ex.what()));
        }
    }
    // Atmosphere: the sky lookups (one full-screen pass each, into RGBA16F) and the sky background,
    // drawn where the depth buffer is still clear (depth test against the read-only depth).
    auto load_sky = [&](GamePipeline& gp, const char* name, const GamePipeline::Options& opt) {
        const auto tag = globals_.pipeline(name);
        try {
            if (tag.is_none()) throw std::runtime_error("not in render globals");
            gp.load(pm, gpu, tag, scopes, opt);
        } catch (const std::exception& ex) {
            shading_.notes.push_back(std::format("{} not loaded: {}", name, ex.what()));
        }
    };
    rigid_scope_ = load_scope(pm, globals_.scope("rigid_model"));
    transparent_scope_ = load_scope(pm, globals_.scope("transparent"));
    transparent_adv_scope_ = load_scope(pm, globals_.scope("transparent_advanced"));
    // Volumetrics: the quarter-resolution min / max depth (x = max, y = min view depth, z = mean,
    // w = where the mean lies between them), the blur (far, near) and the upres, added to the HDR.
    load_sky(vol_downsample_, "downsample_full_depth_to_quarter_max_min", {DXGI_FORMAT_R32G32B32A32_FLOAT, false});
    load_sky(vol_blur_, "volumetrics_blur_results", {DXGI_FORMAT_R16G16B16A16_FLOAT, false, {DXGI_FORMAT_R16G16B16A16_FLOAT}});
    load_sky(vol_upres_, "volumetrics_upres_1", {DXGI_FORMAT_R16G16B16A16_FLOAT, true});
    load_sky(atmo_density_, "atmo_depth_angle_density_lookup_generate", {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
    load_sky(sky_near_, "sky_lookup_generate_near", {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
    load_sky(sky_far_, "sky_lookup_generate_far", {DXGI_FORMAT_R16G16B16A16_FLOAT, false});
    {
        GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, false,
            {GBuffer::kFormats[3], GBuffer::kFormats[3], GBuffer::kFormats[3], GBuffer::kFormats[3]}};
        opt.write_masks = {0xF, 0, 0, 0, 0xF};
        opt.depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        load_sky(sky_, "sky", opt);
    }
    auto lookup = [&](TagHash tag) {
        if (tag.is_none()) throw std::runtime_error("Missing render_globals BRDF lookup");
        return gpu.texture(load_texture(pm, tag));
    };
    tint_lookup_ = lookup(globals_.specular_tint_lookup);
    lobe_lookup_ = lookup(globals_.specular_lobe_lookup);
    lobe3d_lookup_ = lookup(globals_.specular_lobe_3d_lookup);
    iridescence_lookup_ = lookup(globals_.iridescence_lookup);
    atmosphere_noise_ = lookup(globals_.atmosphere_noise);
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
        auto lvs = compile_viewer_env("vs_main", "vs_5_0", kLinearDepthHlsl);
        auto lps = compile_viewer_env("ps_main", "ps_5_0", kLinearDepthHlsl);
        pd.pRootSignature = env_rs_.Get();
        pd.VS = {lvs->GetBufferPointer(), lvs->GetBufferSize()};
        pd.PS = {lps->GetBufferPointer(), lps->GetBufferSize()};
        pd.RTVFormats[0] = DXGI_FORMAT_R32G32_FLOAT;
        check_hr(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&linear_depth_pso_)), "linear depth PSO");
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 10};
    check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)), "game HDR RTV heap");
    load_post(pm, gpu, scopes);
    load_sky_ambient(pm, gpu, scopes);
    load_froxels(pm, gpu, scopes);
    load_ssr(pm, gpu, scopes);
    load_light_grid(pm, gpu, scopes);
    gpu.flush();
}

void GameLighting::resize(Gpu& gpu, uint32_t w, uint32_t h) {
    // Conditional extern bindings may still reference the targets about to be released. Own
    // this invalidation here so headless callers and GUI callers have the same resize contract.
    auto clear_owned = [&](uint8_t id, uint32_t offset, ID3D12Resource* old) {
        if (old && ext_.texture(id, offset).resource == old) ext_.set_texture(id, offset, nullptr);
    };
    clear_owned(ext::Deferred, 0xC0, light_diffuse_.Get());
    clear_owned(ext::Deferred, 0xC8, light_specular_.Get());
    clear_owned(ext::Deferred, 0xD0, env_.Get());
    for (uint32_t offset : {0u, 8u, 0x10u}) clear_owned(ext::ShadowMask, offset, shadow_mask_.Get());
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_R16G16B16A16_FLOAT, {0, 0, 0, 0}};
    hdr_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    gpu.device()->CreateRenderTargetView(hdr_.Get(), nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    // Also a UAV: ssr_apply_reflection blends the reflections into it.
    env_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
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
    D3D12_CLEAR_VALUE ov{DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 1, 1, 1}};
    ao_ = gpu.texture2d(w, h, ov.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ov);
    gpu.device()->CreateRenderTargetView(ao_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 5 * inc});
    D3D12_CLEAR_VALUE lv{DXGI_FORMAT_R32G32_FLOAT, {0, 0, 0, 0}};
    linear_depth_ = gpu.texture2d(w, h, lv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &lv);
    gpu.device()->CreateRenderTargetView(linear_depth_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 6 * inc});
    {
        vw_ = std::max(1u, (w + 3) / 4);
        vh_ = std::max(1u, (h + 3) / 4);
        D3D12_DESCRIPTOR_HEAP_DESC vd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 7};
        check_hr(gpu.device()->CreateDescriptorHeap(&vd, IID_PPV_ARGS(&vol_rtv_heap_)), "volumetrics RTV heap");
        const auto vb = vol_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_CLEAR_VALUE mm{DXGI_FORMAT_R32G32B32A32_FLOAT, {0, 0, 0, 0}};
        vol_minmax_ = gpu.texture2d(vw_, vh_, mm.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &mm);
        gpu.device()->CreateRenderTargetView(vol_minmax_.Get(), nullptr, vb);
        for (int i = 0; i < 6; ++i) {
            auto& r = i < 4 ? vol_layer_[i] : vol_blurred_[i - 4];
            r = gpu.texture2d(vw_, vh_, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
            gpu.device()->CreateRenderTargetView(r.Get(), nullptr, {vb.ptr + size_t(1 + i) * inc});
        }
    }
    sky_near_rt_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    sky_far_rt_ = gpu.texture2d(w, h, cv.Format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    gpu.device()->CreateRenderTargetView(sky_near_rt_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 7 * inc});
    gpu.device()->CreateRenderTargetView(sky_far_rt_.Get(), nullptr, {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 8 * inc});
    atmo_density_rt_ = gpu.texture2d(kAtmoDensitySize, kAtmoDensitySize, cv.Format,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv);
    gpu.device()->CreateRenderTargetView(atmo_density_rt_.Get(), nullptr,
        {rtv_heap_->GetCPUDescriptorHandleForHeapStart().ptr + 9 * inc});
    w_ = w;
    h_ = h;
    resize_post(gpu, w, h);
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
    // .x / .y: Lambert weight of opaque / translucent surfaces in the deferred light PS (lerp by the
    // G-buffer translucency; zero turns local-light diffuse off), .z: its wrap floor; .w scales two
    // shading-pass terms. Engine values unknown: full Lambert, no wrap, full scale.
    if (!ext.has(ext::Frame, 0x240, 16)) ext.estimate(ext::Frame, 0x240, float4{1, 1, 0, 1}, "deferred diffuse weights (meaning read from the light PS, value chosen)");
    if (!ext.zero_policy(ext::Frame)) ext.zero_when_missing(ext::Frame, "engine frame value with no known meaning: zero");
    // Constants a caller supplied are kept; the estimate follows time and exposure every frame.
    if (ext.has_frame_constants() && !ext.frame_constants_estimated()) return;
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

namespace {
M4 inverse(const M4& m);
}

void fill_view_externs(ExternStore& ext, uint32_t width, uint32_t height, const ViewMatrices& vm) {
    const float w = float(width), h = float(height);

    // The game uses a backward camera Z axis (the forward distance in transparent PS is
    // dot(-cb12[6], world - eye)). Change both sides of the camera-space transforms so
    // a view ray transformed to world space stays aligned with the viewer's projection.
    M4 cam_to_world;
    cam_to_world.c[0] = v4(vm.right, 0);
    cam_to_world.c[1] = v4(vm.up, 0);
    cam_to_world.c[2] = v4(vm.fwd * -1.0f, 0);
    cam_to_world.c[3] = v4(vm.eye, 1);
    M4 world_to_cam;
    world_to_cam.c[0] = {vm.right.x, vm.up.x, -vm.fwd.x, 0};
    world_to_cam.c[1] = {vm.right.y, vm.up.y, -vm.fwd.y, 0};
    world_to_cam.c[2] = {vm.right.z, vm.up.z, -vm.fwd.z, 0};
    world_to_cam.c[3] = {-dot(vm.right, vm.eye), -dot(vm.up, vm.eye), dot(vm.fwd, vm.eye), 1};
    M4 cam_to_proj = vm.proj;
    for (float& v : cam_to_proj.c[2]) v = -v;
    // A perspective view's jitter (NDC offset, temporal anti-aliasing) is taken out of the rays.
    const float jx = vm.ortho > 0 ? 0 : vm.proj.c[2][0], jy = vm.ortho > 0 ? 0 : vm.proj.c[2][1];
    M4 proj_to_ray;  // (ndc.x, ndc.y, any, 1) -> camera-space ray with z = -1, w = 0
    proj_to_ray.c[0] = {1 / vm.fx, 0, 0, 0};
    proj_to_ray.c[1] = {0, 1 / vm.fy, 0, 0};
    proj_to_ray.c[2] = {0, 0, 0, 0};
    proj_to_ray.c[3] = {-jx / vm.fx, -jy / vm.fy, -1, 0};
    M4 pixel_to_ray;
    pixel_to_ray.c[0] = {2 / (w * vm.fx), 0, 0, 0};
    pixel_to_ray.c[1] = {0, -2 / (h * vm.fy), 0, 0};
    pixel_to_ray.c[2] = {0, 0, 0, 0};
    pixel_to_ray.c[3] = {(-1 - jx) / vm.fx, (1 - jy) / vm.fy, -1, 0};
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
    // sky PS reconstructs a point at device depth zero through +0x1E0. The infinite
    // projection's inverse gives w=0 there, so use a finite 50km sky point to avoid 0/0
    // in its motion-vector reprojection (the shader uses a finite engine far plane).
    M4 sky_clip_to_world = inverse(vm.vp_abs);
    const float sky_depth = vm.ortho > 0 ? 0 : vm.vp_rel.c[3][2] / 50000.0f;
    for (int a = 0; a < 4; ++a) sky_clip_to_world.c[3][a] += sky_clip_to_world.c[2][a] * sky_depth;
    ext.estimate(ext::View, 0x1E0, sky_clip_to_world, "sky motion reprojection at 50km (engine far distance unknown)");
    // +0x3E0: the previous frame's world -> clip. The G-buffer PS (and sky) write the motion vector
    // 0.5 * (ndc(+0x3E0) - ndc(+0x1A0)), y flipped, into RT3 for resolve_taa.
    ext.set(ext::View, 0x3E0, vm.vp_prev);
    // +0x2A0 (goriath unk220): target uv (0..1, y down) -> world, the projective inverse after
    // uv -> ndc; cubemap_apply_sky_copy_ao takes its view vector as cb12[7] (eye) minus it.
    M4 uv_to_ndc;
    uv_to_ndc.c[0] = {2, 0, 0, 0};
    uv_to_ndc.c[1] = {0, -2, 0, 0};
    uv_to_ndc.c[2] = {0, 0, 1, 0};
    uv_to_ndc.c[3] = {-1, 1, 0, 1};
    ext.set(ext::View, 0x2A0, mul(inverse(vm.vp_abs), uv_to_ndc));
    // Local lights and their shaft shaders reconstruct camera-relative positions from
    // (pixel x, pixel y, device depth, 1). Publish the matrix for this viewport.
    M4 pixel_to_ndc = uv_to_ndc;
    pixel_to_ndc.c[0][0] /= w;
    pixel_to_ndc.c[1][1] /= h;
    ext.set(ext::View, 0x2E0, mul(inverse(vm.vp_rel), pixel_to_ndc));
}

V3 sun_direction(const LightingParams& p) {
    return normalize(V3{std::cos(p.sun_azimuth) * std::cos(p.sun_elevation), std::sin(p.sun_azimuth) * std::cos(p.sun_elevation),
                        std::sin(p.sun_elevation)});
}

void GameLighting::fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p, bool shadowed) {
    fill_view_externs(ext_, gb.width(), gb.height(), vm);
    ext_.set(ext::Deferred, 0x00, float4{0, 1 / vm.vp_rel.c[3][2], 0, 0});
    ext_.set(ext::Deferred, 0x40, float4{float(gb.width()), float(gb.height()), 0, 0});
    ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), GBuffer::kDepthSrvFormat);
    ext_.set_texture(ext::Deferred, 0xA8, gb.rt[0].Get());
    ext_.set_texture(ext::Deferred, 0xB0, gb.rt[1].Get());
    ext_.set_texture(ext::Deferred, 0xB8, gb.rt[2].Get());
    ext_.set(ext::Frame, 0x00, p.time);
    ext_.set(ext::Frame, 0x04, p.time);
    ext_.set(ext::Frame, 0x14, p.delta_time);
    // exposure_scale: the auto exposure (post) times the viewer's compensation.
    ext_.set(ext::Frame, 0x1C, exposure_);
    estimate_engine_frame(ext_, p.time, p.delta_time);
    // +0x10: the time of day in days (goriath: time_of_day / 3600 s); the shading fog reads the
    // density LUT's time column with it.
    ext_.set(ext::Frame, 0x10, time_of_day(p));
    if (!atm_.valid)
        ext_.note_estimate("extern 1 +0x10 = 0.5", "time of day without a map: noon");
    fill_atmosphere(p);
    if (!froxels_on(p) || !froxel_vol_)
        ext_.estimate_texture(ext::Atmosphere, 0x108, no_atmosphere_.Get(),
                              "atmosphere froxel volume disabled or not generated yet: transmittance 1, in-scatter 0");
    for (uint8_t id : {ext::Atmosphere, uint8_t(80), uint8_t(81), uint8_t(82)})
        if (!ext_.zero_policy(id)) ext_.zero_when_missing(id, "world state from a map (not loaded): zero");
    if (!lights_.empty() || !cubemaps_.empty() || sky_ambient_on(p)) {
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
    // Extern 93 +0x00: the ambient occlusion every cubemap_apply pipeline reads (hdao output).
    if (p.ssao && hdao_.loaded()) ext_.set_texture(93, 0x00, ao_.Get());
    else ext_.estimate_texture(93, 0x00, unshadowed_.Get(), p.ssao ? "SSAO pipeline not loaded: unoccluded (white)" : "SSAO off: unoccluded (white)");
    if (!ext_.zero_policy(ext::VolumeFog)) ext_.zero_when_missing(ext::VolumeFog, "no volumetric fog pass yet: zero");
    // Marathon's original consumers differ from the reference game's frame layout:
    // uber light C0 is Texture3D; shading D0 is the thin-film/iridescence table;
    // atmosphere D8 is Texture3D noise. Never bind the D2 table shifted by 8 bytes.
    ext_.set_texture(ext::Frame, 0xB8, lobe_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xC0, lobe3d_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xC8, tint_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD0, iridescence_lookup_.Get());
    ext_.set_texture(ext::Frame, 0xD8, atmosphere_noise_.Get());
    ext_.note_estimate("Frame BRDF textures: B8 lobe, C0 3D lobe, C8 tint, D0 iridescence",
                       "Marathon shader C0 requires Texture3D and D0 reads thin-film colour; reference game's offsets differ; unused B8/C8 roles inferred");
    ext_.note_estimate("Frame +0xD8 = render globals +0x40 3D scalar noise",
                       "original atmosphere noise CS requires Texture3D; matching scalar 48^3 game resource, engine binding code unavailable");

    // GlobalLighting: a map's sun and hemisphere ambient. Without a map it is nim's light set-up
    // (the panel's sun / sky), laid out as the shading reads it: the sun is multiplied by
    // 1/(4 pi) and the ambient by 1/4 there, so nim's values are scaled to match.
    const V3 sun_dir = sun_toward(p);
    constexpr std::string_view chosen = "meaning reverse-engineered, value chosen (v0.3.1)";
    ext_.estimate(ext::GlobalLighting, 0x50, float4{0, 0, 1, 0}, chosen);  // hemisphere axis
    ext_.estimate(ext::GlobalLighting, 0xD0, float4{1, 1, 0, 0}, chosen);  // light-buffer occlusion
    // The global channels were reset this frame (update_global_channels).
    if (atm_.valid && p.atmosphere) {
        // A map: the sun and the hemisphere ambient are the global channels (sun_color x
        // sun_intensity, up / down_ambient_color x intensity, their sharpness), in the order the
        // shading reads GlobalLighting; the ambient is then scaled by global_ambient_intensity
        // (ch 0x82) in the shading pass, and the map's diffuse probes (cubemap volumes) do the rest.
        auto ch = [&](uint32_t i) { return i < ext_.channels.size() ? ext_.channels[i] : float4{}; };
        auto scaled = [](float4 c, float k) { return float4{c[0] * k, c[1] * k, c[2] * k, 0}; };
        constexpr std::string_view from_channels = "global channels copied into GlobalLighting (order matches; engine copy code not data)";
        ext_.estimate(ext::GlobalLighting, 0x10, scaled(ch(0x09), ch(0x08)[0]), from_channels);  // sun_color x sun_intensity
        ext_.estimate(ext::GlobalLighting, 0x30, v4(sun_dir, 0), "map sun path at the time of day");
        ext_.estimate(ext::GlobalLighting, 0x70, scaled(ch(0x57), ch(0x58)[0]), from_channels);  // up_ambient
        ext_.estimate(ext::GlobalLighting, 0x80, scaled(ch(0x5A), ch(0x5B)[0]), from_channels);  // down_ambient
        ext_.estimate(ext::GlobalLighting, 0x90, float4{ch(0x59)[0], ch(0x5C)[0], 0, 0}, from_channels);  // sharpness
        if (!ext_.zero_policy(ext::GlobalLighting))
            ext_.zero_when_missing(ext::GlobalLighting, "global lighting value with no known meaning: zero");
        return;
    }
    constexpr std::string_view viewer_light = "viewer light set-up (no map lighting loaded)";
    const V3 sun = p.sun_color * (p.sun_intensity * 4.0f);
    const float ka = p.entity_preview ? 0.0f : 4.0f * p.sky_intensity;
    ext_.estimate(ext::GlobalLighting, 0x10, v4(sun, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x30, v4(sun_dir, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x70, v4(p.sky_top * ka, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x80, v4(p.ground * ka, 0), viewer_light);
    ext_.estimate(ext::GlobalLighting, 0x90, float4{0.35f, 0.35f, 0, 0}, chosen);  // up / down sharpness
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

const std::vector<float4>& GameLighting::update_global_channels(const LightingParams& p) {
    ext_.channels.assign(globals_.channel_defaults.begin(), globals_.channel_defaults.end());
    env_channels_ = 0;
    if (p.assume_lights_on && !light_state_.empty()) {
        std::string list;
        for (const auto& [slot, v] : light_state_) {
            if (slot >= ext_.channels.size()) continue;
            ext_.channels[slot] = {v, v, v, v};
            list += std::format("{}{:02X} ({:08X}) = {:g}", list.empty() ? "" : ", ", slot,
                                slot < ext_.channel_ids.size() ? ext_.channel_ids[slot] : 0u, v);
        }
        ext_.note_estimate("light state channels: " + list,
                           "render-global channels the map's lights read that default to 0 and no map data sets (script state); "
                           "each set to the one of 0 / 1 under which the map's lights give more light; map environment expressions still override");
    }
    if (atm_.valid && p.atmosphere && p.environment && !atm_.data.channel_expressions.empty()) {
        // The sequencer inputs (docs/PROGRESS.md 3.6.1): the sequence component's slot table
        // names them. A sequence parameter (input 0) is the time of day; the others are world
        // global channels (rain_amount, sky_transition_weather, fog_amount), whose values the
        // active script state sets.
        const float t = p.time_of_day >= 0 ? p.time_of_day : atm_.data.default_time;
        const std::vector<SequencerInput>* current = nullptr;
        static const std::vector<SequencerInput> none;
        TfxEnvironment env;
        env.sequencer_input = [&](uint8_t i) -> float4 {
            const auto& inputs = current ? *current : none;
            if (inputs.empty()) {
                // No slot table in this map's data: the layout seen everywhere else.
                ext_.note_estimate("map environment sequencer inputs: 0 = time of day, others 0", "sequence slot table not found in this map");
                const float v = i == 0 ? t : 0;
                return {v, v, v, v};
            }
            if (i >= inputs.size()) throw std::runtime_error(std::format("sequencer input {} is not in the sequence's slot table", i));
            const auto& in = inputs[i];
            if (!in.world()) {
                if (i != 0) throw std::runtime_error(std::format("sequence parameter {:08X} (input {}) has no known source", in.name, i));
                return {t, t, t, t};
            }
            float v = 0;
            if (const auto it = in.world_slot >= 0 ? global_overrides_.find(uint32_t(in.world_slot)) : global_overrides_.end(); it != global_overrides_.end())
                v = it->second;
            else
                ext_.note_estimate(std::format("map environment input {} (world channel {:08X}, slot {}) = 0", i, in.name, in.world_slot),
                                   "no active script state supplied for this world channel");
            return {v, v, v, v};
        };
        for (const auto& x : atm_.data.channel_expressions) {
            const auto it = std::find(ext_.channel_ids.begin(), ext_.channel_ids.end(), x.channel);
            if (it == ext_.channel_ids.end()) continue;  // not a render-globals channel (object / unnamed)
            const size_t slot = size_t(it - ext_.channel_ids.begin());
            if (slot >= ext_.channels.size()) continue;
            current = x.inputs.get();
            run_tfx(x.bytecode, x.constants, std::span(ext_.channels).subspan(slot), env);
            ++env_channels_;
        }
    }
    ext_.world_channels = global_overrides_;
    if (atm_.valid) {
        if (time_of_day_slot_ >= 0)
            ext_.world_channels.try_emplace(uint32_t(time_of_day_slot_), time_of_day(p));
        if (p.atmosphere) ext_.set_channel("sun_atmosphere_direction", v4(sun_toward(p), 0));
    }
    // day_night_state (world channel A12171A8): 217 of Outpost's lights, eight of its shadowing key
    // lights among them, scale their colour by it (colour x 19 x state). No script of the map sets it
    // and the engine's rule is not in game data: 1 while the map's sun is below the horizon, else 0.
    if (day_night_slot_ >= 0 && atm_.valid && p.atmosphere && !ext_.world_channels.contains(uint32_t(day_night_slot_))) {
        const float night = p.day_night_state >= 0 ? p.day_night_state : sun_toward(p).z < 0 ? 1.0f : 0.0f;
        ext_.world_channels[uint32_t(day_night_slot_)] = night;
        ext_.note_estimate(std::format("world channel day_night_state = {:g}", night),
                           p.day_night_state >= 0 ? "set by the viewer" : "no script state supplies it; 1 while the map's sun is below the horizon");
    }
    return ext_.channels;
}

void GameLighting::set_global_channels(const std::map<uint32_t, float>& values) {
    global_overrides_ = values;
    ext_.world_channels = values;
}

void GameLighting::render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                          const LightingParams& p, const TransparentFn& transparents, SunShadow* shadow, bool needs_light_grid) {
    if (!ready()) throw std::runtime_error("Original game lighting is unavailable");
    if (p.entity_preview)
        ext_.note_estimate("entity preview: orbit_goliath_013b room probe near the origin",
                           "the isolated asset does not store a location; it is previewed in the authored interior probe");
    if (gb.width() != w_ || gb.height() != h_) resize(gpu, gb.width(), gb.height());
    const bool shadowed = shadow && shadow->valid && shadow->depth;
    ProfileScope ps_setup("frame.light.setup");
    update_global_channels(p);
    update_exposure(gpu, p);
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
    ps_setup.stop();
    ext_.set_texture(ext::Postprocess, 0x00, hdr_.Get());
    const bool atmosphere = atm_.valid && p.atmosphere;
    shading_.validate(ext_, scopes);
    const bool post = p.post && post_tonemap_.loaded();
    if (!post) final_combine_.validate(ext_, scopes);
    if (atmosphere) sky_.validate(ext_, scopes);
    // The complete pass graph (atmosphere, lighting volumes, exposure, grading and temporal
    // histories) is not yet decoded. Its outputs must be supplied by their real producers.
    // Transparent parts (render stage 8: the sky objects, glass ...) are drawn over the sky into the
    // HDR target, before the light shafts; SV_Target4 updates the G-buffer motion texture for TAA.
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
    sd.Format = GBuffer::kDepthSrvFormat;
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
    ec.bg_top = p.entity_preview ? V3{0.20f, 0.20f, 0.20f} : p.background_top;
    ec.bg_bottom = p.entity_preview ? V3{0.11f, 0.11f, 0.11f} : p.background_bottom;
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
        dsd.Format = GBuffer::kDepthSrvFormat;
        gpu.device()->CreateShaderResourceView(gb.depth.Get(), &dsd, gpu.srv().cpu(env_srv_ + 2));
        dsd.Format = DXGI_FORMAT_R32_FLOAT;
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
    if (atmosphere) {
        ProfileScope ps_("frame.light.atmosphere");
        render_sky_lookups(gpu, cl, scopes);
        render_atmosphere_density(gpu, cl);
        render_froxels(gpu, cl, vm, p, shadow);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
    }
    // Consumed inside the callback, after resize and this frame's producers.
    ext_.set_texture(ext::Transparent, 0x00, atmosphere ? sky_near_rt_.Get() : nullptr);
    ext_.set_texture(ext::Transparent, 0x10, atmosphere ? sky_far_rt_.Get() : nullptr);
    ext_.set_texture(ext::Transparent, 0x20, atmosphere ? atmo_density_rt_.Get() : nullptr);
    // The transparent and shaft PS use the same depth-to-Z expression as opaque shading.
    // Supply both its texture and the coefficients from the same froxel producer.
    ext_.set(ext::Transparent, 0xD0, atmosphere ? ext_.get_v(ext::Atmosphere, 0x130) : float4{0, 0, 1, 0});
    if (!froxels_on(p))
        ext_.estimate_texture(ext::Transparent, 0x78, no_atmosphere_.Get(), "atmosphere froxels disabled: clear air (1,1,0,0)");
    ext_.zero_when_missing(ext::Transparent, "transparent light shaft slices / engine constants not reconstructed: zero");
    grid_ready_ = false;
    for (uint32_t off : {0x28u, 0x30u, 0x38u, 0x40u}) ext_.set_texture(ext::Transparent, off, nullptr);
    ext_.set_texture(ext::Transparent, 0xE0, nullptr);  // engine-bound reflection cube of an isolated preview
    // The sky ambient writes the specular IBL buffer in place of the viewer environment.
    const bool sky_ambient = own_env && sky_ambient_on(p);
    if (sky_ambient) {
        render_sky_hemisphere(gpu, cl);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
    }
    const bool volumetrics = p.volumetrics && !vol_lights_.empty() && vol_downsample_.loaded() && vol_blur_.loaded() && vol_upres_.loaded();
    if ((p.ssao && hdao_.loaded()) || volumetrics || transparents) render_linear_depth(gpu, cl, gb, vm);
    if (p.ssao && hdao_.loaded()) { ProfileScope ps_("frame.light.ssao"); render_ssao(gpu, cl, gb, vm, scopes); }
    lights_drawn_ = cubemaps_drawn_ = cubemaps_failed_ = 0;
    vol_drawn_ = 0;
    if (own_env) {
        Gpu::barrier(cl, env_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (p.entity_preview) cl->ClearRenderTargetView(env_rtv, clear, 0, nullptr);
        else if (!sky_ambient) env_pass(env_ibl_pso_.Get(), env_rtv);
    }
    if (!lights_.empty() || !cubemaps_.empty() || sky_ambient) {
        // Light buffers: first the cubemap volumes (diffuse probes + specular IBL over the sky's).
        // Their PS writes alpha = 1 - volume weight for a src + dst * src.a blend, so a volume
        // replaces what is under it (nested volumes over the large ones): drawn after the local
        // lights it would erase them. Then the local lights add on top, and
        // apply_ssao_to_light_buffers multiplies both buffers.
        Gpu::barrier(cl, light_diffuse_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        Gpu::barrier(cl, light_specular_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const float zero[4]{};
        cl->ClearRenderTargetView({hdr.ptr + 3 * inc}, zero, 0, nullptr);
        cl->ClearRenderTargetView({hdr.ptr + 4 * inc}, zero, 0, nullptr);
        if (sky_ambient) {
            // The sky everywhere first (diffuse light buffer, specular IBL); the cubemap volumes
            // then replace it where they are.
            const bool overhead = overhead_occlusion_on(p);
            if (overhead) prepare_overhead_occlusion(gpu, cl);
            const D3D12_CPU_DESCRIPTOR_HANDLE bufs[] = {{hdr.ptr + 3 * inc}, env_rtv};
            cl->OMSetRenderTargets(2, bufs, FALSE, nullptr);
            (overhead ? sky_apply_overhead_ : sky_apply_).draw(gpu, cl, ext_, scopes);
        }
        if (!cubemaps_.empty() && own_env) {
            { ProfileScope ps_("frame.light.cubemaps"); draw_cubemaps(gpu, cl, gb, vm, scopes, p.probe_distance_fade); }
            ext_.note_estimate("light buffers: cubemap volumes, then local lights, then the SSAO multiply (the probes also read the AO)",
                               "engine pass order not in game data; the cubemap alpha replaces what is under it");
        }
        if (!lights_.empty()) { ProfileScope ps_("frame.light.draw_lights"); draw_lights(gpu, cl, gb, vm, scopes); }
        if (p.ssao && hdao_.loaded() && apply_ssao_.loaded()) {
            constexpr uint8_t kApply = 0x26;
            // Extern 38 is the full-screen input shared with final_combine: its own store here.
            hdao_ext_.set_texture(kApply, 0x00, ao_.Get());
            hdao_ext_.set(kApply, 0x50, float4{float(w_), float(h_), 1 / float(w_), 1 / float(h_)});  // cb0[4].zw: pixel to uv
            hdao_ext_.set(kApply, 0xC0, float4{1, 1, 1, 1});  // cb0[2].x: strength
            ext_.note_estimate("apply_ssao_to_light_buffers strength 1 (extern 38 +0xC0)", "engine-side SSAO setting (not in game data)");
            const D3D12_CPU_DESCRIPTOR_HANDLE bufs[] = {{hdr.ptr + 3 * inc}, {hdr.ptr + 4 * inc}};
            cl->OMSetRenderTargets(2, bufs, FALSE, nullptr);
            apply_ssao_.draw(gpu, cl, hdao_ext_, scopes);
        }
        Gpu::barrier(cl, light_diffuse_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Gpu::barrier(cl, light_specular_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    const auto* preview_probe = p.entity_preview && !cubemaps_.empty() ? &cubemaps_.front() : nullptr;
    if (transparents && (sky_ambient || preview_probe) && needs_light_grid) {
        try {
            ProfileScope ps_("frame.light.grid");
            render_light_grid(gpu, cl, vm, scopes, sky_ambient, overhead_occlusion_on(p), preview_probe, shadow);
        } catch (const std::exception& ex) {
            ext_.note_estimate("transparent light grid is unavailable", ex.what());
        }
    }
    // Screen-space reflections replace the specular IBL where a ray hits what is on screen.
    ssr_drawn_ = false;
    ProfileScope ps_ssr("frame.light.ssr");
    if (own_env && render_ssr(gpu, cl, gb, vm, p)) {
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
    }
    ps_ssr.stop();
    if (own_env) Gpu::barrier(cl, env_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    cl->OMSetRenderTargets(1, &hdr, FALSE, nullptr);
    cl->SetGraphicsRootSignature(nullptr);
    { ProfileScope ps_("frame.light.shading"); shading_.draw(gpu, cl, ext_, scopes, true); }
    if (atmosphere) {
        // The sky pipeline puts its triangle at the view's far depth (cb12[13].x): with the depth
        // test it covers only the pixels no surface was drawn to.
        // Map sky SV_Target4 to RT3: resolve_taa needs sky motion when the camera turns.
        Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {hdr, gb.null_rtv, gb.null_rtv, gb.null_rtv, gb.rtv[3]};
        cl->OMSetRenderTargets(5, targets, FALSE, &gb.dsv_ro);
        sky_.draw(gpu, cl, ext_, scopes, true);
        Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    } else {
        env_pass(env_bg_pso_.Get(), hdr);
        ext_.note_estimate("background gradient", "viewer environment (no map sky)");
    }
    if (transparents) {
        const float4 depth_decode = ext_.get_v(ext::Deferred, 0x00);
        // The transparent view's reciprocal-depth flag is clear. Its soft intersections
        // compare cb2[0].y * t10.x + cb2[0].x directly with distance in metres.
        ext_.set(ext::Deferred, 0x00, float4{0, 1, 0, 0});
        ext_.set_texture(ext::Deferred, 0x98, linear_depth_.Get(), DXGI_FORMAT_R32G32_FLOAT);
        { ProfileScope ps_("frame.light.transparents"); transparents(cl, hdr); }
        ext_.set(ext::Deferred, 0x00, depth_decode);
        ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), GBuffer::kDepthSrvFormat);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
    }
    if (volumetrics) { ProfileScope ps_("frame.light.volumetrics"); render_volumetrics(gpu, cl, gb, vm, hdr, scopes); }
    Gpu::barrier(cl, hdr_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (p.ssr && ssr_loaded_) keep_ssr_history(cl, vm);
    else ssr_history_valid_ = false;  // a later frame must not reflect an image this old
    if (post) {
        { ProfileScope ps_("frame.light.post"); render_post(gpu, cl, gb, p, scopes); }
    } else {
        Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &gb.lit_rtv, FALSE, nullptr);
        final_combine_.draw(gpu, cl, ext_, scopes, true);
        Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    for (auto& rt : gb.rt)
        Gpu::barrier(cl, rt.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, gb.depth.Get(), depth_read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

// ---- atmosphere ----------------------------------------------------------------------------------
// See docs/PROGRESS.md 3.2. The map gives the sky lookups (3D: azimuth, elevation, time-of-day key),
// the density LUT and the sun path; the remaining extern 7 values are the global fog / sky
// channels copied in their own order (estimate), the engine code that copies them is not data.
void GameLighting::set_atmosphere(PackageManager& pm, Gpu& gpu, const MapAtmosphere* a) {
    ae_valid_ = false;
    ae_fence_ = {};
    taa_valid_ = false;
    grid_key_.reset();
    if (atm_.valid) gpu.wait_idle();
    hemi_inputs_.reset();
    atm_ = {};
    atm_notes_.clear();
    if (!a) return;
    if (!a->valid()) {
        atm_notes_.push_back("map has no atmosphere component (808084A0)");
        return;
    }
    if (!sky_near_.loaded() || !sky_far_.loaded() || !sky_.loaded() || !atmo_density_.loaded()) {
        atm_notes_.push_back("sky pipelines not loaded");
        return;
    }
    try {
        std::map<uint32_t, ComPtr<ID3D12Resource>> lookups;
        for (size_t k = 0; k < 4; ++k) {
            // A map without the second lookup set reads the first (the sets blend by channel 0x2D).
            const TagHash tag = !a->lookups[k].is_none() ? a->lookups[k] : a->lookups[k & 1];
            if (tag.is_none()) throw std::runtime_error(std::format("sky lookup {} missing", k));
            auto& lookup = lookups[tag.value];
            if (!lookup) lookup = gpu.texture(load_texture(pm, tag));
            atm_.lookups[k] = lookup;
        }
        if (a->density_lut.is_none()) throw std::runtime_error("density LUT missing");
        atm_.density_lut = gpu.texture(load_texture(pm, a->density_lut));
        atm_.data = *a;
        atm_.valid = true;
        if (a->paths[0].empty()) atm_notes_.push_back("no sun path (80808346): the viewer sun direction is used");
    } catch (const std::exception& ex) {
        atm_ = {};
        atm_notes_.push_back(std::format("atmosphere not loaded: {}", ex.what()));
    }
    if (atm_.valid && !a->lens_glare.is_none() && !a->lens_upsample.is_none()) {
        try {
            const Scope* scopes[] = {&frame_scope_, &view_scope_};
            atm_.lens_glare.load(pm, gpu, a->lens_glare, scopes, {DXGI_FORMAT_R16G16B16A16_FLOAT, true});
            atm_.lens_upsample.load(pm, gpu, a->lens_upsample, scopes, {DXGI_FORMAT_R8G8B8A8_UNORM, false});
        } catch (const std::exception& ex) {
            atm_notes_.push_back(std::format("map lens glare not loaded: {}", ex.what()));
        }
    }
    gpu.flush();
}

V3 sun_direction(const LightingParams& p, const MapAtmosphere* a) {
    if (!a || !p.atmosphere || a->paths[0].empty()) return sun_direction(p);
    const auto d = a->path_direction(0, p.time_of_day >= 0 ? p.time_of_day : a->default_time);
    return {d[0], d[1], d[2]};
}

void GameLighting::fill_atmosphere(const LightingParams& p) {
    constexpr uint8_t A = ext::Atmosphere;
    if (!atm_.valid || !p.atmosphere) {
        // Clear air: no fog density, no sky textures (the shading pass then reads zero in-scatter).
        for (uint32_t off : {0x40u, 0x58u, 0x80u, 0xC0u, 0xE0u, 0xF0u})
            if (ext_.texture(A, off).resource) ext_.set_texture(A, off, nullptr);
        for (uint32_t off : {0x174u, 0x1A4u}) ext_.set(A, off, 0.0f);
        return;
    }
    const float t = p.time_of_day >= 0 ? p.time_of_day : atm_.data.default_time;
    if (p.time_of_day < 0)
        ext_.note_estimate(std::format("map time of day = {:.3f}", t), atm_.data.default_time_source);
    auto ch = [&](uint32_t i) { return i < ext_.channels.size() ? ext_.channels[i] : float4{}; };
    constexpr std::string_view copied = "global channel copied into extern 7 (order matches; engine copy code not data)";
    constexpr std::string_view chosen = "meaning read from the sky / shading DXIL, value chosen";
    auto est = [&](uint32_t off, float v, std::string_view why) {
        ext_.set(A, off, v);
        ext_.note_estimate(std::format("extern 7 +0x{:X} = {:g}", off, v), why);
    };
    ext_.set_texture(A, 0x40, atm_.lookups[0].Get());  // near set; render_sky_lookups swaps in far
    ext_.set_texture(A, 0x58, atm_.lookups[2].Get());
    ext_.set(A, 0x70, atm_.data.slice_coordinate(t));
    ext_.set_texture(A, 0x80, atm_.density_lut.Get());
    // t2 of the lookups: a paraboloid map multiplying the sun glow (cloud cover?); none found in
    // the map data. White = no change, and its blend weight (+0x1F8) is 0.
    ext_.estimate_texture(A, 0xC0, unshadowed_.Get(), "sky lookup paraboloid map (source not found): white");
    for (uint32_t off : {0xC0u, 0xD0u, 0xE0u}) {
        const float4 row{off == 0xC0u ? 1.0f : 0.0f, off == 0xD0u ? 1.0f : 0.0f, off == 0xE0u ? 1.0f : 0.0f, 0};
        ext_.set(ext::Postprocess, off, row);
    }
    ext_.note_estimate("extern 38 +0xC0..+0xE0 = identity (sky paraboloid basis)", chosen);
    ext_.set_texture(A, 0xE0, sky_far_rt_.Get());
    ext_.set_texture(A, 0xF0, sky_near_rt_.Get());
    // The lookups' phase is (1 + g^2 + 2 g dot(ray, +0x110))^-1.5: +0x110 points away from the sun.
    const V3 sun = sun_toward(p);
    ext_.estimate(A, 0x110, float4{-sun.x, -sun.y, -sun.z, 0}, "sun light direction (map sun path, time of day)");
    // The original shading PS subtracts .y before applying the analytic distance fog.
    // The volume has already integrated that fog over its depth range: starting the
    // analytic term at zero applies it a second time in front of the volume's end.
    ext_.estimate(A, 0x130, froxels_on(p) ? float4{kFroxelNear, kFroxelFar, kFroxelCurve, 1 / (kFroxelFar - kFroxelNear)} : float4{0, 0, 1, 0},
                  "single estimated froxel depth mapping; analytic fog starts beyond volume (engine cascades unknown)");
    ext_.set(A, 0x150, ch(0x0A));  // sun_glow_color
    ext_.note_estimate("extern 7 +0x150 = sun_glow_color", copied);
    auto glow_g = [](float shape) { return shape > 1 ? 1 - 1 / shape : 0.0f; };
    est(0x160, glow_g(ch(0x0D)[0]), "sky_sun_glow_shape as g = 1 - 1/shape");
    est(0x164, ch(0x0E)[0], copied);
    // +0x170 scales the in-scatter and the sky (cb0[8] / cb0[9] of the shading and sky PS):
    // inscatter_intensity (ch 0x1C, name by FNV-1), which the map environments drive.
    est(0x170, ch(0x1C)[0], "inscatter_intensity: the only in-scatter scale among the channels (extern slot by shader use)");
    est(0x174, ch(0x0F)[0], copied);  // fog_density (per km)
    // fog_density_lookup_start / _end (ch 0x10 / 0x11) and fog_inscatter_lookup_start / _end
    // (ch 0x12 / 0x13, names by FNV-1) are distances in metres: the TFX of the shading pass and
    // of the froxel in-scatter CS use them unscaled against view distances in metres, and the
    // ship interior (Marathon Deck) authors 10 .. 10000, which only reads as metres. The
    // in-scatter CS scales the extinction by saturate((d - start) / (end - start)) and the sun
    // in-scatter by the second pair (docs/PROGRESS.md 3.2.4).
    // The shader divides by the width. Give an undefined range a finite transition
    // when the source supplies no positive width.
    auto range = [&](uint32_t off, float start, float end, std::string_view name) {
        ext_.set(A, off, start);
        if (end > start) {
            ext_.set(A, off + 4, end);
        } else {
            constexpr float width = 1.0f;
            ext_.set(A, off + 4, start + width);
            ext_.note_estimate(std::format("extern 7 +0x{:X} / +0x{:X} = {:g}..{:g} m ({} unset)",
                                           off, off + 4, start, start + width, name),
                               "one metre for an undefined fog transition; engine guard unknown");
        }
    };
    range(0x178, ch(0x10)[0], ch(0x11)[0], "fog_density_lookup_start / _end");
    range(0x180, ch(0x12)[0], ch(0x13)[0], "fog_inscatter_lookup_start / _end");
    ext_.set(A, 0x190, ch(0x16));  // fog_decay_color for transparent spectral attenuation
    ext_.note_estimate("extern 7 +0x190 = fog_decay_color", copied);
    est(0x1A0, ch(0x17)[0], "fog_decay_scale: the channel after fog_decay_color (+0x190)");
    est(0x188, ch(0x15)[0], copied);  // fog_height_falloff
    est(0x18C, ch(0x14)[0], copied);  // fog_start_height
    est(0x1A4, ch(0x18)[0], copied);  // layered_fog_density
    est(0x1A8, ch(0x19)[0], copied);  // layered_fog_falloff
    est(0x1AC, ch(0x1A)[0], copied);  // layered_fog_start_height
    est(0x1C4, ch(0x2E)[0] / 360.0f, "sky_snapshot_rotation in turns");
    est(0x1C8, ch(0x2F)[0], copied);  // sky_snapshot_intensity
    est(0x1D0, 1.0f, chosen);  // sky: weight of the froxel in-scatter
    ext_.set(A, 0x1E0, ch(0x2C));  // sky_color_override (w = weight)
    ext_.note_estimate("extern 7 +0x1E0 = sky_color_override", copied);
    est(0x1F0, glow_g(ch(0x0B)[0]), "sun_glow_shape as g = 1 - 1/shape");
    est(0x1F4, ch(0x0C)[0], copied);  // sun_glow_intensity
    est(0x1F8, 0.0f, chosen);  // paraboloid map weight
}

void GameLighting::render_sky_lookups(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& scopes) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE near_rtv{base.ptr + 7 * inc}, far_rtv{base.ptr + 8 * inc};
    Gpu::barrier(cl, sky_near_rt_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Gpu::barrier(cl, sky_far_rt_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Both passes read their lookup pair at +0x40 / +0x58 (the engine rebinds them in between).
    ext_.set_texture(ext::Atmosphere, 0x40, atm_.lookups[0].Get());
    ext_.set_texture(ext::Atmosphere, 0x58, atm_.lookups[2].Get());
    cl->OMSetRenderTargets(1, &near_rtv, FALSE, nullptr);
    sky_near_.draw(gpu, cl, ext_, scopes);
    ext_.set_texture(ext::Atmosphere, 0x40, atm_.lookups[1].Get());
    ext_.set_texture(ext::Atmosphere, 0x58, atm_.lookups[3].Get());
    cl->OMSetRenderTargets(1, &far_rtv, FALSE, nullptr);
    sky_far_.draw(gpu, cl, ext_, scopes);
    Gpu::barrier(cl, sky_near_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, sky_far_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// PS SV_Position minus 0.5: first texel = distance 0 / elevation -1; last = 50km / +1.
void GameLighting::render_atmosphere_density(Gpu& gpu, ID3D12GraphicsCommandList* cl) {
    if (!atmo_density_.loaded()) throw std::runtime_error("Atmosphere density LUT pipeline is unavailable");
    // Kept between frames: a fresh store allocates and clears 1 MB. Every block it reads is
    // replaced below.
    auto& lut = atmo_lut_;
    for (uint8_t id : {ext::Atmosphere, ext::View, ext::Frame}) lut.copy_block_from(id, ext_);
    M4 identity;
    for (int a = 0; a < 4; ++a) identity.c[a][a] = 1;
    lut.set(ext::View, 0x160, identity);  // fullscreen quad, independent of the perspective camera
    const float step = 1.0f / float(kAtmoDensitySize - 1);
    lut.set(ext::Atmosphere, 0xD0, float4{float(kAtmoDensitySize), float(kAtmoDensitySize), step, step});
    const auto frame = ext_.frame_constants();
    const auto view_vs = evaluate_scope(view_scope_, ScopeStage::Vertex, lut);
    const auto view_ps = evaluate_scope(view_scope_, ScopeStage::Pixel, lut);
    auto upload = [&](const auto& values) {
        const auto a = gpu.dynamic(values.size() * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    const auto cb13 = upload(frame), cb12_vs = upload(view_vs), cb12_ps = upload(view_ps);
    ScopeBufferFn scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 13) return cb13;
        if (reg == 12) return stage == ShaderStage::Vertex ? cb12_vs : cb12_ps;
        return 0;
    };
    atmo_density_.validate(lut, scopes);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const auto inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE target{base.ptr + 9 * inc};
    const D3D12_VIEWPORT vp{0, 0, float(kAtmoDensitySize), float(kAtmoDensitySize), 0, 1};
    const D3D12_RECT sc{0, 0, LONG(kAtmoDensitySize), LONG(kAtmoDensitySize)};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    Gpu::barrier(cl, atmo_density_rt_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->OMSetRenderTargets(1, &target, FALSE, nullptr);
    atmo_density_.draw(gpu, cl, lut, scopes, true);
    Gpu::barrier(cl, atmo_density_rt_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ext_.note_estimate("transparent atmosphere density LUT: 256x256 RGBA16F",
        "original game shader; engine texture resolution and format unknown");
}

// ---- SSAO (hdao) -------------------------------------------------------------------------------
// hdao_ps (the technique's PS; a CS variant does the same): per pixel, 12 sample pairs mirrored
// around it in the second depth input; a pair darkens when both samples are nearer than the
// centre (relative depth, saturate(cb25 / z * z_n + cb26)) and not rejected as too near
// (saturate(cb27 / z * (z_a + z_b) + cb28)); ao = max(1 - fade(z), exp(cb31 * sum^2)).
// Its inputs come from extern 0x21 (goriath "hdao", fields unnamed); what each feeds was read from
// the DXIL. The tuning values are engine code, not data: chosen here and listed as estimates.
// Linear depth (viewer-made input, see kLinearDepthHlsl): hdao t1 and volumetrics_upres_1 t3.
void GameLighting::render_linear_depth(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE lin_rtv{base.ptr + 6 * inc};
    const float near_z = vm.vp_rel.c[3][2];
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        sd.Format = GBuffer::kDepthSrvFormat;
        gpu.device()->CreateShaderResourceView(gb.depth.Get(), &sd, gpu.srv().cpu(env_srv_));
        const float4 k{near_z, kSkyViewDepth, 0, 0};
        const auto cb = gpu.dynamic(sizeof(k));
        std::memcpy(cb.cpu, &k, sizeof(k));
        Gpu::barrier(cl, linear_depth_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->OMSetRenderTargets(1, &lin_rtv, FALSE, nullptr);
        cl->SetGraphicsRootSignature(env_rs_.Get());
        cl->SetPipelineState(linear_depth_pso_.Get());
        ID3D12DescriptorHeap* heaps[] = {gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootConstantBufferView(0, cb.gpu);
        cl->SetGraphicsRootDescriptorTable(1, gpu.srv().gpu(env_srv_));
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
        Gpu::barrier(cl, linear_depth_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void GameLighting::render_ssao(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE ao_rtv{base.ptr + 5 * inc};
    const float near_z = vm.vp_rel.c[3][2];
    auto& e = hdao_ext_;
    constexpr uint8_t kHdao = 0x21;
    const float w = float(w_), h = float(h_);
    e.set_texture(kHdao, 0x50, gb.depth.Get(), GBuffer::kDepthSrvFormat);  // t0: device depth (cb0[3] = its size)
    e.set_texture(kHdao, 0x58, linear_depth_.Get(), DXGI_FORMAT_R32G32_FLOAT);  // t1: view depth, gathered from R and G
    e.set(kHdao, 0x60, float4{0, 0, 1 / w, 1 / h});           // cb0[2].zw: t1 texel size
    e.set(kHdao, 0x70, float4{w, h, 0, 0});                    // cb0[3].xy: t0 size
    e.set(kHdao, 0x80, float4{0, 1 / near_z, 0, 0});           // cb0[4].xy: 1 / z = x + y * d (as Deferred +0x00)
    // Tuning (estimates): sample radius 10 px; a sample 1 % nearer than the centre half occludes
    // (slope 50); samples more than 10 % nearer are rejected; exp(-0.04 * sum^2); fade 60 - 100 m.
    const float radius = 10, slope = 50, reject = 0.1f, falloff = 0.04f;
    e.set(kHdao, 0x20, float4{0, 0, radius, radius});           // cb0[22].zw: pattern scale (t1 texels)
    e.set(kHdao, 0x00, float4{-slope, slope, 0, 0});            // cb0[25], cb0[26]
    e.set(kHdao, 0x10, float4{1 / (2 * reject), -(1 - reject) / reject, 0, 0});  // cb0[27], cb0[28]
    e.set(kHdao, 0x30, float4{0, 0, -falloff, 0});              // cb0[31]
    e.set(kHdao, 0x40, float4{0, 1, -1.0f / 40, 2.5f});        // cb0[33..36]: near / far fade
    ext_.note_estimate("SSAO (hdao) tuning: radius 10 px, slope 50, reject 10 %, exp(-0.04 sum^2), fade 60-100 m",
                       "engine-side hdao settings (not in game data)");
    ext_.note_estimate("SSAO second depth input = view depth made by nim", "the engine's producer of hdao t1 is not identified");
    Gpu::barrier(cl, ao_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl->OMSetRenderTargets(1, &ao_rtv, FALSE, nullptr);
    hdao_.draw(gpu, cl, e, scopes);
    Gpu::barrier(cl, ao_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// ---- Local lights ------------------------------------------------------------------------------
namespace {

M4 m4(const std::array<float, 16>& a) {
    M4 m;
    for (int c = 0; c < 4; ++c) m.c[c] = {a[size_t(c * 4)], a[size_t(c * 4 + 1)], a[size_t(c * 4 + 2)], a[size_t(c * 4 + 3)]};
    return m;
}
// The orientation of a projective map (its Jacobian is det(M) / w^4): the sign of the 4x4 determinant.
float det4(const M4& m) {
    auto d3 = [&](int c0, int c1, int c2, int r0, int r1, int r2) {
        auto e = [&](int c, int r) { return m.c[c][size_t(r)]; };
        return e(c0, r0) * (e(c1, r1) * e(c2, r2) - e(c2, r1) * e(c1, r2)) - e(c1, r0) * (e(c0, r1) * e(c2, r2) - e(c2, r1) * e(c0, r2)) +
               e(c2, r0) * (e(c0, r1) * e(c1, r2) - e(c1, r1) * e(c0, r2));
    };
    return m.c[0][0] * d3(1, 2, 3, 1, 2, 3) - m.c[1][0] * d3(0, 2, 3, 1, 2, 3) + m.c[2][0] * d3(0, 1, 3, 1, 2, 3) - m.c[3][0] * d3(0, 1, 2, 1, 2, 3);
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
// The supporting corner gives the maximum signed distance of an AABB to each clip plane.
struct LightFrustum {
    explicit LightFrustum(const ViewMatrices& vm) : eye{vm.eye.x, vm.eye.y, vm.eye.z} {
        for (size_t c = 0; c < 4; ++c) {
            const auto& col = vm.vp_rel.c[c];
            planes[0][c] = col[3] + col[0];
            planes[1][c] = col[3] - col[0];
            planes[2][c] = col[3] + col[1];
            planes[3][c] = col[3] - col[1];
            planes[4][c] = col[3];
        }
    }
    bool outside(const std::array<float, 3>& lo, const std::array<float, 3>& hi) const {
        for (const auto& p : planes) {
            const float max_distance = p[3] + p[0] * ((p[0] >= 0 ? hi[0] : lo[0]) - eye[0]) +
                p[1] * ((p[1] >= 0 ? hi[1] : lo[1]) - eye[1]) + p[2] * ((p[2] >= 0 ? hi[2] : lo[2]) - eye[2]);
            if (max_distance < -0.001f) return true;
        }
        return false;
    }
    std::array<float, 3> eye;
    std::array<float4, 5> planes;
};

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

void GameLighting::make_cube(Gpu& gpu) {
    if (cube_) return;
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

void GameLighting::clear_lights(Gpu& gpu) {
    // Frames in flight can still read the lights' shadow maps.
    for (const auto& ll : lights_)
        if (ll.shadow) { gpu.wait_idle(); break; }
    lights_.clear();
    vol_lights_.clear();
    light_shadow_items_ = 0;
    ssr_history_valid_ = false;  // another map's image
}

// Light state channels (LightingParams::assume_lights_on): for every render-global channel that
// defaults to 0 and that some light's pixel expression reads, the summed magnitude of those lights'
// evaluated constants with the channel at 0 and at 1; the larger one is taken. Other inputs are
// the render-global defaults, extern and per-object values 1 (as `nimcli lights` evaluates them).
void GameLighting::choose_light_state() {
    light_state_.clear();
    const auto& defaults = globals_.channel_defaults;
    auto zero = [&](size_t c) { return c < defaults.size() && defaults[c] == float4{}; };
    std::unordered_map<const Technique*, size_t> uses;
    for (const auto& ll : lights_)
        if (ll.technique) ++uses[ll.technique.get()];
    std::map<size_t, std::array<double, 2>> totals;
    for (const auto& [tech, count] : uses) {
        const auto& ps = tech->stage(ShaderStage::Pixel);
        std::set<size_t> read;
        int forced = -1;
        float forced_value = 0;
        TfxEnvironment env;
        env.extern_vec4 = [](uint8_t, uint8_t) { return float4{1, 1, 1, 1}; };
        env.extern_float = [](uint8_t, uint8_t) { return 1.0f; };
        env.world_channel = [](uint8_t) { return float4{1, 1, 1, 1}; };
        env.object_channel = [](uint32_t) { return float4{1, 1, 1, 1}; };
        env.global_channel = [&](uint8_t c) {
            read.insert(c);
            if (int(c) == forced) return float4{forced_value, forced_value, forced_value, forced_value};
            return c < defaults.size() ? defaults[c] : float4{};
        };
        auto magnitude = [&]() {
            std::vector<float4> out;
            try {
                evaluate_tfx(ps.bytecode, ps.constants, ps.cbuffer_init, out, env);
            } catch (const std::exception&) { return -1.0; }
            double sum = 0;
            for (const auto& v : out)
                for (float x : v)
                    if (std::isfinite(x)) sum += std::abs(x);
            return sum;
        };
        if (magnitude() < 0) continue;
        const std::set<size_t> first = read;
        for (size_t c : first) {
            if (!zero(c)) continue;
            forced = int(c);
            forced_value = 0;
            const double off = magnitude();
            forced_value = 1;
            const double on = magnitude();
            forced = -1;
            if (off < 0 || on < 0) continue;
            totals[c][0] += off * double(count);
            totals[c][1] += on * double(count);
        }
    }
    for (const auto& [c, t] : totals)
        if (t[1] > t[0] * 1.000001) light_state_[c] = 1;
}

void GameLighting::set_lights(PackageManager& pm, Gpu& gpu, std::span<const MapLight* const> lights) {
    clear_lights(gpu);
    light_notes_.clear();
    make_cube(gpu);
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
                    opt.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    opt.cbuffer_floor = 64;
                    lp->load(pm, gpu, l->technique, scopes, opt);
                    light_pipelines_.push_back(std::move(lp));
                }
                it = keys.emplace(key, index).first;
            }
            LocalLight local{it->second, tech, *l};
            {  // the grid registers (grid_lights)
                const auto& ps = tech->stage(ShaderStage::Pixel);
                for (size_t r = 0; r < ps.cbuffer_init.size(); ++r) {
                    const auto& v = ps.cbuffer_init[r];
                    if (v[0] < 0 && v[1] > 0 && v[2] == 0 && v[3] == 0) {
                        local.grid_falloff = local.grid_falloff < 0 ? int(r) : -2;  // -2: ambiguous
                    }
                }
                if (local.grid_falloff == -2) local.grid_falloff = -1;
                const auto program = disassemble_tfx(ps.bytecode);
                if (auto at = program.find("push_extern_float 04 54"); at != std::string::npos)
                    if (auto pop = program.find("pop_output ", at); pop != std::string::npos)
                        local.grid_weight = std::stoi(program.substr(pop + 11, 2), nullptr, 16);
            }
            const M4 node = m4(l->node), light_space = m4(l->light_space);
            local.world_volume = mul(node, light_space);
            local.world_to_box = inverse(light_box_to_world(node, light_space));
            local.mirrored = det4(local.world_volume) > 0;
            if (l->shadowing && !l->shadow_technique.is_none() && light_shadow_view(*l)) {
                // The +0xD8 technique: its own shaders, the same pipeline sharing.
                try {
                    auto& stech = techniques[l->shadow_technique.value];
                    if (!stech) stech = std::make_shared<const Technique>(load_technique(pm, l->shadow_technique));
                    std::string skey = "S";
                    for (auto which : {ShaderStage::Vertex, ShaderStage::Pixel}) {
                        const auto& st = stech->stage(which);
                        skey += std::format("{:08X}:", st.shader.value);
                        for (const auto& t : st.textures) skey += std::format("t{}={:08X},", t.slot, t.texture.value);
                        for (const auto& sm : st.samplers) skey += std::format("s{:08X},", sm.value);
                        skey += '|';
                    }
                    auto sit = keys.find(skey);
                    if (sit == keys.end()) {
                        const uint32_t index = uint32_t(light_pipelines_.size());
                        for (bool front : {true, false}) {
                            auto lp = std::make_unique<GamePipeline>();
                            GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, true, {DXGI_FORMAT_R16G16B16A16_FLOAT}};
                            opt.volume = true;
                            opt.cull_front = front;
                            opt.depth_func = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                            opt.cbuffer_floor = 64;
                            lp->load(pm, gpu, l->shadow_technique, scopes, opt);
                            light_pipelines_.push_back(std::move(lp));
                        }
                        sit = keys.emplace(skey, index).first;
                    }
                    local.shadow_pipeline = sit->second;
                    local.shadow_technique = stech;
                } catch (const std::exception& ex) {
                    ++failures[std::format("shadowing technique: {}", std::string(ex.what()).substr(0, 100))];
                }
            } else if (l->shadowing) {
                ++failures["shadowing light without a +0xD8 technique or a frustum along +X: drawn unshadowed"];
            }
            lights_.push_back(std::move(local));
        } catch (const std::exception& ex) {
            ++failures[std::string(ex.what()).substr(0, 120)];
        }
    }
    for (const auto& [why, n] : failures) light_notes_.push_back(std::format("{} lights left out: {}", n, why));
    choose_light_state();
    // Light shafts: the same per-shader pipeline sharing, four quarter-resolution targets added
    // into (far, near, slices 0-3, slices 4-7). The techniques also read the rigid_model,
    // transparent and transparent_advanced scopes.
    vol_lights_.clear();
    std::map<std::string, uint32_t> vol_keys;
    std::map<std::string, size_t> vol_failures;
    for (size_t i = 0; i < lights_.size(); ++i) {
        const MapLight& l = lights_[i].light;
        if (l.volumetrics.is_none()) continue;
        try {
            auto& tech = techniques[l.volumetrics.value];
            if (!tech) tech = std::make_shared<const Technique>(load_technique(pm, l.volumetrics));
            std::string key;
            for (auto which : {ShaderStage::Vertex, ShaderStage::Pixel}) {
                const auto& st = tech->stage(which);
                key += std::format("{:08X}:", st.shader.value);
                for (const auto& t : st.textures) key += std::format("t{}={:08X},", t.slot, t.texture.value);
                for (const auto& sm : st.samplers) key += std::format("s{:08X},", sm.value);
                key += '|';
            }
            auto it = vol_keys.find(key);
            if (it == vol_keys.end()) {
                const uint32_t index = uint32_t(vol_pipelines_.size());
                for (bool front : {true, false}) {
                    auto vp = std::make_unique<GamePipeline>();
                    GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                                              {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT}};
                    opt.volume = true;
                    opt.cull_front = front;
                    opt.cbuffer_floor = 80;
                    vp->load(pm, gpu, l.volumetrics, scopes, opt);
                    vol_pipelines_.push_back(std::move(vp));
                }
                it = vol_keys.emplace(key, index).first;
            }
            VolumetricLight vl{it->second, tech, i};
            if (lights_[i].shadow_technique && !l.shadow_volumetrics.is_none()) {
                auto& stech = techniques[l.shadow_volumetrics.value];
                if (!stech) stech = std::make_shared<const Technique>(load_technique(pm, l.shadow_volumetrics));
                std::string skey = "S";
                for (auto which : {ShaderStage::Vertex, ShaderStage::Pixel}) {
                    const auto& st = stech->stage(which);
                    skey += std::format("{:08X}:", st.shader.value);
                    for (const auto& t : st.textures) skey += std::format("t{}={:08X},", t.slot, t.texture.value);
                    for (const auto& sm : st.samplers) skey += std::format("s{:08X},", sm.value);
                    skey += '|';
                }
                auto sit = vol_keys.find(skey);
                if (sit == vol_keys.end()) {
                    const uint32_t index = uint32_t(vol_pipelines_.size());
                    for (bool front : {true, false}) {
                        auto vp = std::make_unique<GamePipeline>();
                        GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                                                  {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT}};
                        opt.volume = true;
                        opt.cull_front = front;
                        opt.cbuffer_floor = 80;
                        vp->load(pm, gpu, l.shadow_volumetrics, scopes, opt);
                        vol_pipelines_.push_back(std::move(vp));
                    }
                    sit = vol_keys.emplace(skey, index).first;
                }
                vl.shadow_pipeline = sit->second;
                vl.shadow_technique = stech;
            }
            vol_lights_.push_back(std::move(vl));
        } catch (const std::exception& ex) {
            ++vol_failures[std::string(ex.what()).substr(0, 120)];
        }
    }
    for (const auto& [why, n] : vol_failures) light_notes_.push_back(std::format("{} light shafts left out: {}", n, why));
    if (!lights_.empty())
        ext_.note_estimate("local lights: deferred_light +0x100..+0x130 = (0,0,0,1) (plane masks off), +0x150 = 1, +0x160 = 1 (fade: fully on)",
                           "goriath DeferredLight layout; +0x160 (float 0x58) is the output scale of the light PS (cb0[36], saturated), read by 458 of Outpost's 4236 lights");
}

std::optional<ViewMatrices> light_shadow_view(const MapLight& l) {
    // +0x60 maps the cube (u, v, w) to light-local homogeneous coordinates. A spot light has
    // x = c3.x, (y, z) = u * c0.yz + v * c1.yz, W = w * c2.w + c3.w: forward +X, u / v across, the
    // near plane at w = -1 and the far plane at w = +1 (Outpost 80AF301F: tan 1.0355 = tan(+0xC4),
    // 0.56 .. 55.9 m, +0xC0 = 56).
    const auto& m = l.light_space;
    auto at = [&](int col, int row) { return m[size_t(col * 4 + row)]; };
    const float eps = 1e-4f;
    if (std::fabs(at(0, 0)) > eps || std::fabs(at(1, 0)) > eps || std::fabs(at(0, 3)) > eps || std::fabs(at(1, 3)) > eps) return std::nullopt;
    if (std::fabs(at(2, 0)) > eps || std::fabs(at(2, 1)) > eps || std::fabs(at(2, 2)) > eps) return std::nullopt;
    if (std::fabs(at(3, 1)) > eps || std::fabs(at(3, 2)) > eps || at(3, 0) <= eps) return std::nullopt;
    const float x = at(3, 0), w_near = at(3, 3) - at(2, 3), w_far = at(3, 3) + at(2, 3);
    if (w_near <= eps || w_far <= eps || w_far >= w_near) return std::nullopt;
    const float near_z = x / w_near, far_z = x / w_far;
    // The map's axes are those of the light box (light_box_to_world): the +0xE0 shaft PS looks
    // its box-space samples up at u = 0.5 - (box.y - 0.5) * far / x, v = 0.5 - (box.x - 0.5) * far / x
    // (box.y along local Y, box.x along local Z), so NDC x = -Y / (x tan), NDC y = Z / (x tan), with
    // the box's half extents at the far plane as the tangents.
    const float tan_u = (std::fabs(at(0, 1)) + std::fabs(at(1, 1))) / x, tan_v = (std::fabs(at(0, 2)) + std::fabs(at(1, 2))) / x;
    if (tan_u <= eps || tan_v <= eps) return std::nullopt;
    const M4 node = m4(l.node);
    auto dir = [&](float lx, float ly, float lz) {
        return normalize(V3{node.c[0][0] * lx + node.c[1][0] * ly + node.c[2][0] * lz, node.c[0][1] * lx + node.c[1][1] * ly + node.c[2][1] * lz,
                            node.c[0][2] * lx + node.c[1][2] * ly + node.c[2][2] * lz});
    };
    ViewMatrices v;
    v.eye = {node.c[3][0], node.c[3][1], node.c[3][2]};
    v.fwd = dir(1, 0, 0);
    v.up = dir(0, 0, 1);
    v.right = normalize(cross(v.fwd, v.up));  // -Y of the light
    v.up = cross(v.right, v.fwd);
    v.fx = 1 / tan_u;
    v.fy = 1 / tan_v;
    M4 rot;
    rot.c[0] = {v.right.x, v.up.x, v.fwd.x, 0};
    rot.c[1] = {v.right.y, v.up.y, v.fwd.y, 0};
    rot.c[2] = {v.right.z, v.up.z, v.fwd.z, 0};
    rot.c[3] = {0, 0, 0, 1};
    M4 proj;  // reversed depth: near -> 1, far -> 0
    proj.c[0] = {v.fx, 0, 0, 0};
    proj.c[1] = {0, v.fy, 0, 0};
    proj.c[2] = {0, 0, -near_z / (far_z - near_z), 1};
    proj.c[3] = {0, 0, near_z * far_z / (far_z - near_z), 0};
    v.proj = proj;
    v.vp_rel = mul(proj, rot);
    M4 tr;
    tr.c[0] = {1, 0, 0, 0};
    tr.c[1] = {0, 1, 0, 0};
    tr.c[2] = {0, 0, 1, 0};
    tr.c[3] = {-v.eye.x, -v.eye.y, -v.eye.z, 1};
    v.vp_abs = mul(v.vp_rel, tr);
    v.vp_prev = v.vp_abs;
    return v;
}

std::vector<SunShadow*> GameLighting::light_shadows_to_render(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, size_t scene_items,
                                                              size_t budget) {
    constexpr uint32_t kSize = 1024;
    std::vector<SunShadow*> out;
    if (!light_shadows_) return out;
    const LightFrustum frustum(vm);
    if (scene_items != light_shadow_items_) {
        for (auto& ll : lights_)
            if (ll.shadow) ll.shadow->valid = false;
        light_shadow_items_ = scene_items;
    }
    for (auto& ll : lights_) {
        if (out.size() >= budget) break;
        if (!ll.shadow_technique || (ll.shadow && ll.shadow->valid) || frustum.outside(ll.light.lo, ll.light.hi)) continue;
        const auto view = light_shadow_view(ll.light);
        if (!view) continue;
        if (!ll.shadow) ll.shadow = std::make_unique<SunShadow>();
        ll.shadow->resize(gpu, kSize);
        if (ll.shadow_readable) {
            Gpu::barrier(cl, ll.shadow->depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            ll.shadow_readable = false;
        }
        ll.shadow->vm = *view;
        out.push_back(ll.shadow.get());
    }
    if (!out.empty() || light_shadows_used_)
        ext_.note_estimate("shadowing lights: 1024^2 depth map of the loaded scene per light (reversed depth, frustum of the light matrix), deferred_shadow +0x30.x = -7000, +0x180 = 2 (21 taps), deferred_light +0x154 = 1",
                           "the engine shadow map render (resolution, atlas, caster set, bias, filter quality) is not in game data; slots read from the +0xD8 PS, -7000 is the D2 default (Alkahest)");
    return out;
}

void GameLighting::bind_light_shadow(ID3D12GraphicsCommandList* cl, LocalLight& ll, const ViewMatrices& vm) {
    constexpr uint8_t kShadow = 6;  // deferred_shadow
    if (!ext_.zero_policy(kShadow)) ext_.zero_when_missing(kShadow, "deferred shadow value with no known meaning: zero");
    if (!ll.shadow_readable) {
        Gpu::barrier(cl, ll.shadow->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ll.shadow_readable = true;
    }
    M4 from_rel;  // camera-relative world -> world
    from_rel.c[0] = {1, 0, 0, 0}; from_rel.c[1] = {0, 1, 0, 0}; from_rel.c[2] = {0, 0, 1, 0};
    from_rel.c[3] = {vm.eye.x, vm.eye.y, vm.eye.z, 1};
    ext_.set_texture(kShadow, 0x00, ll.shadow->depth.Get(), DXGI_FORMAT_R32_FLOAT);
    ext_.set(kShadow, 0x18, float(ll.shadow->size));
    ext_.set(kShadow, 0x1C, float(ll.shadow->size));
    ext_.set(kShadow, 0x30, float4{-7000, 0, 1, 0});  // .x: slope of the depth comparison (reversed depth)
    ext_.set(kShadow, 0xB0, float4{0, 0, 1, 1});      // uv offset / scale: the whole map
    ext_.set(kShadow, 0xC0, mul(ll.shadow->vm.vp_abs, from_rel));
    ext_.set(kShadow, 0x180, 2.0f);                   // > 1: the 21-tap filter of the PS
}

// The extern writes of one light, to the store (when drawing) or to the overlay (ahead).
template <class Sink>
void GameLighting::write_light(Sink& sink, const LocalLight& ll, const ViewMatrices& vm, bool shadowed) const {
    M4 to_rel;  // world -> camera-relative world
    to_rel.c[0] = {1, 0, 0, 0}; to_rel.c[1] = {0, 1, 0, 0}; to_rel.c[2] = {0, 0, 1, 0};
    to_rel.c[3] = {-vm.eye.x, -vm.eye.y, -vm.eye.z, 1};
    M4 from_rel;  // camera-relative world -> world (bind_light_shadow's +0xC0)
    from_rel.c[0] = {1, 0, 0, 0}; from_rel.c[1] = {0, 1, 0, 0}; from_rel.c[2] = {0, 0, 1, 0};
    from_rel.c[3] = {vm.eye.x, vm.eye.y, vm.eye.z, 1};
    const MapLight& l = ll.light;
    const M4 node_rel = mul(to_rel, m4(l.node));
    const M4 volume = mul(to_rel, ll.world_volume);  // unit cube -> camera-relative world
    sink.set(15, 0x00, mul(vm.vp_rel, volume));
    sink.set(ext::DeferredLight, 0x00, 0.0f);
    M4 relative_to_box = ll.world_to_box;
    for (size_t i = 0; i < 4; ++i)
        relative_to_box.c[3][i] += vm.eye.x * relative_to_box.c[0][i] + vm.eye.y * relative_to_box.c[1][i] +
            vm.eye.z * relative_to_box.c[2][i];
    sink.set(ext::DeferredLight, 0x40, relative_to_box);
    sink.set(ext::DeferredLight, 0xC0, node_rel);
    for (uint32_t off : {0x100u, 0x110u, 0x120u, 0x130u}) sink.set(ext::DeferredLight, off, float4{0, 0, 0, 1});
    sink.set(ext::DeferredLight, 0x150, 1.0f);
    // +0x154: weight of the shadow term in the shadowing PS (lit = 1 + w * (taps - 1)).
    sink.set(ext::DeferredLight, 0x154, shadowed ? 1.0f : 0.0f);
    if (shadowed) {  // bind_light_shadow's values
        sink.set(6, 0x18, float(ll.shadow->size));
        sink.set(6, 0x1C, float(ll.shadow->size));
        sink.set(6, 0x30, float4{-7000, 0, 1, 0});
        sink.set(6, 0xB0, float4{0, 0, 1, 1});
        sink.set(6, 0xC0, mul(ll.shadow->vm.vp_abs, from_rel));
        sink.set(6, 0x180, 2.0f);
    }
    sink.set(ext::DeferredLight, 0x160, 1.0f);  // the PS multiplies both outputs by saturate(+0x160) (cb0[36].x): light fade, fully on
}

void GameLighting::draw_lights(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{base.ptr + 3 * inc}, {base.ptr + 4 * inc}};
    cl->OMSetRenderTargets(2, rtvs, FALSE, &gb.dsv_ro);
    if (!ext_.zero_policy(ext::DeferredLight)) ext_.zero_when_missing(ext::DeferredLight, "deferred light value with no known meaning: zero");
    if (!ext_.zero_policy(15)) ext_.zero_when_missing(15, "simple geometry value with no known meaning: zero");
    const LightFrustum frustum(vm);
    GamePipeline::VolumeBatch batch;
    const bool light_debug = std::getenv("NIM_LIGHT_DEBUG") != nullptr;
    light_shadows_used_ = 0;
    size_t draws = 0;
    volume_overlay_.clear();  // this pass's writes only, over the store as it is now
    for (size_t index = 0; index < lights_.size(); ++index) {
        auto& ll = lights_[index];
        if (frustum.outside(ll.light.lo, ll.light.hi)) continue;
        const bool shadowed = light_shadows_ && ll.shadow && ll.shadow->valid && ll.shadow_technique;
        if (shadowed && !ext_.zero_policy(6)) ext_.zero_when_missing(6, "deferred shadow value with no known meaning: zero");
        if (volume_draws_.size() <= draws) volume_draws_.resize(draws + 1);
        auto& d = volume_draws_[draws++];
        d.light = index;
        d.shadowed = shadowed;
        // Only the far side of the volume is drawn (the camera may be inside it); the cube's
        // winding on screen flips with the handedness of its transform.
        d.pipeline = light_pipelines_[(shadowed ? ll.shadow_pipeline : ll.pipeline) + (ll.mirrored ? 1 : 0)].get();
        d.technique = shadowed ? ll.shadow_technique.get() : ll.technique.get();
        write_light(volume_overlay_, ll, vm, shadowed);
        volume_overlay_.snapshot(d.overlay);
    }
    evaluate_volume_draws(draws);
    for (size_t k = 0; k < draws; ++k) {
        const auto& d = volume_draws_[k];
        auto& ll = lights_[d.light];
        const bool shadowed = d.shadowed;
        write_light(ext_, ll, vm, shadowed);
        if (shadowed) {
            bind_light_shadow(cl, ll, vm);
            ++light_shadows_used_;
        }
        GamePipeline& gp = *d.pipeline;
        try {
            if (!d.error.empty()) throw std::runtime_error(d.error);
            gp.draw_volume(gpu, cl, ext_, scopes, d.technique, cube_vbv_, cube_ibv_, 36, {}, &batch, &d.constants);
            if (light_debug && lights_drawn_ < 2) {
                const auto& vs = d.constants[0];  // evaluated ahead (draw_volume does not copy them)
                const auto& ps = d.constants[1];
                std::string t = std::format("light {} mirrored {}\n", ll.technique->tag.str(), ll.mirrored);
                for (size_t k = 0; k < 4; ++k) t += std::format(" vs{:2} {:.3g} {:.3g} {:.3g} {:.3g}\n", 0x23 + k, vs[0x23 + k][0], vs[0x23 + k][1], vs[0x23 + k][2], vs[0x23 + k][3]);
                for (size_t k = 0; k < 0x2B && k < ps.size(); ++k) t += std::format(" ps{:2X} {:.4g} {:.4g} {:.4g} {:.4g}\n", k, ps[k][0], ps[k][1], ps[k][2], ps[k][3]);
                std::fputs(t.c_str(), stdout);
            }
            ++lights_drawn_;
        } catch (const std::exception& ex) {
            if (light_notes_.size() < 32) light_notes_.push_back(std::format("{}: {}", ll.technique->tag.str(), ex.what()));
        }
    }
}

// The map's local lights near `center` as the transparent light grid reads them. Each light's PS
// constants are evaluated with the deferred pass's writes (unshadowed); the registers are found
// at load (set_lights): falloff F from its init, colour F - 1, cone F - 3, diffuse weight from
// the +0x150 product. The grid CS takes a capsule light with an SH lobe per colour; nim fills the
// main lobe only.
std::vector<GameLighting::GridLight> GameLighting::grid_lights(const ViewMatrices& vm, const float4& center, float radius) {
    std::vector<GridLight> out;
    size_t draws = 0;
    grid_light_misses_ = 0;
    volume_overlay_.clear();
    for (size_t index = 0; index < lights_.size(); ++index) {
        auto& ll = lights_[index];
        if (ll.grid_falloff < 3 || ll.grid_weight < 0) {
            ++grid_light_misses_;
            continue;
        }
        float d2 = 0;
        for (size_t a = 0; a < 3; ++a) {
            const float d = center[a] - std::clamp(center[a], ll.light.lo[a], ll.light.hi[a]);
            d2 += d * d;
        }
        if (d2 > radius * radius) continue;
        if (volume_draws_.size() <= draws) volume_draws_.resize(draws + 1);
        auto& d = volume_draws_[draws++];
        d.light = index;
        d.shadowed = false;
        d.pipeline = light_pipelines_[ll.pipeline].get();
        d.technique = ll.technique.get();
        write_light(volume_overlay_, ll, vm, false);
        volume_overlay_.snapshot(d.overlay);
    }
    evaluate_volume_draws(draws);
    for (size_t k = 0; k < draws; ++k) {
        const auto& d = volume_draws_[k];
        const auto& ll = lights_[d.light];
        const auto& ps = d.constants[1];
        const size_t f = size_t(ll.grid_falloff), w = size_t(ll.grid_weight);
        if (!d.error.empty() || ps.size() <= std::max(f, w)) {
            ++grid_light_misses_;
            continue;
        }
        const float a = ps[f][0], b = ps[f][1];
        if (!(a < 0) || !(b > 0)) {
            ++grid_light_misses_;
            continue;
        }
        GridLight g;
        const auto& n = ll.light.node;
        g.position = {n[12], n[13], n[14]};
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        g.axis = len > 0 ? std::array{n[0] / len, n[1] / len, n[2] / len} : std::array{1.0f, 0.0f, 0.0f};
        g.colour = {ps[f - 1][0], ps[f - 1][1], ps[f - 1][2]};
        // saturate(a d + b) as the CS's saturate((end - d) / (end - start)).
        g.end = -b / a;
        g.start = (1 - b) / a;
        g.intensity = ps[w][0];
        // Cone (scale, bias, exponent): saturate(bias + scale cos)^exponent, cos from the +X axis.
        // The CS's cone is linear between cos(inner) and cos(outer); the exponent is dropped.
        const auto& cone = ps[f - 3];
        if (cone[0] > 0 && cone[2] > 0) {
            const float outer = -cone[1] / cone[0], inner = (1 - cone[1]) / cone[0];
            if (outer > -1 && inner <= 1) {
                g.outer = std::acos(std::clamp(outer, -1.0f, 1.0f));
                g.inner = std::acos(std::clamp(inner, -1.0f, 1.0f));
            }
        }
        if (g.intensity > 0 && g.end > 0 && (g.colour[0] > 0 || g.colour[1] > 0 || g.colour[2] > 0)) out.push_back(g);
    }
    return out;
}

// Evaluates the TFX of the first `count` volume draws on the worker pool. Each sees the store as it
// is now plus its own recorded writes (the overlay), which is the store's state at its draw: the
// draws in between only write the overlay's words.
void GameLighting::evaluate_volume_draws(size_t count, std::span<const Scope* const> scopes) {
    WorkerPool::instance().parallel_for(count, [&](size_t k) {
        auto& d = volume_draws_[k];
        d.error.clear();
        const auto env = ext_.environment(&d.overlay);
        try {
            d.pipeline->evaluate_stages(env, d.technique, d.constants);
        } catch (const std::exception& ex) {
            d.error = ex.what();
        }
        for (size_t s = 0; s < scopes.size() && s * 2 + 1 < d.scopes.size(); ++s)
            for (const ScopeStage ss : {ScopeStage::Vertex, ScopeStage::Pixel}) {
                const size_t slot = s * 2 + (ss == ScopeStage::Pixel ? 1 : 0);
                d.scopes[slot].clear();
                d.scope_errors[slot].clear();
                const auto& scope_stage_data = scopes[s]->stages[size_t(ss)];
                if (scope_stage_data.cbuffer_init.empty() || scope_stage_data.cbuffer_slot < 0) continue;
                if (!d.pipeline->binds_shared_cbuffer(ss == ScopeStage::Vertex ? ShaderStage::Vertex : ShaderStage::Pixel,
                                                      uint32_t(scope_stage_data.cbuffer_slot))) continue;
                try {
                    d.scopes[slot] = evaluate_scope(*scopes[s], ss, env);
                } catch (const std::exception& ex) {
                    d.scope_errors[slot] = ex.what();
                }
            }
    }, 4);
}

// ---- Volumetrics -------------------------------------------------------------------------------
// The light shaft chain (docs/PROGRESS.md 3.3), read from the DXIL of the passes:
//  1. downsample_full_depth_to_quarter_max_min (extern 35): per 4x4 block, view depth max / min.
//  2. each light's volumetrics technique, drawn as the light volume at quarter resolution with a
//     quarter-resolution view: marches the lit cone between the eye and the block's max (far
//     layer, o0) and min (near layer, o1) depth and spreads the energy into 8 depth slices (o2,
//     o3, for transparents). t10 (transparent scope, Deferred +0x98) is the min / max texture.
//  3. volumetrics_blur_results: depth-aware 3x3 blur of both layers (extern 38 inputs).
//  4. volumetrics_upres_1: bilateral upsample against the full-resolution view depth (Deferred
//     +0x98 = (depth, depth) as nim's linear depth), added to the HDR target.
void GameLighting::render_volumetrics(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                                      D3D12_CPU_DESCRIPTOR_HANDLE hdr, const ScopeBufferFn& full_scopes) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto vb = vol_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    auto rtv = [&](size_t i) { return D3D12_CPU_DESCRIPTOR_HANDLE{vb.ptr + i * inc}; };
    const float w = float(w_), h = float(h_), lw = float(vw_), lh = float(vh_);
    const float near_z = vm.vp_rel.c[3][2];
    constexpr std::string_view chosen = "meaning read from the volumetrics DXIL, value chosen";
    auto upload = [&](const std::vector<float4>& values) {
        auto a = gpu.dynamic(std::max<size_t>(values.size(), 1) * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    const D3D12_VIEWPORT qvp{0, 0, lw, lh, 0, 1}, fvp{0, 0, w, h, 0, 1};
    const D3D12_RECT qsc{0, 0, LONG(vw_), LONG(vh_)}, fsc{0, 0, LONG(w_), LONG(h_)};
    cl->RSSetViewports(1, &qvp);
    cl->RSSetScissorRects(1, &qsc);

    // 1. Quarter-resolution min / max view depth.
    constexpr uint8_t kDownsample = 0x23;
    ext_.set_texture(kDownsample, 0x00, gb.depth.Get(), GBuffer::kDepthSrvFormat);
    ext_.set(kDownsample, 0x10, float4{lw, lh, 1 / lw, 1 / lh});  // cb0[2]: target size
    ext_.set(kDownsample, 0x20, float4{w, h, 1 / w, 1 / h});      // cb0[3]: source size (x: 4 source texels per target texel)
    // The original shader takes the reciprocal before computing the depth range.
    // With an infinite projection, clear depth would make its range/mean non-finite.
    // Use the same finite sky distance as the full-resolution depth passed to upres.
    const float4 shaft_depth_decode{1 / kSkyViewDepth, 1 / near_z, 0, 0};
    ext_.set(kDownsample, 0x30, shaft_depth_decode);
    Gpu::barrier(cl, vol_minmax_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto minmax_rtv = rtv(0);
    cl->OMSetRenderTargets(1, &minmax_rtv, FALSE, nullptr);
    vol_downsample_.draw(gpu, cl, ext_, full_scopes);
    Gpu::barrier(cl, vol_minmax_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 2. The lights, with a quarter-resolution view (cb12 and the pixel -> world matrix).
    fill_view_externs(ext_, vw_, vh_, vm);
    M4 pixel_to_rel;
    for (uint32_t i = 0; i < 4; ++i) pixel_to_rel.c[i] = ext_.get_v(ext::View, 0x2E0 + 16 * i);
    const auto q12v = upload(evaluate_scope(view_scope_, ScopeStage::Vertex, ext_));
    const auto q12p = upload(evaluate_scope(view_scope_, ScopeStage::Pixel, ext_));
    const Scope* extra_scopes[] = {&rigid_scope_, &transparent_scope_, &transparent_adv_scope_};
    const VolumeDraw* current_draw = nullptr;  // its object scopes were evaluated ahead
    // transparent_advanced (cb8) has no bytecode: the engine fills it per object. The light
    // shaft PS reads only cb8[7].w, the weight of the froxel transmittance t25.x * t25.y in
    // lerp(fog colour (global channel 0x16), 1, weight * transmittance), the shaft's
    // attenuation by the atmosphere. Zero turned every shaft into the fog colour (about
    // 0.002, i.e. invisible); one leaves the attenuation to the froxels.
    std::array<float4, 37> shaft_advanced{};
    shaft_advanced[7][3] = 1;
    ext_.note_estimate("light shaft transparent_advanced cb8[7].w = 1 (froxel transmittance weight)",
                       "engine-filled per-object value; read as the weight of the froxel transmittance in the shaft PS");
    const auto shaft_advanced_cb = upload({shaft_advanced.begin(), shaft_advanced.end()});
    ScopeBufferFn qscopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 12) return stage == ShaderStage::Vertex ? q12v : q12p;
        if (reg == 13) return full_scopes(stage, 13);
        const ScopeStage ss = scope_stage(stage);
        for (size_t i = 0; i < std::size(extra_scopes); ++i) {
            const Scope* s = extra_scopes[i];
            const auto& st = s->stages[size_t(ss)];
            if (st.cbuffer_slot != int32_t(reg) || st.cbuffer_init.empty()) continue;
            if (s == &transparent_adv_scope_) return shaft_advanced_cb;
            if (current_draw && (ss == ScopeStage::Vertex || ss == ScopeStage::Pixel)) {
                const size_t slot = i * 2 + (ss == ScopeStage::Pixel ? 1 : 0);
                if (!current_draw->scope_errors[slot].empty()) throw std::runtime_error(current_draw->scope_errors[slot]);
                if (!current_draw->scopes[slot].empty()) return upload(current_draw->scopes[slot]);
            }
            return upload(evaluate_scope(*s, ss, ext_));
        }
        return 0;
    };
    ext_.set_texture(ext::Deferred, 0x98, vol_minmax_.Get());  // t10 of the transparent scope
    const float4 depth_decode = ext_.get_v(ext::Deferred, 0x00);
    ext_.set(ext::Deferred, 0x00, shaft_depth_decode);  // the light march converts the depths back to device depth
    ext_.note_estimate("volumetrics clear depth: finite 1,000,000 m",
                       "finite far distance for the infinite viewer projection; shared with full-resolution linear depth");
    constexpr uint8_t kVolumetricsPass = 0x58, kTemporal = 0x59;
    if (!ext_.zero_policy(kVolumetricsPass))
        ext_.zero_when_missing(kVolumetricsPass, "volumetric fog density slices (extern 88 t0/t1): producer not identified, zero (base density only)");
    ext_.set(kVolumetricsPass, 0x10, 0.0f);
    ext_.note_estimate("extern 88 +0x10 = 0 (march dither off)", "no temporal filter yet: an undithered march instead of temporal noise");
    if (!ext_.zero_policy(ext::Transparent))
        ext_.zero_when_missing(ext::Transparent, "transparent pass inputs (atmosphere volumes, coverage weight) not produced: zero");
    if (!ext_.zero_policy(ext::RigidModel)) ext_.zero_when_missing(ext::RigidModel, "rigid model value with no known meaning: zero");
    if (!ext_.zero_policy(ext::VolumeFog)) ext_.zero_when_missing(ext::VolumeFog, "volume fog value with no known meaning: zero");
    ext_.set(ext::VolumeFog, 0xB0, 1.0f);  // cb0[39]: shaft intensity scale
    ext_.set(ext::VolumeFog, 0xB4, 1.0f);  // cb0[40]: march direction (+1: from the far face toward the eye)
    ext_.note_estimate("extern 50 +0xB0 = 1 (shaft intensity), +0xB4 = 1 (march direction)", chosen);
    for (auto& r : vol_layer_) Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE layers[4] = {rtv(1), rtv(2), rtv(3), rtv(4)};
    const float zero[4]{};
    for (const auto& l : layers) cl->ClearRenderTargetView(l, zero, 0, nullptr);
    cl->OMSetRenderTargets(4, layers, FALSE, nullptr);
    M4 to_rel;  // world -> camera-relative world
    to_rel.c[0] = {1, 0, 0, 0}; to_rel.c[1] = {0, 1, 0, 0}; to_rel.c[2] = {0, 0, 1, 0};
    to_rel.c[3] = {-vm.eye.x, -vm.eye.y, -vm.eye.z, 1};
    vol_shadows_used_ = 0;
    const LightFrustum frustum(vm);
    GamePipeline::VolumeBatch batch;
    M4 from_rel;  // camera-relative world -> world (bind_light_shadow's +0xC0)
    from_rel.c[0] = {1, 0, 0, 0}; from_rel.c[1] = {0, 1, 0, 0}; from_rel.c[2] = {0, 0, 1, 0};
    from_rel.c[3] = {vm.eye.x, vm.eye.y, vm.eye.z, 1};
    // The extern writes of one shaft, to the store (when drawing) or to the overlay (ahead).
    auto write_shaft = [&](auto& sink, const LocalLight& ll, bool shadowed) {
        const MapLight& l = ll.light;
        // +0xE0: each march sample is looked up in the light's depth map (t2) through the light
        // box, weighted by +0x154 (cb0[41]).
        const M4 node = m4(l.node), space = m4(l.light_space);
        const M4 node_rel = mul(to_rel, node);
        const M4 volume = mul(node_rel, space);
        const M4 box = light_box_to_world(node, space);  // light box [0,1]^3 -> world
        const M4 rel_to_box = inverse(mul(to_rel, box));
        sink.set(15, 0x00, mul(vm.vp_rel, volume));
        sink.set(ext::DeferredLight, 0x00, 0.0f);
        sink.set(ext::DeferredLight, 0x40, rel_to_box);
        sink.set(ext::DeferredLight, 0xC0, node_rel);
        for (uint32_t off : {0x100u, 0x110u, 0x120u, 0x130u}) sink.set(ext::DeferredLight, off, float4{0, 0, 0, 1});
        sink.set(ext::DeferredLight, 0x150, 1.0f);
        sink.set(ext::DeferredLight, 0x154, shadowed ? 1.0f : 0.0f);
        sink.set(ext::DeferredLight, 0x160, 1.0f);
        if (shadowed) {  // bind_light_shadow's values
            sink.set(6, 0x18, float(ll.shadow->size));
            sink.set(6, 0x1C, float(ll.shadow->size));
            sink.set(6, 0x30, float4{-7000, 0, 1, 0});
            sink.set(6, 0xB0, float4{0, 0, 1, 1});
            sink.set(6, 0xC0, mul(ll.shadow->vm.vp_abs, from_rel));
            sink.set(6, 0x180, 2.0f);
        }
        sink.set(ext::RigidModel, 0x00, box);                           // cb0[42..45]: box -> world
        sink.set(ext::VolumeFog, 0x40, mul(rel_to_box, pixel_to_rel));  // cb0[32..35]: (pixel, depth) -> box
        sink.set(ext::VolumeFog, 0x80, rel_to_box.c[3]);                // cb0[38]: the eye in the box
        return det4(volume) > 0;  // mirrored
    };
    std::vector<const VolumetricLight*> shafts;
    size_t draws = 0;
    volume_overlay_.clear();  // this pass's writes only, over the store as it is now
    for (const auto& vl : vol_lights_) {
        LocalLight& ll = lights_[vl.light];
        if (frustum.outside(ll.light.lo, ll.light.hi)) continue;
        const bool shadowed = light_shadows_ && ll.shadow && ll.shadow->valid && vl.shadow_technique;
        if (shadowed && !ext_.zero_policy(6)) ext_.zero_when_missing(6, "deferred shadow value with no known meaning: zero");
        if (volume_draws_.size() <= draws) volume_draws_.resize(draws + 1);
        auto& d = volume_draws_[draws++];
        d.light = vl.light;
        d.shadowed = shadowed;
        const bool mirrored = write_shaft(volume_overlay_, ll, shadowed);
        d.pipeline = vol_pipelines_[(shadowed ? vl.shadow_pipeline : vl.pipeline) + (mirrored ? 1 : 0)].get();
        d.technique = shadowed ? vl.shadow_technique.get() : vl.technique.get();
        volume_overlay_.snapshot(d.overlay);
        shafts.push_back(&vl);
    }
    evaluate_volume_draws(draws, extra_scopes);
    for (size_t k = 0; k < draws; ++k) {
        const auto& d = volume_draws_[k];
        const VolumetricLight& vl = *shafts[k];
        LocalLight& ll = lights_[vl.light];
        const bool shadowed = d.shadowed;
        write_shaft(ext_, ll, shadowed);
        if (shadowed) {
            bind_light_shadow(cl, ll, vm);
            ++vol_shadows_used_;
        }
        try {
            if (!d.error.empty()) throw std::runtime_error(d.error);
            current_draw = &d;
            d.pipeline->draw_volume(gpu, cl, ext_, qscopes, d.technique, cube_vbv_, cube_ibv_, 36, {}, &batch, &d.constants);
            current_draw = nullptr;
            ++vol_drawn_;
        } catch (const std::exception& ex) {
            current_draw = nullptr;
            if (light_notes_.size() < 32) light_notes_.push_back(std::format("volumetrics {}: {}", vl.technique->tag.str(), ex.what()));
        }
    }
    for (auto& r : vol_layer_) Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 3. Blur: extern 38 +0x08 near, +0x00 far, +0x18 min / max; out (far, near).
    const auto post = ext_.texture(ext::Postprocess, 0x00);
    ext_.set_texture(ext::Postprocess, 0x00, vol_layer_[0].Get());
    ext_.set_texture(ext::Postprocess, 0x08, vol_layer_[1].Get());
    ext_.set_texture(ext::Postprocess, 0x18, vol_minmax_.Get());
    ext_.set(ext::Postprocess, 0x50, float4{lw, lh, 1 / lw, 1 / lh});
    ext_.set(kTemporal, 0xA0, 1.0f);  // cb0[10]: blur on (else the layers pass through)
    for (auto& r : vol_blurred_) Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE blurred[2] = {rtv(5), rtv(6)};
    cl->OMSetRenderTargets(2, blurred, FALSE, nullptr);
    vol_blur_.draw(gpu, cl, ext_, qscopes);
    for (auto& r : vol_blurred_) Gpu::barrier(cl, r.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // 4. Upres into the HDR target (added: the technique carries no blend state).
    cl->RSSetViewports(1, &fvp);
    cl->RSSetScissorRects(1, &fsc);
    fill_view_externs(ext_, w_, h_, vm);
    ext_.set_texture(ext::Postprocess, 0x00, vol_blurred_[0].Get());
    ext_.set_texture(ext::Postprocess, 0x08, vol_blurred_[1].Get());
    ext_.set(ext::Postprocess, 0x50, float4{w, h, 1 / w, 1 / h});
    ext_.set(ext::Postprocess, 0x60, float4{lw, lh, 1 / lw, 1 / lh});
    ext_.set(ext::Postprocess, 0xC0, float4{1, 1, 1, 1});
    ext_.set_texture(ext::Deferred, 0x98, linear_depth_.Get(), DXGI_FORMAT_R32G32_FLOAT);
    ext_.note_estimate("volumetrics: upres added to HDR, scale 1 (extern 38 +0xC0), blur on (extern 89 +0xA0)", chosen);
    cl->OMSetRenderTargets(1, &hdr, FALSE, nullptr);
    vol_upres_.draw(gpu, cl, ext_, full_scopes);

    // Restore what the other passes read.
    ext_.set_texture(ext::Postprocess, 0x00, post.resource, post.format);
    ext_.set(ext::Deferred, 0x00, depth_decode);
    ext_.set_texture(ext::Deferred, 0x98, gb.depth.Get(), GBuffer::kDepthSrvFormat);
}

// ---- Cubemap volumes ---------------------------------------------------------------------------
// cb11[19..20] of the relighting variants and cb11[15].z, as the D2 engine fills them (Alkahest's
// CubemapRenderer, taken from the engine's decompiled function; the Marathon PS reads the same slots).
// The PS scales the probe by (alpha^cb15.z * cb19 * probe + cb20); the non-relighting variant is
// the case cb19 = cubemap_bounce_scale * params.x, cb20 = 1.
std::array<float4, 2> cubemap_relighting(const MapCubemap& volume, const CubemapRelightingInputs& in) {
    const float sun = std::max(in.sun_light_direction[2], 0.5f) * in.sun_scale * (in.sun_intensity / 10.0f);
    std::array<float4, 2> out{};
    for (size_t c = 0; c < 3; ++c) {
        const float relit = (sun * in.sun_color[c] - 1.0f) * volume.flags[0] + 1.0f;
        out[0][c] = in.bounce_scale * volume.probe_params[0] * relit;
        out[1][c] = in.sky_intensity * volume.flags[1] + relit;
    }
    return out;
}

float cubemap_alpha_exponent(float z) {
    return z > 0.5f ? std::pow((z - 0.5f) * 2.0f, 4.0f) * 60.0f + 1.0f : z * 2.0f;
}

void GameLighting::clear_cubemaps(Gpu& gpu) {
    grid_key_.reset();
    // Old frames can still hold views of these resources. Retire them before freeing the
    // cache, instead of retaining every previously visited map for the process lifetime.
    if (!probe_textures_.empty() || !filtered_cubes_.empty() || !probe_shapes_.empty()) gpu.wait_idle();
    cubemaps_.clear();
    probe_textures_.clear();
    filtered_cubes_.clear();
    probe_shapes_.clear();
    cubemap_notes_.clear();
    cubemaps_requested_ = cubemaps_drawn_ = cubemaps_failed_ = 0;
}

bool GameLighting::set_entity_preview(PackageManager& pm, Gpu& gpu) {
    const auto authored = load_entity_preview_probes(pm);
    std::vector<const MapCubemap*> probes;
    for (const auto& probe : authored) probes.push_back(&probe);
    set_cubemaps(pm, gpu, probes);
    ae_valid_ = taa_valid_ = false;
    ae_fence_ = {};
    return !cubemaps_.empty();
}

void GameLighting::set_cubemaps(PackageManager& pm, Gpu& gpu, std::span<const MapCubemap* const> cubemaps, bool diagnostic_csg) {
    std::set<uint32_t> required_textures, required_cubes, required_shapes;
    for (const auto* m : cubemaps) {
        for (const auto t : {m->specular, m->alpha, m->diffuse}) if (!t.is_none()) required_textures.insert(t.value);
        if (!m->specular.is_none()) required_cubes.insert(m->specular.value);
        if (diagnostic_csg && !m->shape_vertices.is_none()) required_shapes.insert(m->shape_vertices.value);
    }
    const bool retire = std::ranges::any_of(probe_textures_, [&](const auto& x) { return !required_textures.contains(x.first); }) ||
        std::ranges::any_of(filtered_cubes_, [&](const auto& x) { return !required_cubes.contains(x.first); }) ||
        std::ranges::any_of(probe_shapes_, [&](const auto& x) { return !required_shapes.contains(x.first); });
    if (retire) gpu.wait_idle();
    cubemaps_.clear();
    std::erase_if(probe_textures_, [&](const auto& x) { return !required_textures.contains(x.first); });
    std::erase_if(filtered_cubes_, [&](const auto& x) { return !required_cubes.contains(x.first); });
    std::erase_if(probe_shapes_, [&](const auto& x) { return !required_shapes.contains(x.first); });
    cubemaps_requested_ = cubemaps.size();
    cubemaps_drawn_ = cubemaps_failed_ = 0;
    cubemap_csg_diagnostic_ = diagnostic_csg;
    cubemap_notes_.clear();
    make_cube(gpu);
    const Scope* scopes[] = {&frame_scope_, &view_scope_};
    std::map<std::string, size_t> failures;
    // The textures not loaded yet are read and decoded ahead on worker threads (one package
    // reader each); the uploads stay in order on this thread. A texture that fails here is read
    // again below, so its error is reported where it was before.
    std::unordered_map<uint32_t, Texture> prefetched;
    {
        ProfileScope profile("load.cubemaps.prefetch");
        std::vector<uint32_t> tags;
        for (const uint32_t t : required_textures)
            if (!probe_textures_.contains(t)) tags.push_back(t);
        std::vector<std::optional<Texture>> loaded(tags.size());
        const size_t readers = std::min<size_t>(tags.size(), WorkerPool::instance().threads());
        if (readers > 1) {
            std::vector<std::unique_ptr<PackageManager>> forks;
            for (size_t i = 0; i < readers; ++i) forks.push_back(pm.fork());
            WorkerPool::instance().parallel_for(readers, [&](size_t r) {
                for (size_t i = r; i < tags.size(); i += readers) {
                    try {
                        loaded[i] = load_texture(*forks[r], TagHash(tags[i]));
                    } catch (const std::exception&) {
                    }
                }
            }, 1);
            for (size_t i = 0; i < tags.size(); ++i)
                if (loaded[i]) prefetched.emplace(tags[i], std::move(*loaded[i]));
        }
    }
    auto load = [&](TagHash t) {
        if (auto p = prefetched.find(t.value); p != prefetched.end()) {
            Texture texture = std::move(p->second);
            prefetched.erase(p);
            return texture;
        }
        return load_texture(pm, t);
    };
    auto texture = [&](TagHash t, bool volume = false) -> ComPtr<ID3D12Resource> {
        if (t.is_none()) return nullptr;
        auto& slot = probe_textures_[t.value];
        if (!slot) slot = gpu.texture(load(t), volume);
        if (volume && slot->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
            throw std::runtime_error("Diffuse probe texture was cached with the wrong dimension");
        return slot;
    };
    if (diagnostic_csg && !cubemap_stencil_) {
        auto stencil = std::make_unique<GamePipeline>();
        GamePipeline::Options opt;
        opt.volume = true;
        opt.stencil_volume = true;
        opt.cbuffer_floor = 64;
        stencil->load(pm, gpu, globals_.pipeline("cubemap_apply_cube_alpha_off_probes_on_relighting_off"), scopes, opt);
        cubemap_stencil_ = std::move(stencil);
    }
    {
        std::vector<uint32_t> wanted;
        for (const MapCubemap* m : cubemaps)
            if (!m->specular.is_none() && !filtered_cubes_.contains(m->specular.value)) wanted.push_back(m->specular.value);
        std::ranges::sort(wanted);
        wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
        // The specular cubes the filter reads (2D cube arrays).
        for (const uint32_t t : wanted)
            if (auto p = prefetched.find(t); p != prefetched.end() && !probe_textures_[t]) {
                try {
                    probe_textures_[t] = gpu.texture(p->second);
                } catch (const std::exception&) {
                    probe_textures_.erase(t);  // read and reported again by the filter
                }
                prefetched.erase(p);
            }
        try {
            ProfileScope profile("load.cubemaps.filter");
            filter_specular_cubes(pm, gpu, wanted);
        } catch (const std::exception& ex) {
            cubemap_notes_.push_back(std::format("specular cube filtering failed: {}", ex.what()));
            return; // never silently substitute the unfiltered chain for a failed original pass
        }
    }
    ProfileScope profile_volumes("load.cubemaps.volumes");
    for (const MapCubemap* m : cubemaps) {
        try {
            // The volume's permutation, as goriath's get_specialized_cubemap_pipeline (box shape).
            const std::string name = std::format("cubemap_apply_cube_alpha_{}_probes_{}_relighting_{}", m->use_alpha() ? "on" : "off",
                                                 m->use_probes() ? "on" : "off", m->use_relighting() ? "on" : "off");
            const auto variant_key = name + (diagnostic_csg ? "/CSG-diagnostic" : "");
            auto it = cubemap_variants_.find(variant_key);
            if (it == cubemap_variants_.end()) {
                const TagHash technique = globals_.pipeline(name);
                if (technique.is_none()) throw std::runtime_error("render_globals has no pipeline " + name);
                const uint32_t index = uint32_t(cubemap_pipelines_.size());
                for (bool front : {true, false}) {
                    auto gp = std::make_unique<GamePipeline>();
                    GamePipeline::Options opt{DXGI_FORMAT_R16G16B16A16_FLOAT, false, {DXGI_FORMAT_R16G16B16A16_FLOAT}};
                    opt.volume = true;
                    opt.stencil_test = diagnostic_csg;
                    opt.cull_front = front;
                    opt.cbuffer_floor = 64;
                    opt.blend_state = 23;  // goriath CubemapRenderer: src + dst * src.a over both targets
                    gp->load(pm, gpu, technique, scopes, opt);
                    cubemap_pipelines_.push_back(std::move(gp));
                }
                it = cubemap_variants_.emplace(variant_key, index).first;
            }
            CubemapVolume v;
            v.pipeline = it->second;
            v.volume = *m;
            const auto raw = texture(m->specular);
            const auto filtered = filtered_cubes_.find(m->specular.value);
            v.specular = filtered != filtered_cubes_.end() ? filtered->second : raw;
            v.alpha = texture(m->alpha);
            v.diffuse = texture(m->diffuse, true);
            if (v.diffuse) {
                const auto d = v.diffuse->GetDesc();
                if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || d.Width != uint64_t(m->probes[0]) * 7 ||
                    d.Height != m->probes[1] || d.DepthOrArraySize != m->probes[2])
                    throw std::runtime_error("Original diffuse grid / Texture3D dimensions do not match");
            }
            if (diagnostic_csg && !m->shape_vertices.is_none()) {
                auto& shape = probe_shapes_[m->shape_vertices.value];
                if (!shape) {
                    const auto mesh = load_vertex_buffer(pm, m->shape_vertices);
                    if (mesh.stride != 12 || mesh.data.empty() || mesh.data.size() % 36)
                        throw std::runtime_error("invalid authored probe boundary triangle list");
                    // The stored CSG is in +local coordinates; the original volume VS negates its
                    // input. Convert that convention exactly, retaining every authored triangle.
                    std::vector<float> vertices(mesh.data.size() / sizeof(float));
                    std::memcpy(vertices.data(), mesh.data.data(), mesh.data.size());
                    for (auto& f : vertices) {
                        if (!std::isfinite(f)) throw std::runtime_error("non-finite probe boundary vertex");
                        f = -f;
                    }
                    shape = gpu.upload(vertices.data(), mesh.data.size());
                }
                v.shape = shape;
                v.shape_vbv = {shape->GetGPUVirtualAddress(), UINT(shape->GetDesc().Width), 12};
                v.shape_vertices = v.shape_vbv.SizeInBytes / 12;
            }
            if (!v.specular || (m->use_alpha() && !v.alpha) || (m->use_probes() && !v.diffuse))
                throw std::runtime_error("volume without its cube / probe textures");
            const auto desc = raw->GetDesc();
            v.max_mip = float(v.specular->GetDesc().MipLevels - 1);
            v.squared = desc.Format != DXGI_FORMAT_BC6H_UF16 && desc.Format != DXGI_FORMAT_BC6H_SF16 &&
                        desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && desc.Format != DXGI_FORMAT_R11G11B10_FLOAT;
            cubemaps_.push_back(std::move(v));
        } catch (const std::exception& ex) {
            ++failures[std::string(ex.what()).substr(0, 120)];
        }
    }
    // The blend keeps what is drawn last on top: large volumes first, nested small ones over them.
    std::stable_sort(cubemaps_.begin(), cubemaps_.end(), [](const CubemapVolume& a, const CubemapVolume& b) {
        const auto& ea = a.volume.extents;
        const auto& eb = b.volume.extents;
        return ea[0] * ea[1] * ea[2] > eb[0] * eb[1] * eb[2];
    });
    for (const auto& [why, n] : failures) cubemap_notes_.push_back(std::format("{} cubemap volumes left out: {}", n, why));
    if (!cubemaps_.empty()) {
        constexpr std::string_view layout = "cubemap_volume scope (cb11) has no bytecode: filled from the SCubemapComponent";
        ext_.note_estimate("cubemaps: cb11 face fades from component +0x50 / +0x60 as fractions of the half extent", layout);
        ext_.note_estimate("cubemaps: cb11 [2].w intensity = component +0x30", layout);
        ext_.note_estimate("cubemaps: cb11 [0] VS = +0x110 (the matrix +0xC0 translation), not +0x20", layout);
        ext_.note_estimate("cubemaps: cb11 [15] = component +0x170 (z remapped), [18].w = 0 for UNORM (gamma-2) cubes (the PS squares them)", layout);
        ext_.note_estimate("cubemaps: GGX filter roughness = mip / last mip, chain down to 4x4",
                           "cubemap_face_filter_ggx inputs (extern 84) are engine-set");
        ext_.note_estimate("cubemaps: draw order largest volume first", "engine sort order not decoded");
        if (diagnostic_csg) ext_.note_estimate("cubemaps: CSG diagnostic (not normal game lighting)",
                           "authored shape membership only; its role in engine lighting application is unverified");
    }
}

void GameLighting::filter_specular_cubes(PackageManager& pm, Gpu& gpu, std::span<const uint32_t> tags) {
    if (tags.empty()) return;
    ID3D12Device* dev = gpu.device();
    constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!cube_filter_) {
        const TagHash technique = globals_.pipeline("cubemap_face_filter_ggx");
        if (technique.is_none()) throw std::runtime_error("render_globals has no pipeline cubemap_face_filter_ggx");
        const Scope* scopes[] = {&frame_scope_, &view_scope_};
        GamePipeline::Options opt{kFormat};
        opt.cbuffer_floor = 16;
        auto gp = std::make_unique<GamePipeline>();
        gp->load(pm, gpu, technique, scopes, opt);
        cube_filter_ = std::move(gp);
    }
    // Extern 84 (cubemap filter state), as the technique's TFX reads it: +0xB0 the face's
    // forward, +0xA0 / +0x90 its axes along the target's x / y, +0xC8 / +0xC4 roughness as mip /
    // last mip, +0xCC the source face size, +0x108 the source cube. D3D cube face order.
    static const float4 kForward[6] = {{1, 0, 0, 0}, {-1, 0, 0, 0}, {0, 1, 0, 0}, {0, -1, 0, 0}, {0, 0, 1, 0}, {0, 0, -1, 0}};
    static const float4 kAxisX[6] = {{0, 0, 1, 0}, {0, 0, -1, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}, {-1, 0, 0, 0}, {1, 0, 0, 0}};
    static const float4 kAxisY[6] = {{0, 1, 0, 0}, {0, 1, 0, 0}, {0, 0, -1, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}};
    constexpr uint8_t kFilter = 84;
    std::array<float4, 16> vs_scope{};  // the full-screen VS reads cb12[13].x (depth) only
    const auto vs_cb = gpu.dynamic(sizeof(vs_scope));
    std::memcpy(vs_cb.cpu, vs_scope.data(), sizeof(vs_scope));
    const ScopeBufferFn scopes = [&](ShaderStage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS { return reg == 12 ? vs_cb.gpu : 0; };

    constexpr size_t kBatch = 16;  // cubes per submission (bounds the temporary source chains)
    constexpr uint32_t kRtvs = 2 * 6 * 9 * kBatch;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvs};
    check_hr(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap (cube filter)");
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    uint32_t rtv_next = 0;
    auto rtv = [&](ID3D12Resource* r, uint32_t mip, uint32_t face) {
        D3D12_RENDER_TARGET_VIEW_DESC d{};
        d.Format = kFormat;
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MipSlice = mip;
        d.Texture2DArray.FirstArraySlice = face;
        d.Texture2DArray.ArraySize = 1;
        const D3D12_CPU_DESCRIPTOR_HANDLE h{rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(rtv_next++ % kRtvs) * inc};
        dev->CreateRenderTargetView(r, &d, h);
        return h;
    };
    auto make = [&](uint32_t size, uint32_t mips) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = rd.Height = size;
        rd.DepthOrArraySize = 6;
        rd.MipLevels = UINT16(mips);
        rd.Format = kFormat;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r;
        check_hr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&r)),
                 "CreateCommittedResource (filtered cube)");
        return r;
    };
    auto to_read = [&](ID3D12GraphicsCommandList* cl, ID3D12Resource* r, uint32_t mip, uint32_t mips) {
        D3D12_RESOURCE_BARRIER b[6]{};
        for (uint32_t f = 0; f < 6; ++f) {
            b[f].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[f].Transition = {r, mip + f * mips, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        }
        cl->ResourceBarrier(6, b);
    };
    bool validated = false;
    auto pass = [&](ID3D12GraphicsCommandList* cl, ID3D12Resource* target, uint32_t mip, uint32_t size, ID3D12Resource* source,
                    uint32_t source_mip, float roughness_mip, float last_mip, float source_size) {
        const D3D12_VIEWPORT vp{0, 0, float(size), float(size), 0, 1};
        const D3D12_RECT sc{0, 0, LONG(size), LONG(size)};
        for (uint32_t f = 0; f < 6; ++f) {
            ext_.set(kFilter, 0xB0, kForward[f]);
            ext_.set(kFilter, 0xA0, kAxisX[f]);
            ext_.set(kFilter, 0x90, kAxisY[f]);
            ext_.set(kFilter, 0xC8, roughness_mip);
            ext_.set(kFilter, 0xC4, last_mip);
            ext_.set(kFilter, 0xCC, source_size);
            ext_.set_texture(kFilter, 0x108, source, DXGI_FORMAT_UNKNOWN, 0, 0, source_mip);
            const auto h = rtv(target, mip, f);
            cl->OMSetRenderTargets(1, &h, FALSE, nullptr);
            cl->RSSetViewports(1, &vp);
            cl->RSSetScissorRects(1, &sc);
            cube_filter_->draw(gpu, cl, ext_, scopes, validated);
            validated = true;
        }
    };
    for (size_t first = 0; first < tags.size(); first += kBatch) {
        // Each batch runs on the immediate list and is flushed before the next: its per-draw
        // descriptors are reused, and its source chains live until the GPU is done with them.
        const uint32_t mark = gpu.transient_mark();
        std::vector<ComPtr<ID3D12Resource>> temps;
        auto* cl = gpu.immediate();
        try {
            for (size_t i = first; i < tags.size() && i < first + kBatch; ++i) {
                auto& raw = probe_textures_[tags[i]];
                if (!raw) raw = gpu.texture(load_texture(pm, TagHash(tags[i])));
                const auto rd = raw->GetDesc();
                if (rd.DepthOrArraySize != 6 || rd.Width != rd.Height) continue;
                const uint32_t size = uint32_t(rd.Width);
                const uint32_t mips = std::max(1, int(std::bit_width(size)) - 2);  // down to 4x4
                // Source chain: mip 0 resampled from the stored cube, each further mip a bilinear
                // half of the one above (the filter's copy path, roughness 0).
                auto src = make(size, mips);
                pass(cl, src.Get(), 0, size, raw.Get(), 0, 0, 1, float(size));
                to_read(cl, src.Get(), 0, mips);
                for (uint32_t m = 1; m < mips; ++m) {
                    pass(cl, src.Get(), m, std::max(1u, size >> m), src.Get(), m - 1, 0, 1, float(size));
                    to_read(cl, src.Get(), m, mips);
                }
                // The filtered chain the cubemap_apply PS samples by roughness.
                auto dst = make(size, mips);
                for (uint32_t m = 0; m < mips; ++m) {
                    pass(cl, dst.Get(), m, std::max(1u, size >> m), src.Get(), 0, float(m), float(mips - 1), float(size));
                    to_read(cl, dst.Get(), m, mips);
                }
                temps.push_back(std::move(src));
                filtered_cubes_[tags[i]] = std::move(dst);
            }
        } catch (...) {
            gpu.flush();  // what was recorded still refers to the source chains
            gpu.transient_rewind(mark);
            for (size_t i = first; i < tags.size() && i < first + kBatch; ++i) filtered_cubes_.erase(tags[i]);
            throw;
        }
        gpu.flush();
        gpu.transient_rewind(mark);
    }
    ext_.set_texture(kFilter, 0x108, nullptr);
}

void GameLighting::draw_cubemaps(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes, bool distance_fade) {
    ext_.note_estimate(distance_fade ? "cubemaps: cb11 [14].w = camera distance fade starting at 250 m or farther" :
                                     "cubemaps: cb11 [14].w = 1 (full-map probe viewing)",
                       "engine distance fade semantics unverified; full-map mode preserves authored spatial bounds, samples and intensities");
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const auto base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{base.ptr + 3 * inc}, {base.ptr + inc}};  // light diffuse, specular IBL
    // Depth plane stays readable by the original PS. Only the separate stencil plane is
    // written; the scene's reversed float depth is never changed by volume membership.
    const auto depth_read = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto stencil_state = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {gb.depth.Get(), 1, before, after};
        cl->ResourceBarrier(1, &b);
    };
    if (cubemap_csg_diagnostic_) {
        stencil_state(depth_read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cl->OMSetStencilRef(0);
    }
    cl->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    constexpr float kHardEdge = 1e-3f;  // a face with no blend: fades over the last 0.1% of its half extent
    auto upload = [&](std::span<const float4> values) {
        auto a = gpu.dynamic(values.size() * sizeof(float4));
        std::memcpy(a.cpu, values.data(), values.size() * sizeof(float4));
        return a.gpu;
    };
    // The volume VS expects the same backward camera axis as the transparent PS.
    // Reuse the frame's view scope; negating cb12[6] again places receivers behind the camera.
    const LightFrustum frustum(vm);
    for (const auto& cv : cubemaps_) {
        const MapCubemap& m = cv.volume;
        if (frustum.outside(m.lo, m.hi)) continue;
        float fade = 1.0f;
        if (distance_fade && m.fade_end > m.fade_start) {
            const std::array<float, 3> eye{vm.eye.x, vm.eye.y, vm.eye.z};
            float d2 = 0;  // distance to the volume's box, not to its center
            for (size_t a = 0; a < 3; ++a) {
                const float d = eye[a] - std::clamp(eye[a], m.lo[a], m.hi[a]);
                d2 += d * d;
            }
            const float start = std::max(250.0f, m.fade_start);
            const float width = std::max(50.0f, m.fade_end - m.fade_start);
            fade = std::clamp(1.0f - (std::sqrt(d2) - start) / width, 0.0f, 1.0f);
        }
        if (fade <= 0) continue;
        std::array<float4, 4> vs{};
        // The box is the unit cube through the matrix +0xC0: its translation is +0x110, and
        // quaternion +0x100 x half extents +0x10 equal its columns to 1e-5 on every volume. +0x20 is
        // a different point (266 of Outpost's 313 volumes, metres away); placing the grid there made
        // overlapping volumes disagree (probe luminance correlation 0.34, against 0.82 at +0x110).
        const auto& center = m.origin;
        vs[0] = {center[0], center[1], center[2], 1};
        vs[1] = {m.rotation[0], m.rotation[1], m.rotation[2], m.rotation[3]};
        vs[2] = {m.extents[0], m.extents[1], m.extents[2], 0};
        std::array<float4, 32> ps{};
        // Face fades: 1 - saturate(|x| * scale - offset) over the outer `blend` of each half extent.
        // They are also the volume's only bound: the box is drawn by its far faces, so every pixel
        // whose ray crosses it runs the PS, and a face without a blend must still cut off at |x| = 1
        // (a hard edge), not leave the factor at 1 for the scene in front of and around the box.
        for (int a = 0; a < 3; ++a) {
            const float bn = std::max(m.blend_neg[size_t(a)], kHardEdge), bp = std::max(m.blend_pos[size_t(a)], kHardEdge);
            ps[0][size_t(a)] = 1.0f / bn, ps[2][size_t(a)] = (1.0f - bn) / bn;
            ps[1][size_t(a)] = 1.0f / bp, ps[3][size_t(a)] = (1.0f - bp) / bp;
        }
        ps[2][3] = m.unk30;
        ps[3][3] = cv.max_mip;
        for (int c = 0; c < 4; ++c)
            ps[size_t(5 + c)] = {m.local_to_world[size_t(c * 4)], m.local_to_world[size_t(c * 4 + 1)], m.local_to_world[size_t(c * 4 + 2)],
                                 m.local_to_world[size_t(c * 4 + 3)]};
        for (int c = 0; c < 3; ++c) ps[size_t(9 + c)] = {m.axes[size_t(c * 4)], m.axes[size_t(c * 4 + 1)], m.axes[size_t(c * 4 + 2)], 0};
        ps[14] = {1.0f / float(m.probes[0]), 1.0f / float(m.probes[1]), 1.0f / float(m.probes[2]), fade};
        ps[15] = {m.probe_params[0], m.probe_params[1], cubemap_alpha_exponent(m.probe_params[2]), m.probe_params[3]};
        ps[18][3] = cv.squared ? 0.0f : 1.0f;  // zero: square the samples
        if (m.use_relighting()) {
            if (ext_.channels.size() <= 0x8B) throw std::runtime_error("Missing original cubemap relighting channels");
            const auto& ch = ext_.channels;
            const auto relit = cubemap_relighting(m, {ch[0x8B][0], ch[0x8A][0], ch[0x89][0], ch[0x08][0], ch[0x09], ch[0x64]});
            ps[19] = relit[0];
            ps[20] = relit[1];
            ext_.note_estimate("cubemap relighting cb11[19..20] from cubemap_bounce_scale, cubemap_relighting_sky_intensity, channel 8F552B79, sun_intensity / 10, sun_color and sun_light_direction; cb11[15].z = 2z, or 60 (2z - 1)^4 + 1 above 0.5",
                "formula of the D2 engine's cubemap constant function (Alkahest); the Marathon PS reads the same slots, its engine code is not in game data. sun_light_direction is the channel's value (nim does not write the sun into it)");
        }
        const auto vs_cb = upload(vs), ps_cb = upload(ps);
        const ScopeBufferFn volume_scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
            if (reg == 11) return stage == ShaderStage::Vertex ? vs_cb : ps_cb;
            return scopes(stage, reg);
        };
        // The VS places -position (the cube turned inside out); the far side is drawn as for lights.
        M4 l2w = m4(m.local_to_world);
        for (int c = 0; c < 3; ++c)
            for (auto& x : l2w.c[c]) x = -x;
        const bool mirrored = det4(mul(vm.vp_abs, l2w)) > 0;
        GamePipeline& gp = *cubemap_pipelines_[cv.pipeline + (mirrored ? 1 : 0)];
        std::vector<std::pair<uint32_t, ID3D12Resource*>> textures{{0, cv.specular.Get()}};
        if (cv.alpha) textures.push_back({1, cv.alpha.Get()});
        if (cv.diffuse) textures.push_back({2, cv.diffuse.Get()});
        try {
          if (cubemap_csg_diagnostic_) {
            cl->OMSetRenderTargets(0, nullptr, FALSE, &gb.dsv_ro);
            cl->ClearDepthStencilView(gb.dsv_ro, D3D12_CLEAR_FLAG_STENCIL, 0, 0, 0, nullptr);
            if (cv.shape) {
                const D3D12_INDEX_BUFFER_VIEW no_indices{};
                cubemap_stencil_->draw_volume(gpu, cl, ext_, volume_scopes, nullptr, cv.shape_vbv, no_indices, cv.shape_vertices);
            } else {
                cubemap_stencil_->draw_volume(gpu, cl, ext_, volume_scopes, nullptr, cube_vbv_, cube_ibv_, 36);
            }
            cl->OMSetRenderTargets(2, rtvs, FALSE, &gb.dsv_ro);
          }
            gp.draw_volume(gpu, cl, ext_, volume_scopes, nullptr, cube_vbv_, cube_ibv_, 36, textures);
            ++cubemaps_drawn_;
        } catch (const std::exception& ex) {
            ++cubemaps_failed_;
            if (cubemap_notes_.size() < 32) cubemap_notes_.push_back(std::format("cubemap {}: {}", m.specular.str(), ex.what()));
        }
    }
    cl->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
    if (cubemap_csg_diagnostic_) stencil_state(D3D12_RESOURCE_STATE_DEPTH_WRITE, depth_read);
}

} // namespace nim
