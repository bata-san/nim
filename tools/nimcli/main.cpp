// nimcli: headless inspection of Marathon packages.
//
//   nimcli [--install DIR] pkgs                 list packages
//   nimcli [--install DIR] ls PKGID             list entries of a package (PKGID in hex)
//   nimcli [--install DIR] types                histogram of (type, subtype, reference) over all entries
//   nimcli [--install DIR] read TAGHASH [OUT]   dump an entry's bytes (hex preview or to file)
//   nimcli [--install DIR] verify [PKGID]       decrypt + decompress every block, report failures
//   nimcli [--install DIR] grep TEXT|hex:BYTES [TYPE]  find entries containing a byte pattern
//   nimcli [--install DIR] strings [OUT.csv] [FIND]   every localized string, all languages
//   nimcli [--install DIR] dialogue [OUT.csv]         every spoken line: speaker, text (all languages), audio
//   nimcli [--install DIR] xref-strings                which tag classes reference string hashes
//   nimcli [--install DIR] xref-tags T/S|class:HASH    which tag classes reference tags of that kind
//   nimcli [--install DIR] static TAG                  static mesh: groups, techniques, shaders, layouts
//   nimcli [--install DIR] shader TAG [OUT.asm]         disassemble a shader header's DXBC
//   nimcli [--install DIR] technique TAG               technique stages, textures (with headers), constants
//   nimcli [--install DIR] find CLASS                   list every tag of a class
//   nimcli [--install DIR] lookup HEX...                what a 32-bit value is: TagHash, string hash, audio event id
//   nimcli [--install DIR] h64 [HEX64...]               hash64 table summary by target class, or resolve values
//   nimcli [--install DIR] xref-values CLASS OFFSET     which classes contain the u32 found at OFFSET of CLASS tags
//                                                       (also reports how many of those also hold string hashes)
#include "nim/content/dialogue.hpp"
#include "nim/content/strings.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/core/binary.hpp"
#include "nim/core/package_manager.hpp"

#include <algorithm>
#include <set>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <map>
#include <unordered_set>
#include <print>
#include <string>
#include <vector>

using namespace nim;
namespace fs = std::filesystem;

