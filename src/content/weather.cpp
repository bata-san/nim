#include "nim/content/weather.hpp"

#include "nim/content/object_channels.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <string_view>

namespace nim {
namespace {

constexpr uint32_t kClassScript = 0x80800000;          // script definitions and string tables
constexpr uint32_t kClassComponent = 0x8080BADB;       // script instance data
constexpr uint32_t kClassChannelRegistry = 0x8080A002;
constexpr uint32_t kClassStringHash = 0x80809FFF;      // { u32 fnv1 } string-hash field

uint32_t u32_at(const std::vector<uint8_t>& d, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}
int64_t i64_at(const std::vector<uint8_t>& d, size_t o) {
    int64_t v = 0;
    if (o + 8 <= d.size()) std::memcpy(&v, d.data() + o, 8);
    return v;
}
float f32_at(const std::vector<uint8_t>& d, size_t o) {
    float v = 0;
    if (o + 4 <= d.size()) std::memcpy(&v, d.data() + o, 4);
    return v;
}
bool contains(const std::vector<uint8_t>& d, std::string_view s) {
    return std::search(d.begin(), d.end(), s.begin(), s.end()) != d.end();
}
bool references(const std::vector<uint8_t>& d, uint32_t tag) {
    for (size_t o = 0; o + 4 <= d.size(); o += 4)
        if (u32_at(d, o) == tag) return true;
    return false;
}

// A relative array field: count at field - 8, the 16-byte array header at field + offset.
size_t array_at(const std::vector<uint8_t>& d, size_t field, uint32_t& count) {
    count = u32_at(d, field - 8);
    return size_t(int64_t(field) + i64_at(d, field) + 0x10);
}

}  // namespace

WorldChannelRegistry load_world_channel_registry(PackageManager& pm) {
    WorldChannelRegistry reg;
    // The client bootstrap holds it (patchable copy preferred).
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassChannelRegistry) continue;
            if (!reg.tag.is_none() && info.name.find("patchable") == std::string::npos) continue;
            reg.tag = TagHash::make(info.pkg_id, uint16_t(i));
        }
    }
    if (reg.tag.is_none()) return reg;
    const auto d = pm.read(reg.tag);
    uint32_t n = 0;
    const size_t a = array_at(d, 0x10, n);
    for (uint32_t k = 0; k < n && a + 8 * k + 8 <= d.size(); ++k) reg.slots[u32_at(d, a + 8 * k)] = u32_at(d, a + 8 * k + 4);
    return reg;
}

std::string MapWeather::name(uint32_t hash) const {
    if (auto it = names.find(hash); it != names.end()) return it->second;
    return std::format("{:08X}", hash);
}

const WeatherState* WeatherController::strongest_state() const {
    const WeatherState* best = nullptr;
    float best_sum = -1;
    for (const auto& st : states) {
        float sum = 0;
        for (const auto& v : st.values) sum += v.max;
        if (sum > best_sum) best_sum = sum, best = &st;
    }
    return best;
}

bool WeatherController::heat_cascade() const {
    // Authored exp_outpost/hz_emberwave channel. Names are often stripped in other packages.
    return std::ranges::any_of(states, [](const WeatherState& st) {
        return std::ranges::any_of(st.values, [](const WeatherValue& v) { return v.channel == 0x798A4399; });
    });
}

std::vector<std::pair<uint32_t, float>> weather_values(const WeatherState& state) {
    std::vector<std::pair<uint32_t, float>> out;
    for (const auto& v : state.values)
        if (v.slot >= 0) out.push_back({uint32_t(v.slot), 0.5f * (v.min + v.max)});
    return out;
}

