#include "nim/content/map.hpp"
#include "nim/content/object_channels.hpp"

#include "nim/content/tfx.hpp"
#include "nim/content/weather.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

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
                                        std::vector<TagHash>* node_tables = nullptr,
                                        std::vector<TagHash>* decorator_groups = nullptr,
                                        std::vector<TagHash>* decal_collections = nullptr,
                                        std::vector<TagHash>* decal_volumes = nullptr) {
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
            if (ce->reference == kClassDecoratorGroup) {
                seen.insert(c.value);
                if (decorator_groups) decorator_groups->push_back(c);
                return;
            }
            if (ce->reference == kClassMapDecals) {
                seen.insert(c.value);
                if (decal_collections) decal_collections->push_back(c);
                return;
            }
            if (ce->reference == 0x80808224) {
                seen.insert(c.value);
                if (decal_volumes) decal_volumes->push_back(c);
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
    for (size_t i = 0; i < l.params.size(); ++i) l.params[i] = r.f32_at(o + i * 4);
    l.technique = r.tag_at(o + technique);
    l.volumetrics = r.tag_at(o + technique + (shadowing ? 8 : 4));
    if (shadowing) {
        l.shadow_technique = r.tag_at(o + technique + 4);
        l.shadow_volumetrics = r.tag_at(o + technique + 12);
        // +0xFC: (light fade start, end, shaft fade start, end) in metres from the camera, e.g.
        // Outpost 80AF301F (130, 160, 100, 155); (0, 0) where it never fades.
        if (r.size() >= o + 0x10C)
            for (size_t i = 0; i < 4; ++i) l.fade_distances[i] = r.f32_at(o + 0xFC + i * 4);
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
            if (map.hidden(r.u64_at(e + 0x70), false)) continue;  // hide_scripted_nodes (3.111)
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
    // Maps the user confirmed are played at night in the game (Outpost). The sun path component's
    // +0x08 (0.5 = noon) is not the start time there, and the engine's time source is not in the
    // data: start such maps at the authored sky keyframe nearest to midnight (estimate).
    if (map.bubble.value == kBubbleOutpost && !out.paths[0].empty()) {
        out.default_time = out.night_time();
        out.default_time_source = "user-confirmed night map: the sky keyframe nearest to midnight (half a day after the sun's highest point; estimate)";
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
            // Kind 5 never shows in game (goriath / DEIMOS: solid red). Kind 6 is a low-resolution
            // greyscale capture of the whole sky (Outpost 80AF8E8B, Cryo Archive 80B37FBD) that
            // covers the authored layers drawn before it; left out as well (estimate).
            if (const uint32_t kind = r.u32_at(o + 0x70); kind == 5 || kind == 6) continue;
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

float MapAtmosphere::night_time() const {
    // Midnight = half a day after the moving sun's highest point. The authored sky keyframes
    // (lookup slices) hold the looks the artists made; use the one nearest to midnight. (The lowest
    // point of the path itself sits next to the t = 0 / 1 wrap, where the environment curves
    // already blend toward the dawn haze.)
    const auto& path = paths[0];
    if (path.empty()) return default_time;
    size_t high = 0;
    for (size_t i = 1; i < path.size(); ++i)
        if (path[i][2] > path[high][2]) high = i;
    const float midnight = std::fmod(float(high) / float(path.size()) + 0.5f, 1.0f);
    float best = default_time, gap = 2;
    for (float t : slice_times) {
        const float d = std::abs(t - midnight), g = std::min(d, 1 - d);
        if (g < gap) gap = g, best = t;
    }
    return best;
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
    out.instance_tags = find_instance_tags(pm, bubble, &terrain_tags, &out.node_tables, &out.decorator_groups,
                                           &out.decal_collections, &out.decal_volumes);
    for (const auto table : additional_tables) {
        const auto tags = find_instance_tags(pm, table, &terrain_tags, &out.node_tables, &out.decorator_groups,
                                             &out.decal_collections, &out.decal_volumes);
        out.instance_tags.insert(out.instance_tags.end(), tags.begin(), tags.end());
    }
    auto unique = [](auto& tags) { std::sort(tags.begin(), tags.end()); tags.erase(std::unique(tags.begin(), tags.end()), tags.end()); };
    // Keep the base map's authored order (probe diagnostic indices must stay stable).
    auto deduplicate = [](auto& tags) { std::set<TagHash> seen; std::erase_if(tags, [&](TagHash t) { return !seen.insert(t).second; }); };
    deduplicate(out.instance_tags);
    deduplicate(out.node_tables);
    deduplicate(out.decorator_groups);
    deduplicate(out.decal_collections);
    deduplicate(out.decal_volumes);
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
        MapStatic m{st.mesh, {}, {}, st.patrol_start, st.regions};
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

std::vector<MapDecal> load_map_decals(PackageManager& pm, const MapLayout& map) {
    constexpr size_t stride = 0x60;
    std::vector<MapDecal> result;
    for (const auto tag : map.decal_collections) {
        const auto bytes = pm.read(tag);
        const TagReader r(bytes);
        const auto records = r.vec_at(0x08);
        if (records.element_class != 0x808081AB || records.data > bytes.size() ||
            records.count > (bytes.size() - records.data) / stride)
            throw std::runtime_error("Invalid decal collection: " + tag.str());
        for (size_t i = 0; i < records.count; ++i) {
            const size_t at = records.data + i * stride;
            MapDecal decal;
            decal.collection = tag;
            decal.ordinal = uint32_t(i);
            decal.technique = r.tag_at(at);
            decal.index = r.tag_at(at + 4);
            decal.vertex = r.tag_at(at + 8);
            decal.triangle_count = r.u32_at(at + 0x0C);
            decal.rows = quat_rows(r, at + 0x10);
            r.copy_at(at + 0x30, decal.scale);
            r.copy_at(at + 0x40, decal.offset);
            const auto uv = r.read_at<std::array<float, 4>>(at + 0x50);
            decal.uv = {uv[0], uv[2], uv[1], uv[3]};
            result.push_back(decal);
        }
    }
    return result;
}

// The projected decals are authored as box instances. The node table references the
// collection, and the separate occlusion bounds give one world-space AABB per box.
std::vector<MapDecalVolumes> load_map_decal_volumes(PackageManager& pm, const MapLayout& map,
                                                  std::array<float, 3> center, float radius) {
    std::vector<MapDecalVolumes> result;
    for (const auto tag : map.decal_volumes) {
        const auto collection = pm.read(tag);
        const TagReader r(collection);
        const auto sets = r.vec_at(0x08);
        const auto bound_data = pm.read(r.tag_at(0x38));
        const TagReader br(bound_data);
        const auto boxes = br.vec_at(0x08);
        if (sets.element_class != 0x8080822C || sets.data > collection.size() ||
            sets.count > (collection.size() - sets.data) / 0x0C ||
            boxes.element_class != 0x8080A7F7 || boxes.data > bound_data.size() ||
            boxes.count > (bound_data.size() - boxes.data) / 0x30)
            throw std::runtime_error("Invalid projected decal collection: " + tag.str());
        if (r.u8_at(0x36) != 0 && r.u8_at(0x36) != 2) continue;
        MapDecalVolumes volumes{tag, r.tag_at(0x28), r.tag_at(0x2C), {}};
        for (size_t i = 0; i < sets.count; ++i) {
            const size_t at = sets.data + i * 0x0C;
            MapDecalVolumeSet set{r.tag_at(at), r.u16_at(at + 4), r.u16_at(at + 6)};
            if (!set.count || set.technique.is_none()) continue;
            if (size_t(set.first) + set.count > boxes.count)
                throw std::runtime_error("Projected decal set exceeds its bounds table: " + tag.str());
            std::array<float, 3> lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
            set.half_thickness = INFINITY;
            for (size_t j = 0; j < set.count; ++j) {
                const size_t box = boxes.data + (size_t(set.first) + j) * 0x30;
                for (size_t k = 0; k < 3; ++k) {
                    const float a = br.f32_at(box + k * 4), b = br.f32_at(box + 0x10 + k * 4);
                    lo[k] = std::min(lo[k], a);
                    hi[k] = std::max(hi[k], b);
                    if (j == 0) set.half_thickness = std::min(set.half_thickness, (b - a) * 0.5f);
                }
            }
            float distance_squared = 0, radius_squared = 0;
            for (size_t k = 0; k < 3; ++k) {
                const float distance = std::max({0.0f, lo[k] - center[k], center[k] - hi[k]});
                distance_squared += distance * distance;
                set.sphere[k] = (lo[k] + hi[k]) * 0.5f;
                const float extent = (hi[k] - lo[k]) * 0.5f;
                radius_squared += extent * extent;
            }
            if (distance_squared > radius * radius) continue;
            set.sphere[3] = std::sqrt(radius_squared);
            volumes.sets.push_back(set);
        }
        if (!volumes.sets.empty()) result.push_back(std::move(volumes));
    }
    return result;
}

MapEntities load_map_entities(PackageManager& pm, const MapLayout& map,
                              const std::unordered_map<uint32_t, DynamicModelMaterials>& materials) {
    MapEntities out;
    std::unordered_multimap<uint32_t, uint32_t> by_component;  // SDynamicModelComponent tag -> model
    for (const auto& [model, m] : materials) by_component.emplace(m.component.value, model);
    std::unordered_map<uint32_t, std::vector<uint32_t>> entity_models;
    // An entity's region selector with no spawn tags (its unconditional entry): which of a
    // model's alternate part groups the placement shows (a terminal's narrow or wide screen).
    // Per (entity, the node's tag pairs): a selector's rows are conditioned on tags the placing
    // node carries (the EXFIL terminal: 8F45EF1B = 4C11F193 wide / 63977E0B narrow screen).
    std::map<std::pair<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>, std::shared_ptr<const RegionPermutations>> entity_regions;
    std::unordered_set<uint32_t> has_selector;
    // model, patrol-start flag, selector and chosen entry -> out.models index
    std::map<std::tuple<uint32_t, bool, uint32_t, uint32_t>, size_t> slot;
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
                const auto node = r.u64_at(e + 0x70);
                if (map.hidden(node, false)) continue;  // hide_scripted_nodes (3.111)
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
                    if (!models.empty()) {
                        try {
                            // A selector exists when some tag set picks a row; rows without
                            // conditions are picked with none.
                            if (entity_has_region_selector(pm, *ent)) has_selector.insert(ent->value);
                        } catch (const std::exception&) {
                        }
                    }
                    it = entity_models.emplace(ent->value, std::move(models)).first;
                    ++out.entities;
                }
                const auto selected_model = map.preview_models.find(node);
                if (it->second.empty() && selected_model == map.preview_models.end()) continue;
                const auto rows = quat_rows(r, e);
                const bool patrol_start = map.patrol_drone_nodes.contains(node);
                std::shared_ptr<const RegionPermutations> regions;
                if (has_selector.contains(ent->value)) {
                    // Every (u32, u32) pair in the node's component records: the selector matches
                    // only its own condition pairs among them.
                    std::vector<std::pair<uint32_t, uint32_t>> tags;
                    for (size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78)), guard = 0;
                         p != e + 0x78 && p >= 4 && p + 0x10 <= data.size() && guard < 64; ++guard) {
                        const uint64_t next = r.u64_at(p);
                        const size_t end = std::min(data.size(), next ? p + size_t(next) : p + 0x100);
                        for (size_t q = p + 8; q + 8 <= end; q += 4) tags.push_back({r.u32_at(q), r.u32_at(q + 4)});
                        if (!next) break;
                        p += size_t(next);
                    }
                    std::ranges::sort(tags);
                    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
                    auto key = std::pair{ent->value, std::move(tags)};
                    auto er = entity_regions.find(key);
                    if (er == entity_regions.end()) {
                        std::shared_ptr<const RegionPermutations> found;
                        try {
                            if (auto rp = entity_region_permutations(pm, *ent, key.second); rp && !rp->region_permutation.empty())
                                found = std::make_shared<const RegionPermutations>(std::move(*rp));
                        } catch (const std::exception&) {
                        }
                        er = entity_regions.emplace(std::move(key), std::move(found)).first;
                    }
                    regions = er->second;
                }
                const auto add = [&](uint32_t m) {
                    if (const auto animated = map.event_actor_models.find(node);
                        animated != map.event_actor_models.end() && animated->second.value == m) return;
                    const auto key = std::tuple{m, patrol_start, regions ? regions->selector.value : 0u, regions ? regions->entry : 0u};
                    auto [s, fresh] = slot.try_emplace(key, out.models.size());
                    if (fresh) out.models.push_back({TagHash(m), {}, {}, patrol_start, regions});
                    out.models[s->second].rows.push_back(rows);
                };
                for (const uint32_t model : it->second) add(model);
                // Some spawn entities share another variant's dynamic-material component.
                // Their explicit model reference is still valid for this selected placement.
                if (selected_model != map.preview_models.end() &&
                    std::ranges::find(it->second, selected_model->second.value) == it->second.end())
                    add(selected_model->second.value);
                ++out.placements;
            }
    }
    return out;
}

