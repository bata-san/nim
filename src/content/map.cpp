#include "nim/content/map.hpp"

#include "nim/content/tfx.hpp"
#include "nim/content/weather.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

namespace nim {
namespace {

// The structure classes between a bubble and its static instances. Anything else (entities,
// lights, terrain, meshes) is not followed.
bool structural(uint32_t cls) {
    switch (cls) {
    case 0x8080AB27:  // SBubbleParent
    case 0x8080A7BB:  // SBubbleDefinition
    case 0x8080B0B5:
    case 0x8080A7C1:  // SMapContainer
    case 0x8080BA05:
    case 0x8080B1A7:  // SMapNodeTable
    case 0x808082D5:  // static instances component payload
    case kClassStaticInstances:
        return true;
    default:
        return false;
    }
}

std::vector<TagHash> find_instance_tags(PackageManager& pm, TagHash bubble, std::vector<TagHash>* terrains = nullptr,
                                        std::vector<TagHash>* node_tables = nullptr) {
    pm.build_hash64_index();
    std::vector<TagHash> queue{bubble}, found;
    std::set<uint32_t> seen{bubble.value};
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const TagHash t = queue[qi];
        const Entry* e = pm.entry(t);
        if (!e) continue;
        if (e->reference == kClassStaticInstances) {
            found.push_back(t);
            continue;
        }
        if (node_tables && e->reference == kClassMapNodeTable) node_tables->push_back(t);
        std::vector<uint8_t> data;
        try {
            data = pm.read(t);
        } catch (...) {
            continue;
        }
        auto visit = [&](TagHash c) {
            if (seen.count(c.value)) return;
            const Entry* ce = pm.entry(c);
            if (!ce || (ce->type != 8 && ce->type != 16)) return;
            if (ce->reference == kClassTerrain) {
                seen.insert(c.value);
                if (terrains) terrains->push_back(c);
                return;
            }
            if (!structural(ce->reference)) return;
            seen.insert(c.value);
            queue.push_back(c);
        };
        // Children are named by TagHash, or (bubble -> 8080B0B5) by a WideHash's hash64.
        for (size_t o = 8; o + 4 <= data.size(); o += 4) {
            uint32_t v;
            std::memcpy(&v, data.data() + o, 4);
            if (v >= 0x80800000u && v != 0xFFFFFFFFu) visit(TagHash(v));
            if ((o & 7) == 0 && o + 8 <= data.size()) {
                uint64_t w;
                std::memcpy(&w, data.data() + o, 8);
                if (w && w != ~0ull)
                    if (auto t = pm.resolve64(w)) visit(*t);
            }
        }
    }
    return found;
}

std::string package_of(PackageManager& pm, TagHash t) {
    for (const auto& info : pm.packages())
        if (info.pkg_id == t.pkg_id()) return info.name;
    return {};
}

}  // namespace

Terrain load_terrain(PackageManager& pm, TagHash tag) {
    auto bytes = pm.read(tag);
    TagReader r(bytes);
    Terrain t;
    t.tag = tag;
    for (int a = 0; a < 3; ++a) {
        t.lo[size_t(a)] = r.f32_at(0x10 + size_t(a) * 4);
        t.hi[size_t(a)] = r.f32_at(0x20 + size_t(a) * 4);
    }
    for (int a = 0; a < 4; ++a) t.offset[size_t(a)] = r.f32_at(0x30 + size_t(a) * 4);
    const auto groups = r.vec_at(0x50);
    for (uint64_t g = 0; g < groups.count; ++g) {
        const size_t e = groups.data + size_t(g) * 0x60;
        Terrain::Group group;
        for (int a = 0; a < 4; ++a) group.texcoord[size_t(a)] = r.f32_at(e + 0x20 + size_t(a) * 4);
        group.dyemap = r.tag_at(e + 0x50);
        t.groups.push_back(group);
    }
    t.vertex0 = r.tag_at(0x60);
    t.vertex1 = r.tag_at(0x64);
    t.index = r.tag_at(0x68);
    t.technique_shadow = r.tag_at(0x6C);
    t.technique_depth = r.tag_at(0x70);
    const auto parts = r.vec_at(0x78);
    for (uint64_t i = 0; i < parts.count; ++i) {
        const size_t e = parts.data + size_t(i) * 12;
        Terrain::Part part;
        part.technique = r.tag_at(e);
        part.index_start = r.u32_at(e + 4);
        part.index_count = uint16_t(r.u32_at(e + 8) & 0xFFFF);
        part.group = uint8_t((r.u32_at(e + 8) >> 16) & 0xFF);
        part.detail = uint8_t(r.u32_at(e + 8) >> 24);
        t.parts.push_back(part);
    }
    return t;
}

