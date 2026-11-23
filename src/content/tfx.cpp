#include "nim/content/tfx.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <set>
#include <stdexcept>
#include <unordered_set>

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
        const auto* entry = pm.entry(st.cbuffer);
        if (!entry) throw std::runtime_error("Missing original constant-buffer header " + st.cbuffer.str());
        const auto data = pm.read(TagHash(entry->reference));
        if (data.size() % sizeof(float4)) throw std::runtime_error("Invalid original constant-buffer byte size");
        // A referenced buffer takes precedence, even when it is shorter than the inline array.
        st.cbuffer_init.resize(data.size() / sizeof(float4));
        if (!data.empty()) std::memcpy(st.cbuffer_init.data(), data.data(), data.size());
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
    t.used_scopes = r.u64_at(0x20);
    t.states = r.u32_at(0x30);
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
    auto special = r.vec_at(0x20);
    for (uint64_t i = 0; i < special.count; ++i) {
        const size_t o = special.data + size_t(i) * 0x20;
        StaticMeshSpecialPart p;
        p.render_stage = r.u8_at(o);
        p.input_layout = r.u8_at(o + 1);
        p.lod_category = r.u8_at(o + 2);
        p.primitive_type = r.u8_at(o + 3);
        p.index = r.tag_at(o + 4);
        p.vertex0 = r.tag_at(o + 8);
        p.vertex1 = r.tag_at(o + 0x0C);
        p.color = r.tag_at(o + 0x10);
        p.index_start = r.u32_at(o + 0x14);
        p.index_count = r.u32_at(o + 0x18);
        p.technique = r.tag_at(o + 0x1C);
        m.special_parts.push_back(p);
    }

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

// SDynamicModel (8080881C): meshes (808087CB, 0x88 bytes) of parts (808087D1, 0x28 bytes), then
// the position scale / offset and texcoord transform the vertex shader applies.
DynamicModel load_dynamic_model(PackageManager& pm, TagHash tag) {
    DynamicModel m;
    m.tag = tag;
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    auto meshes = r.vec_at(0x10);
    m.bone_count = r.u32_at(0x98);
    for (uint64_t i = 0; i < meshes.count; ++i) {
        const size_t o = meshes.data + size_t(i) * 0x88;
        DynamicMesh dm;
        dm.vertex0 = r.tag_at(o + 0x00);
        dm.vertex1 = r.tag_at(o + 0x04);
        dm.weights = r.tag_at(o + 0x08);
        dm.index = r.tag_at(o + 0x10);
        dm.color = r.tag_at(o + 0x14);
        dm.skinning = r.tag_at(o + 0x18);
        auto parts = r.vec_at(o + 0x20);
        for (uint64_t k = 0; k < parts.count; ++k) {
            const size_t q = parts.data + size_t(k) * 0x28;
            DynamicMeshPart p;
            p.technique = r.tag_at(q);
            p.variant_shader_index = r.u16_at(q + 4);
            p.primitive_type = r.u8_at(q + 6);
            p.index_start = r.u32_at(q + 8);
            p.index_count = r.u32_at(q + 0x0C);
            p.external_identifier = r.u16_at(q + 0x14);
            p.flags = r.u32_at(q + 0x18);
            p.lod_role = r.u16_at(q + 0x1C);
            p.lod_category = r.u8_at(q + 0x21);
            dm.parts.push_back(p);
        }
        for (size_t s = 0; s <= kRenderStageCount; ++s) dm.part_range[s] = r.u16_at(o + 0x30 + s * 2);
        for (size_t s = 0; s < kRenderStageCount; ++s) dm.input_layout[s] = r.u8_at(o + 0x64 + s);
        m.meshes.push_back(std::move(dm));
    }
    for (int c = 0; c < 4; ++c) {
        m.model_scale[c] = r.f32_at(0xA0 + c * 4);
        m.model_offset[c] = r.f32_at(0xB0 + c * 4);
    }
    m.texcoord_scale = {r.f32_at(0xC0), r.f32_at(0xC4)};
    m.texcoord_offset = {r.f32_at(0xC8), r.f32_at(0xCC)};
    return m;
}

