#include "nim/content/wwise.hpp"

#include "nim/content/dialogue.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <numbers>
#include <stdexcept>

namespace nim {

namespace {

constexpr uint32_t kSupportedVersion = 150;
constexpr uint32_t kClassAudioEventInfo = 0x8080B12E;
constexpr uint32_t kClassAudioBankRef = 0x8080B12D;  // {Tag bank}

// Bounded little-endian reader over one HIRC object (or the whole bank).
struct Reader {
    std::span<const uint8_t> d;
    size_t p = 0;
    void need(size_t n) const {
        if (p + n > d.size()) throw std::runtime_error(std::format("wwise: read past the end at 0x{:X}", p));
    }
    uint8_t u8() { need(1); return d[p++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, d.data() + p, 2); p += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, d.data() + p, 4); p += 4; return v; }
    int32_t s32() { return int32_t(u32()); }
    float f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    // Variable-length integer: 7 bits per byte, most significant first, bit 7 = more.
    uint32_t var() {
        uint32_t cur = u8(), v = cur & 0x7F;
        for (int i = 0; (cur & 0x80) && i < 5; ++i) {
            cur = u8();
            v = (v << 7) | (cur & 0x7F);
        }
        return v;
    }
    void skip(size_t n) { need(n); p += n; }
};

float as_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// AkPropValue bundles hold floats for most properties and integers for a few (ids, counts, ms).
bool integer_prop(uint8_t id) {
    using namespace wwise_prop;
    return id == AttenuationID || id == Loop || id == DelayTime || id == TransitionTime || id == 0x35 /*MaxNumInstances*/ ||
           id == 0x30 || id == 0x31 || id == 0x36 || (id >= 0x46 && id <= 0x53);
}

void read_props(Reader& r, WwiseProps& out) {
    const size_t n = r.u8();
    std::vector<uint8_t> ids(n);
    for (auto& id : ids) id = r.u8();
    for (size_t i = 0; i < n; ++i) out.values.push_back({ids[i], r.u32()});
    const size_t m = r.u8();
    std::vector<uint8_t> rids(m);
    for (auto& id : rids) id = r.u8();
    for (size_t i = 0; i < m; ++i) {
        const float lo = r.f32(), hi = r.f32();
        out.ranges.push_back({rids[i], lo, hi});
    }
}

void read_graph(Reader& r, size_t count, std::vector<WwiseCurvePoint>* out) {
    for (size_t i = 0; i < count; ++i) {
        WwiseCurvePoint pt;
        pt.x = r.f32();
        pt.y = r.f32();
        pt.interp = r.u32();
        if (out) out->push_back(pt);
    }
}

void skip_rtpc(Reader& r) {
    const size_t n = r.u16();
    for (size_t i = 0; i < n; ++i) {
        r.u32();  // RTPC id
        r.u8();   // type
        r.u8();   // accumulation
        r.var();  // parameter
        r.u32();  // curve id
        r.u8();   // scaling
        read_graph(r, r.u16(), nullptr);
    }
}

void skip_state_chunk(Reader& r) {
    const size_t props = r.var();
    for (size_t i = 0; i < props; ++i) {
        r.var();
        r.u8();
        r.u8();
    }
    const size_t groups = r.var();
    for (size_t g = 0; g < groups; ++g) {
        r.u32();
        r.u8();
        const size_t states = r.var();
        for (size_t s = 0; s < states; ++s) {
            r.u32();
            const size_t n = r.u16();
            r.skip(n * 2 + n * 4);
        }
    }
}

// CAkParameterNodeBase::SetNodeBaseParams (version 150).
void read_node_base(Reader& r, WwiseNode& n) {
    r.u8();  // override parent FX
    if (const size_t fx = r.u8()) {
        r.u8();  // bypass all
        r.skip(fx * 6);
    }
    r.u8();  // override parent metadata
    if (const size_t fx = r.u8()) r.skip(fx * 6);
    n.bus = r.u32();
    n.parent = r.u32();
    r.u8();  // priority / MIDI bits
    read_props(r, n.props);
    // Positioning
    const uint8_t bits = r.u8();
    n.positioning_override = bits & 1;
    const bool has_3d = (bits >> 1) & 1;
    if (n.positioning_override && has_3d) {
        const uint8_t bits3d = r.u8();
        n.spatial = true;
        n.attenuation_on = (bits3d >> 3) & 1;
        if (((bits >> 5) & 3) != 0) {  // automation path
            r.u8();
            r.s32();
            const size_t vertices = r.u32();
            r.skip(vertices * 16);
            const size_t items = r.u32();
            r.skip(items * 8 + items * 12);
        }
    }
    // Aux sends
    const uint8_t aux = r.u8();
    if ((aux >> 3) & 1) r.skip(16);
    r.u32();  // reflections aux bus
    r.skip(6);  // advanced settings
    skip_state_chunk(r);
    skip_rtpc(r);
}

void read_children(Reader& r, WwiseNode& n) {
    const size_t count = r.u32();
    n.children.reserve(count);
    for (size_t i = 0; i < count; ++i) n.children.push_back(r.u32());
}

void read_sound(Reader& r, WwiseNode& n) {
    n.plugin = r.u32();
    n.stream_type = r.u8();
    n.source = r.u32();
    n.media_size = r.u32();
    r.u8();  // source bits
    if ((n.plugin & 0x0F) == 2) r.skip(r.u32());  // source plugin parameters
    read_node_base(r, n);
}

void read_ranseq(Reader& r, WwiseNode& n) {
    read_node_base(r, n);
    n.loop_count = r.u16();
    r.u16();
    r.u16();
    n.transition_ms = r.f32();
    r.f32();
    r.f32();
    n.avoid_repeat = r.u16();
    n.transition_mode = r.u8();
    n.random_mode = r.u8();
    n.mode = r.u8();
    const uint8_t bits = r.u8();
    n.using_weight = bits & 1;
    n.reset_playlist = (bits >> 1) & 1;
    n.continuous = (bits >> 3) & 1;
    read_children(r, n);
    const size_t items = r.u16();
    for (size_t i = 0; i < items; ++i) {
        const uint32_t id = r.u32();
        n.playlist.push_back({id, r.s32()});
    }
}

void read_switch(Reader& r, WwiseNode& n) {
    read_node_base(r, n);
    n.group_type = r.u8();
    n.group = r.u32();
    n.default_switch = r.u32();
    r.u8();
    read_children(r, n);
    const size_t packages = r.u32();
    for (size_t i = 0; i < packages; ++i) {
        const uint32_t id = r.u32();
        std::vector<uint32_t> list(r.u32());
        for (auto& v : list) v = r.u32();
        n.switches.push_back({id, std::move(list)});
    }
    const size_t params = r.u32();
    r.skip(params * 14);
}

void read_layer(Reader& r, WwiseNode& n) {
    read_node_base(r, n);
    read_children(r, n);
    const size_t layers = r.u32();
    for (size_t i = 0; i < layers; ++i) {
        r.u32();
        skip_rtpc(r);
        r.u32();
        r.u8();
        const size_t assoc = r.u32();
        for (size_t k = 0; k < assoc; ++k) {
            r.u32();
            read_graph(r, r.u32(), nullptr);
        }
    }
    r.u8();
}

void skip_except(Reader& r) {
    const size_t n = r.var();
    r.skip(n * 5);
}

WwiseAction read_action(Reader& r, uint32_t id) {
    WwiseAction a;
    a.id = id;
    a.type = r.u16();
    a.target = r.u32();
    a.bus = r.u8() & 1;
    read_props(r, a.props);
    const uint8_t kind = uint8_t(a.type >> 8);
    if (a.type == 0x0403 || a.type == 0x0503) {
        a.fade_curve = r.u8() & 0x1F;
        a.bank = r.u32();
    } else if (kind == 0x01) {
        a.fade_curve = r.u8() & 0x1F;
        r.u8();
        skip_except(r);
    } else if (kind == 0x1E) {
        a.seek_relative = r.u8() != 0;
        a.seek = r.f32();
        a.seek_min = r.f32();
        a.seek_max = r.f32();
    }
    return a;
}

WwiseAttenuation read_attenuation(Reader& r, uint32_t id) {
    WwiseAttenuation a;
    a.id = id;
    r.u8();  // height spread
    if (r.u8() & 1) r.skip(20);  // cone
    a.curve_to_use.resize(19);
    for (auto& c : a.curve_to_use) c = int8_t(r.u8());
    const size_t curves = r.u8();
    for (size_t i = 0; i < curves; ++i) {
        WwiseCurve c;
        c.scaling = r.u8();
        read_graph(r, r.u16(), &c.points);
        a.curves.push_back(std::move(c));
    }
    skip_rtpc(r);
    return a;
}

// Wwise curve shapes over t in [0, 1] (推定: the exact formulas are not decoded).
float shape(uint32_t interp, float t) {
    constexpr float pi = std::numbers::pi_v<float>;
    switch (interp) {
    case 0: return 1 - std::pow(1 - t, 3.0f);                 // Log3
    case 1: return std::sin(t * pi / 2);                      // Sine
    case 2: return 1 - (1 - t) * (1 - t);                     // Log1
    case 3: return t < 0.5f ? 0.5f * std::sqrt(2 * t) : 1 - 0.5f * std::sqrt(2 * (1 - t));  // InvSCurve
    case 5: return t * t * (3 - 2 * t);                       // SCurve
    case 6: return t * t;                                     // Exp1
    case 7: return 1 - std::cos(t * pi / 2);                  // SineRecip
    case 8: return t * t * t;                                 // Exp3
    case 9: return 0;                                         // Constant (holds the left value)
    default: return t;                                        // Linear
    }
}

}  // namespace