namespace {

using Mat = std::array<float, 16>;
Mat mat_mul(const Mat& a, const Mat& b) {  // column-major a * b
    Mat r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = v;
        }
    return r;
}
// Rotation quaternion (x, y, z, w), translation and uniform scale -> column-major matrix.
Mat quat_transform(float x, float y, float z, float w, float tx, float ty, float tz, float s) {
    Mat m{};
    m[0] = (1 - 2 * (y * y + z * z)) * s; m[1] = 2 * (x * y + z * w) * s; m[2] = 2 * (x * z - y * w) * s;
    m[4] = 2 * (x * y - z * w) * s; m[5] = (1 - 2 * (x * x + z * z)) * s; m[6] = 2 * (y * z + x * w) * s;
    m[8] = 2 * (x * z + y * w) * s; m[9] = 2 * (y * z - x * w) * s; m[10] = (1 - 2 * (x * x + y * y)) * s;
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
    return m;
}
Mat read_quat_transform(const TagReader& r, size_t o) {
    return quat_transform(r.f32_at(o), r.f32_at(o + 4), r.f32_at(o + 8), r.f32_at(o + 12),
                          r.f32_at(o + 16), r.f32_at(o + 20), r.f32_at(o + 24), 1.0f);
}
Mat read_mat(const TagReader& r, size_t o) {
    Mat m{};
    for (size_t i = 0; i < 16; ++i) m[i] = r.f32_at(o + i * 4);
    return m;
}
// SLight / SShadowingLight share the layout up to +0xBC; `technique` is the lighting-apply offset.
MapLight read_light(const TagReader& r, size_t o, size_t technique, const Mat& node, TagHash source, bool shadowing) {
    MapLight l;
    l.source = source;
    l.shadowing = shadowing;
    l.node = node;
    l.light_space = read_mat(r, o + 0x60);
    for (size_t i = 0; i < 20; ++i) l.params[i] = r.f32_at(o + i * 4);
    l.technique = r.tag_at(o + technique);
    l.volumetrics = r.tag_at(o + technique + (shadowing ? 8 : 4));
    if (shadowing) {
        l.shadow_technique = r.tag_at(o + technique + 4);
        l.shadow_volumetrics = r.tag_at(o + technique + 12);
    }
    // Bounds: the unit cube's corners through light space (projective) and the node transform.
    const Mat to_world = mat_mul(node, l.light_space);
    l.lo = {1e30f, 1e30f, 1e30f};
    l.hi = {-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 8; ++c) {
        const float p[4] = {c & 1 ? 1.0f : -1.0f, c & 2 ? 1.0f : -1.0f, c & 4 ? 1.0f : -1.0f, 1.0f};
        float q[4]{};
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) q[row] += to_world[size_t(k * 4 + row)] * p[k];
        const float w = std::fabs(q[3]) > 1e-4f ? q[3] : 1e-4f;
        for (int a = 0; a < 3; ++a) {
            l.lo[size_t(a)] = std::min(l.lo[size_t(a)], q[a] / w);
            l.hi[size_t(a)] = std::max(l.hi[size_t(a)], q[a] / w);
        }
    }
    return l;
}

}  // namespace

std::vector<MapLight> load_map_lights(PackageManager& pm, const MapLayout& map) {
    std::vector<MapLight> out;
    std::set<uint32_t> collections;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(t);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            const Mat node = quat_transform(r.f32_at(e), r.f32_at(e + 4), r.f32_at(e + 8), r.f32_at(e + 12), r.f32_at(e + 0x10),
                                            r.f32_at(e + 0x14), r.f32_at(e + 0x18), r.f32_at(e + 0x1C));
            // Components: a list of inline records (type at -4, next relative pointer at +0, payload at +0x10).
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                const uint32_t type = r.u32_at(p - 4);
                const TagHash payload = r.tag_at(p + 0x10);
                const Entry* pe = payload.is_none() ? nullptr : pm.entry(payload);
                try {
                    if (pe && pe->reference == kClassShadowingLight) {
                        auto bytes = pm.read(payload);
                        TagReader lr(bytes);
                        out.push_back(read_light(lr, 0, 0xD4, node, payload, true));
                    } else if (pe && pe->reference == kClassLightCollection && collections.insert(payload.value).second) {
                        auto bytes = pm.read(payload);
                        TagReader lr(bytes);
                        const auto lights = lr.vec_at(0x30), transforms = lr.vec_at(0x40);
                        for (uint64_t k = 0; k < lights.count && k < transforms.count; ++k)
                            out.push_back(read_light(lr, lights.data + size_t(k) * 0x100, 0xC4,
                                                     mat_mul(node, read_quat_transform(lr, transforms.data + size_t(k) * 0x20)), payload, false));
                    }
                } catch (...) {
                    // A light whose data cannot be read is left out.
                }
                (void)type;
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    return out;
}

std::vector<const MapLight*> map_light_region(const std::vector<MapLight>& lights, std::array<float, 3> center, float radius) {
    std::vector<const MapLight*> out;
    for (const auto& l : lights) {
        float d2 = 0;
        for (int a = 0; a < 3; ++a) {
            const float c = center[size_t(a)], d = c < l.lo[size_t(a)] ? l.lo[size_t(a)] - c : c > l.hi[size_t(a)] ? c - l.hi[size_t(a)] : 0;
            d2 += d * d;
        }
        if (d2 <= radius * radius) out.push_back(&l);
    }
    return out;
}