static MapGameplay read_map_gameplay(PackageManager& pm, TagHash activity, const MapEntry* selection) {
    MapGameplay out;
    struct EntityInfo { bool spawn = false; TagHash model; };
    std::unordered_map<uint32_t, EntityInfo> entities;
    auto entity_info = [&](TagHash ent) -> const EntityInfo& {
        if (auto it = entities.find(ent.value); it != entities.end()) return it->second;
        EntityInfo info;
        try {
            const auto ed = pm.read(ent);
            std::set<uint32_t> seen;
            for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, ed.data() + o, 4);
                const TagHash c(v);
                if (!c.is_pkg_ref() || !seen.insert(v).second) continue;
                const Entry* ce = pm.entry(c);
                if (!ce || ce->type != 8 || ce->reference != 0x8080BADB) continue;
                const auto cd = pm.read(c);
                TagReader cr(cd);
                if (cd.size() >= 0xA8 && cr.u32_at(0xA4) == kClassStartComponent) info.spawn = true;
                for (size_t q = 0; q + 4 <= cd.size() && info.model.is_none(); q += 4) {
                    const TagHash m(cr.u32_at(q));
                    if (!m.is_pkg_ref()) continue;
                    if (const Entry* me = pm.entry(m); me && me->type == 8 && me->reference == 0x8080881C) info.model = m;
                }
            }
        } catch (...) {
        }
        return entities.emplace(ent.value, info).first->second;
    };
    std::unordered_map<uint64_t, MapGameplayPoint> by_id;
    TagHash bubble;
    std::vector<MapEntry> activity_maps;
    // The activity's own nodes first; then the other activities on the same bubble, whose nodes
    // only resolve squad locations (3.112: the ambient and TDM activities place none of the squads'
    // location nodes, the expedition activity does; the squads are the bubble family's).
    std::vector<const MapEntry*> order;
    size_t own_maps = 1;
    if (selection) {
        bubble = selection->bubble;
        order.push_back(selection);
    } else {
        activity_maps = list_activity_maps(pm);
        for (const auto& m : activity_maps)
            if (m.activity == activity) {
                bubble = m.bubble;
                order.push_back(&m);
            }
        own_maps = order.size();
        for (const auto& m : activity_maps)
            if (m.bubble == bubble && m.activity != activity) order.push_back(&m);
    }
    for (size_t mi = 0; mi < order.size(); ++mi) {
        const auto& m = *order[mi];
        const bool own = mi < own_maps;
        const auto tables = m.activity.is_none() ? std::vector<TagHash>{} :
            load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map);
        const auto layout = load_map_layout(pm, m.bubble, tables);
        for (const auto table : layout.node_tables) {
            std::vector<uint8_t> data;
            try {
                data = pm.read(table);
            } catch (...) {
                continue;
            }
            TagReader r(data);
            const auto nodes = r.vec_at(0x08);
            for (uint64_t k = 0; k < nodes.count; ++k) {
                const size_t e = nodes.data + size_t(k) * 0x90;
                if (e + 0x90 > data.size()) break;
                out.nodes += own;
                MapGameplayPoint pt;
                for (size_t a = 0; a < 4; ++a) pt.rotation[a] = r.f32_at(e + a * 4);
                for (size_t a = 0; a < 3; ++a) pt.position[a] = r.f32_at(e + 0x10 + a * 4);
                pt.id = r.u64_at(e + 0x70);
                bool start = false;  // a start component on the node itself (the player starts)
                for (size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78)), guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                    const uint64_t next = r.u64_at(p);
                    if (r.u32_at(p - 4) == kClassStartComponent) start = true;
                    if (r.u32_at(p - 4) == 0x80802EC2) {
                        const uint32_t n = std::min(r.u32_at(p + 0x10), 16u);
                        for (uint32_t q = 0; q < n && p + 0x18 + q * 4 <= data.size(); ++q)
                            if (const uint32_t h = r.u32_at(p + 0x14 + q * 4); h != 0x811C9DC5u) pt.labels.push_back(h);
                    }
                    if (!next) break;
                    p += size_t(next);
                }
                const uint64_t h64 = r.u64_at(e + 0x30);
                if (!own) {
                    if (pt.id && pt.id != ~0ull && !by_id.contains(pt.id)) {
                        if (h64 && h64 != ~0ull)
                            if (const auto ent = pm.resolve64(h64)) pt.entity = *ent, pt.model = entity_info(*ent).model;
                        by_id.emplace(pt.id, std::move(pt));
                    }
                    continue;
                }
                bool spawn = start;
                if (h64 && h64 != ~0ull)
                    if (const auto ent = pm.resolve64(h64)) {
                        pt.entity = *ent;
                        const auto& info = entity_info(*ent);
                        pt.model = info.model;
                        spawn = spawn || info.spawn;
                        if (!spawn && !pt.labels.empty() && !info.model.is_none()) out.loot.push_back(pt);
                    }
                if (spawn) out.spawns.push_back(pt);
                if (pt.id && pt.id != ~0ull) by_id.try_emplace(pt.id, std::move(pt));
            }
        }
    }
    if (bubble.is_none()) throw std::runtime_error(std::format("{}: not an activity with maps", activity.str()));
    // Script components of the map's package family (3.65, 3.66).
    std::unordered_map<uint32_t, std::string> modules;
    auto module_of = [&](uint32_t script) -> const std::string& {
        if (auto it = modules.find(script); it != modules.end()) return it->second;
        std::string name;
        try {
            const auto d = pm.read(TagHash(script));
            const std::string text(d.begin(), d.end());
            if (const auto at = text.find("bs\\"); at != std::string::npos) name = text.c_str() + at;
        } catch (...) {
        }
        return modules.emplace(script, name.empty() ? TagHash(script).str() : name).first->second;
    };
    // Characters: 8080B1A7 descriptors whose entity (+0x60 hash64) has a bs/character/character.bs
    // component. Squads name them by the descriptor's own hash64.
    std::unordered_map<uint32_t, bool> character_entity;
    auto is_character = [&](TagHash ent) {
        if (auto it = character_entity.find(ent.value); it != character_entity.end()) return it->second;
        bool yes = false;
        try {
            const auto ed = pm.read(ent);
            std::set<uint32_t> seen;
            for (size_t o = 0; o + 4 <= ed.size() && !yes; o += 4) {
                uint32_t v;
                std::memcpy(&v, ed.data() + o, 4);
                const TagHash c(v);
                if (!c.is_pkg_ref() || !seen.insert(v).second) continue;
                const Entry* ce = pm.entry(c);
                if (!ce || ce->type != 8 || ce->reference != 0x8080BADB) continue;
                const auto cd = pm.read(c);
                for (size_t q = 0; q + 4 <= cd.size() && !yes; q += 4) {
                    uint32_t w;
                    std::memcpy(&w, cd.data() + q, 4);
                    if (const TagHash dh(w); dh.is_pkg_ref())
                        if (const Entry* de = pm.entry(dh); de && de->type == 8 && de->reference == 0x80800000)
                            yes = module_of(w).ends_with("\\character\\character.bs");
                }
            }
        } catch (...) {
        }
        return character_entity.emplace(ent.value, yes).first->second;
    };
    std::unordered_map<uint32_t, std::optional<size_t>> character_of;  // 8080B1A7 tag -> out.characters
    auto character = [&](TagHash item) -> std::optional<size_t> {
        if (auto it = character_of.find(item.value); it != character_of.end()) return it->second;
        std::optional<size_t> res;
        try {
            const auto d = pm.read(item);
            if (d.size() >= 0x68) {
                TagReader r(d);
                if (const auto ent = pm.resolve64(r.u64_at(0x60)); ent && is_character(*ent)) {
                    MapCharacter c{item, *ent, {}};
                    try {
                        if (const auto tags = r.vec_at(0x130); tags.element_class == 0x8080402D)
                            for (uint64_t q = 0; q < tags.count; ++q)
                                c.tags.push_back({r.u32_at(tags.data + size_t(q) * 8), r.u32_at(tags.data + size_t(q) * 8 + 4)});
                    } catch (const std::exception&) {
                    }
                    for (size_t o = 0; o + 0x1C <= d.size(); o += 4)
                        if (r.u32_at(o) == 0x80802EC2) {
                            const uint32_t n = std::min(r.u32_at(o + 0x14), 16u);
                            for (uint32_t q = 0; q < n && o + 0x1C + q * 4 <= d.size(); ++q)
                                if (const uint32_t h = r.u32_at(o + 0x18 + q * 4); h != 0x811C9DC5u) c.labels.push_back(h);
                            break;
                        }
                    res = out.characters.size();
                    out.characters.push_back(std::move(c));
                }
            }
        } catch (...) {
        }
        return character_of.emplace(item.value, res).first->second;
    };
    // A component's name table (8080B1E2 at +0x80): records 89AB8080 {u64 string offset from the
    // field, u32 component, u32 class, u64 offset}, 0x20 after it 88AB8080 {u32 component, u32
    // class, u64 field offset}; the strings follow the records.
    auto names_of = [&](uint32_t table) {
        std::vector<std::pair<uint32_t, std::string>> names;
        const TagHash t(table);
        if (!t.is_pkg_ref()) return names;
        const Entry* e = pm.entry(t);
        if (!e || e->type != 8 || e->reference != 0x8080B1E2) return names;
        try {
            const auto d = pm.read(t);
            TagReader r(d);
            for (size_t o = 0; o + 0x30 <= d.size(); o += 4) {
                if (r.u32_at(o) != 0x8080AB89u) continue;
                const size_t at = o + 4 + size_t(r.u64_at(o + 4));
                if (at >= d.size() || r.u32_at(o + 0x20) != 0x8080AB88u) continue;
                const char* c = reinterpret_cast<const char*>(d.data() + at);
                names.emplace_back(uint32_t(r.u64_at(o + 0x2C)), std::string(c, strnlen(c, d.size() - at)));
            }
        } catch (...) {
        }
        std::ranges::sort(names);
        return names;
    };
    // Slot header: 12 bytes, the slot's 64-bit id 0x20 after it. Components refer to other slots by
    // that id (a squad to its spawn location, a point group's owner to the group). The 12 bytes
    // differ per map (Outpost 1134D592..., Perimeter 7661882A...): the header is found where it
    // sits, right after the component's record {u32 kind, u32 its own tag, u32 class, u64 0xB0}
    // (3.112). The Outpost bytes are the fallback.
    static constexpr uint8_t kSlotHeader[12] = {0x11, 0x34, 0xD5, 0x92, 0x55, 0xC2, 0x5F, 0xC9, 0x98, 0xFB, 0xA8, 0x2E};
    auto find_header = [](const std::vector<uint8_t>& d, uint32_t own) -> size_t {
        for (size_t o = 0; o + 16 + 0x28 <= d.size(); o += 4) {
            uint32_t a;
            uint64_t b;
            std::memcpy(&a, d.data() + o, 4);
            std::memcpy(&b, d.data() + o + 8, 8);
            if (a == own && b == 0xB0) return o + 16;
        }
        const auto it = std::search(d.begin(), d.end(), std::begin(kSlotHeader), std::end(kSlotHeader));
        return it == d.end() || size_t(it - d.begin()) + 0x28 > d.size() ? SIZE_MAX : size_t(it - d.begin());
    };
    std::vector<size_t> header_at;  // per raw component: its slot header offset, or SIZE_MAX
    std::vector<MapScriptObject> all;
    std::vector<std::vector<uint8_t>> raw;
    std::unordered_map<uint64_t, size_t> by_slot;
    std::map<std::string, size_t> census;
    const PackageInfo* home = pm.info(bubble.pkg_id());
    const std::string family = home ? home->name.substr(0, home->name.rfind('_') + 1) : std::string();
    for (const auto& info : pm.packages()) {
        if (family.empty() || !info.name.starts_with(family)) continue;
        Package& pk = pm.open(info);
        for (uint32_t i = 0; i < pk.entries().size(); ++i) {
            const Entry& en = pk.entries()[i];
            if (en.type != 8 || en.reference != 0x8080BADB) continue;
            std::vector<uint8_t> d;
            try {
                d = pk.read_entry(i);
            } catch (...) {
                continue;
            }
            if (d.size() < 0xB0) continue;
            MapScriptObject obj;
            obj.component = TagHash::make(info.pkg_id, uint16_t(i));
            for (size_t o = 0; o + 8 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                if (const TagHash h(v); h.is_pkg_ref() && obj.definition.is_none())
                    if (const Entry* se = pm.entry(h); se && se->type == 8 && se->reference == 0x80800000) {
                        obj.definition = h;
                        obj.module = module_of(v);
                    }
                uint64_t w;
                std::memcpy(&w, d.data() + o, 8);
                if (w == 0 || w == ~0ull) continue;
                if (auto f = by_id.find(w); f != by_id.end()) obj.placements.emplace_back(uint32_t(o), f->second);
                else if (const auto t = pm.resolve64(w))
                    if (const Entry* te = pm.entry(*t); te && te->type == 8 && te->reference == 0x8080B1A7)
                        if (const auto c = character(*t)) obj.characters.emplace_back(uint32_t(o), *c);
            }
            if (obj.definition.is_none()) {
                std::memcpy(&obj.native_class, d.data() + 0xAC, 4);
                obj.module = std::format("native {:08X}", obj.native_class);
            }
            ++census[obj.module];
            uint32_t table;
            std::memcpy(&table, d.data() + 0x80, 4);
            obj.names = names_of(table);
            const size_t hdr = find_header(d, obj.component.value);
            if (hdr != SIZE_MAX) {
                std::memcpy(&obj.slot, d.data() + hdr + 0x20, 8);
                if (obj.slot && obj.slot != ~0ull) by_slot.try_emplace(obj.slot, all.size());
            }
            header_at.push_back(hdr);
            all.push_back(std::move(obj));
            raw.push_back(std::move(d));
        }
    }
    std::vector<char> linked(all.size(), 0);
    for (size_t k = 0; k < all.size(); ++k) {
        const auto& d = raw[k];
        for (size_t o = 0; o + 8 <= d.size(); o += 4) {
            uint64_t w;
            std::memcpy(&w, d.data() + o, 8);
            if (w == 0 || w == ~0ull || w == all[k].slot) continue;
            if (auto f = by_slot.find(w); f != by_slot.end() && f->second != k) {
                all[k].links.emplace_back(uint32_t(o), f->second);
                linked[f->second] = 1;
            }
        }
    }
    // Keep the components that place, spawn or link something, and the ones linked to.
    std::vector<size_t> index(all.size(), SIZE_MAX), orig;
    for (size_t k = 0; k < all.size(); ++k)
        if (!all[k].placements.empty() || !all[k].characters.empty() || !all[k].links.empty() || linked[k] ||
            (selection && !all[k].definition.is_none())) {
            index[k] = out.scripts.size();
            orig.push_back(k);
            out.scripts.push_back(std::move(all[k]));
        }
    for (auto& obj : out.scripts) {
        std::erase_if(obj.links, [&](const auto& l) { return index[l.second] == SIZE_MAX; });
        for (auto& l : obj.links) l.second = index[l.second];
    }
    out.modules.assign(census.begin(), census.end());
    std::ranges::stable_sort(out.modules, [](const auto& a, const auto& b) { return a.second > b.second; });
    // Firing areas, objectives and point groups (3.67).
    auto typed_list = [](const std::vector<uint8_t>& d, uint32_t type) -> std::pair<size_t, size_t> {
        for (size_t o = 0; o + 0x14 <= d.size(); o += 4) {
            uint32_t a, c;
            std::memcpy(&a, d.data() + o, 4);
            std::memcpy(&c, d.data() + o + 12, 4);
            if (a != 0x8080BFCDu || c != type) continue;
            uint64_t n;
            std::memcpy(&n, d.data() + o + 4, 8);
            return {o + 0x14, size_t(n)};
        }
        return {0, 0};
    };
    auto header = [&](size_t raw_index) { return header_at[raw_index]; };
    auto rotate = [](const std::array<float, 4>& q, std::array<float, 3> v) {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
        return std::array<float, 3>{v[0] + w * tx + (y * tz - z * ty), v[1] + w * ty + (z * tx - x * tz), v[2] + w * tz + (x * ty - y * tx)};
    };
    std::vector<std::optional<size_t>> area_of(out.scripts.size()), owner_of(out.scripts.size());
    for (size_t k = 0; k < out.scripts.size(); ++k)
        for (const auto& l : out.scripts[k].links)
            if (!owner_of[l.second]) owner_of[l.second] = k;
    for (size_t k = 0; k < out.scripts.size(); ++k) {
        const auto& o = out.scripts[k];
        const auto& d = raw[orig[k]];
        const size_t hh = header(orig[k]);
        if (hh == SIZE_MAX) continue;
        TagReader r(d);
        if (o.native_class == kClassAiFiringArea && hh + 0x194 <= d.size()) {
            const auto [vo, nv] = typed_list(d, 0x80800094);
            const auto [io, ni] = typed_list(d, 0x8080BA19);
            if (!nv || vo + nv * 16 > d.size() || io + ni * 3 > d.size()) continue;  // template without a polygon
            MapFiringArea fa;
            fa.script = k;
            for (size_t a = 0; a < 4; ++a) fa.rotation[a] = r.f32_at(hh + 0x70 + a * 4);
            for (size_t a = 0; a < 3; ++a) fa.translation[a] = r.f32_at(hh + 0x80 + a * 4);
            fa.height = r.f32_at(hh + 0x190);
            const std::array<float, 4> inv{-fa.rotation[0], -fa.rotation[1], -fa.rotation[2], fa.rotation[3]};
            for (size_t v = 0; v < nv; ++v) {
                std::array<float, 3> local{};
                for (size_t a = 0; a < 3; ++a) local[a] = r.f32_at(vo + v * 16 + a * 4) - fa.translation[a];
                fa.outline.push_back(rotate(inv, local));
            }
            for (size_t i = 0; i < ni; ++i)
                if (d[io + i * 3] < nv && d[io + i * 3 + 1] < nv && d[io + i * 3 + 2] < nv)
                    fa.triangles.push_back({d[io + i * 3], d[io + i * 3 + 1], d[io + i * 3 + 2]});
            // Debug path: string record 80800065 {u64 length, chars}; the last one is the area's.
            for (size_t at = d.size() >= 16 ? (d.size() - 16) & ~size_t(3) : 0; at > hh; at -= 4) {
                if (r.u32_at(at) != 0x80800065u) continue;
                const size_t n = size_t(r.u64_at(at + 4));
                if (at + 12 + n > d.size()) break;
                std::string path(reinterpret_cast<const char*>(d.data() + at + 12), strnlen(reinterpret_cast<const char*>(d.data() + at + 12), n));
                fa.name = path.substr(path.find_last_of('.') + 1);
                break;
            }
            area_of[k] = out.firing_areas.size();
            out.firing_areas.push_back(std::move(fa));
        }
    }
    std::vector<std::optional<size_t>> objective_of(out.scripts.size());
    for (size_t k = 0; k < out.scripts.size(); ++k) {
        const auto& o = out.scripts[k];
        if (o.native_class == kClassAiObjective) {
            MapObjective ob;
            ob.script = k;
            std::set<size_t> seen;
            for (const auto& l : o.links)
                if (area_of[l.second] && seen.insert(*area_of[l.second]).second) ob.firing_areas.push_back(*area_of[l.second]);
            objective_of[k] = out.objectives.size();
            out.objectives.push_back(std::move(ob));
        } else if (o.native_class == kClassPointGroup) {
            MapPointGroup pg;
            pg.script = k;
            pg.owner = owner_of[k];
            std::set<size_t> seen;
            for (const auto& l : o.links) {
                const auto& target = out.scripts[l.second];
                if (target.native_class != kClassPoint || !seen.insert(l.second).second) continue;
                const auto& pd = raw[orig[l.second]];
                const size_t ph = header(orig[l.second]);
                if (ph == SIZE_MAX || ph + 0x8C > pd.size()) continue;
                TagReader pr(pd);
                pg.points.push_back({pr.f32_at(ph + 0x80), pr.f32_at(ph + 0x84), pr.f32_at(ph + 0x88)});
            }
            const auto [so, ns] = typed_list(raw[orig[k]], 0x80800090);
            const auto& d = raw[orig[k]];
            TagReader r(d);
            for (size_t i = 0; i < ns && so + i * 16 + 12 <= d.size(); ++i)
                pg.spline.push_back({r.f32_at(so + i * 16), r.f32_at(so + i * 16 + 4), r.f32_at(so + i * 16 + 8)});
            out.point_groups.push_back(std::move(pg));
        }
    }
    // Squads: their members, group and spawn positions (3.66).
    std::vector<std::optional<size_t>> group_of(out.scripts.size());
    for (size_t k = 0; k < out.scripts.size(); ++k)
        if (out.scripts[k].module.ends_with("\\slots\\squad_group.bs"))
            for (const auto& l : out.scripts[k].links) group_of[l.second] = k;
    std::vector<std::map<std::string, size_t>> votes(out.characters.size());
    for (size_t k = 0; k < out.scripts.size(); ++k) {
        const auto& o = out.scripts[k];
        if (o.native_class != kClassAiSquad || o.characters.empty()) continue;
        MapSquad sq;
        sq.script = k;
        sq.group = group_of[k];
        for (const auto& l : o.links)
            if (objective_of[l.second] && !sq.objective) sq.objective = objective_of[l.second];
        for (const auto& c : o.characters) sq.members.push_back(c.second);
        std::set<uint64_t> seen;
        for (const auto& l : o.links) {
            const auto& target = out.scripts[l.second];
            if (target.native_class == kClassAiSquad || !target.definition.is_none()) continue;
            for (const auto& pl : target.placements)
                if (seen.insert(pl.second.id).second) sq.positions.push_back(pl.second);
        }
        // Name votes: squads spawning a single character, member slots named NAME[_core][_N].
        if (std::ranges::all_of(sq.members, [&](size_t m) { return m == sq.members.front(); })) {
            std::set<std::string> bases;
            for (const auto& [off, n] : o.names) {
                if (n == "perception" || n == "population_label" || n == "tier" || n == "channel") continue;
                std::string b = n;
                if (const auto u = b.find_last_of('_'); u != std::string::npos && u + 1 < b.size() &&
                    std::all_of(b.begin() + u + 1, b.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    b.resize(u);
                if (b.ends_with("_core")) b.resize(b.size() - 5);
                bases.insert(b);
            }
            if (bases.size() == 1) ++votes[sq.members.front()][*bases.begin()];
        }
        out.squads.push_back(std::move(sq));
    }
    for (size_t c = 0; c < out.characters.size(); ++c) {
        size_t best = 0;
        for (const auto& [n, v] : votes[c])
            if (v > best) best = v, out.characters[c].name_estimate = n;
    }
    return out;
}

MapGameplay load_map_gameplay(PackageManager& pm, TagHash activity) {
    return read_map_gameplay(pm, activity, nullptr);
}

MapGameplay load_map_gameplay_selection(PackageManager& pm, const MapEntry& selection) {
    return read_map_gameplay(pm, selection.activity, &selection);
}

std::vector<MapLootContainer> load_map_loot(PackageManager& pm, const MapEntry& selection) {
    // Audited opening motions (PROGRESS §3.137); each clip belongs to the placed model.
    constexpr std::pair<uint32_t, uint32_t> open_clips[] = {
        {0x80A90349, 0x80A9035A}, {0x80A906E4, 0x80ADF30F}, {0x80ADF2EB, 0x80ADF30F},
        {0x80ADF303, 0x80ADF40A}, {0x80ADF77C, 0x80ADF78E}, {0x80ADF796, 0x80ADF7A8},
        {0x80ADF852, 0x80ADF864},
        {0x80ADF878, 0x80ADF88A}, {0x80ADF892, 0x80ADF8A2}, {0x80AF9749, 0x80AF975A},
        {0x80AF9763, 0x80AF9773}, {0x80AF977C, 0x80AF9789}, {0x80AF979F, 0x80AF97B3},
        {0x80AF9A9D, 0x80AF97C7}, {0x80AFB85A, 0x80AF97C7}, {0x80AFB866, 0x80AFB94C},
        {0x80B38D28, 0x80AF9773}, {0x80B3CF04, 0x80AF975A},
    };
    const auto components = selection.activity.is_none() ? std::vector<TagHash>{} :
        load_activity_node_tables(pm, selection.activity, selection.bubble_name, selection.activity_map);
    const auto layout = load_map_layout(pm, selection.bubble, components);
    pm.build_hash64_index();
    std::unordered_map<uint32_t, std::pair<TagHash, TagHash>> models_by_entity;
    std::unordered_map<uint32_t, std::pair<TagHash, TagHash>> models_by_component;
    auto find_model = [&](TagHash entity) -> std::pair<TagHash, TagHash> {
        const auto [known, fresh] = models_by_entity.try_emplace(entity.value);
        if (!fresh) return known->second;
        const auto* entry = pm.entry(entity);
        if (!entry || entry->reference != 0x8080BAAD) return {};
        const auto bytes = pm.read(entity);
        const TagReader reader(bytes);
        const auto list = reader.vec_at(0x08);
        for (size_t i = 0; list.data <= bytes.size() && i < std::min<uint64_t>(list.count, (bytes.size() - list.data) / 12); ++i) {
            const auto component = reader.tag_at(list.data + i * 12);
            const auto [cached, new_component] = models_by_component.try_emplace(component.value);
            if (new_component) {
                const auto* metadata = pm.entry(component);
                if (metadata && metadata->reference == 0x8080BADB) {
                    const auto data = pm.read(component);
                    const TagReader part(data);
                    if (data.size() >= 0x18) {
                        const auto p = part.pointer_at(0x10);
                        // The model reference belongs to the dynamic-model component.
                        if (p >= 4 && p < data.size() && part.u32_at(p - 4) == 0x80808673)
                            for (size_t o = 0; o + 4 <= data.size(); o += 4) {
                                const auto m = part.tag_at(o);
                                const auto clip = std::ranges::find(open_clips, m.value, &std::pair<uint32_t, uint32_t>::first);
                                if (clip != std::end(open_clips)) {
                                    cached->second = {m, TagHash(clip->second)};
                                    break;
                                }
                            }
                    }
                }
            }
            if (!cached->second.first.is_none()) return known->second = cached->second;
        }
        return {};
    };
    std::vector<MapLootContainer> out;
    std::unordered_set<uint64_t> seen;
    for (const auto table : layout.node_tables) {
        const auto bytes = pm.read(table);
        const TagReader reader(bytes);
        const auto nodes = reader.vec_at(0x08);
        if (nodes.data > bytes.size()) continue;
        for (size_t i = 0; i < std::min<uint64_t>(nodes.count, (bytes.size() - nodes.data) / 0x90); ++i) {
            const size_t p = nodes.data + i * 0x90;
            const uint64_t id = reader.u64_at(p + 0x70);
            if (!id || id == ~0ull || seen.contains(id)) continue;
            const auto entity = pm.resolve64(reader.u64_at(p + 0x30));
            if (!entity) continue;
            const auto [model, clip] = find_model(*entity);
            if (model.is_none()) continue;
            MapLootContainer container{id, {reader.f32_at(p + 0x10), reader.f32_at(p + 0x14), reader.f32_at(p + 0x18)},
                                       *entity, model, clip, {}};
            const size_t link = p + 0x78;
            size_t component = reader.pointer_at(link);
            for (unsigned guard = 0; component != link && component >= 4 && component <= bytes.size() &&
                 bytes.size() - component >= 0x20 && guard < 64; ++guard) {
                if (reader.u32_at(component - 4) == 0x8080402E) {
                    const auto tags = reader.vec_at(component + 0x10);
                    if (tags.element_class == 0x8080402D && tags.data <= bytes.size())
                        for (size_t j = 0; j < std::min<uint64_t>(tags.count, (bytes.size() - tags.data) / 8); ++j)
                            container.tags.emplace_back(reader.u32_at(tags.data + j * 8), reader.u32_at(tags.data + j * 8 + 4));
                }
                const uint64_t next = reader.u64_at(component);
                if (!next || next > bytes.size() - component) break;
                component += size_t(next);
            }
            seen.insert(id);
            out.push_back(std::move(container));
        }
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) { return std::tie(a.model, a.position, a.node) < std::tie(b.model, b.position, b.node); });
    return out;
}