TagHash DynamicModelMaterials::technique(uint16_t variant, uint32_t permutation,
                                        std::span<const std::pair<uint32_t, uint32_t>> tags) const {
    if (variant == 0xFFFF || variant >= technique_map.size()) return TagHash();
    const Range& r = technique_map[variant];
    if (r.count == 0) return TagHash();
    // A range's condition rows start at its +8 field; ranges with the same value share the rows
    // (§3.119). UESC 80B80192 has 110 rows for 220 techniques: ranges 0/1 share rows 0..9, 2/5 rows
    // 10..99, 3/4 rows 100..109, and row k of ranges 0 and 1 picks techniques with the same PS
    // (80A4BACF / 80A4BB01 -> 80A4BAC8). Reading the rows from the technique start gave the
    // miniboss's second body range the default palette and the boss's the miniboss palette.
    const size_t first = r.condition_start;
    if (!tags.empty() && uint64_t(first) + r.count <= technique_conditions.size() &&
        uint64_t(r.start) + r.count <= techniques.size()) {
        // 871AC0EA is the "any" value: rows pair each named combination with an any-row, so a
        // character no named row fits (the recruit's type 4CF9B596, which no row names) takes the
        // first any-row instead of an arbitrary permutation. Named values outrank it.
        constexpr uint32_t kAny = 0x871AC0EA;
        auto value_of = [&](uint32_t key) -> std::optional<uint32_t> {
            for (const auto& [k, v] : tags)
                if (k == key) return v;
            return std::nullopt;
        };
        // A group holds when, for each key it names, the character's value is one of the
        // group's values for that key (several values of one key are alternatives).
        auto holds = [&](const std::vector<std::pair<uint32_t, uint32_t>>& group) {
            if (group.empty()) return false;
            for (const auto& [key, unused] : group) {
                const auto v = value_of(key);
                bool ok = false;
                for (const auto& [k, want] : group) {
                    if (k != key) continue;
                    if (want == kAny)
                        ok = true;
                    else
                        ok |= v && *v == want;
                }
                if (!ok) return false;
            }
            return true;
        };
        std::optional<size_t> selected;
        size_t best = 0;
        for (size_t k = 0; k < r.count; ++k) {
            const size_t i = size_t(r.start) + k;
            if (techniques[i].is_none()) continue;
            size_t score = 0;
            bool match = true;
            for (const uint16_t g : technique_conditions[first + k]) {
                if (g >= part_tags.size() || !holds(part_tags[g])) { match = false; break; }
                const bool other = std::ranges::any_of(part_tags[g], [](const auto& kv) { return kv.second == kAny; });
                score += other ? 1 : 16;  // a named value outranks "any"
            }
            if (match && (!selected || score > best)) {
                selected = i;
                best = score;
            }
        }
        if (selected) return techniques[*selected];
    }
    const size_t i = size_t(r.start) + permutation % r.count;
    return i < techniques.size() ? techniques[i] : TagHash();
}

namespace {
constexpr uint32_t kRegionKey = 0x51E7A18D;       // fnv1("region")
constexpr uint32_t kPermutationKey = 0x4C58EF8F;  // fnv1("permutation")
constexpr uint32_t kNoneValue = 0x2CA33BDB;       // fnv1("none")
}  // namespace