std::vector<MapCubemap> load_map_cubemaps(PackageManager& pm, const MapLayout& map) {
    std::vector<MapCubemap> out;
    // Preserve distinct authored records even when they share a transform/specular cube.
    // Hashes must never pass through float (near 0x80xxxxxx it loses their low bits).
    std::set<std::vector<uint8_t>> seen;
    for (TagHash t : map.node_tables) {
        const auto data = pm.read(t);
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                const size_t c = p + 0x10;  // the component's payload
                if (r.u32_at(p - 4) == kClassCubemapComponent && c + 0x1D0 <= data.size()) {
                    MapCubemap m;
                    for (int a = 0; a < 3; ++a) {
                        m.extents[size_t(a)] = std::fabs(r.f32_at(c + 0x10 + size_t(a) * 4));
                        m.center[size_t(a)] = r.f32_at(c + 0x20 + size_t(a) * 4);
                        m.origin[size_t(a)] = r.f32_at(c + 0x110 + size_t(a) * 4);
                    }
                    for (int k = 0; k < 4; ++k) {
                        m.rotation[size_t(k)] = r.f32_at(c + 0x100 + size_t(k) * 4);
                        m.probe_params[size_t(k)] = r.f32_at(c + 0x170 + size_t(k) * 4);
                        m.flags[size_t(k)] = r.f32_at(c + 0x180 + size_t(k) * 4);
                        m.shape[size_t(k)] = r.f32_at(c + 0x70 + size_t(k) * 4);
                        m.unk80[size_t(k)] = r.f32_at(c + 0x80 + size_t(k) * 4);
                    }
                    for (int k = 0; k < 16; ++k) {
                        m.local_to_world[size_t(k)] = r.f32_at(c + 0xC0 + size_t(k) * 4);
                        m.axes[size_t(k)] = r.f32_at(c + 0x120 + size_t(k) * 4);
                    }
                    for (int a = 0; a < 3; ++a) m.probes[size_t(a)] = std::max(1u, r.u32_at(c + 0x160 + size_t(a) * 4));
                    m.unk30 = r.f32_at(c + 0x30);
                    for (int k = 0; k < 3; ++k) {
                        m.blend_neg[size_t(k)] = r.f32_at(c + 0x50 + uint32_t(k) * 4);
                        m.blend_pos[size_t(k)] = r.f32_at(c + 0x60 + uint32_t(k) * 4);
                    }
                    m.fade_start = r.f32_at(c + 0x3C);
                    m.fade_end = r.f32_at(c + 0x40);
                    m.specular = r.tag_at(c + 0x1B8);
                    m.alpha = r.tag_at(c + 0x1BC);
                    m.diffuse = r.tag_at(c + 0x1C0);
                    m.shape_vertices = r.tag_at(c + 0x1C4);
                    m.shape_records = r.tag_at(c + 0x1B4);
                    // World bounds: the eight corners of the unit cube through local_to_world.
                    m.lo = {1e30f, 1e30f, 1e30f};
                    m.hi = {-1e30f, -1e30f, -1e30f};
                    for (int k = 0; k < 8; ++k) {
                        const float s[3] = {k & 1 ? 1.0f : -1.0f, k & 2 ? 1.0f : -1.0f, k & 4 ? 1.0f : -1.0f};
                        for (int a = 0; a < 3; ++a) {
                            const float v = m.local_to_world[size_t(12 + a)] + s[0] * m.local_to_world[size_t(a)] +
                                            s[1] * m.local_to_world[size_t(4 + a)] + s[2] * m.local_to_world[size_t(8 + a)];
                            m.lo[size_t(a)] = std::min(m.lo[size_t(a)], v);
                            m.hi[size_t(a)] = std::max(m.hi[size_t(a)], v);
                        }
                        // Normal cubemap VS uses +0x20, quaternion +0x100 and +0x10
                        // half extents. The auxiliary matrix can have a different centre;
                        // culling must also contain the cube actually rasterized by that VS.
                        const auto& q = m.rotation;
                        const std::array<float, 3> v{s[0] * m.extents[0], s[1] * m.extents[1], s[2] * m.extents[2]};
                        const float dot = q[0]*v[0] + q[1]*v[1] + q[2]*v[2];
                        const float norm = q[3]*q[3] - q[0]*q[0] - q[1]*q[1] - q[2]*q[2];
                        for (size_t a = 0; a < 3; ++a) {
                            const size_t b = (a + 1) % 3, caxis = (a + 2) % 3;
                            const float world = m.center[a] + norm*v[a] + 2*q[a]*dot + 2*q[3]*(q[b]*v[caxis] - q[caxis]*v[b]);
                            m.lo[a] = std::min(m.lo[a], world);
                            m.hi[a] = std::max(m.hi[a], world);
                        }
                    }
                    std::vector<uint8_t> key(data.begin() + c + 0x10, data.begin() + c + 0x1D0);
                    if (!m.specular.is_none() && seen.insert(std::move(key)).second) out.push_back(m);
                }
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    return out;
}

namespace {
// The global channel expressions (8080B64E sequence nodes) in an entity's components.
void read_channel_expressions(PackageManager& pm, TagHash entity, std::set<uint32_t>& seen_components,
                              std::vector<MapChannelExpression>& out) {
    const Entry* ee = pm.entry(entity);
    if (!ee || ee->type != 8 || ee->reference != 0x8080BAAD) return;
    const auto ed = pm.read(entity);
    // The entity's components, read once: the expressions are in one, the name table of the
    // sequencer's slots (outputs, then inputs) in its companion.
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> components;
    std::set<uint32_t> listed;
    for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
        uint32_t v;
        std::memcpy(&v, ed.data() + o, 4);
        if (v < 0x80800000u || v == 0xFFFFFFFFu || !listed.insert(v).second) continue;
        const Entry* ce = pm.entry(TagHash(v));
        if (!ce || ce->type != 8 || ce->reference != kClassComponent) continue;
        components.emplace_back(v, pm.read(TagHash(v)));
    }
    constexpr uint32_t kClassOutputName = 0x80809F46, kClassSlotNames = 0x8080AF85, kEmptyName = 0x811C9DC5;
    const size_t first_new = out.size();
    for (const auto& [v, cd] : components) {
        if (!seen_components.insert(v).second) continue;
        TagReader r(cd);
        // The output names in index order: {self, 80809F46, ..., +0x30 name}.
        std::vector<uint32_t> names;
        for (size_t c = 0; c + 0x38 <= cd.size(); c += 4)
            if (r.u32_at(c) == v && r.u32_at(c + 4) == kClassOutputName) names.push_back(r.u32_at(c + 0x30));
        for (size_t c = 0; c + 0x50 <= cd.size(); c += 4) {
            if (r.u32_at(c) != kClassSequenceGlobalChannel) continue;
            const size_t p = c + 4;
            MapChannelExpression x;
            x.record_id = r.u32_at(p);
            x.index = r.u32_at(p + 0x28);
            // Without a name table the record's own word is all there is (most outputs match it).
            x.channel = x.index < names.size() ? names[x.index] : x.record_id;
            x.start = r.f32_at(p + 0x14);
            x.cycle = r.f32_at(p + 0x18);
            x.component = TagHash(v);
            const auto bc = r.vec_at(p + 0x30);
            const auto k = r.vec_at(p + 0x40);
            if (bc.data + bc.count > cd.size() || k.data + k.count * 16 > cd.size()) continue;
            x.bytecode.assign(cd.begin() + ptrdiff_t(bc.data), cd.begin() + ptrdiff_t(bc.data + bc.count));
            for (uint64_t i = 0; i < k.count; ++i)
                x.constants.push_back({r.f32_at(k.data + i * 16), r.f32_at(k.data + i * 16 + 4),
                                       r.f32_at(k.data + i * 16 + 8), r.f32_at(k.data + i * 16 + 12)});
            out.push_back(std::move(x));
        }
    }
    if (out.size() == first_new) return;
    // Slot names: Vec<8080AF85> of {namespace, name, slot}. The slots past the outputs are the
    // inputs, in order.
    const size_t outputs = out.size() - first_new;
    std::map<uint32_t, SequencerInput> by_slot;
    for (const auto& [v, cd] : components) {
        TagReader r(cd);
        for (size_t c = 8; c + 8 <= cd.size(); c += 4) {
            if (r.u32_at(c) != kClassSlotNames) continue;
            const uint64_t n = r.u64_at(c - 8);
            if (n == 0 || n > 4096 || c + 8 + n * 12 > cd.size()) continue;
            for (uint64_t i = 0; i < n; ++i) {
                const size_t e = c + 8 + size_t(i) * 12;
                const uint32_t slot = r.u32_at(e + 8);
                if (slot < outputs) continue;
                SequencerInput in;
                in.name_space = r.u32_at(e);
                in.name = r.u32_at(e + 4);
                if (in.name_space != kEmptyName || !by_slot.contains(slot)) by_slot[slot] = in;
            }
        }
    }
    if (by_slot.empty()) return;
    auto inputs = std::make_shared<std::vector<SequencerInput>>();
    for (uint32_t slot = uint32_t(outputs); by_slot.contains(slot); ++slot) inputs->push_back(by_slot[slot]);
    for (size_t i = first_new; i < out.size(); ++i) out[i].inputs = inputs;
}
}  // namespace

