#include "nim/content/tfx.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <stdexcept>

namespace nim {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFF;

// Vertex element format codes used by the game's input layout tables (index -> DXGI, byte size).
struct ElemFormat { uint32_t dxgi; uint32_t size; };
constexpr ElemFormat kElemFormats[34] = {
    {0, 0}, {41, 4}, {16, 8}, {6, 12}, {2, 16}, {28, 4}, {30, 4}, {38, 4}, {14, 8}, {12, 8},
    {37, 4}, {13, 8}, {34, 4}, {10, 8}, {32, 4}, {31, 4}, {25, 4}, {24, 4}, {43, 4}, {18, 8},
    {4, 16}, {42, 4}, {17, 8}, {3, 16}, {59, 2}, {61, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {29, 4}, {26, 4}, {13, 8},
};
constexpr const char* kSemantics[9] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE",
                                       "TEXCOORD", "TANGENT",     "BINORMAL",     "COLOR"};

// Reads a dynamic core (textures, bytecode, constants, samplers, cbuffer) starting at `core`.
TechniqueStage read_core(PackageManager& pm, const TagReader& r, size_t core) {
    TechniqueStage st;

    auto tex = r.vec_at(core + 0x00);  // 24-byte entries: u32 slot, pad, WideHash {u32 h32, pad, u64 h64}
    for (uint64_t i = 0; i < tex.count; ++i) {
        size_t e = tex.data + size_t(i) * 24;
        if (auto t = resolve_wide_hash(pm, r.u32_at(e + 8), r.u64_at(e + 16)))
            st.textures.push_back({r.u32_at(e), *t});
    }
    auto bc = r.vec_at(core + 0x18);
    st.bytecode.assign(r.span().begin() + bc.data, r.span().begin() + bc.data + bc.count);
    auto k = r.vec_at(core + 0x28);
    st.constants.resize(size_t(k.count));
    for (uint64_t i = 0; i < k.count; ++i)
        for (int c = 0; c < 4; ++c) st.constants[i][c] = r.f32_at(k.data + size_t(i) * 16 + c * 4);
    auto init = r.vec_at(core + 0x48);
    st.cbuffer_init.resize(size_t(init.count));
    for (uint64_t i = 0; i < init.count; ++i)
        for (int c = 0; c < 4; ++c) st.cbuffer_init[i][c] = r.f32_at(init.data + size_t(i) * 16 + c * 4);
    auto smp = r.vec_at(core + 0x38);  // 16-byte entries, TagHash first
    for (uint64_t i = 0; i < smp.count; ++i) st.samplers.push_back(r.tag_at(smp.data + size_t(i) * 16));
    st.cbuffer_slot = int32_t(r.u32_at(core + 0x60));
    st.cbuffer = r.tag_at(core + 0x64);
    // A stage may keep its cbuffer contents in a separate buffer (header 32/7 -> data 40/7)
    // instead of inline; the inline array then holds a single placeholder register.
    if (!st.cbuffer.is_none()) {
        if (const Entry* e = pm.entry(st.cbuffer); e && e->type == 32) {
            auto data = pm.read(TagHash(e->reference));
            std::vector<std::array<float, 4>> regs(data.size() / 16);
            std::memcpy(regs.data(), data.data(), regs.size() * 16);
            if (regs.size() > st.cbuffer_init.size()) st.cbuffer_init = std::move(regs);
        }
    }
    return st;
}

TechniqueStage read_stage(PackageManager& pm, const TagReader& r, size_t o) {
    TechniqueStage st = read_core(pm, r, o + 8);
    st.shader = r.tag_at(o);
    return st;
}

std::string c_string_at(const TagReader& r, size_t off) { return r.cstr_at(off); }

size_t bits_per_pixel(uint32_t f) {
    switch (f) {
    case 2: case 3: case 4: return 128;
    case 6: return 96;
    case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 18: return 64;
    case 71: case 72: case 80: case 81: return 4;  // BC1, BC4
    case 74: case 75: case 77: case 78: case 83: case 84: case 95: case 96: case 98: case 99: return 8;  // BC2/3/5/6/7
    case 56: case 57: case 59: case 61: return f == 61 ? 8 : 16;
    case 65: return 8;
    default: return 32;
    }
}
bool is_block_compressed(uint32_t f) { return f >= 70 && f <= 84 || f >= 94 && f <= 99; }

} // namespace