std::optional<RegionPermutations> entity_region_permutations(PackageManager& pm, TagHash entity,
                                                             std::span<const std::pair<uint32_t, uint32_t>> tags) {
    const auto bytes = pm.read(entity);
    const TagReader r(bytes);
    const auto comps = r.vec_at(0x08);
    // State keys from the part groups (+0x28 Vec<8080BACC>) of the entity's dynamic model
    // components (resource class 80808678, or 80808655 wrapping one).
    std::set<uint32_t> state_keys;
    for (uint64_t k = 0; k < comps.count; ++k) {
        const TagHash c(r.u32_at(comps.data + size_t(k) * 12));
        const Entry* e = c.is_pkg_ref() ? pm.entry(c) : nullptr;
        if (!e || e->type != 8 || e->reference != kClassComponent) continue;
        try {
            const auto cb = pm.read(c);
            const TagReader cr(cb);
            const uint64_t rel = cr.u64_at(0x18);
            if (rel == 0 || 0x18 + rel < 4 || 0x18 + rel >= cb.size()) continue;
            const size_t base = 0x18 + size_t(rel);
            const uint32_t cls = cr.u32_at(base - 4);
            if (cls != 0x80808678 && cls != 0x80808655) continue;
            const auto groups = cr.vec_at(base + 0x28);
            if (groups.element_class != 0x8080BACC) continue;
            for (uint64_t g = 0; g < groups.count; ++g) {
                const auto pairs = cr.vec_at(groups.data + size_t(g) * 0x18 + 8);
                if (pairs.element_class != 0x8080BAD0) continue;
                for (uint64_t q = 0; q < pairs.count; ++q) {
                    const uint32_t key = cr.u32_at(pairs.data + size_t(q) * 8), value = cr.u32_at(pairs.data + size_t(q) * 8 + 4);
                    if (value == kNoneValue && key != kRegionKey && key != kPermutationKey) state_keys.insert(key);
                }
            }
        } catch (const std::exception&) {
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> state;
    for (const uint32_t key : state_keys) {
        uint32_t value = kNoneValue;
        for (const auto& [k, v] : tags)
            if (k == key) value = v;
        state.push_back({key, value});
    }
    for (uint64_t k = 0; k < comps.count; ++k) {
        const TagHash c(r.u32_at(comps.data + size_t(k) * 12));
        const Entry* e = c.is_pkg_ref() ? pm.entry(c) : nullptr;
        if (!e || e->type != 8 || e->reference != kClassComponent) continue;
        try {
            const auto cb = pm.read(c);
            const TagReader cr(cb);
            const uint64_t rel = cr.u64_at(0x18);
            if (rel == 0 || 0x18 + rel < 4 || 0x18 + rel >= cb.size()) continue;
            const size_t base = 0x18 + size_t(rel);
            if (cr.u32_at(base - 4) != 0x80804034) continue;
            const auto regions = cr.vec_at(base + 0xE8);
            const auto entries = cr.vec_at(base + 0xF8);
            if (regions.element_class != 0x80800070 || entries.element_class != 0x80804033) continue;
            RegionPermutations out;
            out.selector = c;
            std::optional<size_t> best;
            uint64_t best_conditions = 0;
            for (uint64_t i = 0; i < entries.count; ++i) {
                const size_t at = entries.data + size_t(i) * 0x30;
                const auto conditions = cr.vec_at(at + 0x10);
                bool hold = true;
                for (uint64_t q = 0; q < conditions.count && hold; ++q) {
                    const std::pair<uint32_t, uint32_t> want{cr.u32_at(conditions.data + size_t(q) * 8),
                                                             cr.u32_at(conditions.data + size_t(q) * 8 + 4)};
                    hold = std::ranges::find(tags, want) != tags.end();
                }
                if (!hold) continue;
                if (!best || conditions.count > best_conditions) {
                    best = at, best_conditions = conditions.count, out.ties = 0;
                } else if (conditions.count == best_conditions) {
                    ++out.ties;
                }
            }
            if (!best) return std::nullopt;
            out.state = state;
            out.entry = cr.u32_at(*best + 4);
            const auto permutations = cr.vec_at(*best + 0x20);
            for (uint64_t q = 0; q < std::min(permutations.count, regions.count); ++q)
                out.region_permutation.push_back({cr.u32_at(regions.data + size_t(q) * 4), cr.u32_at(permutations.data + size_t(q) * 4)});
            return out;
        } catch (const std::exception&) {
        }
    }
    return std::nullopt;
}

namespace {
// The resource of each component of an entity, with its class (resource pointer at +0x18).
template <typename F> void for_each_component_resource(PackageManager& pm, TagHash entity, F&& f) {
    const auto bytes = pm.read(entity);
    const TagReader r(bytes);
    const auto comps = r.vec_at(0x08);
    for (uint64_t k = 0; k < comps.count; ++k) {
        const TagHash c(r.u32_at(comps.data + size_t(k) * 12));
        const Entry* e = c.is_pkg_ref() ? pm.entry(c) : nullptr;
        if (!e || e->type != 8 || e->reference != kClassComponent) continue;
        try {
            const auto cb = pm.read(c);
            const TagReader cr(cb);
            const uint64_t rel = cr.u64_at(0x18);
            if (rel == 0 || 0x18 + rel < 4 || 0x18 + rel >= cb.size()) continue;
            const size_t base = 0x18 + size_t(rel);
            if (f(c, cr, base, cr.u32_at(base - 4))) return;
        } catch (const std::exception&) {
        }
    }
}
}  // namespace

std::optional<EntityWeapon> entity_weapon(PackageManager& pm, TagHash entity, std::span<const std::pair<uint32_t, uint32_t>> tags) {
    constexpr uint32_t kWeaponType = 0xEAFBB478;  // fnv1("weapon_type")
    std::optional<EntityWeapon> out;
    pm.build_hash64_index();
    for_each_component_resource(pm, entity, [&](TagHash, const TagReader& r, size_t base, uint32_t cls) {
        if (cls != 0x80804656) return false;
        const auto sets = r.vec_at(base + 0x28);
        const auto rows = r.vec_at(base + 0x190);
        if (sets.element_class != 0x8080BACC || rows.element_class != 0x8080465E) return false;
        for (uint64_t i = 0; i < rows.count; ++i) {
            const size_t row = rows.data + size_t(i) * 0x40;
            const uint32_t set = r.u32_at(row + 8);
            if (set >= sets.count) continue;
            const auto pairs = r.vec_at(sets.data + size_t(set) * 0x18 + 8);
            if (!pairs.count) continue;
            bool hold = true;
            uint32_t weapon_type = 0;
            for (uint64_t q = 0; q < pairs.count && hold; ++q) {
                const std::pair<uint32_t, uint32_t> want{r.u32_at(pairs.data + size_t(q) * 8), r.u32_at(pairs.data + size_t(q) * 8 + 4)};
                hold = std::ranges::find(tags, want) != tags.end();
                if (want.first == kWeaponType) weapon_type = want.second;
            }
            if (!hold) continue;
            const auto weapons = r.vec_at(row + 0x30);
            if (!weapons.count || weapons.element_class != 0x80804664) continue;
            const auto weapon = pm.resolve64(r.u64_at(weapons.data + 0x10));
            if (!weapon) continue;
            out = EntityWeapon{weapon_type, *weapon, {}};
            return true;
        }
        return false;
    });
    if (!out) return std::nullopt;
    for_each_component_resource(pm, out->weapon, [&](TagHash, const TagReader& r, size_t base, uint32_t cls) {
        if (cls != 0x808032B3) return false;
        if (const auto visual = pm.resolve64(r.u64_at(base + 0x160))) out->visual = *visual;
        return true;
    });
    return out;
}

namespace {
using MaterialDefaults = std::map<uint32_t, std::array<float, 4>>;

MaterialDefaults material_defaults(const TagReader& r, size_t definition) {
    MaterialDefaults result;
    const auto channels = r.vec_at(definition + 0x120);
    if (channels.element_class != 0x8080AF86) return result;
    for (uint64_t c = 0; c < channels.count; ++c) {
        const size_t at = channels.data + size_t(c) * 0x70;
        const auto constants = r.vec_at(at + 0x18);
        if (!constants.count) continue;
        std::array<float, 4> value{};
        for (int k = 0; k < 4; ++k) value[k] = r.f32_at(constants.data + k * 4);
        result[r.u32_at(at)] = value;
    }
    return result;
}
}

MaterialDefaults entity_material_defaults(PackageManager& pm, TagHash entity) {
    MaterialDefaults defaults;
    for_each_component_resource(pm, entity, [&](TagHash, const TagReader& r, size_t base, uint32_t cls) {
        if (cls == 0x8080AF75) defaults.merge(material_defaults(r, base));
        return false;
    });
    return defaults;
}

bool part_group_visible(std::span<const std::pair<uint32_t, uint32_t>> group_tags, const RegionPermutations& regions) {
    constexpr uint32_t kRegion = kRegionKey;
    constexpr uint32_t kPermutation = kPermutationKey;
    // An alternate part set of a state key (the helmet decals of 6A807A1A = 7CB23A4E and of
    // {..., none}) is drawn only for the character's state. Both were drawn before, and the
    // alternate pass in scene.cpp kept the lower external identifier (the 7CB23A4E set).
    for (const auto& [key, value] : regions.state) {
        bool names = false, holds = false;
        for (const auto& [k, v] : group_tags)
            if (k == key) names = true, holds = holds || v == value;
        if (names && !holds) return false;
    }
    bool names_permutation = false, constrained = false;
    for (const auto& [k, v] : group_tags) names_permutation |= k == kPermutation;
    if (!names_permutation) return true;
    for (const auto& [k, region] : group_tags) {
        if (k != kRegion) continue;
        for (const auto& [r, permutation] : regions.region_permutation) {
            if (r != region) continue;
            constrained = true;
            if (std::ranges::find(group_tags, std::pair{kPermutation, permutation}) != group_tags.end()) return true;
        }
    }
    return !constrained;  // a region the selector does not list is not narrowed down
}

std::unordered_map<uint32_t, DynamicModelMaterials> index_dynamic_model_materials(PackageManager& pm) {
    // Offsets inside SDynamicModelComponent (goriath): model 0x234, technique map 0x3D8,
    // techniques 0x418. The struct sits somewhere inside the component tag, so find it by its
    // model tag and validate the vectors around it.
    std::unordered_map<uint32_t, bool> models;
    std::vector<TagHash> entities;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            if (p.entries()[i].type == 8 && p.entries()[i].reference == kClassDynamicModel)
                models[TagHash::make(info.pkg_id, uint16_t(i)).value] = true;
            if (p.entries()[i].type == 8 && p.entries()[i].reference == 0x8080BAAD)
                entities.push_back(TagHash::make(info.pkg_id, uint16_t(i)));
        }
    }
    std::unordered_map<uint32_t, DynamicModelMaterials> out;
    using Defaults = std::map<uint32_t, std::array<float, 4>>;
    std::unordered_map<uint32_t, Defaults> channel_components;
    std::unordered_set<uint32_t> exfil_components;  // components reading exfil_activation_progress
    constexpr uint32_t kExfilActivationProgress = 0x4AE64998;  // fnv1("exfil_activation_progress")
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassComponent) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            std::vector<uint8_t> bytes;
            try {
                bytes = pm.read(tag);
            } catch (...) {
                continue;
            }
            for (size_t o = 0; o + 4 <= bytes.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, bytes.data() + o, 4);
                if (v == kExfilActivationProgress) {
                    exfil_components.insert(tag.value);
                    break;
                }
            }
            TagReader r(bytes);
            try {
                const size_t definition = r.pointer_at(0x18);
                if (definition >= 4 && definition < bytes.size() && r.u32_at(definition - 4) == 0x8080AF75) {
                    auto defaults = material_defaults(r, definition);
                    if (!defaults.empty()) {
                        channel_components.emplace(tag.value, std::move(defaults));
                    }
                    continue;
                }
            } catch (...) {
            }
            for (size_t o = 0x234; o + 4 <= bytes.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, bytes.data() + o, 4);
                if (!models.count(v) || out.count(v)) continue;
                const size_t base = o - 0x234;
                if (base < 4 || base + 0x428 > bytes.size()) continue;
                const uint32_t component_class = r.u32_at(base - 4);
                // The wrapped component carries its subtype after the owner tag.
                const bool wrapped = component_class == 0x80808655 && r.tag_at(base) == tag &&
                                     r.u32_at(base + 4) == 0x80808644;
                if (component_class != 0x80808678 && !wrapped) continue;
                try {
                    auto map = r.vec_at(base + 0x3D8);
                    auto techs = r.vec_at(base + 0x418);
                    if ((map.count && map.element_class != 0x80808681) ||
                        (wrapped && techs.count && techs.element_class != 0x80800014) ||
                        map.data > bytes.size() || map.count > (bytes.size() - map.data) / 12 ||
                        techs.data > bytes.size() || techs.count > (bytes.size() - techs.data) / 4) continue;
                    DynamicModelMaterials m;
                    m.component = tag;
                    for (uint64_t k = 0; k < map.count; ++k) {
                        const size_t q = map.data + size_t(k) * 12;
                        m.technique_map.push_back({r.u32_at(q), r.u32_at(q + 4), r.u32_at(q + 8)});
                    }
                    for (uint64_t k = 0; k < techs.count; ++k) m.techniques.push_back(r.tag_at(techs.data + size_t(k) * 4));
                    if (std::any_of(m.technique_map.begin(), m.technique_map.end(), [&](const auto& rg) {
                            return uint64_t(rg.start) + rg.count > m.techniques.size();
                        })) continue;
                    for (const auto& rg : m.technique_map)
                        if (rg.condition_start == 0) {
                            m.permutation_count = std::max(1u, rg.count);
                            break;
                        }
                    m.default_permutation = m.permutation_count > 2 ? m.permutation_count - 1 : 0;
                    try {
                        if (const auto groups = r.vec_at(base + 0x28); groups.element_class == 0x8080BACC)
                            for (uint64_t k = 0; k < groups.count; ++k) {
                                auto& set = m.part_tags.emplace_back();
                                const auto pairs = r.vec_at(groups.data + size_t(k) * 0x18 + 8);
                                if (pairs.element_class != 0x8080BAD0) continue;
                                for (uint64_t q = 0; q < pairs.count; ++q)
                                    set.push_back({r.u32_at(pairs.data + size_t(q) * 8), r.u32_at(pairs.data + size_t(q) * 8 + 4)});
                            }
                    } catch (const std::exception&) {
                        m.part_tags.clear();
                    }
                    try {
                        // Each 80808682 condition row is 8 bytes: {u16 count, u16 first} for the
                        // required tag-set groups, followed by {u16, u16} for the matching-key
                        // wildcard (871AC0EA) mask. count == 0 (first == 0xFFFF) is the default
                        // row; count == 1 names part_tags[first] directly; count >= 2 slices the
                        // u16 index table at +0x3F8.
                        const auto indices = r.vec_at(base + 0x3F8);
                        const auto conditions = r.vec_at(base + 0x408);
                        if (conditions.count <= techs.count && conditions.element_class == 0x80808682 &&
                            indices.element_class == 0x80800006 &&
                            conditions.data <= bytes.size() && conditions.count <= (bytes.size() - conditions.data) / 8 &&
                            indices.data <= bytes.size() && indices.count <= (bytes.size() - indices.data) / 2) {
                            for (size_t ci = 0; ci < conditions.count; ++ci) {
                                const size_t offset = conditions.data + ci * 8;
                                const auto count = r.u16_at(offset), first = r.u16_at(offset + 2);
                                auto& groups = m.technique_conditions.emplace_back();
                                if (count == 1) {
                                    if (first >= m.part_tags.size()) {
                                        m.technique_conditions.clear();
                                        break;
                                    }
                                    groups.push_back(first);
                                } else if (count > 1) {
                                    if (uint64_t(first) + count > indices.count) {
                                        m.technique_conditions.clear();
                                        break;
                                    }
                                    for (uint32_t j = 0; j < count; ++j)
                                        groups.push_back(r.u16_at(indices.data + size_t(first + j) * 2));
                                }
                            }
                        }
                    } catch (const std::exception&) {
                        m.technique_conditions.clear();
                    }
                    out.emplace(v, std::move(m));
                } catch (...) {
                }
            }
        }
    }
    // A model with inline techniques still needs its component: sibling channel components
    // contain the scales, positions, visibility and effect states its VS/PS programs read.
    std::unordered_map<uint32_t, std::vector<uint32_t>> models_of;
    for (const auto& [model, material] : out) models_of[material.component.value].push_back(model);
    for (TagHash entity : entities) {
        try {
            auto bytes = pm.read(entity);
            TagReader r(bytes);
            auto components = r.vec_at(0x08);
            Defaults defaults;
            std::vector<uint32_t> placed;
            bool exfil = false;
            for (uint64_t c = 0; c < components.count; ++c) {
                const uint32_t component = r.u32_at(components.data + size_t(c) * 12);
                exfil = exfil || exfil_components.count(component);
                if (auto f = channel_components.find(component); f != channel_components.end())
                    defaults.insert(f->second.begin(), f->second.end());
                if (auto f = models_of.find(component); f != models_of.end())
                    placed.insert(placed.end(), f->second.begin(), f->second.end());
            }
            for (uint32_t model : placed) {
                if (defaults.size() > out[model].object_defaults.size()) out[model].object_defaults = defaults;
                if (exfil) out[model].exfil_device = true;
            }
        } catch (...) {
        }
    }
    return out;
}