float WwiseCurve::eval(float x) const {
    if (points.empty()) return 0;
    if (x <= points.front().x) return points.front().y;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[i + 1];
        if (x <= b.x) {
            const float t = b.x > a.x ? (x - a.x) / (b.x - a.x) : 1.0f;
            return a.y + (b.y - a.y) * shape(a.interp, std::clamp(t, 0.0f, 1.0f));
        }
    }
    return points.back().y;
}

float WwiseAttenuation::gain(float distance) const {
    if (curve_to_use.empty() || curve_to_use[0] < 0 || size_t(curve_to_use[0]) >= curves.size()) return 1;
    const auto& c = curves[size_t(curve_to_use[0])];
    const float v = c.eval(distance);
    if (c.scaling == 2) return std::clamp(1 + v, 0.0f, 4.0f);  // dB curves store linear gain - 1
    return std::clamp(v, 0.0f, 4.0f);
}

float WwiseAttenuation::max_distance() const {
    if (curve_to_use.empty() || curve_to_use[0] < 0 || size_t(curve_to_use[0]) >= curves.size()) return 0;
    const auto& pts = curves[size_t(curve_to_use[0])].points;
    return pts.empty() ? 0 : pts.back().x;
}

bool WwiseProps::has(uint8_t id) const {
    return std::ranges::any_of(values, [&](const auto& v) { return v.first == id; });
}
float WwiseProps::f(uint8_t id, float fallback) const {
    for (const auto& [k, v] : values)
        if (k == id) return integer_prop(id) ? float(int32_t(v)) : as_float(v);
    return fallback;
}
uint32_t WwiseProps::u(uint8_t id, uint32_t fallback) const {
    for (const auto& [k, v] : values)
        if (k == id) return integer_prop(id) ? v : uint32_t(std::max(0.0f, as_float(v)));
    return fallback;
}
std::pair<float, float> WwiseProps::range(uint8_t id) const {
    for (const auto& [k, lo, hi] : ranges)
        if (k == id) return {lo, hi};
    return {0, 0};
}