namespace {

uint32_t parse_hex(const std::string& s) { return uint32_t(std::stoul(s, nullptr, 16)); }

void hexdump(std::span<const uint8_t> d, size_t limit) {
    for (size_t i = 0; i < std::min(d.size(), limit); i += 16) {
        std::print("{:08X}  ", i);
        for (size_t j = 0; j < 16; ++j)
            i + j < d.size() ? std::print("{:02X} ", d[i + j]) : std::print("   ");
        std::print(" ");
        for (size_t j = 0; j < 16 && i + j < d.size(); ++j)
            std::print("{}", (d[i + j] >= 0x20 && d[i + j] < 0x7F) ? char(d[i + j]) : '.');
        std::println("");
    }
}

int cmd_pkgs(PackageManager& pm) {
    for (const auto& i : pm.packages())
        std::println("{:04X}  patch {:<3} lang {:<2} {}", i.pkg_id, i.newest_patch, i.language, i.name);
    std::println("{} packages", pm.packages().size());
    return 0;
}

int cmd_ls(PackageManager& pm, uint16_t id) {
    Package& p = pm.open(id);
    const auto& h = p.header();
    std::println("{} pkg {:04X} patch {} group {:016X} entries {} blocks {} named {} h64 {}", p.name(),
                 h.pkg_id, h.patch_id, h.group_id, h.entry_count, h.block_count, h.named_tag_count,
                 h.h64_count);
    for (size_t i = 0; i < p.entries().size(); ++i) {
        const auto& e = p.entries()[i];
        std::println("{}  type {:>3}/{}  ref {:08X}  size {:>9}", TagHash::make(id, uint16_t(i)).str(),
                     e.type, e.subtype, e.reference, e.file_size);
    }
    for (const auto& t : p.named_tags()) std::println("named {} {:08X} {}", t.hash.str(), t.class_hash, t.name);
    return 0;
}

int cmd_types(PackageManager& pm) {
    struct Stat { size_t count = 0; uint64_t bytes = 0; };
    std::map<std::tuple<int, int, uint32_t>, Stat> stats;
    for (const auto& i : pm.packages()) {
        for (const auto& e : pm.open(i).entries()) {
            auto& s = stats[{e.type, e.subtype, e.reference}];
            s.count++;
            s.bytes += e.file_size;
        }
    }
    std::vector<std::pair<std::tuple<int, int, uint32_t>, Stat>> v(stats.begin(), stats.end());
    std::ranges::sort(v, [](auto& a, auto& b) { return a.second.count > b.second.count; });
    for (auto& [k, s] : v)
        std::println("type {:>3}/{}  ref {:08X}  count {:>7}  bytes {:>12}", std::get<0>(k), std::get<1>(k),
                     std::get<2>(k), s.count, s.bytes);
    return 0;
}

int cmd_read(PackageManager& pm, TagHash h, const char* out) {
    const Entry* e = pm.entry(h);
    if (!e) { std::println(stderr, "no such tag {}", h.str()); return 1; }
    auto data = pm.read(h);
    std::println("{} type {}/{} ref {:08X} size {}", h.str(), e->type, e->subtype, e->reference, data.size());
    if (out) {
        std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
        std::println("wrote {}", out);
    } else {
        hexdump(data, 0x200);
    }
    return 0;
}

int cmd_verify(PackageManager& pm, int only) {
    size_t ok = 0, bad = 0;
    uint64_t bytes = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& i : pm.packages()) {
        if (only >= 0 && i.pkg_id != only) continue;
        Package& p = pm.open(i);
        for (uint32_t b = 0; b < p.blocks().size(); ++b) {
            try {
                bytes += p.read_block(b).size();
                ok++;
            } catch (const std::exception& ex) {
                if (bad++ < 20) std::println("FAIL {}", ex.what());
            }
        }
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::println("blocks ok {} failed {}  {:.1f} MiB in {:.2f}s ({:.0f} MiB/s)", ok, bad, bytes / 1048576.0, s,
                 bytes / 1048576.0 / s);
    return bad ? 1 : 0;
}

std::vector<uint8_t> parse_pattern(const std::string& s) {
    std::vector<uint8_t> out;
    if (s.starts_with("hex:")) {
        for (size_t i = 4; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
    } else {
        out.assign(s.begin(), s.end());
    }
    return out;
}

int cmd_grep(PackageManager& pm, const std::string& pattern, int only_type) {
    auto pat = parse_pattern(pattern);
    size_t hits = 0;
    for (const auto& i : pm.packages()) {
        Package& p = pm.open(i);
        for (uint32_t idx = 0; idx < p.entries().size(); ++idx) {
            const Entry& e = p.entries()[idx];
            if (e.file_size == 0 || (only_type >= 0 && e.type != only_type)) continue;
            if (e.type == 26 || e.type == 27 || e.type == 40 || e.type == 48) {
                if (only_type < 0) continue;  // skip bulk media unless asked for explicitly
            }
            std::vector<uint8_t> d;
            try { d = p.read_entry(idx); } catch (...) { continue; }
            auto it = std::search(d.begin(), d.end(), pat.begin(), pat.end());
            if (it != d.end()) {
                std::println("{}  type {:>3}/{}  ref {:08X}  @0x{:X}  size {}  {}", TagHash::make(i.pkg_id, uint16_t(idx)).str(),
                             e.type, e.subtype, e.reference, size_t(it - d.begin()), e.file_size, i.name);
                if (++hits >= 200) return 0;
            }
        }
    }
    std::println("{} hits", hits);
    return 0;
}

std::string csv_field(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out += c;
    }
    return out + "\"";
}

int cmd_strings(PackageManager& pm, const char* out_path, const char* find) {
    auto t0 = std::chrono::steady_clock::now();
    StringTable table;
    table.load_all(pm);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const auto& st = table.stats();
    std::println("containers {} (failed {})  strings {} (unique hashes {})  in {:.2f}s", st.containers,
                 st.containers_failed, st.strings, st.unique_hashes, secs);
    std::println("parts {}  placeholders: args {} vars {} inputs {} values {} unknown {}", st.decode.parts,
                 st.decode.arguments, st.decode.variables, st.decode.input_glyphs, st.decode.values,
                 st.decode.unknown_kinds);
    std::println("string-data tags: {} in install, {} referenced, {} orphaned", st.data_tags_total,
                 st.data_tags_referenced, st.orphan_data_tags.size());
    for (size_t i = 0; i < std::min<size_t>(st.orphan_data_tags.size(), 10); ++i)
        std::println("  orphan {}", st.orphan_data_tags[i].str());
    for (size_t l = 0; l < kTextLanguageCount; ++l)
        std::println("  {:<7} {}", language_code(TextLanguage(l)), st.non_empty[l]);

    if (out_path) {
        std::ofstream f(out_path, std::ios::binary);
        f << "\xEF\xBB\xBF" << "hash,container";
        for (size_t l = 0; l < kTextLanguageCount; ++l) f << ',' << language_code(TextLanguage(l));
        f << "\n";
        for (const auto& s : table.strings()) {
            f << std::format("{:08X},{}", s.hash, s.container.str());
            for (const auto& t : s.text) f << ',' << csv_field(t);
            f << "\n";
        }
        std::println("wrote {}", out_path);
    }
    if (find) {
        size_t shown = 0;
        for (const auto& s : table.strings()) {
            bool hit = false;
            for (const auto& t : s.text) hit |= t.find(find) != std::string::npos;
            if (!hit) continue;
            std::println("{:08X} [{}]\n  en: {}\n  ja: {}", s.hash, s.container.str(), s.text[0], s.text[1]);
            if (++shown >= 30) break;
        }
    }
    return st.containers_failed ? 1 : 0;
}

// Reverse-engineering aid: for every tag class, count 4-byte-aligned values that are known string hashes.
int cmd_xref_strings(PackageManager& pm) {
    StringTable table;
    table.load_all(pm);
    struct Stat { size_t tags = 0, tags_with_hits = 0, hits = 0; TagHash sample; size_t sample_off = 0; };
    std::map<uint32_t, Stat> by_class;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference == kClassLocalizedStrings || e.reference == kClassStringData) continue;
            auto& st = by_class[e.reference];
            st.tags++;
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            size_t hits = 0;
            for (size_t o = 8; o + 4 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                if (v > 0x00FFFFFF && table.find(v)) {
                    if (!hits && !st.tags_with_hits) { st.sample = TagHash::make(info.pkg_id, uint16_t(i)); st.sample_off = o; }
                    hits++;
                }
            }
            if (hits) { st.tags_with_hits++; st.hits += hits; }
        }
    }
    std::vector<std::pair<uint32_t, Stat>> v(by_class.begin(), by_class.end());
    std::ranges::sort(v, [](auto& a, auto& b) { return a.second.hits > b.second.hits; });
    for (auto& [cls, s] : v) {
        if (!s.hits) break;
        std::println("class {:08X}  tags {:>7}  with-hits {:>7}  hits {:>8}  sample {} @0x{:X}", cls, s.tags,
                     s.tags_with_hits, s.hits, s.sample.str(), s.sample_off);
    }
    return 0;
}