size_t hide_scripted_nodes(PackageManager& pm, MapLayout& map, TagHash activity, uint64_t preview) {
    map.hidden_nodes.clear();
    map.hidden_fx_nodes.clear();
    map.patrol_drone_nodes.clear();
    map.event_previews.clear();
    map.preview_models.clear();
    map.event_groups.clear();
    map.event_fx_owner.clear();
    map.event_actor_models.clear();
    // Every event is loaded with the map and played on demand (3.133); `preview` only names the
    // one a caller wants first and changes nothing here.
    (void)preview;
    std::unordered_map<uint64_t, std::array<float, 3>> map_positions;
    std::unordered_map<uint64_t, MapEventPlacement> available_placements;
    std::set<uint64_t> event_ids;
    // The event's models: the placement at `id` with `model` (none: resolved from the entity).
    auto placement_of = [&](uint64_t id, TagHash model) -> std::optional<MapEventPlacement> {
        const auto p = available_placements.find(id);
        if (p == available_placements.end()) return std::nullopt;
        auto out = p->second;
        if (!model.is_none()) out.model = model;
        return out;
    };
    const auto available = list_activity_maps(pm);
    const bool expedition = std::ranges::any_of(available, [&](const MapEntry& entry) {
        return entry.bubble == map.bubble && (entry.name.starts_with("exp_") || entry.name.starts_with("expeditions_")) &&
            (activity.is_none() || entry.activity == activity);
    });
    std::vector<TagHash> activities;
    if (!activity.is_none()) activities.push_back(activity);
    else
        for (const auto& m : available)
            if (m.bubble == map.bubble && std::ranges::find(activities, m.activity) == activities.end()) activities.push_back(m.activity);
    // The room-lockdown script also links doors and gameplay nodes. Match the ship
    // placements by entity so those other objects are not removed with it.
    for (TagHash table : map.node_tables) {
        const auto data = pm.read(table);
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t entry = nodes.data + size_t(i) * 0x90;
            if (entry + 0x90 > data.size()) break;
            const uint64_t id = r.u64_at(entry + 0x70);
            if (id == 0 || id == ~0ull) continue;
            const std::array position{r.f32_at(entry + 0x10), r.f32_at(entry + 0x14), r.f32_at(entry + 0x18)};
            if (expedition) map_positions.try_emplace(id, position);
            const auto entity = pm.resolve64(r.u64_at(entry + 0x30));
            if (!entity) continue;
            if (expedition)
                available_placements.try_emplace(id, MapEventPlacement{id, *entity, {}, quat_rows(r, entry)});
            const bool lockdown = entity->value == 0x80B08991 || entity->value == 0x80B0D412;
            const bool dropship = entity->value == 0x80ACA699 || entity->value == 0x80ACA69F || entity->value == 0x80ACA6A4;
            if (!lockdown && !(expedition && dropship)) continue;
            map.hidden_nodes.insert(id);
            if (expedition && event_ids.insert(id).second) {
                map.event_previews.push_back({id, lockdown ? "lockdown ship" : "UESC dropship", position, 1});
                MapEventGroup group{id, {id}, {}, {}, dropship};
                if (auto p = placement_of(id, dropship ? TagHash(0x80ACA5E7) : TagHash())) group.placements.push_back(*p);
                map.event_groups.push_back(std::move(group));
            }
        }
    }
    for (const TagHash a : activities) {
        const MapGameplay g = load_map_gameplay(pm, a);
        for (const auto& group : g.point_groups) {
            if (group.spline.size() < 2 || !group.owner || *group.owner >= g.scripts.size()) continue;
            const auto& owner = g.scripts[*group.owner];
            if (!owner.module.ends_with("uesc_scan_drone.bs")) continue;
            for (const auto& [offset, placement] : owner.placements)
                if (offset == 0x758 && placement.id && !placement.model.is_none())
                    map.patrol_drone_nodes.insert(placement.id);
        }
        auto place_all = [&](size_t k, std::unordered_set<uint64_t>& into) {
            for (const auto& [off, p] : g.scripts[k].placements)
                if (p.id) into.insert(p.id);
        };
        // Keep cold air / frost hazard FX (cryo_env_hazard.es) visible; hide all other
        // event/script-driven FX (Compiler aura terminals, dcon, toxic gas, etc.) so they
        // do not play while idle (user decision, 2026-11-24).
        std::unordered_set<uint64_t> cold_fx_nodes;
        auto walk_placements = [&](size_t root, auto&& on_node) {
            std::vector<size_t> at{root};
            std::set<size_t> seen{root};
            for (int depth = 0; depth <= 2 && !at.empty(); ++depth) {
                std::vector<size_t> next;
                for (const size_t x : at) {
                    on_node(x);
                    for (const auto& [off, l] : g.scripts[x].links)
                        if (seen.insert(l).second) next.push_back(l);
                }
                at = std::move(next);
            }
        };
        for (size_t k = 0; k < g.scripts.size(); ++k) {
            const std::string& m = g.scripts[k].module;
            if (m.find("cryo_env_hazard") != std::string::npos)
                walk_placements(k, [&](size_t x) { place_all(x, cold_fx_nodes); });
        }
        for (size_t k = 0; k < g.scripts.size(); ++k) {
            const std::string& m = g.scripts[k].module;
            if (m.empty() || m.starts_with("native ") || m.find("cryo_env_hazard") != std::string::npos) continue;
            const std::string name = m.substr(m.find_last_of("\\") + 1);
            const bool flyover = expedition && name == "pf_dropship_flyover.es";
            const bool event_root = expedition && (m.starts_with("bs\\activities\\events\\") ||
                m.starts_with("bs\\world\\dynamic_events\\") || name.starts_with("enc_") ||
                m.starts_with("bs\\world\\cryo_archive\\boss_encounters\\") ||
                name == "pf_uesc_convoy.es" || name == "pf_night_uesc_convoy.es" ||
                name == "pf_hvt_engineer.es" || name == "intercept_ro_encounter_prefab.es" ||
                name == "pf_le_ambush_encounter.es" ||
                name == "pf_dcon.bs" || name.starts_with("pf_dcon_") || name == "dcon_combiner.bs" ||
                name == "compiler_aura_terminals.es");
            std::set<uint64_t> placements;
            std::map<uint64_t, TagHash> ship_props;  // the flyover's hidden ships and props
            if (flyover) {
                // The flyover's directly linked native layouts carry its ship and set dressing.
                // The two props share model 80B580E0. Keep the gate tied to the original node.
                for (const auto& [off, source] : g.scripts[k].links)
                    for (const auto& [field, p] : g.scripts[source].placements) {
                        if (!map_positions.contains(p.id)) continue;
                        if (p.entity.value == 0x80B580F3 || p.entity.value == 0x80B580F9)
                            map.hidden_nodes.insert(p.id);
                        if (map.hidden_nodes.contains(p.id)) ship_props.try_emplace(p.id, p.model);
                    }
            }
            walk_placements(k, [&](size_t x) {
                for (const auto& [off, point] : g.scripts[x].placements) {
                    if (!point.id || cold_fx_nodes.contains(point.id)) continue;
                    map.hidden_fx_nodes.insert(point.id);
                    if (event_root && map_positions.contains(point.id)) placements.insert(point.id);
                }
            });
            const uint64_t key = g.scripts[k].component.value;
            if (!placements.empty() && event_ids.insert(key).second) {
                const auto origin = std::ranges::find_if(g.scripts[k].placements, [&](const auto& field) {
                    return placements.contains(field.second.id);
                });
                const uint64_t anchor = origin == g.scripts[k].placements.end() ? *placements.begin() : origin->second.id;
                map.event_previews.push_back({key, name, map_positions.at(anchor), placements.size()});
                {
                    MapEventGroup group{key, {placements.begin(), placements.end()}, {}, {}, false};
                    // Convoy route groups in source-to-destination order (see 3.131).
                    const std::array<uint32_t, 5> route_tags =
                        key == 0x80AFB4DE ? std::array{0x80AFCDA8u, 0x80AFCD92u, 0x80AFCD61u, 0x80AFCD3Eu, 0x80AFCD16u} :
                        key == 0x80B5723C ? std::array{0x80B5749Cu, 0x80B5747Fu, 0x80B57466u, 0x80B5744Eu, 0x80B5743Fu} :
                                            std::array<uint32_t, 5>{};
                    if (route_tags.front()) {
                        std::vector<std::array<float, 3>> route;
                        for (const auto tag : route_tags) {
                            const auto group = std::ranges::find_if(g.point_groups, [&](const MapPointGroup& pg) {
                                return g.scripts[pg.script].component.value == tag;
                            });
                            if (group == g.point_groups.end() || group->spline.size() < 2) {
                                route.clear();
                                break;
                            }
                            const auto& points = group->spline;
                            size_t join = 0;
                            if (!route.empty()) {
                                // Adjacent authored sections overlap. Join at their nearest pair.
                                size_t end = route.size() - 1;
                                float closest = std::numeric_limits<float>::infinity();
                                for (size_t i = route.size() / 2; i < route.size(); ++i)
                                    for (size_t j = 0; j <= points.size() / 2; ++j) {
                                        const float d = std::hypot(route[i][0] - points[j][0], route[i][1] - points[j][1],
                                                                   route[i][2] - points[j][2]);
                                        if (d < closest) closest = d, end = i, join = j;
                                    }
                                route.resize(end + 1);
                            }
                            route.insert(route.end(), points.begin() + std::ptrdiff_t(join), points.end());
                        }
                        group.route = std::move(route);
                    }
                    std::map<uint64_t, TagHash> models = ship_props;
                    walk_placements(k, [&](size_t x) {
                        for (const auto& [off, point] : g.scripts[x].placements)
                            if (placements.contains(point.id) && !point.model.is_none()) models.try_emplace(point.id, point.model);
                    });
                    for (const auto id : placements) models.try_emplace(id, TagHash());
                    for (const auto& [id, model] : models)
                        if (auto p = placement_of(id, model)) group.placements.push_back(*p);
                    map.event_groups.push_back(std::move(group));
                }
            }
        }
    }
    // The ship nodes' FX are hidden with them; an event runs its FX nodes while it plays.
    for (const auto& group : map.event_groups) {
        for (const auto id : group.fx_nodes) map.event_fx_owner.try_emplace(id, group.id);
        if (auto preview = std::ranges::find(map.event_previews, group.id, &MapEventPreview::id); preview != map.event_previews.end())
            preview->nodes = group.fx_nodes;
    }
    std::ranges::sort(map.event_previews, {}, [](const MapEventPreview& event) {
        return std::tie(event.name, event.id);
    });
    return map.hidden_nodes.size() + map.hidden_fx_nodes.size();
}