WwiseBank parse_wwise_bank(std::span<const uint8_t> bytes) {
    WwiseBank bank;
    Reader top{bytes};
    bool header = false;
    while (top.p + 8 <= bytes.size()) {
        const uint32_t tag = top.u32();
        const size_t size = top.u32();
        top.need(size);
        const auto chunk = bytes.subspan(top.p, size);
        top.p += size;
        Reader r{chunk};
        if (tag == 0x44484B42) {  // BKHD
            bank.version = r.u32();
            bank.id = r.u32();
            header = true;
            if (bank.version != kSupportedVersion)
                throw std::runtime_error(std::format("wwise: bank version {} (only {} is read)", bank.version, kSupportedVersion));
        } else if (tag == 0x58444944) {  // DIDX
            while (r.p + 12 <= chunk.size()) {
                const uint32_t id = r.u32(), off = r.u32(), len = r.u32();
                bank.media[id] = {off, len};
            }
        } else if (tag == 0x41544144) {  // DATA
            bank.data.assign(chunk.begin(), chunk.end());
        } else if (tag == 0x43524948) {  // HIRC
            const size_t count = r.u32();
            for (size_t i = 0; i < count; ++i) {
                const uint8_t type = r.u8();
                const size_t len = r.u32();
                r.need(len);
                Reader o{chunk.subspan(r.p, len)};
                r.p += len;
                const uint32_t id = o.u32();
                try {
                    switch (type) {
                    case 2: case 5: case 6: case 7: case 9: {
                        WwiseNode n;
                        n.type = type;
                        n.id = id;
                        if (type == 2) read_sound(o, n);
                        else if (type == 5) read_ranseq(o, n);
                        else if (type == 6) read_switch(o, n);
                        else if (type == 9) read_layer(o, n);
                        else {
                            read_node_base(o, n);
                            read_children(o, n);
                        }
                        bank.nodes[id] = std::move(n);
                        break;
                    }
                    case 3: bank.actions[id] = read_action(o, id); break;
                    case 4: {
                        const size_t n = o.var();
                        auto& list = bank.events[id];
                        for (size_t k = 0; k < n; ++k) list.push_back(o.u32());
                        break;
                    }
                    case 14: bank.attenuations[id] = read_attenuation(o, id); break;
                    default: ++bank.other_objects; break;
                    }
                } catch (const std::exception& e) {
                    throw std::runtime_error(std::format("wwise: HIRC object {:08X} type {}: {}", id, type, e.what()));
                }
                if ((type == 2 || type == 5 || type == 6 || type == 7 || type == 9 || type == 4 || type == 14) && o.p != len)
                    throw std::runtime_error(std::format("wwise: HIRC object {:08X} type {} read {} of {} bytes", id, type, o.p, len));
            }
        }
    }
    if (!header) throw std::runtime_error("wwise: no BKHD");
    return bank;
}

