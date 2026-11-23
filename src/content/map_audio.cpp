#include "nim/content/map_audio.hpp"
#include "nim/content/dialogue.hpp"
#include "nim/content/entity_audio.hpp"
#include "nim/content/wwise.hpp"
#include "nim/content/tfx.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>

namespace nim {

namespace {
constexpr uint32_t kCompPolylineAudio = 0x80802A95u;
constexpr uint32_t kClassPolylineAudio = 0x80802A97u;
constexpr uint32_t kCompPointAudio = 0x80803135u;
constexpr uint32_t kClassPointAudio = 0x80803137u;
constexpr uint32_t kClassGlobalAudio = 0x8080B107u;
constexpr uint32_t kRainAmount = 0x9C4DE32Eu;  // fnv1("rain_amount")

std::array<float, 3> closest_on_segment(std::array<float, 3> p, std::array<float, 3> a, std::array<float, 3> b) {
    const float abx = b[0] - a[0], aby = b[1] - a[1], abz = b[2] - a[2];
    const float len2 = abx * abx + aby * aby + abz * abz;
    if (len2 <= 1e-8f) return a;
    const float t = std::clamp(((p[0] - a[0]) * abx + (p[1] - a[1]) * aby + (p[2] - a[2]) * abz) / len2, 0.0f, 1.0f);
    return {a[0] + t * abx, a[1] + t * aby, a[2] + t * abz};
}

// The audio events an entity starts on events: those its components name only inside reaction
// tables (8080AF57 / 8080AF59, reached from 8080AFBA components: arrays 8080AF61 / 8080AF67 of
// {u64 event key, actions}, the play-audio actions 80809E50 holding the event's hash64). Events
// also named elsewhere (the FX sequences of 8080B69A components) are not counted. `component`: for
// the other events, the component (8080BADB) under which each is first named.
struct EntityAudioSources {
    std::set<uint32_t> event_started;
    std::unordered_map<uint32_t, uint32_t> component;
};
// Whether one of the entity's components names `channel` (an FNV-1 name its sequences read).
bool entity_reads_channel(PackageManager& pm, TagHash entity, uint32_t channel) {
    std::vector<uint8_t> ed;
    try {
        ed = pm.read(entity);
    } catch (...) {
        return false;
    }
    std::set<uint32_t> listed;
    for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
        uint32_t c;
        std::memcpy(&c, ed.data() + o, 4);
        if (c < 0x80800000u || c == ~0u || !listed.insert(c).second) continue;
        const Entry* ce = pm.entry(TagHash(c));
        if (!ce || ce->type != 8 || ce->reference != 0x8080BADBu) continue;
        std::vector<uint8_t> cd;
        try {
            cd = pm.read(TagHash(c));
        } catch (...) {
            continue;
        }
        for (size_t q = 0; q + 4 <= cd.size(); q += 4) {
            uint32_t v;
            std::memcpy(&v, cd.data() + q, 4);
            if (v == channel) return true;
        }
    }
    return false;
}

EntityAudioSources entity_audio_sources(PackageManager& pm, TagHash entity) {
    constexpr uint32_t kComponent = 0x8080BADBu, kAudioEvent = 0x8080B128u;
    constexpr uint32_t kReactions = 0x8080AF57u, kReactionSet = 0x8080AF59u;
    std::set<uint32_t> in_reactions, elsewhere, seen;
    EntityAudioSources out;
    uint32_t root = 0;
    auto audio = [&](const std::vector<uint8_t>& d, size_t o) -> uint32_t {
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        if (const Entry* c = v >= 0x80800000u && v != ~0u ? pm.entry(TagHash(v)) : nullptr; c && c->type == 8 && c->reference == kAudioEvent)
            return v;
        if (o % 8 || o + 8 > d.size()) return 0;
        uint64_t h;
        std::memcpy(&h, d.data() + o, 8);
        if (!(h >> 32) || h == ~0ull) return 0;
        const auto t = pm.resolve64(h);
        const Entry* c = t ? pm.entry(*t) : nullptr;
        return c && c->type == 8 && c->reference == kAudioEvent ? t->value : 0;
    };
    std::function<void(TagHash, int)> walk = [&](TagHash t, int level) {
        if (!seen.insert(t.value).second) return;
        const Entry* e = pm.entry(t);
        if (!e || e->type != 8) return;
        std::vector<uint8_t> d;
        try {
            d = pm.read(t);
        } catch (...) {
            return;
        }
        auto& into = e->reference == kReactions || e->reference == kReactionSet ? in_reactions : elsewhere;
        for (size_t o = 0; o + 4 <= d.size(); o += 4)
            if (const uint32_t ev = audio(d, o)) {
                into.insert(ev);
                if (&into == &elsewhere) out.component.try_emplace(ev, root);
            }
        if (level >= 2) return;
        for (size_t o = 0; o + 4 <= d.size(); o += 4) {
            uint32_t v;
            std::memcpy(&v, d.data() + o, 4);
            if (v < 0x80800000u || v == ~0u || seen.contains(v)) continue;
            const Entry* c = pm.entry(TagHash(v));
            if (c && c->type == 8 && c->reference != kAudioEvent && c->reference != kComponent) walk(TagHash(v), level + 1);
        }
    };
    std::vector<uint8_t> ed;
    try {
        ed = pm.read(entity);
    } catch (...) {
        return {};
    }
    for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
        uint32_t v;
        std::memcpy(&v, ed.data() + o, 4);
        if (const Entry* c = v >= 0x80800000u && v != ~0u ? pm.entry(TagHash(v)) : nullptr; c && c->type == 8 && c->reference == kComponent)
            root = v, walk(TagHash(v), 0);
    }
    for (const auto v : in_reactions)
        if (!elsewhere.contains(v)) out.event_started.insert(v);
    return out;
}
}  // namespace

