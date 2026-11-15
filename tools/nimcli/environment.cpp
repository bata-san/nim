// nimcli environment BUBBLE [ACTIVITY]: where a selection's global channel expressions come from.
// Lists every environment entity (node table, node, entity, component) with its 8080B64E
// records, their raw header fields and their values over the day for the sequencer inputs,
// so the choice between several authored environments is made from data and not by draw order.
#include "nim/content/map.hpp"
#include "nim/content/object_channels.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_reader.hpp"

#include <cstring>
#include <map>
#include <print>
#include <set>
#include <string>
#include <vector>

using namespace nim;

namespace {
struct Expr {
    uint32_t channel = 0, record_id = 0, index = 0;
    size_t offset = 0;
    std::vector<uint8_t> header;  // payload +0x00..+0x50
    std::vector<uint8_t> bytecode;
    std::vector<float4> constants;
};

std::set<uint8_t> inputs_of(const std::vector<uint8_t>& bc) {
    // Walk with the VM itself so operand bytes are never mistaken for opcodes.
    std::set<uint8_t> in;
    TfxEnvironment env;
    env.sequencer_input = [&](uint8_t i) -> float4 { in.insert(i); return {}; };
    std::array<float4, 16> out{};
    try { run_tfx(bc, {}, out, env); } catch (...) {}
    return in;
}
}  // namespace