float WwisePlayState::unit() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return float(rng & 0x00FFFFFFu) / float(0x01000000u);
}

bool wwise_is_under(const WwiseBank& bank, uint32_t node, uint32_t ancestor) {
    for (int depth = 0; depth < 64 && node; ++depth) {
        if (node == ancestor) return true;
        const auto it = bank.nodes.find(node);
        if (it == bank.nodes.end()) return false;
        node = it->second.parent;
    }
    return false;
}

namespace {

struct Builder {
    const WwiseBank& bank;
    WwisePlayState& state;
    std::vector<WwiseVoice>& out;

    float randomized(const WwiseProps& p, uint8_t id) {
        float v = p.f(id);
        const auto [lo, hi] = p.range(id);
        if (lo != 0 || hi != 0) v += lo + (hi - lo) * state.unit();
        return v;
    }

    // Plays node `id`; returns the index range of the voices it added.
    std::pair<size_t, size_t> play(uint32_t id, float delay, float fade, int depth) {
        const size_t first = out.size();
        const auto it = bank.nodes.find(id);
        if (it == bank.nodes.end() || depth > 32) return {first, first};
        const auto& n = it->second;
        if (n.props.has(wwise_prop::Probability) && state.unit() * 100 >= n.props.f(wwise_prop::Probability, 100))
            return {first, first};
        switch (n.type) {
        case 2: sound(n, delay, fade); break;
        case 9:
            for (const auto c : n.children) play(c, delay, fade, depth + 1);
            break;
        case 6: {
            const std::vector<uint32_t>* list = nullptr;
            for (const auto& [sw, nodes] : n.switches)
                if (sw == n.default_switch) list = &nodes;
            if (!list && !n.switches.empty()) list = &n.switches.front().second;
            if (list)
                for (const auto c : *list) play(c, delay, fade, depth + 1);
            break;
        }
        case 5: ranseq(n, delay, fade, depth); break;
        default: break;
        }
        return {first, out.size()};
    }