MapAtmosphere load_map_atmosphere(PackageManager& pm, const MapLayout& map) {
    MapAtmosphere out;
    bool found_atmosphere = false, found_sun = false;
    std::set<uint32_t> seen_entities, seen_components;
    pm.build_hash64_index();
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(t);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            if (const uint64_t h64 = r.u64_at(e + 0x30); h64 && h64 != ~0ull)
                if (const auto ent = pm.resolve64(h64); ent && seen_entities.insert(ent->value).second) try {
                        read_channel_expressions(pm, *ent, seen_components, out.channel_expressions);
                    } catch (...) {
                        // An unreadable entity has no environment for nim.
                    }
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                const size_t c = p + 0x10;
                const uint32_t type = r.u32_at(p - 4);
                if (type == kClassAtmosphereComponent && !found_atmosphere && c + 0x108 <= data.size()) {
                    for (size_t k = 0; k < 4; ++k)
                        if (auto h = resolve_wide_hash(pm, r.u32_at(c + 0x80 + k * 16), r.u64_at(c + 0x88 + k * 16)))
                            out.lookups[k] = *h;
                    out.density_lut = r.tag_at(c + 0xC0);
                    for (size_t k = 0; k < 16; ++k) out.slice_times[k] = r.f32_at(c + 0xC8 + k * 4);
                    found_atmosphere = out.valid();
                } else if (type == kClassSunPathComponent && !found_sun && c + 0x10 <= data.size()) {
                    out.default_time = r.f32_at(c + 0x08);
                    try {
                        auto set = pm.read(r.tag_at(c));
                        TagReader sr(set);
                        for (size_t k = 0; k < 4; ++k) {
                            auto table = pm.read(sr.tag_at(0x10 + k * 4));
                            TagReader tr(table);
                            const auto dirs = tr.vec_at(0x30);
                            for (uint64_t d = 0; d < dirs.count && dirs.data + d * 16 + 12 <= table.size(); ++d) {
                                const size_t o = dirs.data + size_t(d) * 16;
                                out.paths[k].push_back({tr.f32_at(o), tr.f32_at(o + 4), tr.f32_at(o + 8)});
                            }
                        }
                        found_sun = !out.paths[0].empty();
                    } catch (...) {
                        // Without a sun path the viewer sun direction is used.
                    }
                    try {
                        const TagHash optics = r.tag_at(c + 0x0C);
                        const Entry* entry = pm.entry(optics);
                        if (entry && entry->reference == 0x80808351) {
                            const auto bytes = pm.read(optics);
                            const TagReader lenses(bytes);
                            out.lens_glare = lenses.tag_at(0);
                            out.lens_upsample = lenses.tag_at(4);
                        }
                    } catch (...) {
                        // Some maps have no lens glare techniques.
                    }
                }
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    // The world channels the inputs name: their slots in the world channel registry.
    std::set<const std::vector<SequencerInput>*> resolved;
    std::optional<WorldChannelRegistry> registry;
    for (auto& x : out.channel_expressions) {
        if (!x.inputs || !resolved.insert(x.inputs.get()).second) continue;
        for (auto& in : const_cast<std::vector<SequencerInput>&>(*x.inputs)) {
            if (!in.world()) continue;
            if (!registry) registry = load_world_channel_registry(pm);
            if (const auto it = registry->slots.find(in.name); it != registry->slots.end()) in.world_slot = int(it->second);
        }
    }
    return out;
}