ParticleSystem load_particle_system(PackageManager& pm, TagHash tag) {
    const Entry* e = pm.entry(tag);
    if (!e || e->type != 8 || e->reference != kClassParticleSystem)
        throw std::runtime_error(std::format("{}: not a particle system", tag.str()));
    const auto d = pm.read(tag);
    TagReader r(d);
    if (d.size() < 0x2C) throw std::runtime_error(std::format("{}: particle system too short", tag.str()));
    ParticleSystem s;
    s.tag = tag;
    s.definition = r.tag_at(0x00);
    s.init = r.tag_at(0x04);
    s.update = r.tag_at(0x08);
    s.instance = r.tag_at(0x10);
    s.draw = r.tag_at(0x14);
    const auto def = pm.read(s.definition);
    TagReader dr(def);
    if (def.size() < 0x138 || dr.u32_at(0x120) != 0x811C9DC5u)
        throw std::runtime_error(std::format("{}: unknown particle definition layout", s.definition.str()));
    s.lifetime = dr.f32_at(0x128);
    s.rate = dr.f32_at(0x134);
    s.state_float4s = dr.u16_at(0x48);
    s.instance_float4s = dr.u16_at(0x4C);
    if (d.size() >= 0x28) {
        const uint64_t h = r.u64_at(0x20);
        if (h && h != ~0ull) {
            pm.build_hash64_index();
            if (const auto m = pm.resolve64(h); m && pm.entry(*m) && pm.entry(*m)->reference == 0x808081F1) {
                const auto md = pm.read(*m);
                for (size_t o = 0; o + 4 <= md.size() && s.mesh.is_none(); o += 4) {
                    uint32_t v;
                    std::memcpy(&v, md.data() + o, 4);
                    if (v < 0x80800000u || v == ~0u) continue;
                    if (const Entry* me = pm.entry(TagHash(v)); me && me->type == 8 && me->reference == kClassDynamicModel) s.mesh = TagHash(v);
                }
            }
        }
    }
    return s;
}