    size_t pick(const WwiseNode& n) {
        const auto& list = n.playlist;
        if (n.mode == 1) {  // sequence
            auto& at = state.sequence[n.id];
            const size_t k = at % list.size();
            at = k + 1;
            return k;
        }
        auto& recent = state.recent[n.id];
        const size_t avoid = n.random_mode == 1 ? list.size() - 1 : std::min<size_t>(n.avoid_repeat, list.size() - 1);
        while (recent.size() > avoid) recent.erase(recent.begin());
        double total = 0;
        for (size_t k = 0; k < list.size(); ++k)
            if (std::ranges::find(recent, list[k].first) == recent.end()) total += std::max(1, list[k].second);
        double roll = state.unit() * total;
        size_t chosen = 0;
        for (size_t k = 0; k < list.size(); ++k) {
            if (std::ranges::find(recent, list[k].first) != recent.end()) continue;
            chosen = k;
            roll -= std::max(1, list[k].second);
            if (roll < 0) break;
        }
        if (avoid) recent.push_back(list[chosen].first);
        return chosen;
    }

    void ranseq(const WwiseNode& n, float delay, float fade, int depth) {
        if (n.playlist.empty()) return;
        if (!n.continuous) {
            play(n.playlist[pick(n)].first, delay, fade, depth + 1);
            return;
        }
        // Continuous: the whole playlist, one item after another (shuffled or in order), the
        // container's loop count times; an endless one plays a round and is played again when
        // its last voice ends (WwiseVoice::repeat).
        const size_t rounds = n.loop_count == 0 ? 1 : n.loop_count;
        const bool crossfade = n.transition_mode == 1 || n.transition_mode == 2;
        const bool gap = n.transition_mode == 3;
        const float t = n.transition_ms / 1000.0f;
        int previous = -1;
        for (size_t round = 0; round < rounds; ++round)
            for (size_t i = 0; i < n.playlist.size(); ++i) {
                const auto [a, b] = play(n.playlist[pick(n)].first, previous < 0 ? delay : 0, fade, depth + 1);
                if (a == b) continue;
                if (previous >= 0)
                    for (size_t k = a; k < b; ++k) {
                        out[k].after = previous;
                        out[k].overlap = crossfade ? t : gap ? -t : 0;
                    }
                previous = int(b - 1);
            }
        if (n.loop_count == 0 && previous >= 0) out[size_t(previous)].repeat = n.id;
    }

    void sound(const WwiseNode& s, float delay, float fade) {
        WwiseVoice v;
        v.sound = s.id;
        v.source = s.source;
        v.stream_type = s.stream_type;
        v.delay = delay;
        v.fade_in = fade;
        v.loops = s.props.has(wwise_prop::Loop) ? s.props.u(wwise_prop::Loop) : 1;
        const WwiseNode* positioning = nullptr;
        uint32_t node = s.id;
        for (int depth = 0; depth < 64 && node; ++depth) {
            const auto it = bank.nodes.find(node);
            if (it == bank.nodes.end()) break;
            const auto& n = it->second;
            v.volume_db += randomized(n.props, wwise_prop::Volume) + n.props.f(wwise_prop::MakeUpGain) +
                           n.props.f(wwise_prop::OutputBusVolume);
            v.pitch_cents += randomized(n.props, wwise_prop::Pitch);
            v.delay += randomized(n.props, wwise_prop::InitialDelay);
            if (!positioning && n.positioning_override) positioning = &n;
            if (!v.attenuation && n.props.has(wwise_prop::AttenuationID)) v.attenuation = n.props.u(wwise_prop::AttenuationID);
            node = n.parent;
        }
        if (positioning) {
            v.spatial = positioning->spatial;
            if (!positioning->attenuation_on) v.attenuation = 0;
            else if (positioning->props.has(wwise_prop::AttenuationID))
                v.attenuation = positioning->props.u(wwise_prop::AttenuationID);
        }
        if (!v.spatial) v.attenuation = 0;
        v.delay = std::max(0.0f, v.delay);
        out.push_back(v);
    }
};

}  // namespace