std::vector<MapCubemap> load_entity_preview_probes(PackageManager& pm) {
    const TagHash orbit{0x80A76433};  // orbit_goliath_013b interior
    const auto* entry = pm.entry(orbit);
    if (!entry || entry->reference != kClassBubbleParent) return {};
    MapLayout preview;
    preview.bubble = orbit;
    find_instance_tags(pm, orbit, nullptr, &preview.node_tables);
    auto probes = load_map_cubemaps(pm, preview);
    const auto at_origin = std::find_if(probes.begin(), probes.end(), [](const MapCubemap& probe) {
        for (size_t i = 0; i < 3; ++i) if (probe.lo[i] > 0 || probe.hi[i] < 0) return false;
        return probe.use_probes() && !probe.use_relighting() && !probe.specular.is_none() && !probe.diffuse.is_none();
    });
    if (at_origin == probes.end()) return {};
    return {*at_origin};
}

std::vector<MapStatic> load_map_sky_objects(PackageManager& pm, const MapLayout& map) {
    constexpr uint32_t kClassSkyComponent = 0x80808378, kClassSkyModelRef = 0x80808383;
    std::map<uint32_t, MapStatic> models;
    std::set<std::tuple<uint32_t, std::array<float, 12>, float>> seen;
    std::set<uint32_t> collections;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(t);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                if (r.u32_at(p - 4) == kClassSkyComponent) collections.insert(r.u32_at(p + 0x10));
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    for (uint32_t c : collections) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(TagHash(c));
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto objects = r.vec_at(0x08);
        for (uint64_t i = 0; i < objects.count; ++i) {
            const size_t o = objects.data + size_t(i) * 0x90;
            if (o + 0x90 > data.size()) break;
            if (r.u32_at(o + 0x70) == 5) continue;
            const TagHash ref = r.tag_at(o + 0x60);
            const Entry* re = pm.entry(ref);
            if (!re || re->reference != kClassSkyModelRef) continue;
            TagHash model;
            try {
                const auto rd = pm.read(ref);
                if (rd.size() < 0x0C) continue;
                model = TagReader(rd).tag_at(0x08);
            } catch (...) {
                continue;
            }
            if (model.is_none()) continue;
            std::array<float, 12> row{};
            for (int k = 0; k < 4; ++k)
                for (int a = 0; a < 3; ++a) row[size_t(a * 4 + k)] = r.f32_at(o + size_t(k) * 16 + size_t(a) * 4);
            const float order = r.f32_at(o + 0x64);
            if (!std::isfinite(order) || !std::all_of(row.begin(), row.end(), [](float v) { return std::isfinite(v); }))
                continue;
            // Activity layers repeat records. Keep the exact tag so different sky models at
            // the same transform cannot collide when the low tag bits differ.
            if (!seen.emplace(model.value, row, order).second) continue;
            auto& m = models[model.value];
            m.mesh = model;
            m.rows.push_back(row);
            m.sky_order.push_back(order);
        }
    }
    std::vector<MapStatic> out;
    for (auto& [_, m] : models) out.push_back(std::move(m));
    return out;
}

std::array<float, 3> MapAtmosphere::path_direction(int i, float t) const {
    const auto& path = paths[size_t(i)];
    if (path.empty()) return {0, 0, 1};
    const float f = (t - std::floor(t)) * float(path.size());
    const size_t a = size_t(f) % path.size(), b = (a + 1) % path.size();
    const float w = f - std::floor(f);
    std::array<float, 3> d{};
    float len = 0;
    for (size_t k = 0; k < 3; ++k) {
        d[k] = path[a][k] * (1 - w) + path[b][k] * w;
        len += d[k] * d[k];
    }
    len = std::sqrt(std::max(len, 1e-12f));
    for (auto& v : d) v /= len;
    return d;
}

float MapAtmosphere::slice_coordinate(float t) const {
    // The keys are increasing times; before the first or after the last key the end slice is held.
    const auto& k = slice_times;
    if (t <= k[0]) return 0.5f / 16;
    for (size_t i = 0; i + 1 < k.size(); ++i)
        if (t < k[i + 1]) {
            const float f = k[i + 1] > k[i] ? (t - k[i]) / (k[i + 1] - k[i]) : 0;
            return (float(i) + f + 0.5f) / 16;
        }
    return 15.5f / 16;
}