VertexBuffer load_vertex_buffer(PackageManager& pm, TagHash header) {
    auto h = pm.read(header);
    BinaryReader r(h);
    VertexBuffer vb;
    vb.stride = r.u16_at(4);
    vb.vtype = r.u16_at(6);
    vb.data_tag = TagHash(pm.entry(header)->reference);
    vb.data = pm.read(vb.data_tag);
    return vb;
}

DecoratorGroup load_decorator_group(PackageManager& pm, TagHash tag) {
    auto read_class = [&](TagHash ref, uint32_t cls) {
        const auto* entry = pm.entry(ref);
        if (!entry || entry->type != 8 || entry->reference != cls)
            throw std::runtime_error(std::format("Decorator {}: expected {:08X} at {}", tag.str(), cls, ref.str()));
        return pm.read(ref);
    };
    const auto group_bytes = read_class(tag, kClassDecoratorGroup);
    const TagReader group(group_bytes);
    const auto models = group.vec_at(0x08);
    if (models.count != 1 || models.element_class != 0x80808598)
        throw std::runtime_error("Unrecognized decorator model table: " + tag.str());
    const auto model = read_class(group.tag_at(models.data), 0x80808599);
    const auto stream = read_class(group.tag_at(0x48), 0x8080858B);
    const TagReader source(stream);
    DecoratorGroup result;
    result.model = TagReader(model).tag_at(0x08);
    const auto decode = read_class(source.tag_at(0x14), 0x80808586);
    const BinaryReader constants(decode);
    for (size_t i = 0; i < result.decode.size(); ++i) constants.copy_at(i * 16, result.decode[i]);
    const TagHash header = source.tag_at(0x18);
    const auto* entry = pm.entry(header);
    if (!entry || entry->type != 32 || entry->subtype != 4)
        throw std::runtime_error("Invalid decorator vertex header: " + header.str());
    result.instances = load_vertex_buffer(pm, header);
    if (result.instances.stride != 16 || result.instances.data.empty() || result.instances.data.size() % 16)
        throw std::runtime_error("Invalid decorator instance stream: " + header.str());
    const size_t count = result.instances.data.size() / 16;
    auto checked_table = [&](const TagReader& reader, size_t field, uint32_t cls, size_t stride) {
        const auto table = reader.vec_at(field);
        if (table.element_class != cls || table.data > reader.size() || table.count > (reader.size() - table.data) / stride)
            throw std::runtime_error(std::format("Invalid decorator table +0x{:X}: {}", field, tag.str()));
        return table;
    };
    auto offsets = [&](size_t field) {
        const auto table = checked_table(group, field, 0x80800007, sizeof(uint32_t));
        if (table.count < 2) throw std::runtime_error("Missing decorator ranges: " + tag.str());
        std::vector<uint32_t> values(size_t(table.count));
        std::memcpy(values.data(), group.span().data() + table.data, values.size() * sizeof(uint32_t));
        if (values.front() != 0 || values.back() != count || !std::is_sorted(values.begin(), values.end()))
            throw std::runtime_error("Decorator ranges do not match vertex stream: " + tag.str());
        return values;
    };
    result.part_offsets = offsets(0x18);
    const auto cluster_offsets = offsets(0x38);
    const auto settings_bytes = read_class(TagReader(model).tag_at(0x30), 0x8080859B);
    const TagReader settings(settings_bytes);
    const auto distances = checked_table(settings, 0x18, 0x80800090, sizeof(float4));
    if (distances.count + 1 != result.part_offsets.size())
        throw std::runtime_error("Decorator distance table does not match part count: " + tag.str());
    result.part_distance_thresholds.resize(size_t(distances.count));
    for (size_t i = 0; i < distances.count; ++i) {
        auto& values = result.part_distance_thresholds[i];
        settings.copy_at(distances.data + i * sizeof(float4), values);
        if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v) && v >= 0; }) ||
            !std::is_sorted(values.begin(), values.end()))
            throw std::runtime_error("Invalid decorator distance thresholds: " + tag.str());
    }

    const auto bounds_bytes = read_class(group.tag_at(0x4C), 0x8080A7F5);
    const TagReader bounds(bounds_bytes);
    constexpr size_t kBoundsStride = 48;
    const auto boxes = checked_table(bounds, 0x08, 0x8080A7F7, kBoundsStride);
    if (boxes.count + 1 != cluster_offsets.size())
        throw std::runtime_error("Decorator bounds do not match cluster count: " + tag.str());
    result.clusters.reserve(size_t(boxes.count));
    for (size_t i = 0; i < boxes.count; ++i) {
        const uint32_t first = cluster_offsets[i];
        const uint32_t end = cluster_offsets[i + 1];
        const auto part_end = std::upper_bound(result.part_offsets.begin(), result.part_offsets.end(), first);
        if (first == end || part_end == result.part_offsets.end() || end > *part_end)
            throw std::runtime_error("Decorator cluster crosses part ranges: " + tag.str());
        const auto part_id = uint32_t(part_end - result.part_offsets.begin() - 1);
        DecoratorCluster cluster{{}, {}, first, end - first, part_id};
        bounds.copy_at(boxes.data + i * kBoundsStride, cluster.min);
        bounds.copy_at(boxes.data + i * kBoundsStride + sizeof(float4), cluster.max);
        for (size_t axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(cluster.min[axis]) || !std::isfinite(cluster.max[axis]) || cluster.min[axis] > cluster.max[axis])
                throw std::runtime_error("Invalid decorator cluster bounds: " + tag.str());
        }
        result.clusters.push_back(cluster);
    }
    return result;
}