std::optional<TagHash> resolve_wide_hash(PackageManager& pm, uint32_t hash32, uint64_t hash64) {
    if (hash32 != kNone && hash32 != 0) return TagHash(hash32);
    if (hash64 == 0) return std::nullopt;
    return pm.resolve64(hash64);
}

Technique load_technique(PackageManager& pm, TagHash tag) {
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    Technique t;
    t.tag = tag;
    t.bind_mode = r.u32_at(0x08);
    for (size_t s = 0; s < size_t(ShaderStage::Count); ++s) t.stages[s] = read_stage(pm, r, 0x58 + s * 0x88);
    return t;
}

std::vector<uint8_t> load_shader_bytecode(PackageManager& pm, TagHash header) {
    const Entry* e = pm.entry(header);
    if (!e) throw std::runtime_error("missing shader header " + header.str());
    TagHash data(e->reference);  // header.reference -> type 41 bytecode
    auto bc = pm.read(data);
    if (bc.size() < 4 || std::memcmp(bc.data(), "DXBC", 4) != 0)
        throw std::runtime_error("not DXBC: " + data.str());
    return bc;
}

StaticMesh load_static_mesh(PackageManager& pm, TagHash tag) {
    StaticMesh m;
    m.tag = tag;
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    m.data_tag = r.tag_at(0x08);
    auto techs = r.vec_at(0x10);
    for (uint64_t i = 0; i < techs.count; ++i) m.techniques.push_back(r.tag_at(techs.data + size_t(i) * 4));

    auto db = pm.read(m.data_tag);
    TagReader d(db);
    auto groups = d.vec_at(0x08);  // 6 bytes each
    for (uint64_t i = 0; i < groups.count; ++i) {
        size_t o = groups.data + size_t(i) * 6;
        m.groups.push_back({d.u16_at(o), d.u8_at(o + 2), d.u8_at(o + 3)});
    }
    auto parts = d.vec_at(0x18);  // 12 bytes each
    for (uint64_t i = 0; i < parts.count; ++i) {
        size_t o = parts.data + size_t(i) * 12;
        m.parts.push_back({d.u32_at(o), d.u32_at(o + 4), d.u8_at(o + 8), d.u8_at(o + 10), d.u8_at(o + 11)});
    }
    auto bufs = d.vec_at(0x28);  // 4 TagHashes each
    for (uint64_t i = 0; i < bufs.count; ++i) {
        size_t o = bufs.data + size_t(i) * 16;
        m.buffers.push_back({d.tag_at(o), d.tag_at(o + 4), d.tag_at(o + 8), d.tag_at(o + 12)});
    }
    for (int c = 0; c < 3; ++c) m.mesh_offset[c] = d.f32_at(0x40 + c * 4);
    m.mesh_scale = d.f32_at(0x4C);
    m.texcoord_scale = d.f32_at(0x50);
    m.texcoord_offset = {d.f32_at(0x54), d.f32_at(0x58)};
    return m;
}

VertexBuffer load_vertex_buffer(PackageManager& pm, TagHash header) {
    auto h = pm.read(header);
    BinaryReader r(h);
    VertexBuffer vb;
    vb.stride = r.u16_at(4);
    vb.vtype = r.u16_at(6);
    vb.data = pm.read(TagHash(pm.entry(header)->reference));
    return vb;
}

IndexBuffer load_index_buffer(PackageManager& pm, TagHash header) {
    auto h = pm.read(header);
    BinaryReader r(h);
    IndexBuffer ib;
    ib.is_32bit = r.u8_at(1) != 0;
    ib.data = pm.read(TagHash(pm.entry(header)->reference));
    return ib;
}