std::vector<const MapCubemap*> map_cubemap_region(const std::vector<MapCubemap>& cubemaps, std::array<float, 3> center, float radius) {
    std::vector<const MapCubemap*> out;
    for (const auto& m : cubemaps) {
        float d2 = 0;
        for (int a = 0; a < 3; ++a) {
            const float c = center[size_t(a)], d = c < m.lo[size_t(a)] ? m.lo[size_t(a)] - c : c > m.hi[size_t(a)] ? c - m.hi[size_t(a)] : 0;
            d2 += d * d;
        }
        if (d2 <= radius * radius) out.push_back(&m);
    }
    return out;
}

std::vector<MapStart> load_map_starts(PackageManager& pm, const MapLayout& map) {
    std::vector<MapStart> out;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(t);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                if (r.u32_at(p - 4) == kClassStartComponent) {
                    MapStart s;
                    for (size_t k = 0; k < 4; ++k) s.rotation[k] = r.f32_at(e + k * 4);
                    for (size_t k = 0; k < 3; ++k) s.position[k] = r.f32_at(e + 0x10 + k * 4);
                    s.name = r.u32_at(p + 0x10);
                    out.push_back(s);
                    break;
                }
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    return out;
}

std::vector<MapStart> find_map_starts(PackageManager& pm, const MapLayout& map) {
    auto out = load_map_starts(pm, map);
    if (!out.empty()) return out;
    // A bubble view has none of its own: the first activity on this bubble that has some.
    for (const auto& m : list_activity_maps(pm)) {
        if (m.bubble != map.bubble) continue;
        try {
            out = load_map_starts(pm, load_map_layout(pm, m.bubble, load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map)));
        } catch (...) {
            continue;
        }
        if (!out.empty()) return out;
    }
    return out;
}

std::array<float, 3> MapStart::forward() const {
    // The node's local +X through its quaternion (x, y, z, w).
    const float x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
    return {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
}

std::vector<const Terrain*> map_terrain_region(const MapLayout& map, std::array<float, 3> center, float radius) {
    std::vector<std::pair<float, const Terrain*>> found;
    for (const auto& t : map.terrains) {
        float d2 = 0;  // distance from the centre to the patch's box
        for (int a = 0; a < 3; ++a) {
            const float c = center[size_t(a)], d = c < t.lo[size_t(a)] ? t.lo[size_t(a)] - c : c > t.hi[size_t(a)] ? c - t.hi[size_t(a)] : 0;
            d2 += d * d;
        }
        if (d2 <= radius * radius) found.emplace_back(d2, &t);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const Terrain*> out;
    for (const auto& f : found) out.push_back(f.second);
    return out;
}

MapLayout load_map_layout(PackageManager& pm, TagHash bubble, std::span<const TagHash> additional_tables) {
    MapLayout out;
    out.bubble = bubble;
    std::vector<TagHash> terrain_tags;
    out.instance_tags = find_instance_tags(pm, bubble, &terrain_tags, &out.node_tables);
    for (const auto table : additional_tables) {
        const auto tags = find_instance_tags(pm, table, &terrain_tags, &out.node_tables);
        out.instance_tags.insert(out.instance_tags.end(), tags.begin(), tags.end());
    }
    auto unique = [](auto& tags) { std::sort(tags.begin(), tags.end()); tags.erase(std::unique(tags.begin(), tags.end()), tags.end()); };
    // Keep the base map's authored order (probe diagnostic indices must stay stable).
    auto deduplicate = [](auto& tags) { std::set<TagHash> seen; std::erase_if(tags, [&](TagHash t) { return !seen.insert(t).second; }); };
    deduplicate(out.instance_tags);
    deduplicate(out.node_tables);
    unique(terrain_tags);
    for (TagHash t : terrain_tags) {
        try {
            out.terrains.push_back(load_terrain(pm, t));
        } catch (...) {
        }
    }
    std::unordered_map<uint32_t, size_t> index;  // mesh -> statics slot
    out.lo = {1e30f, 1e30f, 1e30f};
    out.hi = {-1e30f, -1e30f, -1e30f};
    for (TagHash it : out.instance_tags) {
        auto bytes = pm.read(it);
        TagReader r(bytes);
        const auto transforms = r.vec_at(0x40);
        const auto statics = r.vec_at(0x78);
        const auto groups = r.vec_at(0x88);
        for (uint64_t g = 0; g < groups.count; ++g) {
            const size_t ge = groups.data + size_t(g) * 16;
            const uint32_t start = r.u32_at(ge), count = r.u32_at(ge + 4), si = r.u32_at(ge + 8);
            if (si >= statics.count || uint64_t(start) + count > transforms.count) continue;
            const TagHash mesh = r.tag_at(statics.data + size_t(si) * 4);
            if (mesh.is_none()) continue;
            auto [slot, fresh] = index.try_emplace(mesh.value, out.statics.size());
            if (fresh) out.statics.push_back({mesh, {}});
            auto& rows = out.statics[slot->second].rows;
            for (uint32_t i = 0; i < count; ++i) {
                const size_t te = transforms.data + size_t(start + i) * 0x60;
                const float x = r.f32_at(te), y = r.f32_at(te + 4), z = r.f32_at(te + 8), w = r.f32_at(te + 12);
                const float tx = r.f32_at(te + 0x10), ty = r.f32_at(te + 0x14), tz = r.f32_at(te + 0x18);
                float s = r.f32_at(te + 0x1C);
                if (!std::isfinite(s) || s == 0) s = 1;
                // Rotation matrix of the unit quaternion, times the uniform scale.
                const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x,
                            wy = w * y, wz = w * z;
                rows.push_back({(1 - 2 * (yy + zz)) * s, 2 * (xy - wz) * s, 2 * (xz + wy) * s, tx,
                                2 * (xy + wz) * s, (1 - 2 * (xx + zz)) * s, 2 * (yz - wx) * s, ty,
                                2 * (xz - wy) * s, 2 * (yz + wx) * s, (1 - 2 * (xx + yy)) * s, tz});
                out.lo = {std::min(out.lo[0], tx), std::min(out.lo[1], ty), std::min(out.lo[2], tz)};
                out.hi = {std::max(out.hi[0], tx), std::max(out.hi[1], ty), std::max(out.hi[2], tz)};
                out.instances++;
            }
        }
    }
    if (!out.instances) out.lo = out.hi = {0, 0, 0};
    return out;
}

std::vector<MapEntry> list_maps(PackageManager& pm) {
    std::vector<MapEntry> out;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassBubbleParent) continue;
            MapEntry m;
            m.bubble = TagHash::make(info.pkg_id, uint16_t(i));
            m.package = info.name;
            for (TagHash it : find_instance_tags(pm, m.bubble)) {
                auto bytes = pm.read(it);
                TagReader r(bytes);
                m.statics += r.vec_at(0x78).count;
                m.instances += r.vec_at(0x40).count;
            }
            out.push_back(std::move(m));
        }
    }
    return out;
}