// Reverse-engineering aid: which classes hold TagHashes pointing at entries matching `target`
// ("26/6" = type/subtype, or "class:8080B9B8").
int cmd_xref_tags(PackageManager& pm, const std::string& target) {
    int want_type = -1, want_sub = -1;
    uint32_t want_class = 0;
    if (target.starts_with("class:")) want_class = parse_hex(target.substr(6));
    else { want_type = std::stoi(target); want_sub = std::stoi(target.substr(target.find('/') + 1)); }
    auto matches = [&](const Entry& e) {
        if (want_class) return e.type == 8 && e.reference == want_class;
        return e.type == want_type && e.subtype == want_sub;
    };
    struct Stat { size_t tags = 0, tags_with_hits = 0, hits = 0; TagHash sample; size_t sample_off = 0; };
    std::map<uint32_t, Stat> by_class;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8) continue;
            auto& st = by_class[e.reference];
            st.tags++;
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            size_t hits = 0;
            for (size_t o = 8; o + 4 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                TagHash h(v);
                if (!h.is_pkg_ref()) continue;
                const Entry* te = pm.entry(h);
                if (!te || !matches(*te)) continue;
                if (!st.tags_with_hits && !hits) { st.sample = TagHash::make(info.pkg_id, uint16_t(i)); st.sample_off = o; }
                hits++;
            }
            if (hits) { st.tags_with_hits++; st.hits += hits; }
        }
    }
    std::vector<std::pair<uint32_t, Stat>> v(by_class.begin(), by_class.end());
    std::ranges::sort(v, [](auto& a, auto& b) { return a.second.hits > b.second.hits; });
    for (auto& [cls, s] : v) {
        if (!s.hits) break;
        std::println("class {:08X}  tags {:>7}  with-hits {:>7}  hits {:>8}  sample {} @0x{:X}", cls, s.tags,
                     s.tags_with_hits, s.hits, s.sample.str(), s.sample_off);
    }
    return 0;
}