static MapWeather load_map_weather_family(PackageManager& pm, TagHash bubble) {
    MapWeather w;
    w.registry = load_world_channel_registry(pm);
    const PackageInfo* home = pm.info(bubble.pkg_id());
    if (!home) return w;
    // The map's package family: the bubble package's name without its trailing number.
    const std::string family = home->name.substr(0, home->name.rfind('_') + 1);

    struct Tag { TagHash tag; uint32_t cls; };
    std::vector<Tag> scripts, components;
    for (const auto& info : pm.packages()) {
        if (!info.name.starts_with(family)) continue;
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8) continue;
            if (e.reference == kClassScript) scripts.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.reference});
            if (e.reference == kClassComponent) components.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.reference});
        }
    }
    // Script type tags of the controller, and the strings that name hashes.
    // Several script tags mention a type's name; the records say which one is the type.
    std::vector<uint32_t> controller_defs, state_insts, value_insts;
    for (const auto& s : scripts) {
        const auto d = pm.read(s.tag);
        constexpr std::string_view scope = "world_global_channel_controller.c_script_world_global_channel_";
        if (contains(d, std::string(scope) + "controller_definition")) controller_defs.push_back(s.tag.value);
        if (contains(d, std::string(scope) + "state_instance")) state_insts.push_back(s.tag.value);
        if (contains(d, std::string(scope) + "value_instance")) value_insts.push_back(s.tag.value);
        size_t run = 0;
        for (size_t i = 0; i <= d.size(); ++i) {
            const bool printable = i < d.size() && d[i] >= 0x20 && d[i] < 0x7F;
            if (printable) { ++run; continue; }
            if (run >= 2) {
                const std::string_view text(reinterpret_cast<const char*>(d.data() + i - run), run);
                w.names.try_emplace(fnv1(text), std::string(text));
                size_t start = 0;
                for (size_t k = 0; k <= text.size(); ++k)
                    if (k == text.size() || std::string_view(".\\/\" ").find(text[k]) != std::string_view::npos) {
                        if (k > start) w.names.try_emplace(fnv1(text.substr(start, k - start)), std::string(text.substr(start, k - start)));
                        start = k + 1;
                    }
            }
            run = 0;
        }
    }
    if (controller_defs.empty() || state_insts.empty() || value_insts.empty()) return w;
    auto is = [](const std::vector<uint32_t>& set, uint32_t v) { return std::ranges::find(set, v) != set.end(); };

    const uint32_t default_hash = fnv1("default");
    for (const auto& c : components) {
        const auto d = pm.read(c.tag);
        if (!std::ranges::any_of(controller_defs, [&](uint32_t t) { return references(d, t); })) continue;
        WeatherController wc;
        wc.tag = c.tag;
        // The instance's bundle path: hashed words around "default" (outer bundle, then slots).
        for (size_t o = 4; o + 4 <= d.size(); o += 4) {
            if (u32_at(d, o) != default_hash) continue;
            std::vector<std::string> words;
            if (auto it = w.names.find(u32_at(d, o - 4)); it != w.names.end()) words.push_back(it->second);
            for (size_t k = 1; k <= 3; ++k)
                if (auto it = w.names.find(u32_at(d, o + 4 * k)); it != w.names.end()) words.push_back(it->second);
            for (size_t k = 0; k < words.size(); ++k) wc.name += (k ? "/" : "") + words[k];
            break;
        }
        if (wc.name.empty()) wc.name = c.tag.str();
        // Channel states: (self, state instance) records with the state's name at +0x2C and its
        // channel values (inline, 0xA0 each) through the array field at +0x38. A value has its
        // range at +0x10 / +0x14 and its world channel as a string hash at +0x50.
        for (size_t o = 0; o + 0x40 <= d.size(); o += 8) {
            if (u32_at(d, o) != c.tag.value || !is(state_insts, u32_at(d, o + 4))) continue;
            WeatherState st;
            st.name = u32_at(d, o + 0x2C);
            uint32_t n = 0;
            const size_t arr = array_at(d, o + 0x38, n);
            if (n == 0 || n > 256) continue;
            for (uint32_t k = 0; k < n; ++k) {
                const size_t e = arr + size_t(k) * 0xA0;
                if (e + 0x54 > d.size() || u32_at(d, e) != c.tag.value || !is(value_insts, u32_at(d, e + 4))) continue;
                if (u32_at(d, e + 0x44) != kClassStringHash) continue;
                WeatherValue v;
                v.channel = u32_at(d, e + 0x50);
                v.min = f32_at(d, e + 0x10);
                v.max = f32_at(d, e + 0x14);
                if (auto it = w.registry.slots.find(v.channel); it != w.registry.slots.end()) v.slot = int(it->second);
                st.values.push_back(v);
            }
            const bool seen = std::ranges::any_of(wc.states, [&](const WeatherState& s) {
                if (s.name != st.name || s.values.size() != st.values.size()) return false;
                for (size_t k = 0; k < s.values.size(); ++k)
                    if (s.values[k].channel != st.values[k].channel || s.values[k].min != st.values[k].min || s.values[k].max != st.values[k].max)
                        return false;
                return true;
            });
            if (!seen && !st.values.empty()) wc.states.push_back(std::move(st));
        }
        if (!wc.states.empty()) w.controllers.push_back(std::move(wc));
    }
    return w;
}

MapWeather load_map_weather(PackageManager& pm, TagHash bubble) {
    auto w = load_map_weather_family(pm, bubble);
    for (const auto& c : w.controllers)
        if (c.heat_cascade()) { w.heat_cascade_controller = c; return w; }
    // The installed Outpost scripts provide the original on/off values. Reuse those values,
    // with the current registry's slots, instead of inventing a heat material or new lighting.
    for (const auto& info : pm.packages()) {
        if (info.language != 0 || !info.name.starts_with("w64_outpost_")) continue;
        const auto shared = load_map_weather_family(pm, TagHash::make(info.pkg_id, 0));
        for (const auto& c : shared.controllers) {
            if (!c.heat_cascade()) continue;
            w.heat_cascade_controller = c;
            w.heat_cascade_shared = true;
            for (auto& st : w.heat_cascade_controller->states)
                for (auto& v : st.values) {
                    const auto it = w.registry.slots.find(v.channel);
                    v.slot = it == w.registry.slots.end() ? -1 : int(it->second);
                }
            return w;
        }
        break; // all packages of that family were searched together
    }
    return w;
}

}  // namespace nim
