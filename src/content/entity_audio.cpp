#include "nim/content/entity_audio.hpp"

#include "nim/content/dialogue.hpp"
#include "nim/content/wwise.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <unordered_set>

namespace nim {

namespace {
constexpr uint32_t kClassComponent = 0x8080BADB;
constexpr uint32_t kClassArrayHeader = 0x8080BFCD;
constexpr uint32_t kClassSoundTable = 0x8080AE30;
constexpr uint32_t kClassSoundChoice = 0x8080AE34;
constexpr uint32_t kClassDialogueTable = 0x8080B4D1;
constexpr uint32_t kClassDialogueKey = 0x8080AA34;
constexpr uint32_t kNoCondition = 0x811C9DC5;  // FNV-1 offset basis: the empty name

uint32_t u32(const std::vector<uint8_t>& d, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}
uint64_t u64(const std::vector<uint8_t>& d, size_t o) {
    uint64_t v = 0;
    if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8);
    return v;
}

std::optional<TagHash> audio_event(PackageManager& pm, uint64_t h) {
    if (!h || h == ~0ull || !(h >> 32)) return std::nullopt;
    const auto t = pm.resolve64(h);
    if (!t) return std::nullopt;
    const Entry* e = pm.entry(*t);
    if (!e || e->type != 8 || e->reference != kClassAudioEvent) return std::nullopt;
    return t;
}

void read_sound_tables(PackageManager& pm, const std::vector<uint8_t>& d, EntitySounds& out) {
    TagReader r(d);
    for (size_t h = 4; h + 16 <= d.size(); h += 4) {
        if (u32(d, h - 4) != kClassArrayHeader || u32(d, h + 8) != kClassSoundTable) continue;
        const uint64_t count = u64(d, h);
        const size_t data = h + 16;
        if (count == 0 || count > 4096 || data + count * 0x18 > d.size()) continue;
        ++out.tables;
        for (uint64_t k = 0; k < count; ++k) {
            const size_t entry = data + k * 0x18;
            const uint32_t name = u32(d, entry);
            try {
                const auto choices = r.vec_at(entry + 8);
                if (choices.element_class != kClassSoundChoice || choices.data + choices.count * 0x38 > d.size()) continue;
                std::vector<std::pair<bool, TagHash>> found;
                for (uint64_t c = 0; c < choices.count; ++c) {
                    const size_t at = choices.data + c * 0x38;
                    if (const auto ev = audio_event(pm, u64(d, at + 0x20))) found.push_back({u32(d, at + 8) != kNoCondition, *ev});
                }
                std::ranges::stable_sort(found, {}, &std::pair<bool, TagHash>::first);
                auto& list = out.by_name[name];
                for (const auto& [conditional, ev] : found)
                    if (std::ranges::find(list, ev) == list.end()) list.push_back(ev);
            } catch (const std::exception&) {
            }
        }
    }
}

void read_dialogue_table(PackageManager& pm, const std::vector<uint8_t>& d, EntitySounds& out) {
    TagReader r(d);
    try {
        const auto keys = r.vec_at(0x18);
        if (keys.element_class != kClassDialogueKey || keys.data + keys.count * 0x10 > d.size()) return;
        ++out.dialogue_tables;
        std::vector<std::pair<uint32_t, size_t>> starts;
        for (uint64_t k = 0; k < keys.count; ++k)
            starts.push_back({u32(d, keys.data + k * 0x10), r.pointer_at(keys.data + k * 0x10 + 8)});
        for (size_t k = 0; k < starts.size(); ++k) {
            const size_t begin = starts[k].second;
            size_t end = d.size();
            for (const auto& [_, s] : starts)
                if (s > begin) end = std::min(end, s);
            auto& list = out.barks[starts[k].first];
            for (size_t o = (begin + 7) & ~size_t(7); o + 8 <= end; o += 8)
                if (const auto ev = audio_event(pm, u64(d, o)); ev && std::ranges::find(list, *ev) == list.end()) list.push_back(*ev);
        }
    } catch (const std::exception&) {
    }
}
}  // namespace

const std::vector<TagHash>* EntitySounds::find(uint16_t kind, uint32_t name, uint32_t value) const {
    if (kind == 5) {
        const auto it = by_name.find(name);
        return it == by_name.end() || it->second.empty() ? nullptr : &it->second;
    }
    if (kind == 4) {
        const auto it = barks.find(value);
        return it == barks.end() || it->second.empty() ? nullptr : &it->second;
    }
    return nullptr;
}

EntitySounds load_entity_sounds(PackageManager& pm, std::span<const TagHash> entities, bool include_close_loops) {
    EntitySounds out;
    std::unordered_set<uint32_t> seen, named;
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
        if (e->reference == kClassDialogueTable) read_dialogue_table(pm, d, out);
        read_sound_tables(pm, d, out);
        if (e->reference != kClassDialogueTable)
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                if (const Entry* c = pm.entry(TagHash(u32(d, o))); c && c->type == 8 && c->reference == kClassAudioEvent)
                    named.insert(u32(d, o));
                if (o % 8 == 0)
                    if (const auto ev = audio_event(pm, u64(d, o))) named.insert(ev->value);
            }
        if (level >= 2) return;
        for (size_t o = 0; o + 4 <= d.size(); o += 4) {
            const uint32_t v = u32(d, o);
            if (v < 0x80800000u || v == ~0u || seen.contains(v)) continue;
            const Entry* c = pm.entry(TagHash(v));
            if (c && c->type == 8 && c->reference != kClassAudioEvent && c->reference != kClassComponent) walk(TagHash(v), level + 1);
        }
    };
    for (const auto entity : entities) {
        std::vector<uint8_t> d;
        try {
            d = pm.read(entity);
        } catch (...) {
            continue;
        }
        for (size_t o = 0; o + 4 <= d.size(); o += 4) {
            const uint32_t v = u32(d, o);
            if (v < 0x80800000u || v == ~0u) continue;
            if (const Entry* c = pm.entry(TagHash(v)); c && c->type == 8 && c->reference == kClassComponent) walk(TagHash(v), 0);
        }
    }
    // Component loops: named events with a stop event whose play starts an endless voice.
    std::unordered_set<uint32_t> in_tables;
    for (const auto& [_, list] : out.by_name)
        for (auto t : list) in_tables.insert(t.value);
    for (const auto& [_, list] : out.barks)
        for (auto t : list) in_tables.insert(t.value);
    std::vector<uint32_t> sorted(named.begin(), named.end());
    std::ranges::sort(sorted);
    for (const uint32_t v : sorted) {
        if (in_tables.contains(v)) continue;
        try {
            const auto ev = load_wwise_event(pm, TagHash(v));
            if (!ev.stop_id || ev.stop_id == kNoCondition || !ev.bank->events.contains(ev.stop_id)) continue;
            // Beds only: a long stream heard from afar (the short, close loops are state FX).
            if (include_close_loops ? ev.longest < 2 || ev.max_distance <= 1 : ev.longest < 8 || ev.max_distance < 40) continue;
            WwisePlayState state;
            const auto voices = wwise_play_event(*ev.bank, ev.play_id, state);
            if (std::ranges::any_of(voices, [](const WwiseVoice& w) { return w.loops == 0 || w.repeat; })) out.loops.push_back(TagHash(v));
        } catch (const std::exception&) {
        }
    }
    return out;
}

}  // namespace nim