int cmd_xref_values(PackageManager& pm, uint32_t src_class, size_t src_off) {
    std::unordered_set<uint32_t> values;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != src_class) continue;
            auto d = p.read_entry(i);
            uint32_t v;
            if (src_off + 4 <= d.size()) { std::memcpy(&v, d.data() + src_off, 4); values.insert(v); }
        }
    }
    values.erase(0); values.erase(0xFFFFFFFF); values.erase(0x811C9DC5);
    std::println("{} distinct values from class {:08X} @0x{:X}", values.size(), src_class, src_off);
    StringTable table;
    table.load_all(pm);
    struct Stat { size_t tags = 0, tags_with_hits = 0, hits = 0, with_strings = 0; TagHash sample; size_t sample_off = 0; };
    std::map<uint32_t, Stat> by_class;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference == src_class) continue;
            auto& st = by_class[e.reference];
            st.tags++;
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            size_t hits = 0, str_hits = 0;
            for (size_t o = 8; o + 4 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                if (values.contains(v)) {
                    if (!st.tags_with_hits && !hits) { st.sample = TagHash::make(info.pkg_id, uint16_t(i)); st.sample_off = o; }
                    hits++;
                } else if (v > 0x00FFFFFF && table.find(v)) {
                    str_hits++;
                }
            }
            if (hits) { st.tags_with_hits++; st.hits += hits; if (str_hits) st.with_strings++; }
        }
    }
    std::vector<std::pair<uint32_t, Stat>> v(by_class.begin(), by_class.end());
    std::ranges::sort(v, [](auto& a, auto& b) { return a.second.hits > b.second.hits; });
    for (auto& [cls, s] : v) {
        if (!s.hits) break;
        std::println("class {:08X}  tags {:>7}  with-hits {:>7}  hits {:>8}  also-strings {:>6}  sample {} @0x{:X}", cls,
                     s.tags, s.tags_with_hits, s.hits, s.with_strings, s.sample.str(), s.sample_off);
    }
    return 0;
}

int cmd_lookup(PackageManager& pm, const std::vector<std::string>& values) {
    StringTable table;
    table.load_all(pm);
    std::unordered_map<uint32_t, TagHash> events;  // audio event id -> 8080B128 tag
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != 0x8080B128) continue;
            auto d = p.read_entry(i);
            events.emplace(BinaryReader(d).u32_at(8), TagHash::make(info.pkg_id, uint16_t(i)));
        }
    }
    for (const auto& s : values) {
        uint32_t v = parse_hex(s);
        std::print("{:08X}:", v);
        TagHash h(v);
        if (const Entry* e = h.is_pkg_ref() ? pm.entry(h) : nullptr)
            std::print("  tag type {}/{} class {:08X} size {}", e->type, e->subtype, e->reference, e->file_size);
        if (auto* str = table.find(v)) std::print("  string en=\"{}\" ja=\"{}\"", str->text[0], str->text[1]);
        if (auto it = events.find(v); it != events.end()) std::print("  audio-event {}", it->second.str());
        std::println("");
    }
    return 0;
}