int cmd_environment(PackageManager& pm, TagHash bubble, TagHash activity, bool verbose) {
    std::vector<TagHash> extra;
    if (!activity.is_none())
        for (const auto& m : list_activity_maps(pm))
            if (m.activity == activity && m.bubble == bubble) extra = load_activity_node_tables(pm, activity, m.bubble_name, m.activity_map);
    const MapLayout map = load_map_layout(pm, bubble, extra);
    const MapLayout base = activity.is_none() ? map : load_map_layout(pm, bubble);
    const std::set<uint32_t> bubble_tables = [&] { std::set<uint32_t> s; for (auto t : base.node_tables) s.insert(t.value); return s; }();
    const RenderGlobals globals = load_render_globals(pm);
    pm.build_hash64_index();
    std::println("selection: {} node tables ({} from the bubble)", map.node_tables.size(), bubble_tables.size());

    std::map<uint32_t, std::vector<std::string>> component_users;  // component -> "table node entity"
    std::map<uint32_t, std::vector<Expr>> components;
    std::vector<uint32_t> order;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try { data = pm.read(t); } catch (...) { continue; }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            const uint64_t h64 = r.u64_at(e + 0x30);
            if (!h64 || h64 == ~0ull) continue;
            const auto ent = pm.resolve64(h64);
            if (!ent) continue;
            const Entry* ee = pm.entry(*ent);
            if (!ee || ee->type != 8 || ee->reference != 0x8080BAAD) continue;
            std::vector<uint8_t> ed;
            try { ed = pm.read(*ent); } catch (...) { continue; }
            for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, ed.data() + o, 4);
                if (v < 0x80800000u || v == 0xFFFFFFFFu) continue;
                const Entry* ce = pm.entry(TagHash(v));
                if (!ce || ce->type != 8 || ce->reference != kClassComponent) continue;
                if (!components.contains(v)) {
                    std::vector<uint8_t> cd;
                    try { cd = pm.read(TagHash(v)); } catch (...) { continue; }
                    TagReader cr(cd);
                    std::vector<Expr> found;
                    std::vector<uint32_t> names;  // output table: {self, 80809F46, ..., +0x30 name}
                    for (size_t c = 0; c + 0x38 <= cd.size(); c += 4)
                        if (cr.u32_at(c) == v && cr.u32_at(c + 4) == 0x80809F46) names.push_back(cr.u32_at(c + 0x30));
                    for (size_t c = 0; c + 0x54 <= cd.size(); c += 4) {
                        if (cr.u32_at(c) != kClassSequenceGlobalChannel) continue;
                        const size_t p = c + 4;
                        Expr x;
                        x.record_id = cr.u32_at(p);
                        x.index = cr.u32_at(p + 0x28);
                        x.channel = x.index < names.size() ? names[x.index] : x.record_id;
                        x.offset = p;
                        x.header.assign(cd.begin() + ptrdiff_t(p), cd.begin() + ptrdiff_t(p + 0x50));
                        try {
                            const auto bc = cr.vec_at(p + 0x30);
                            const auto k = cr.vec_at(p + 0x40);
                            if (bc.data + bc.count > cd.size() || k.data + k.count * 16 > cd.size()) continue;
                            x.bytecode.assign(cd.begin() + ptrdiff_t(bc.data), cd.begin() + ptrdiff_t(bc.data + bc.count));
                            for (uint64_t j = 0; j < k.count; ++j)
                                x.constants.push_back({cr.f32_at(k.data + j * 16), cr.f32_at(k.data + j * 16 + 4),
                                                       cr.f32_at(k.data + j * 16 + 8), cr.f32_at(k.data + j * 16 + 12)});
                        } catch (...) { continue; }
                        found.push_back(std::move(x));
                    }
                    components[v] = std::move(found);
                    if (!components[v].empty()) order.push_back(v);
                }
                if (components[v].empty()) continue;
                component_users[v].push_back(std::format("table {}{} node {} entity {} at ({:.1f}, {:.1f}, {:.1f}) node+0x30 {:016X}",
                    t.str(), bubble_tables.contains(t.value) ? " [bubble]" : " [activity]", i, ent->str(),
                    r.f32_at(e + 0x10), r.f32_at(e + 0x14), r.f32_at(e + 0x18), h64));
            }
        }
    }

    const float times[] = {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f};
    for (uint32_t c : order) {
        const auto& exprs = components[c];
        size_t render = 0;
        for (const auto& x : exprs)
            if (std::ranges::find(globals.channel_ids, x.channel) != globals.channel_ids.end()) ++render;
        std::println("\ncomponent {}: {} expressions ({} render-global channels), placed {} times", TagHash(c).str(), exprs.size(), render,
                     component_users[c].size());
        for (size_t u = 0; u < component_users[c].size() && u < 6; ++u) std::println("  {}", component_users[c][u]);
        if (component_users[c].size() > 6) std::println("  ... {} more", component_users[c].size() - 6);
        for (const auto& x : exprs) {
            const auto it = std::ranges::find(globals.channel_ids, x.channel);
            const bool is_global = it != globals.channel_ids.end();
            const char* name = object_channel_name(x.channel);
            const auto in = inputs_of(x.bytecode);
            std::string ins;
            for (auto i : in) ins += std::format("{} ", i);
            std::string hdr;
            if (verbose)
                for (size_t o = 4; o < 0x30; o += 4) {
                    uint32_t v; float f;
                    std::memcpy(&v, x.header.data() + o, 4);
                    std::memcpy(&f, x.header.data() + o, 4);
                    if (v) hdr += std::format(" +{:02X}={:08X}({:.4g})", o, v, f);
                }
            if (x.record_id != x.channel) hdr += std::format("  (record word {:08X})", x.record_id);
            std::println("  {} {:08X} {}{} inputs[{}] bc {} B, {} consts{}", is_global ? std::format("ch{:02X}", it - globals.channel_ids.begin()) : std::string("    "),
                         x.channel, name ? name : "?", is_global ? "" : " (not a render-global)", ins, x.bytecode.size(), x.constants.size(), hdr);
            if (!is_global && !verbose) continue;
            for (float in1 : {0.0f, 1.0f, 100.0f}) {
                if (in1 != 0 && !in.contains(1)) continue;
                std::string row = std::format("      in1={:<4}", in1);
                for (float t : times) {
                    TfxEnvironment env;
                    env.sequencer_input = [&](uint8_t i) -> float4 { const float v = i == 0 ? t : i == 1 ? in1 : 0; return {v, v, v, v}; };
                    std::array<float4, 16> out{};
                    try {
                        run_tfx(x.bytecode, x.constants, out, env);
                        row += std::format(" | {:.3g},{:.3g},{:.3g},{:.3g}", out[0][0], out[0][1], out[0][2], out[0][3]);
                    } catch (const std::exception& ex) {
                        row += std::format(" | error {}", ex.what());
                        break;
                    }
                }
                std::println("{}", row);
            }
        }
    }
    std::println("\n{} components with channel expressions; times shown: 0, 1/8 ... 7/8 of the cycle", order.size());
    return 0;
}
