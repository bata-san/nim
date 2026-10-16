#include "nim/content/map.hpp"

#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
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

std::vector<TagHash> find_instance_tags(PackageManager& pm, TagHash bubble, std::vector<TagHash>* terrains = nullptr) {
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

MapLayout load_map_layout(PackageManager& pm, TagHash bubble) {
    MapLayout out;
    out.bubble = bubble;
    std::vector<TagHash> terrain_tags;
    out.instance_tags = find_instance_tags(pm, bubble, &terrain_tags);
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
    std::vector<std::pair<float, MapStatic>> found;
    const float r2 = radius * radius;
    for (const auto& st : map.statics) {
        MapStatic m{st.mesh, {}};
        float nearest = 1e30f;
        for (const auto& r : st.rows) {
            const float dx = r[3] - center[0], dy = r[7] - center[1], dz = r[11] - center[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > r2) continue;
            m.rows.push_back(r);
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

}  // namespace nim