IndexBuffer load_index_buffer(PackageManager& pm, TagHash header) {
    auto h = pm.read(header);
    BinaryReader r(h);
    IndexBuffer ib;
    ib.is_32bit = r.u8_at(1) != 0;
    ib.data_tag = TagHash(pm.entry(header)->reference);
    ib.data = pm.read(ib.data_tag);
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
    t.tile_count = r.u16_at(0x2A);
    if (t.tile_count > 1)
        for (uint32_t i = 0; i < 4; ++i) t.tile_uv[i] = r.f32_at(0x10 + 4 * i);
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

Texture load_texture(PackageManager& pm, TagHash header, uint32_t max_dim) {
    auto h = pm.read(header);
    BinaryReader r(h);
    const uint16_t w0 = r.u16_at(0x22), h0 = r.u16_at(0x24), depth = r.u16_at(0x26), arr = r.u16_at(0x28);
    if (!max_dim || std::max(w0, h0) <= max_dim || depth > 1 || arr > 1 || r.u16_at(0x20) != 0xCAFE)
        return load_texture(pm, header);
    Texture t;
    t.dxgi_format = r.u32_at(0x04);
    t.tile_count = r.u16_at(0x2A);
    if (t.tile_count > 1)
        for (uint32_t i = 0; i < 4; ++i) t.tile_uv[i] = r.f32_at(0x10 + 4 * i);
    const uint16_t mips_hint = r.u8_at(0x2D);
    uint16_t smallest = std::min(w0, h0), full = 1;
    while ((1u << full) <= smallest) ++full;
    const uint16_t mips = std::max<uint16_t>(1, std::min<uint16_t>(full, mips_hint ? mips_hint : 1));
    const size_t bpp = bits_per_pixel(t.dxgi_format);
    auto mip_size = [&](uint16_t m) {
        size_t w = std::max(1, w0 >> m), hh = std::max(1, h0 >> m);
        return is_block_compressed(t.dxgi_format) ? ((w + 3) / 4) * ((hh + 3) / 4) * bpp * 2 : w * hh * bpp / 8;
    };
    uint16_t skip = 0;
    while (skip + 1 < mips && std::max(w0 >> skip, h0 >> skip) > int(max_dim)) ++skip;

    // The data entry holds the chain from some mip down (all of it when there is no large buffer).
    std::vector<uint8_t> small = pm.read(TagHash(pm.entry(header)->reference));
    size_t tail = 0;
    int small_first = -1;
    for (int m = mips - 1; m >= 0; --m) {
        tail += mip_size(uint16_t(m));
        if (tail == small.size()) small_first = m;
    }
    std::vector<uint8_t> data;
    uint16_t first = 0;
    const TagHash large(r.u32_at(0x3C));
    if (small_first >= 0 && small_first <= skip) {
        data = std::move(small);
        first = uint16_t(small_first);
    } else if (!large.is_none() && pm.entry(large)) {
        data = pm.read(large);
    } else {
        data = std::move(small);
    }
    size_t off = 0;
    for (uint16_t m = first; m < skip; ++m) off += mip_size(m);
    if (off >= data.size()) return load_texture(pm, header);
    t.width = uint16_t(std::max(1, w0 >> skip));
    t.height = uint16_t(std::max(1, h0 >> skip));
    size_t total = 0;
    uint16_t covered = 0;
    for (uint16_t m = skip; m < mips; ++m) {
        const size_t sz = mip_size(m);
        if (off + total + sz > data.size()) break;
        total += sz;
        covered++;
    }
    if (!covered) return load_texture(pm, header);
    // Block-compressed textures need the top mip in whole blocks.
    if (is_block_compressed(t.dxgi_format) && ((t.width & 3) || (t.height & 3))) return load_texture(pm, header);
    t.mip_count = covered;
    t.data.assign(data.begin() + off, data.begin() + off + total);
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
    g.atmosphere_noise = r.tag_at(0x40);
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

ScopeStage scope_stage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex: return ScopeStage::Vertex;
    case ShaderStage::Pixel: return ScopeStage::Pixel;
    case ShaderStage::Geometry: return ScopeStage::Geometry;
    case ShaderStage::Hull: return ScopeStage::Hull;
    case ShaderStage::Domain: return ScopeStage::Domain;
    case ShaderStage::Compute: return ScopeStage::Compute;
    default: throw std::invalid_argument("Invalid shader stage");
    }
}