std::vector<WwiseVoice> wwise_play_event(const WwiseBank& bank, uint32_t event_id, WwisePlayState& state) {
    std::vector<WwiseVoice> out;
    const auto it = bank.events.find(event_id);
    if (it == bank.events.end()) return out;
    Builder b{bank, state, out};
    for (const auto action_id : it->second) {
        const auto a = bank.actions.find(action_id);
        if (a == bank.actions.end() || a->second.bus) continue;
        const auto& act = a->second;
        if (act.type == 0x0403 || act.type == 0x0503) {
            float delay = float(int32_t(act.props.u(wwise_prop::DelayTime))) / 1000.0f;
            const auto [lo, hi] = act.props.range(wwise_prop::DelayTime);
            delay += (lo + (hi - lo) * state.unit()) / 1000.0f;
            const float fade = float(int32_t(act.props.u(wwise_prop::TransitionTime))) / 1000.0f;
            b.play(act.target, std::max(0.0f, delay), std::max(0.0f, fade), 0);
        } else if ((act.type >> 8) == 0x1E && act.seek_relative) {
            const float seek = std::clamp(act.seek + act.seek_min + (act.seek_max - act.seek_min) * state.unit(), 0.0f, 1.0f);
            for (auto& v : out)
                if (v.after < 0 && wwise_is_under(bank, v.sound, act.target)) v.seek = seek;
        }
    }
    return out;
}

std::vector<WwiseVoice> wwise_play_node(const WwiseBank& bank, uint32_t node, WwisePlayState& state) {
    std::vector<WwiseVoice> out;
    Builder{bank, state, out}.play(node, 0, 0, 0);
    return out;
}

std::vector<uint32_t> wwise_stop_targets(const WwiseBank& bank, uint32_t event_id) {
    std::vector<uint32_t> out;
    const auto it = bank.events.find(event_id);
    if (it == bank.events.end()) return out;
    for (const auto action_id : it->second)
        if (const auto a = bank.actions.find(action_id); a != bank.actions.end() && (a->second.type >> 8) == 0x01)
            out.push_back(a->second.target);
    return out;
}

std::unordered_map<uint32_t, TagHash> index_wwise_streams(PackageManager& pm) {
    std::unordered_map<uint32_t, TagHash> out;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type == 26 && e.subtype == 7 && e.file_size > 64) out.try_emplace(e.reference, TagHash::make(info.pkg_id, uint16_t(i)));
        }
    }
    return out;
}