Texture load_texture(PackageManager& pm, TagHash header) {
    auto h = pm.read(header);
    BinaryReader r(h);
    Texture t;
    t.dxgi_format = r.u32_at(0x04);
    if (r.u16_at(0x20) != 0xCAFE) throw std::runtime_error("texture header without 0xCAFE: " + header.str());
    t.width = r.u16_at(0x22);
    t.height = r.u16_at(0x24);
    t.depth = r.u16_at(0x26);
    t.array_size = r.u16_at(0x28);
    uint16_t mips_hint = r.u8_at(0x2D);
    TagHash large(r.u32_at(0x3C));

    uint16_t smallest = std::min(t.width, t.height);
    uint16_t full = 1;
    while ((1u << full) <= smallest) ++full;
    t.mip_count = std::max<uint16_t>(1, std::min<uint16_t>(full, mips_hint ? mips_hint : 1));

    // The large buffer, when present, holds the complete mip chain; otherwise the data entry does.
    if (!large.is_none() && pm.entry(large)) t.data = pm.read(large);
    else t.data = pm.read(TagHash(pm.entry(header)->reference));

    // Trim the mip count to what the data actually covers.
    size_t bpp = bits_per_pixel(t.dxgi_format), total = 0;
    uint16_t covered = 0;
    for (uint16_t m = 0; m < t.mip_count; ++m) {
        size_t w = std::max(1, t.width >> m), hh = std::max(1, t.height >> m);
        size_t sz = is_block_compressed(t.dxgi_format) ? ((w + 3) / 4) * ((hh + 3) / 4) * bpp * 2
                                                        : w * hh * bpp / 8;
        if (total + sz * t.array_size * t.depth > t.data.size()) break;
        total += sz * t.array_size * t.depth;
        covered++;
    }
    t.mip_count = std::max<uint16_t>(1, covered);
    return t;
}

TagHash RenderGlobals::pipeline(std::string_view name) const {
    for (const auto& [n, t] : pipelines)
        if (n == name) return t;
    return TagHash();
}
TagHash RenderGlobals::scope(std::string_view name) const {
    for (const auto& [n, t] : scopes)
        if (n == name) return t;
    return TagHash();
}

RenderGlobals load_render_globals(PackageManager& pm) {
    RenderGlobals g;
    // Class 8080B61C is shared by many small tables; the engine looks this one up by name.
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (const auto& nt : p.named_tags())
            if (nt.name == "render_globals" && (g.tag.is_none() || info.name.find("patchable") != std::string::npos))
                g.tag = nt.hash;
    }
    if (g.tag.is_none()) throw std::runtime_error("no named tag \"render_globals\"");
    auto top_bytes = pm.read(g.tag);
    TagReader top(top_bytes);
    auto list = top.vec_at(0x08);  // 0x10-byte entries, data Tag at +8
    if (!list.count) throw std::runtime_error("render globals: empty");
    auto bytes = pm.read(top.tag_at(list.data + 8));
    TagReader r(bytes);
    auto named = [&](size_t vec_off, auto& out) {
        auto v = r.vec_at(vec_off);  // 0x10-byte entries: name pointer, u32, Tag
        for (uint64_t i = 0; i < v.count; ++i) {
            size_t e = v.data + size_t(i) * 0x10;
            out.emplace_back(c_string_at(r, r.pointer_at(e)), r.tag_at(e + 0x0C));
        }
    };
    named(0x10, g.scopes);
    named(0x20, g.pipelines);
    if (TagHash lut = r.tag_at(0x30); !lut.is_none()) {
        auto lb = pm.read(lut);
        TagReader l(lb);
        g.specular_tint_lookup = l.tag_at(0x08);
        g.specular_lobe_lookup = l.tag_at(0x0C);
        g.specular_lobe_3d_lookup = l.tag_at(0x10);
        g.iridescence_lookup = l.tag_at(0x14);
    }
    if (TagHash ch = r.tag_at(0x34); !ch.is_none()) {
        auto cb = pm.read(ch);
        TagReader c(cb);
        auto ids = c.vec_at(0x08);
        for (uint64_t i = 0; i < ids.count; ++i) g.channel_ids.push_back(c.u32_at(ids.data + size_t(i) * 4));
        auto vals = c.vec_at(0x18);
        for (uint64_t i = 0; i < vals.count; ++i) {
            std::array<float, 4> v;
            for (int k = 0; k < 4; ++k) v[k] = c.f32_at(vals.data + size_t(i) * 16 + k * 4);
            g.channel_defaults.push_back(v);
        }
    }
    return g;
}

Scope load_scope(PackageManager& pm, TagHash tag) {
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    Scope s;
    s.tag = tag;
    s.name = c_string_at(r, r.pointer_at(0x08));
    for (size_t i = 0; i < size_t(ScopeStage::Count); ++i) s.stages[i] = read_core(pm, r, 0x48 + i * 0x80);
    return s;
}

