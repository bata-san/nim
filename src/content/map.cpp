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

std::vector<TagHash> find_instance_tags(PackageManager& pm, TagHash bubble) {
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
            if (!ce || (ce->type != 8 && ce->type != 16) || !structural(ce->reference)) return;
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

MapLayout load_map_layout(PackageManager& pm, TagHash bubble) {
    MapLayout out;
    out.bubble = bubble;
    out.instance_tags = find_instance_tags(pm, bubble);
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

}  // namespace nim