std::vector<MapParticle> load_map_particles(PackageManager& pm, const MapLayout& map) {
    std::vector<MapParticle> out;
    std::set<std::pair<uint64_t, uint32_t>> emitted;
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> entity_systems;  // (system, gate)
    for (const auto table : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(table);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t k = 0; k < nodes.count; ++k) {
            const size_t e = nodes.data + size_t(k) * 0x90;
            if (e + 0x90 > data.size()) break;
            const uint64_t node = r.u64_at(e + 0x70);
            // hide_scripted_nodes (3.111); an event's FX load too and run while it plays (3.133).
            const auto owner = map.event_fx_owner.find(node);
            const uint64_t event = owner == map.event_fx_owner.end() ? 0 : owner->second;
            if (!event && map.hidden(node, true)) continue;
            const uint64_t h64 = r.u64_at(e + 0x30);
            if (!h64 || h64 == ~0ull) continue;
            const auto ent = pm.resolve64(h64);
            if (!ent) continue;
            auto it = entity_systems.find(ent->value);
            if (it == entity_systems.end()) {
                std::vector<std::pair<uint32_t, uint32_t>> systems;
                const Entry* ee = pm.entry(*ent);
                if (ee && ee->type == 8 && ee->reference == 0x8080BAAD) {
                    try {
                        const auto ed = pm.read(*ent);
                        std::set<uint32_t> listed;
                        std::map<uint32_t, uint32_t> found;  // system -> gate
                        bool turret = false;
                        for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                            uint32_t c;
                            std::memcpy(&c, ed.data() + o, 4);
                            if (c < 0x80800000u || c == ~0u || !listed.insert(c).second) continue;
                            const Entry* ce = pm.entry(TagHash(c));
                            if (!ce || ce->type != 8 || ce->reference != kClassComponent) continue;
                            const auto cd = pm.read(TagHash(c));
                            uint32_t gate = 0;
                            for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
                                uint32_t v;
                                std::memcpy(&v, cd.data() + q, 4);
                                if (v == kChannelExfilActivationProgress) gate = v;
                                else if (v == kChannelDevicePosition && !gate) gate = v;
                                if (v == kChannelTurret) turret = true;
                            }
                            for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
                                uint32_t v;
                                std::memcpy(&v, cd.data() + q, 4);
                                if (v < 0x80800000u || v == ~0u) continue;
                                const Entry* pe = pm.entry(TagHash(v));
                                if (pe && pe->type == 8 && pe->reference == kClassParticleSystem) {
                                    auto [at, fresh] = found.try_emplace(v, gate);
                                    if (!fresh && !gate) at->second = 0;  // also held ungated
                                }
                            }
                        }
                        systems.assign(found.begin(), found.end());
                        if (turret)
                            for (auto& sg : systems) sg.second = kChannelTurret;
                    } catch (...) {
                    }
                }
                it = entity_systems.emplace(ent->value, std::move(systems)).first;
            }
            if (it->second.empty()) continue;
            const auto rows = quat_rows(r, e);
            for (const auto& [s, gate] : it->second) {
                // Hide the Compiler room's dark/haze FX entity (80AA4CE0: 80AA4CA0/80AA4CBF/80AA4CDE).
                if (map.bubble.value == 0x80B385FAu && ent->value == 0x80AA4CE0u) continue;
                if (node && node != ~0ull && !emitted.emplace(node, s).second) continue;
                out.push_back({TagHash(s), rows, gate, event});
            }
        }
    }
    return out;
}