std::vector<MapEntry> list_activity_maps(PackageManager& pm) {
    pm.build_hash64_index();
    std::vector<MapEntry> out;
    std::map<TagHash, std::string> names;
    for (const auto& info : pm.packages()) for (const auto& named : pm.open(info).named_tags()) {
        if (named.class_hash == 0x8080B386) names.try_emplace(named.hash, named.name);
    }
    std::vector<NamedTag> candidates;
    for (const auto& info : pm.packages()) {
        const auto& entries = pm.open(info).entries();
        for (uint32_t i = 0; i < entries.size(); ++i) if (entries[i].reference == 0x8080B386 && (entries[i].type == 8 || entries[i].type == 16)) {
            const auto tag = TagHash::make(info.pkg_id, uint16_t(i));
            const auto name = names.find(tag);
            candidates.push_back({tag, 0x8080B386, name == names.end() ? std::format("{} / {} (unnamed)", info.name, tag.str()) : name->second});
        }
    }
    for (const auto& named : candidates) {
        // Same SActivity map/ambient/phase selection as DEIMOS. Also expose unnamed activities
        // by their actual tag, and keep registered names without a destination-dot prefix.
      try {
        const auto data = pm.read(named.hash);
        TagReader r(data);
        const auto maps = r.vec_at(0x50);
        if (maps.count && maps.element_class != 0x8080AB3E) throw std::runtime_error("unknown activity map row class");
        for (uint64_t k = 0; k < maps.count; ++k) {
            const size_t row = maps.data + size_t(k) * 0x38;
            const auto refs = r.vec_at(row + 0x28);
            if (!refs.count) continue;
            // ActivityTab uses map_references[0] (including its documented multi-map limitation).
            const auto bubble = resolve_wide_hash(pm, r.u32_at(refs.data), r.u64_at(refs.data + 8));
            if (!bubble) throw std::runtime_error("unresolved activity map reference");
            const auto* entry = pm.entry(*bubble);
            if (!entry || entry->reference != kClassBubbleParent) throw std::runtime_error("activity map is not a bubble");
            MapEntry m;
            m.bubble = *bubble;
            m.package = package_of(pm, *bubble);
            m.name = std::format("{} / {:08X}", named.name, r.u32_at(row + 8));
            m.activity = named.hash;
            m.activity_map = uint32_t(k);
            m.bubble_name = r.u32_at(row + 8);
            out.push_back(std::move(m));
        }
      } catch (const std::exception& ex) {
        throw std::runtime_error(std::format("activity {} ({}): {}", named.hash.str(), named.name, ex.what()));
      }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

std::vector<TagHash> load_activity_node_tables(PackageManager& pm, TagHash activity, uint32_t bubble_name, uint32_t map_index) {
    std::set<TagHash> tables, activities, components;
    auto load = [&](auto&& self, TagHash tag) -> void {
        if (tag.is_none() || !activities.insert(tag).second) return;
        const auto data = pm.read(tag);
        TagReader r(data);
        const auto maps = r.vec_at(0x50);
        for (uint64_t k = 0; k < maps.count; ++k) {
            const size_t row = maps.data + size_t(k) * 0x38;
            if (tag == activity && k != map_index) continue;
            if (r.u32_at(row + 8) != bubble_name) continue;
            const auto phases = r.vec_at(row + 0x18);
            if (phases.count && phases.element_class != 0x8080B463) throw std::runtime_error("unknown activity phase class");
            for (uint64_t p = 0; p < phases.count; ++p) {
                // SUnk80808948 -> SUnk80808e89.unk18 -> SUnk80808ebe.components
                const auto phase = pm.read(r.tag_at(phases.data + size_t(p) * 24 + 0x14));
                const auto list_data = pm.read(TagReader(phase).tag_at(0x18));
                TagReader list(list_data);
                const auto refs = list.vec_at(8);
                for (uint64_t i = 0; i < refs.count; ++i) {
                    const auto wrapper = pm.read(list.tag_at(refs.data + size_t(i) * 4));
                    const auto component = TagReader(wrapper).tag_at(0x20);
                    if (!components.insert(component).second) continue;
                    const auto bytes = pm.read(component);
                    TagReader cr(bytes);
                    // Marathon's definition class ids differ from the older DEIMOS B1A5/B250
                    // cases. Resolve only typed node-table references authored in THIS selected
                    // phase component, never all node tables in the package family.
                    for (size_t off = 8; off + 4 <= bytes.size(); off += 4) {
                        const auto target = cr.tag_at(off);
                        if (!target.is_pkg_ref()) continue;
                        const auto* entry = pm.entry(target);
                        if (entry && entry->reference == kClassMapNodeTable && entry->type == 8) tables.insert(target);
                    }
                }
            }
            break;  // DEIMOS ambient lookup selects the first matching bubble_name
        }
        const auto ambient = resolve_wide_hash(pm, r.u32_at(0x68), r.u64_at(0x70));
        if (ambient) self(self, *ambient);
    };
    load(load, activity);
    return {tables.begin(), tables.end()};
}

std::array<float, 3> map_focus(const MapLayout& map) {
    std::array<float, 3> out{};
    for (int a = 0; a < 3; ++a) {
        std::vector<float> v;
        v.reserve(map.instances);
        for (const auto& st : map.statics)
            for (const auto& r : st.rows) v.push_back(r[size_t(a) * 4 + 3]);
        if (v.empty()) continue;
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        out[size_t(a)] = v[v.size() / 2];
    }
    return out;
}

std::vector<MapStatic> map_region(const MapLayout& map, std::array<float, 3> center, float radius) {
    return map_region(map.statics, center, radius);
}

std::vector<MapStatic> map_region(const std::vector<MapStatic>& placed, std::array<float, 3> center, float radius) {
    std::vector<std::pair<float, MapStatic>> found;
    const float r2 = radius * radius;
    for (const auto& st : placed) {
        MapStatic m{st.mesh, {}, {}};
        float nearest = 1e30f;
        for (size_t i = 0; i < st.rows.size(); ++i) {
            const auto& r = st.rows[i];
            const float dx = r[3] - center[0], dy = r[7] - center[1], dz = r[11] - center[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) continue;
            m.rows.push_back(r);
            if (!st.sky_order.empty()) m.sky_order.push_back(st.sky_order.at(i));
            nearest = std::min(nearest, d2);
        }
        if (!m.rows.empty()) found.emplace_back(nearest, std::move(m));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<MapStatic> out;
    out.reserve(found.size());
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

namespace {
std::array<float, 12> quat_rows(const TagReader& r, size_t te) {
    const float x = r.f32_at(te), y = r.f32_at(te + 4), z = r.f32_at(te + 8), w = r.f32_at(te + 12);
    const float tx = r.f32_at(te + 0x10), ty = r.f32_at(te + 0x14), tz = r.f32_at(te + 0x18);
    float s = r.f32_at(te + 0x1C);
    if (!std::isfinite(s) || s == 0) s = 1;
    const float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    return {(1 - 2 * (yy + zz)) * s, 2 * (xy - wz) * s, 2 * (xz + wy) * s, tx,
            2 * (xy + wz) * s, (1 - 2 * (xx + zz)) * s, 2 * (yz - wx) * s, ty,
            2 * (xz - wy) * s, 2 * (yz + wx) * s, (1 - 2 * (xx + yy)) * s, tz};
}
}  // namespace

MapEntities load_map_entities(PackageManager& pm, const MapLayout& map,
                              const std::unordered_map<uint32_t, DynamicModelMaterials>& materials) {
    MapEntities out;
    std::unordered_multimap<uint32_t, uint32_t> by_component;  // SDynamicModelComponent tag -> model
    for (const auto& [model, m] : materials) by_component.emplace(m.component.value, model);
    std::unordered_map<uint32_t, std::vector<uint32_t>> entity_models;
    std::unordered_map<uint32_t, size_t> slot;  // model -> out.models index
    for (const auto table : map.node_tables) {
            std::vector<uint8_t> data;
            try {
                data = pm.read(table);
            } catch (...) {
                continue;
            }
            ++out.node_tables;
            TagReader r(data);
            const auto nodes = r.vec_at(0x08);
            for (uint64_t k = 0; k < nodes.count; ++k) {
                const size_t e = nodes.data + size_t(k) * 0x90;
                if (e + 0x90 > data.size()) break;
                ++out.nodes;
                const uint64_t h64 = r.u64_at(e + 0x30);
                if (!h64 || h64 == ~0ull) continue;
                const auto ent = pm.resolve64(h64);
                if (!ent) continue;
                auto it = entity_models.find(ent->value);
                if (it == entity_models.end()) {
                    std::vector<uint32_t> models;
                    const Entry* ee = pm.entry(*ent);
                    if (ee && ee->type == 8 && ee->reference == 0x8080BAAD) {
                        try {
                            const auto ed = pm.read(*ent);
                            std::set<uint32_t> seen;
                            for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                                uint32_t v;
                                std::memcpy(&v, ed.data() + o, 4);
                                if (!seen.insert(v).second) continue;
                                const auto [a, b] = by_component.equal_range(v);
                                for (auto m = a; m != b; ++m) models.push_back(m->second);
                            }
                        } catch (...) {
                        }
                    }
                    it = entity_models.emplace(ent->value, std::move(models)).first;
                    ++out.entities;
                }
                if (it->second.empty()) continue;
                const auto rows = quat_rows(r, e);
                for (uint32_t m : it->second) {
                    auto [s, fresh] = slot.try_emplace(m, out.models.size());
                    if (fresh) out.models.push_back({TagHash(m), {}});
                    out.models[s->second].rows.push_back(rows);
                }
                ++out.placements;
            }
    }
    return out;
}

}  // namespace nim