WwiseEventData load_wwise_event(PackageManager& pm, TagHash event, const std::unordered_map<uint32_t, TagHash>* all_streams) {
    const Entry* e = pm.entry(event);
    if (!e || e->type != 8 || e->reference != kClassAudioEvent)
        throw std::runtime_error(std::format("{}: not an audio event", event.str()));
    const auto bytes = pm.read(event);
    TagReader r(bytes);
    WwiseEventData out;
    out.tag = event;
    out.play_id = r.u32_at(0x08);
    out.stop_id = r.u32_at(0x0C);
    if (const TagHash info = r.tag_at(0x14); !info.is_none())
        if (const Entry* ie = pm.entry(info); ie && ie->reference == kClassAudioEventInfo) {
            const auto ib = pm.read(info);
            TagReader ir(ib);
            if (ib.size() >= 12) {
                out.max_distance = ir.f32_at(4);
                out.longest = ir.f32_at(8);
            }
        }
    const auto streams = r.vec_at(0x20);
    for (uint64_t k = 0; k < streams.count; ++k) {
        const TagHash w = r.tag_at(streams.data + k * 4);
        if (const Entry* we = pm.entry(w)) out.streams[we->reference] = w;
    }
    // The bank: +0x18 names a 8080B12D tag holding the bank's tag (shared banks), else it is a
    // nearby 26/6 entry of the same package (usually just before the event, its reference the
    // bank id). Either way, the bank whose events hold the play id.
    std::string why = "no bank";
    auto try_bank = [&](TagHash b) {
        const Entry* be = pm.entry(b);
        if (!be || be->type != 26 || be->subtype != 6) return;
        try {
            auto bank = parse_wwise_bank(pm.read(b));
            if (bank.events.contains(out.play_id)) out.bank = std::make_shared<const WwiseBank>(std::move(bank));
            else why = std::format("bank {} lacks event {:08X}", b.str(), out.play_id);
        } catch (const std::exception& ex) {
            why = ex.what();
        }
    };
    if (const TagHash ref = r.tag_at(0x18); !ref.is_none())
        if (const Entry* re = pm.entry(ref); re && re->type == 8 && re->reference == kClassAudioBankRef && re->file_size >= 4) {
            const auto rb = pm.read(ref);
            try_bank(TagReader(rb).tag_at(0));
        }
    for (int step = 1; step <= 128 && !out.bank; ++step) {
        const int delta = step <= 64 ? -step : step - 64;
        const TagHash b(uint32_t(int64_t(event.value) + delta));
        if (b.pkg_id() == event.pkg_id()) try_bank(b);
    }
    if (!out.bank) throw std::runtime_error(std::format("{}: {}", event.str(), why));
    if (all_streams)  // sources the event does not list (shared streams)
        for (const auto& [id, n] : out.bank->nodes)
            if (n.type == 2 && !out.streams.contains(n.source) && !out.bank->media.contains(n.source))
                if (const auto it = all_streams->find(n.source); it != all_streams->end()) out.streams[n.source] = it->second;
    return out;
}

std::vector<uint8_t> wwise_source_bytes(PackageManager& pm, const WwiseEventData& event, uint32_t source) {
    if (const auto s = event.streams.find(source); s != event.streams.end()) return pm.read(s->second);
    if (const auto m = event.bank->media.find(source);
        m != event.bank->media.end() && size_t(m->second.first) + m->second.second <= event.bank->data.size())
        return {event.bank->data.begin() + m->second.first, event.bank->data.begin() + m->second.first + m->second.second};
    return {};
}

PcmAudio wwise_downmix(PcmAudio pcm) {
    const size_t n = pcm.channels;
    if (n <= 2) return pcm;
    const size_t frames = pcm.frames();
    const bool ambisonic = n == 4 || n == 9 || n == 16;
    std::vector<int16_t> out(frames * (ambisonic || n == 6 ? 2 : 1));
    auto clamp16 = [](float v) { return int16_t(std::clamp(v, -32768.0f, 32767.0f)); };
    for (size_t f = 0; f < frames; ++f) {
        const int16_t* in = pcm.samples.data() + f * n;
        if (ambisonic) {
            out[f * 2] = clamp16(0.5f * (float(in[0]) + float(in[1])));
            out[f * 2 + 1] = clamp16(0.5f * (float(in[0]) - float(in[1])));
        } else if (n == 6) {
            constexpr float k = 0.7071f, norm = 1.0f / (1 + 2 * k);
            out[f * 2] = clamp16(norm * (float(in[0]) + k * float(in[2]) + k * float(in[4])));
            out[f * 2 + 1] = clamp16(norm * (float(in[1]) + k * float(in[2]) + k * float(in[5])));
        } else {
            float sum = 0;
            for (size_t c = 0; c < n; ++c) sum += float(in[c]);
            out[f] = clamp16(sum / float(n));
        }
    }
    pcm.channels = uint16_t(ambisonic || n == 6 ? 2 : 1);
    pcm.samples = std::move(out);
    return pcm;
}

}  // namespace nim