// An FX light's SLight +0x00..+0x5F are all zero in the record: the FX sequence writes them while
// it plays. The values nim runs it with (estimate, listed by GameLighting::set_lights): unused clip
// planes (0, 0, -1, 500000) with exponent 0 as on the authored map lights, and the weights
// +0x50 supplies the same diffuse and shadow weights as the authored map lights.
// +0x58 is the FX light's fade phase. Use mid-fade for sequences with no decoded phase.
static void fill_fx_light_record(MapLight& l) {
    l.fx = true;
    if (std::ranges::any_of(l.params, [](float v) { return v != 0; })) return;
    for (size_t k = 0; k < 4; ++k) l.params[k * 4 + 2] = -1, l.params[k * 4 + 3] = 500000;
    l.params[20] = 1, l.params[21] = 1;
    l.params[22] = 0.5f;
}

std::vector<MapLight> load_map_fx_lights(PackageManager& pm, const MapLayout& map, size_t* attached) {
    constexpr uint32_t kSequenceLight = 0x80808323, kFnvEmpty = 0x811C9DC5;
    struct EntityLight {
        TagHash tag;
        bool named = false;
    };
    std::vector<MapLight> out;
    size_t named = 0;
    std::unordered_map<uint32_t, std::vector<EntityLight>> entity_lights;
    std::unordered_map<uint32_t, std::vector<uint8_t>> light_bytes;
    for (const auto table : map.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(table);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t k = 0; k < nodes.count; ++k) {
            const size_t e = nodes.data + size_t(k) * 0x90;
            if (e + 0x90 > data.size()) break;
            const uint64_t id = r.u64_at(e + 0x70);
            const auto owner = map.event_fx_owner.find(id);
            const uint64_t event = owner == map.event_fx_owner.end() ? 0 : owner->second;
            if (!event && map.hidden(id, true)) continue;  // hide_scripted_nodes (3.111)
            const uint64_t h64 = r.u64_at(e + 0x30);
            if (!h64 || h64 == ~0ull) continue;
            const auto ent = pm.resolve64(h64);
            if (!ent) continue;
            auto it = entity_lights.find(ent->value);
            if (it == entity_lights.end()) {
                std::vector<EntityLight> lights;
                const Entry* ee = pm.entry(*ent);
                if (ee && ee->type == 8 && ee->reference == 0x8080BAAD) {
                    try {
                        const auto ed = pm.read(*ent);
                        std::set<uint32_t> listed;
                        bool turret = false;
                        for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                            uint32_t c;
                            std::memcpy(&c, ed.data() + o, 4);
                            if (c < 0x80800000u || c == ~0u || !listed.insert(c).second) continue;
                            const Entry* ce = pm.entry(TagHash(c));
                            if (!ce || ce->type != 8 || ce->reference != kClassComponent) continue;
                            const auto cd = pm.read(TagHash(c));
                            for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
                                uint32_t v;
                                std::memcpy(&v, cd.data() + q, 4);
                                if (v == kChannelTurret) turret = true;
                            }
                            bool fx = false, gated = false;
                            std::vector<EntityLight> found;
                            for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
                                uint32_t v;
                                std::memcpy(&v, cd.data() + q, 4);
                                if (v == kChannelExfilActivationProgress || v == kChannelDevicePosition) gated = true;
                                if (v < 0x80800000u || v == ~0u) continue;
                                const Entry* pe = pm.entry(TagHash(v));
                                if (!pe || pe->type != 8) continue;
                                if (pe->reference == kClassParticleSystem) fx = true;
                                if (pe->reference != kClassShadowingLight && pe->reference != kClassLightCollection) continue;
                                // The sequence's light record ends with the light tag.
                                if (q < 0x28) continue;
                                uint32_t cls, name;
                                std::memcpy(&cls, cd.data() + q - 0x28, 4);
                                std::memcpy(&name, cd.data() + q - 0x24, 4);
                                if (cls != kSequenceLight) continue;
                                if (std::ranges::none_of(found, [&](const EntityLight& l) { return l.tag.value == v; }))
                                    found.push_back({TagHash(v), name != kFnvEmpty});
                            }
                            if (!fx || gated) continue;
                            for (const auto& l : found)
                                if (std::ranges::none_of(lights, [&](const EntityLight& x) { return x.tag == l.tag; })) lights.push_back(l);
                        }
                        if (turret) lights.clear();  // the turret's FX are left out (kChannelTurret)
                    } catch (...) {
                    }
                }
                it = entity_lights.emplace(ent->value, std::move(lights)).first;
            }
            if (it->second.empty()) continue;
            const Mat node = quat_transform(r.f32_at(e), r.f32_at(e + 4), r.f32_at(e + 8), r.f32_at(e + 12), r.f32_at(e + 0x10),
                                            r.f32_at(e + 0x14), r.f32_at(e + 0x18), r.f32_at(e + 0x1C));
            for (const auto& el : it->second) {
                try {
                    auto& bytes = light_bytes[el.tag.value];
                    if (bytes.empty()) bytes = pm.read(el.tag);
                    TagReader lr(bytes);
                    const Entry* le = pm.entry(el.tag);
                    if (le->reference == kClassShadowingLight) {
                        out.push_back(read_light(lr, 0, 0xD4, node, el.tag, true));
                        fill_fx_light_record(out.back());
                        out.back().event = event;
                        named += el.named;
                    } else {
                        const auto lights = lr.vec_at(0x30), transforms = lr.vec_at(0x40);
                        for (uint64_t i = 0; i < lights.count && i < transforms.count; ++i) {
                            out.push_back(read_light(lr, lights.data + size_t(i) * 0x100, 0xC4,
                                                     mat_mul(node, read_quat_transform(lr, transforms.data + size_t(i) * 0x20)), el.tag, false));
                            fill_fx_light_record(out.back());
                            out.back().event = event;
                            named += el.named;
                        }
                    }
                } catch (...) {
                    // A light whose data cannot be read is left out.
                }
            }
        }
    }
    if (attached) *attached = named;
    return out;
}