int cmd_h64(PackageManager& pm, const std::vector<std::string>& values) {
    pm.build_hash64_index();
    if (values.empty()) {
        std::map<std::pair<int, uint32_t>, size_t> by_target;
        size_t total = 0;
        for (const auto& info : pm.packages())
            for (const auto& h : pm.open(info).hash64s()) {
                const Entry* e = pm.entry(h.hash32);
                by_target[{e ? e->type : -1, e ? e->reference : 0}]++;
                total++;
            }
        std::println("{} hash64 entries", total);
        for (auto& [k, n] : by_target) if (n > 50) std::println("  type {:>3} class {:08X}  {}", k.first, k.second, n);
        return 0;
    }
    for (const auto& s : values) {
        uint64_t v = std::stoull(s, nullptr, 16);
        auto t = pm.resolve64(v);
        const Entry* e = t ? pm.entry(*t) : nullptr;
        std::println("{:016X} -> {}", v, t ? std::format("{} type {}/{} class {:08X}", t->str(), e ? e->type : 0,
                                                              e ? e->subtype : 0, e ? e->reference : 0) : "-");
    }
    return 0;
}

int cmd_dialogue(PackageManager& pm, const char* out_path) {
    auto t0 = std::chrono::steady_clock::now();
    StringTable table;
    table.load_all(pm);
    DialogueStats st;
    auto lines = load_all_dialogue(pm, &st);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto text = [&](uint32_t h, size_t lang) -> std::string {
        const LocalizedString* s = table.find(h);
        return s ? s->text[lang] : std::string();
    };
    size_t with_text = 0, with_speaker = 0, with_wem = 0;
    for (const auto& l : lines) {
        with_text += !text(l.subtitle_hash, 0).empty();
        with_speaker += !text(l.speaker_hash, 0).empty();
        with_wem += !l.wem.empty();
    }
    std::println("tags scanned {}  tables {}  entries {}  lines {}  other variants {}  in {:.1f}s", st.tags_scanned,
                 st.tables, st.entries, st.lines, st.other_variants, secs);
    std::println("lines with text {}  with speaker {}  audio event resolved {}  with WEM {}", with_text,
                 with_speaker, st.audio_resolved, with_wem);
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 8); ++i) {
        const auto& l = lines[i * (lines.size() / 8)];
        std::println("  [{}] {}: {}\n      {}: {}", l.table.str(), text(l.speaker_hash, 0), text(l.subtitle_hash, 0),
                     text(l.speaker_hash, 1), text(l.subtitle_hash, 1));
    }
    if (out_path) {
        std::ofstream f(out_path, std::ios::binary);
        f << "\xEF\xBB\xBF" << "table,line_id,speaker_en,speaker_ja,subtitle_hash";
        for (size_t lg = 0; lg < kTextLanguageCount; ++lg) f << ',' << language_code(TextLanguage(lg));
        f << ",caption_en,audio_event,wem\n";
        for (const auto& l : lines) {
            f << std::format("{},{:08X},{},{},{:08X}", l.table.str(), l.line_id, csv_field(text(l.speaker_hash, 0)),
                             csv_field(text(l.speaker_hash, 1)), l.subtitle_hash);
            for (size_t lg = 0; lg < kTextLanguageCount; ++lg) f << ',' << csv_field(text(l.subtitle_hash, lg));
            std::string caption = text(l.caption_hash, 0);
            f << ',' << csv_field(caption == text(l.subtitle_hash, 0) ? "" : caption);
            f << ',' << (l.audio_event.is_none() ? "" : l.audio_event.str()) << ",\"";
            for (size_t w = 0; w < l.wem.size(); ++w) f << (w ? " " : "") << l.wem[w].str();
            f << "\"\n";
        }
        std::println("wrote {}", out_path);
    }
    return 0;
}

int cmd_find(PackageManager& pm, uint32_t cls) {
    size_t n = 0;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.reference != cls || (e.type != 8 && e.type != 16)) continue;
            std::println("{}  type {:>3}/{}  size {:>8}  {}", TagHash::make(info.pkg_id, uint16_t(i)).str(), e.type,
                         e.subtype, e.file_size, info.name);
            n++;
        }
    }
    std::println("{} tags", n);
    return 0;
}