std::vector<Scope> load_technique_scopes(PackageManager& pm, const RenderGlobals& globals, const Technique& technique) {
    std::vector<Scope> scopes;
    for (uint32_t bit = 0; bit < 64; ++bit) {
        if (!(technique.used_scopes & (uint64_t{1} << bit))) continue;
        if (bit >= globals.scopes.size())
            throw std::runtime_error(std::format("{}: unknown shared scope bit {}", technique.tag.str(), bit));
        const auto& [name, tag] = globals.scopes[bit];
        auto scope = load_scope(pm, tag);
        if (scope.name != name) throw std::runtime_error("Render globals scope name/order mismatch");
        scopes.push_back(std::move(scope));
    }
    return scopes;
}

StageResourceSources resolve_stage_resources(const Technique& technique, ShaderStage stage,
                                             std::span<const Scope* const> scopes) {
    StageResourceSources result;
    auto apply = [&](const TechniqueStage& core, TagHash owner) {
        const auto assignments = inspect_tfx_bindings(core.bytecode);
        for (const auto& texture : core.textures) {
            result.textures[texture.slot] = texture.texture;
            result.extern_textures.erase(texture.slot);
        }
        for (const auto& texture : assignments.extern_textures) {
            result.extern_textures[texture.slot] = texture;
            result.textures.erase(texture.slot);
        }
        for (const auto& texture : assignments.extern_uavs) result.extern_uavs[texture.slot] = texture;
        for (const auto& sampler : assignments.samplers) {
            if (sampler.index >= core.samplers.size()) throw std::runtime_error("Shared sampler index out of bounds");
            result.samplers[sampler.slot] = core.samplers[sampler.index];
        }
        if (!owner.is_none() && core.cbuffer_slot > 0 && (!core.cbuffer_init.empty() || !core.bytecode.empty()))
            result.cbuffer_scopes[uint32_t(core.cbuffer_slot)] = owner;
    };
    for (const auto* scope : scopes) {
        if (!scope) throw std::invalid_argument("Null original scope");
        apply(scope->stages[size_t(scope_stage(stage))], scope->tag);
    }
    apply(technique.stage(stage), TagHash());
    return result;
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

std::vector<ShaderInput> shader_inputs(std::span<const uint8_t> bc) {
    auto u32 = [&](size_t o) {
        if (o + 4 > bc.size()) throw std::runtime_error("DXBC truncated");
        uint32_t v;
        std::memcpy(&v, bc.data() + o, 4);
        return v;
    };
    if (bc.size() < 32 || std::memcmp(bc.data(), "DXBC", 4) != 0) throw std::runtime_error("not DXBC");
    std::vector<ShaderInput> out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts; ++i) {
        const size_t part = u32(32 + size_t(i) * 4);
        if (std::memcmp(bc.data() + part, "ISG1", 4) != 0) continue;
        const size_t base = part + 8;
        const uint32_t count = u32(base);
        size_t o = base + u32(base + 4);
        for (uint32_t k = 0; k < count; ++k, o += 32) {  // stream, name, index, sysvalue, type, reg, mask, min precision
            const uint32_t name = u32(o + 4), sysvalue = u32(o + 12);
            if (sysvalue != 0) continue;
            ShaderInput in;
            for (size_t c = base + name; c < bc.size() && bc[c]; ++c) in.semantic += char(bc[c]);
            in.index = u32(o + 8);
            in.reg = u32(o + 20);
            in.component_type = uint8_t(u32(o + 16));
            in.component_mask = bc[o + 24] & 0x0F;
            in.never_read = in.component_mask != 0 && (in.component_mask & bc[o + 25]) == 0;
            out.push_back(std::move(in));
        }
    }
    return out;
}