std::vector<TagHash> entity_shield_systems(PackageManager& pm, TagHash entity) {
    std::vector<TagHash> out;
    const auto ed = pm.read(entity);
    std::set<uint32_t> listed;
    for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
        uint32_t c;
        std::memcpy(&c, ed.data() + o, 4);
        if (c < 0x80800000u || c == ~0u || !listed.insert(c).second) continue;
        const Entry* ce = pm.entry(TagHash(c));
        if (!ce || ce->type != 8 || ce->reference != kClassComponent) continue;
        const auto cd = pm.read(TagHash(c));
        bool shield = false;
        std::vector<TagHash> systems;
        for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
            uint32_t v;
            std::memcpy(&v, cd.data() + q, 4);
            if (const char* name = object_channel_name(v); name && std::strstr(name, "shield")) shield = true;
            if (v < 0x80800000u || v == ~0u) continue;
            const Entry* pe = pm.entry(TagHash(v));
            if (pe && pe->type == 8 && pe->reference == kClassParticleSystem && std::ranges::find(systems, TagHash(v)) == systems.end())
                systems.push_back(TagHash(v));
        }
        if (shield)
            for (const auto s : systems)
                if (std::ranges::find(out, s) == out.end()) out.push_back(s);
    }
    return out;
}

}  // namespace nim