const char* kStageNames[] = {"VS", "HS", "DS", "GS", "PS", "CS"};

int cmd_static(PackageManager& pm, TagHash tag) {
    pm.build_hash64_index();
    StaticMesh m = load_static_mesh(pm, tag);
    auto layouts = load_input_layouts(pm);
    std::println("static {} data {} offset ({:.3f},{:.3f},{:.3f}) scale {:.3f} uv scale {:.3f} uv offset ({:.3f},{:.3f})",
                 m.tag.str(), m.data_tag.str(), m.mesh_offset[0], m.mesh_offset[1], m.mesh_offset[2], m.mesh_scale,
                 m.texcoord_scale, m.texcoord_offset[0], m.texcoord_offset[1]);
    for (size_t b = 0; b < m.buffers.size(); ++b)
        std::println("  buffers[{}] ib {} vb0 {} vb1 {} color {}", b, m.buffers[b].index.str(), m.buffers[b].vertex0.str(),
                     m.buffers[b].vertex1.str(), m.buffers[b].color.str());
    for (size_t g = 0; g < m.groups.size(); ++g) {
        const auto& gr = m.groups[g];
        const auto& part = m.parts.at(gr.part_index);
        std::print("  group {:>2} part {:>2} stage {:>2} layout {:>3} idx [{}, +{}) prim {} lod {}", g, gr.part_index,
                   gr.render_stage, gr.input_layout, part.index_start, part.index_count, part.primitive_type,
                   part.lod_category);
        if (g < m.techniques.size()) {
            Technique t = load_technique(pm, m.techniques[g]);
            std::print("  tech {} bind {}", t.tag.str(), t.bind_mode);
            for (size_t s = 0; s < 6; ++s)
                if (!t.stages[s].shader.is_none())
                    std::print(" {}={} tex{} cb{}", kStageNames[s], t.stages[s].shader.str(), t.stages[s].textures.size(),
                               t.stages[s].cbuffer_slot);
        }
        std::println("");
    }
    std::set<int> used;
    for (const auto& g : m.groups) used.insert(g.input_layout);
    for (int l : used) {
        std::print("  layout {}:", l);
        for (const auto& e : layouts[l])
            std::print(" [{}]{}{} fmt{} ({}B){}", e.buffer, e.semantic, e.semantic_index, e.dxgi_format, e.size,
                       e.instanced ? " inst" : "");
        std::println("");
    }
    return 0;
}

int cmd_shader(PackageManager& pm, TagHash header, const char* out) {
    // Marathon shaders are DXIL (SM6) inside a DXBC container; disassemble with `dxc -dumpbin OUT`.
    auto bc = load_shader_bytecode(pm, header);
    std::string path = out ? out : header.str() + ".dxil";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bc.data()), std::streamsize(bc.size()));
    std::println("wrote {} ({} bytes, DXIL in DXBC container; view with: dxc -dumpbin {})", path, bc.size(), path);
    return 0;
}