std::vector<ShaderInput> shader_outputs(std::span<const uint8_t> bc) {
    auto u32 = [&](size_t o) {
        if (o + 4 > bc.size()) throw std::runtime_error("DXBC truncated");
        uint32_t v;
        std::memcpy(&v, bc.data() + o, 4);
        return v;
    };
    if (bc.size() < 32 || std::memcmp(bc.data(), "DXBC", 4) != 0) throw std::runtime_error("not DXBC");
    std::vector<ShaderInput> out;
    const uint32_t parts = u32(28);
    for (uint32_t i = 0; i < parts; ++i) {
        const size_t part = u32(32 + size_t(i) * 4);
        if (std::memcmp(bc.data() + part, "OSG1", 4) != 0) continue;
        const size_t base = part + 8;
        const uint32_t count = u32(base);
        size_t o = base + u32(base + 4);
        for (uint32_t k = 0; k < count; ++k, o += 32) {  // stream, name, index, sysvalue, type, reg, mask, min precision
            const uint32_t name = u32(o + 4), sysvalue = u32(o + 12);
            (void)sysvalue;
            ShaderInput in;
            for (size_t c = base + name; c < bc.size() && bc[c]; ++c) in.semantic += char(bc[c]);
            in.index = u32(o + 8);
            in.reg = u32(o + 20);
            const uint8_t mask = bc[o + 24], never_writes = bc[o + 25];
            in.never_written = mask && (never_writes & mask) == mask;
            out.push_back(std::move(in));
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