std::vector<std::vector<InputElement>> load_input_layouts(PackageManager& pm) {
    // The newest copy of the layout table lives in the patchable bootstrap package.
    std::optional<TagHash> table;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i)
            if (p.entries()[i].type == 8 && p.entries()[i].reference == 0x80808661 &&
                (!table || info.name.find("patchable") != std::string::npos))
                table = TagHash::make(info.pkg_id, uint16_t(i));
    }
    if (!table) throw std::runtime_error("no vertex input layout table (class 80808661)");
    auto tb = pm.read(*table);
    TagReader t(tb);

    auto sets_bytes = pm.read(t.tag_at(0x0C));
    TagReader s(sets_bytes);
    auto sets = s.vec_at(0x08);
    std::vector<std::vector<std::array<uint8_t, 3>>> element_sets(size_t(sets.count));
    for (uint64_t i = 0; i < sets.count; ++i) {
        auto el = s.vec_at(sets.data + size_t(i) * 16);
        for (uint64_t k = 0; k < el.count; ++k) {
            size_t o = el.data + size_t(k) * 3;
            element_sets[i].push_back({s.u8_at(o), s.u8_at(o + 1), s.u8_at(o + 2)});
        }
    }

    auto map_bytes = pm.read(t.tag_at(0x34));
    TagReader m(map_bytes);
    auto layouts = m.vec_at(0x08);
    std::vector<std::vector<InputElement>> out(256);
    for (uint64_t i = 0; i < layouts.count; ++i) {
        size_t o = layouts.data + size_t(i) * 0x1C;
        uint8_t index = m.u8_at(o);
        for (uint32_t b = 0; b < 4; ++b) {
            uint32_t set = m.u32_at(o + 8 + b * 4);
            if (set == kNone || set >= element_sets.size()) continue;
            bool instanced = m.u8_at(o + 0x18 + b) != 0;
            for (auto [sem, sem_idx, fmt] : element_sets[set]) {
                InputElement e;
                e.semantic = sem < 9 ? kSemantics[sem] : "UNKNOWN";
                e.semantic_index = sem_idx;
                e.dxgi_format = fmt < 34 ? kElemFormats[fmt].dxgi : 0;
                e.size = fmt < 34 ? kElemFormats[fmt].size : 0;
                e.buffer = b;
                e.instanced = instanced;
                out[index].push_back(e);
            }
        }
    }
    return out;
}

std::vector<ShaderBinding> shader_bindings(std::span<const uint8_t> bc) {
    auto u32 = [&](size_t o) {
        if (o + 4 > bc.size()) throw std::runtime_error("DXBC truncated");
        uint32_t v;
        std::memcpy(&v, bc.data() + o, 4);
        return v;
    };
    if (bc.size() < 32 || std::memcmp(bc.data(), "DXBC", 4) != 0) throw std::runtime_error("not DXBC");
    std::vector<ShaderBinding> out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts; ++i) {
        const size_t part = u32(32 + size_t(i) * 4);
        if (std::memcmp(bc.data() + part, "PSV0", 4) != 0) continue;
        size_t o = part + 8;
        o += 4 + u32(o);  // PSVRuntimeInfo
        const uint32_t count = u32(o);
        o += 4;
        if (!count) return out;
        const uint32_t stride = u32(o);
        o += 4;
        for (uint32_t r = 0; r < count; ++r, o += stride) {
            ShaderBinding b;
            switch (u32(o)) {  // PSVResourceType
            case 1: b.type = BindingType::Sampler; break;
            case 2: b.type = BindingType::Cbv; break;
            case 3: case 4: case 5: b.type = BindingType::Srv; break;
            case 6: case 7: case 8: case 9: b.type = BindingType::Uav; break;
            default: break;
            }
            b.space = u32(o + 4);
            b.lower = u32(o + 8);
            b.upper = u32(o + 12);
            if (stride >= 20) b.kind = BindingKind(std::min<uint32_t>(u32(o + 16), uint32_t(BindingKind::Sampler)));
            out.push_back(b);
        }
        return out;
    }
    throw std::runtime_error("DXBC has no PSV0 part");
}

SamplerDesc load_sampler(PackageManager& pm, TagHash tag) {
    const Entry* e = pm.entry(tag);
    if (!e) throw std::runtime_error("missing sampler " + tag.str());
    auto data = pm.read(TagHash(e->reference));
    SamplerDesc d;
    static_assert(sizeof(SamplerDesc) == 52);
    if (data.size() < sizeof(d)) throw std::runtime_error("short sampler " + tag.str());
    std::memcpy(&d, data.data(), sizeof(d));
    return d;
}

} // namespace nim