int cmd_technique(PackageManager& pm, TagHash tag) {
    pm.build_hash64_index();
    Technique t = load_technique(pm, tag);
    std::println("technique {} bind mode {}", t.tag.str(), t.bind_mode);
    for (size_t s = 0; s < 6; ++s) {
        const auto& st = t.stages[s];
        if (st.shader.is_none()) continue;
        std::println(" {} shader {}  bytecode {}B  constants {}  samplers {}  cbuffer slot {} tag {}", kStageNames[s],
                     st.shader.str(), st.bytecode.size(), st.constants.size(), st.samplers.size(), st.cbuffer_slot,
                     st.cbuffer.str());
        for (const auto& tb : st.textures) {
            try {
                Texture tx = load_texture(pm, tb.texture);
                std::println("   t{:<2} {} fmt {} {}x{} mips {} bytes {}", tb.slot, tb.texture.str(), tx.dxgi_format,
                             tx.width, tx.height, tx.mip_count, tx.data.size());
            } catch (const std::exception& ex) {
                std::println("   t{:<2} {} ({})", tb.slot, tb.texture.str(), ex.what());
            }
        }
        for (size_t k = 0; k < st.constants.size(); ++k)
            std::println("   c{:<2} ({:.4f}, {:.4f}, {:.4f}, {:.4f})", k, st.constants[k][0], st.constants[k][1],
                         st.constants[k][2], st.constants[k][3]);
        if (!st.bytecode.empty()) {
            std::print("{}", disassemble_tfx(st.bytecode));
            std::vector<float4> cb(st.cbuffer_init.begin(), st.cbuffer_init.end());
            if (cb.size() < 128) cb.resize(128);
            auto before = cb;
            try {
                auto res = run_tfx(st.bytecode, st.constants, cb);
                std::println("   vm: {} instructions, cbuffer {} regs", res.instructions, st.cbuffer_init.size());
                for (size_t i = 0; i < cb.size(); ++i)
                    if (cb[i] != before[i])
                        std::println("   cb{}[{}] = ({:.4f}, {:.4f}, {:.4f}, {:.4f})", st.cbuffer_slot, i, cb[i][0],
                                     cb[i][1], cb[i][2], cb[i][3]);
                for (const auto& et : res.extern_textures)
                    std::println("   extern texture -> t{} (extern {} offset {})", et.slot, et.extern_id, et.offset);
                for (const auto& u : res.unsupported) std::println("   stubbed op: {}", u);
            } catch (const std::exception& ex) {
                std::println("   vm error: {}", ex.what());
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path install;
    if (args.size() >= 2 && args[0] == "--install") {
        install = args[1];
        args.erase(args.begin(), args.begin() + 2);
    } else if (auto found = PackageManager::find_install()) {
        install = *found;
    } else {
        std::println(stderr, "Marathon install not found; pass --install DIR");
        return 2;
    }
    if (args.empty()) {
        std::println(stderr, "usage: nimcli [--install DIR] pkgs|ls PKGID|types|read TAG [OUT]|verify [PKGID]");
        return 2;
    }
    try {
        PackageManager pm(install);
        const std::string& cmd = args[0];
        if (cmd == "pkgs") return cmd_pkgs(pm);
        if (cmd == "ls" && args.size() >= 2) return cmd_ls(pm, uint16_t(parse_hex(args[1])));
        if (cmd == "types") return cmd_types(pm);
        if (cmd == "read" && args.size() >= 2)
            return cmd_read(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "grep" && args.size() >= 2) return cmd_grep(pm, args[1], args.size() >= 3 ? std::stoi(args[2]) : -1);
        if (cmd == "strings")
            return cmd_strings(pm, args.size() >= 2 && args[1] != "-" ? args[1].c_str() : nullptr,
                               args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "dialogue") return cmd_dialogue(pm, args.size() >= 2 ? args[1].c_str() : nullptr);
        if (cmd == "xref-strings") return cmd_xref_strings(pm);
        if (cmd == "static" && args.size() >= 2) return cmd_static(pm, TagHash(parse_hex(args[1])));
        if (cmd == "shader" && args.size() >= 2)
            return cmd_shader(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "technique" && args.size() >= 2) return cmd_technique(pm, TagHash(parse_hex(args[1])));
        if (cmd == "find" && args.size() >= 2) return cmd_find(pm, parse_hex(args[1]));
        if (cmd == "lookup" && args.size() >= 2) return cmd_lookup(pm, {args.begin() + 1, args.end()});
        if (cmd == "h64") return cmd_h64(pm, {args.begin() + 1, args.end()});
        if (cmd == "xref-tags" && args.size() >= 2) return cmd_xref_tags(pm, args[1]);
        if (cmd == "xref-values" && args.size() >= 3)
            return cmd_xref_values(pm, parse_hex(args[1]), parse_hex(args[2]));
        if (cmd == "verify") return cmd_verify(pm, args.size() >= 2 ? int(parse_hex(args[1])) : -1);
        std::println(stderr, "unknown command {}", cmd);
        return 2;
    } catch (const std::exception& ex) {
        std::println(stderr, "error: {}", ex.what());
        return 1;
    }
}