std::array<float, 3> MapAudioEmitter::closest_point(std::array<float, 3> p) const {
    if (points.empty()) return position;
    if (points.size() == 1) return points[0];
    std::array<float, 3> best = points[0];
    float best_d2 = 1e30f;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto c = closest_on_segment(p, points[i], points[i + 1]);
        const float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = c;
        }
    }
    return best;
}

float MapAudioEmitter::distance_to(std::array<float, 3> p) const {
    if (global) return 0.0f;
    const auto c = closest_point(p);
    const float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

MapAudio load_map_audio(PackageManager& pm, const MapLayout& layout) {
    MapAudio out;
    pm.build_hash64_index();

    struct EventInfo {
        uint32_t event_id = 0;
        std::vector<TagHash> wems;
    };
    std::unordered_map<uint32_t, EventInfo> event_cache;
    std::set<uint32_t> unique_wems;

    auto resolve_event = [&](TagHash ev_tag) -> const EventInfo& {
        if (auto it = event_cache.find(ev_tag.value); it != event_cache.end()) return it->second;
        EventInfo info;
        try {
            const auto bytes = pm.read(ev_tag);
            if (bytes.size() >= 0x30) {
                TagReader er(bytes);
                info.event_id = er.u32_at(0x08);
                const auto streams = er.vec_at(0x20);
                for (uint64_t s = 0; s < streams.count && streams.data + (s + 1) * 4 <= bytes.size(); ++s) {
                    const TagHash wem = er.tag_at(streams.data + size_t(s) * 4);
                    if (!wem.is_pkg_ref()) continue;
                    if (const Entry* we = pm.entry(wem); we && we->type == 26 && we->subtype == 7) {
                        info.wems.push_back(wem);
                        unique_wems.insert(wem.value);
                    }
                }
            }
        } catch (...) {
        }
        return event_cache.emplace(ev_tag.value, std::move(info)).first->second;
    };

    std::set<uint32_t> polyline_tags, point_tags, global_tags;
    std::unordered_map<uint32_t, std::vector<std::vector<TagHash>>> object_events;  // entity -> loop groups
    std::set<uint32_t> rain_entities;  // entities whose components read rain_amount
    std::unordered_map<uint32_t, float> object_distances;
    std::set<std::tuple<uint32_t, std::array<float, 3>>> object_instances;
    for (TagHash table : layout.node_tables) {
        std::vector<uint8_t> data;
        try {
            data = pm.read(table);
        } catch (...) {
            continue;
        }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            const uint64_t entity_hash = r.u64_at(e + 0x30);
            if (entity_hash && entity_hash != ~0ull) {
                const auto entity = pm.resolve64(entity_hash);
                const auto* entry = entity ? pm.entry(*entity) : nullptr;
                if (entry && entry->type == 8 && entry->reference == 0x8080BAAD) {
                    const auto [events, first] = object_events.try_emplace(entity->value);
                    if (first) {
                        // Loops an event starts (named only in the reaction tables) are left out; the
                        // others are grouped by the component naming them.
                        const auto loops = load_entity_sounds(pm, std::span(&*entity, 1), true).loops;
                        const auto sources = loops.empty() ? EntityAudioSources{} : entity_audio_sources(pm, *entity);
                        std::map<uint32_t, std::vector<TagHash>> by_component;
                        for (const auto ev : loops) {
                            if (sources.event_started.contains(ev.value)) {
                                out.event_loops.insert(ev.value);
                                continue;
                            }
                            const auto c = sources.component.find(ev.value);
                            by_component[c == sources.component.end() ? 0u : c->second].push_back(ev);
                        }
                        for (auto& [c, list] : by_component) events->second.push_back(std::move(list));
                        if (!events->second.empty() && entity_reads_channel(pm, *entity, kRainAmount)) rain_entities.insert(entity->value);
                    }
                    const std::array pos{r.f32_at(e + 0x10), r.f32_at(e + 0x14), r.f32_at(e + 0x18)};
                    if (std::ranges::all_of(pos, [](float v) { return std::isfinite(v); })) {
                        // One loop per group and placement (推定: a group's loops are alternatives of
                        // one sequence; its choice is not decoded, so the position picks one).
                        uint32_t seed = 0x811C9DC5u;
                        for (const float v : pos) {
                            uint32_t b;
                            std::memcpy(&b, &v, 4);
                            seed = (seed * 0x01000193u) ^ b;
                        }
                        for (const auto& group : events->second) {
                            const TagHash event = group[seed % group.size()];
                            if (group.size() > 1) ++out.alternative_picks;
                            if (!object_instances.emplace(event.value, pos).second) continue;
                            auto [distance, fresh] = object_distances.try_emplace(event.value, 0);
                            if (fresh) {
                                try { distance->second = load_wwise_event(pm, event).max_distance; }
                                catch (...) { continue; }
                            }
                            if (std::isfinite(distance->second) && distance->second > 1)
                                out.objects.push_back({*entity, event, pos, distance->second, rain_entities.contains(entity->value)});
                        }
                    }
                }
            }
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                const uint32_t type = r.u32_at(p - 4);
                const uint64_t next = r.u64_at(p);
                const size_t end = next ? std::min(data.size(), p + size_t(next)) : std::min(data.size(), p + 0x80);
                if (type == kCompPolylineAudio || type == kCompPointAudio) {
                    for (size_t o = p + 0x10; o + 4 <= end; o += 4) {
                        const TagHash h(r.u32_at(o));
                        if (!h.is_pkg_ref()) continue;
                        if (const Entry* pe = pm.entry(h); pe && pe->type == 8) {
                            if (pe->reference == kClassPolylineAudio) polyline_tags.insert(h.value);
                            else if (pe->reference == kClassPointAudio) point_tags.insert(h.value);
                            else if (pe->reference == kClassGlobalAudio) global_tags.insert(h.value);
                        }
                    }
                }
                if (!next) break;
                p += size_t(next);
            }
        }
    }

    auto read_wide_event = [&](const TagReader& r, size_t off) -> TagHash {
        const auto ev = resolve_wide_hash(pm, r.u32_at(off), r.u64_at(off + 8));
        if (!ev) return {};
        const Entry* pe = pm.entry(*ev);
        if (!pe || pe->type != 8 || pe->reference != kClassAudioEvent) return {};
        return *ev;
    };

    // 1. Polyline / multi-point ambient emitters (80802A97, 0x60-byte records).
    for (uint32_t tv : polyline_tags) {
        std::vector<uint8_t> data;
        try { data = pm.read(TagHash(tv)); } catch (...) { continue; }
        TagReader r(data);
        const auto recs = r.vec_at(0x08);
        for (uint64_t i = 0; i < recs.count; ++i) {
            const size_t o = recs.data + size_t(i) * 0x60;
            if (o + 0x60 > data.size()) break;
            const TagHash ev = read_wide_event(r, o);
            if (ev.is_none()) continue;
            const auto& info = resolve_event(ev);
            MapAudioEmitter em;
            em.event = ev;
            em.event_id = info.event_id;
            em.wems = info.wems;
            em.position = {r.f32_at(o + 0x20), r.f32_at(o + 0x24), r.f32_at(o + 0x28)};
            const float g = r.f32_at(o + 0x38);
            em.gain = (std::isfinite(g) && g > 0.0f && g <= 10.0f) ? g : 1.0f;
            const float rad = r.f32_at(o + 0x50);
            em.radius = (std::isfinite(rad) && rad > 0.0f && rad < 5000.0f) ? rad : 0.0f;
            em.polyline = true;
            try {
                const auto pts = r.vec_at(o + 0x40);
                em.points.reserve(size_t(std::min<uint64_t>(pts.count, 256)));
                for (uint64_t k = 0; k < pts.count && k < 256 && pts.data + (k + 1) * 16 <= data.size(); ++k) {
                    const size_t po = pts.data + size_t(k) * 16;
                    em.points.push_back({r.f32_at(po), r.f32_at(po + 4), r.f32_at(po + 8)});
                }
            } catch (...) {
            }
            out.emitters.push_back(std::move(em));
            ++out.polyline_emitters;
        }
    }

    // 2. Point ambient emitters (80803137, 0x40-byte records).
    for (uint32_t tv : point_tags) {
        std::vector<uint8_t> data;
        try { data = pm.read(TagHash(tv)); } catch (...) { continue; }
        TagReader r(data);
        const auto recs = r.vec_at(0x08);
        for (uint64_t i = 0; i < recs.count; ++i) {
            const size_t o = recs.data + size_t(i) * 0x40;
            if (o + 0x40 > data.size()) break;
            const TagHash ev = read_wide_event(r, o);
            if (ev.is_none()) continue;
            const auto& info = resolve_event(ev);
            MapAudioEmitter em;
            em.event = ev;
            em.event_id = info.event_id;
            em.wems = info.wems;
            em.position = {r.f32_at(o + 0x20), r.f32_at(o + 0x24), r.f32_at(o + 0x28)};
            const float g = r.f32_at(o + 0x38);
            em.gain = (std::isfinite(g) && g > 0.0f && g <= 10.0f) ? g : 1.0f;
            out.emitters.push_back(std::move(em));
            ++out.point_emitters;
        }
    }

    // 3. Global ambient bed (8080B107, 0x10-byte record).
    for (uint32_t tv : global_tags) {
        std::vector<uint8_t> data;
        try { data = pm.read(TagHash(tv)); } catch (...) { continue; }
        TagReader r(data);
        const auto recs = r.vec_at(0x08);
        for (uint64_t i = 0; i < recs.count; ++i) {
            const size_t o = recs.data + size_t(i) * 0x10;
            if (o + 0x10 > data.size()) break;
            const TagHash ev = read_wide_event(r, o);
            if (ev.is_none()) continue;
            const auto& info = resolve_event(ev);
            MapAudioEmitter em;
            em.event = ev;
            em.event_id = info.event_id;
            em.wems = info.wems;
            em.global = true;
            out.emitters.push_back(std::move(em));
            ++out.global_emitters;
        }
    }

    out.unique_events = event_cache.size();
    out.unique_wems = unique_wems.size();
    for (const auto& [k, v] : event_cache) {
        if (!v.wems.empty()) ++out.events_with_wems;
        else ++out.events_without_wems;
    }
    return out;
}

}  // namespace nim
