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
//   nimcli [--install DIR] render-resources [TAG]      original pipeline/scope resource sources (TSV)
//   nimcli [--install DIR] technique TAG               technique stages, textures (with headers), constants
//   nimcli [--install DIR] find CLASS                   list every tag of a class
//   nimcli [--install DIR] lookup HEX...                what a 32-bit value is: TagHash, string hash, audio event id
//   nimcli [--install DIR] fonts [OUT_DIR]               the game's UI fonts (names; optionally write the files)
//   nimcli [--install DIR] h64 [HEX64...]               hash64 table summary by target class, or resolve values
//   nimcli [--install DIR] components [CLASS]           component tags by resource class (or those holding CLASS)
//   nimcli [--install DIR] skeleton COMPONENT           bones of a skeleton component (80809FB7)
//   nimcli [--install DIR] skeletons                    skinned models -> their entity's skeleton and rig, clip counts
//   nimcli [--install DIR] rig COMPONENT                animation rig (8080AD7E): rig -> skeleton bones, spaces
//   nimcli [--install DIR] clip TAG [FRAME]             decode an animation clip (8080AE01), print one frame
//   nimcli [--install DIR] clips [BONES]                decode every clip (or those with BONES bones)
//   nimcli [--install DIR] clipfit MODEL                pose the model's skeleton with every clip of its rig
//   nimcli [--install DIR] skin MODEL                   skin weight summary of a dynamic model
//   nimcli [--install DIR] xref-values CLASS OFFSET     which classes contain the u32 found at OFFSET of CLASS tags
//                                                       (also reports how many of those also hold string hashes)
#include "nim/content/animation.hpp"
#include "nim/content/dialogue.hpp"
#include "nim/content/fonts.hpp"
#include "nim/content/map.hpp"
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

// Breadth-first walk of the tags a tag references (u32 TagHashes at 4-byte alignment, u64 hash64s
// at 8-byte alignment), skipping leaf asset classes. For reverse-engineering map structures.
int cmd_crawl(PackageManager& pm, TagHash root, int max_depth, uint32_t target = 0) {
    pm.build_hash64_index();
    const std::set<uint32_t> leaves = {kClassStaticMesh};
    std::map<uint32_t, std::pair<int, size_t>> seen;  // tag -> depth, order
    std::map<uint32_t, uint32_t> parent;
    std::vector<std::pair<TagHash, int>> queue{{root, 0}};
    std::map<uint32_t, size_t> hist;
    seen[root.value] = {0, 0};
    for (size_t qi = 0; qi < queue.size() && queue.size() < 200000; ++qi) {
        auto [t, d] = queue[qi];
        const Entry* e = pm.entry(t);
        if (!e) continue;
        hist[e->reference]++;
        if (!target && d <= 3) std::println("{:{}}{} class {:08X} size {}", "", d * 2, t.str(), e->reference, e->file_size);
        if (target && e->reference == target) {
            std::string chain;
            for (uint32_t c = t.value; ; c = parent[c]) {
                const Entry* ce = pm.entry(TagHash(c));
                chain = std::format("{}({:08X}) ", TagHash(c).str(), ce ? ce->reference : 0) + chain;
                if (!parent.count(c)) break;
            }
            std::println("{}", chain);
        }
        if (d >= max_depth || (e->type != 8 && e->type != 16) || leaves.count(e->reference)) continue;
        std::vector<uint8_t> data;
        try { data = pm.read(t); } catch (...) { continue; }
        auto visit = [&](TagHash c) {
            const Entry* ce = pm.entry(c);
            if (!ce || (ce->type != 8 && ce->type != 16) || seen.count(c.value)) return;
            seen[c.value] = {d + 1, queue.size()};
            parent[c.value] = t.value;
            queue.push_back({c, d + 1});
        };
        for (size_t o = 8; o + 4 <= data.size(); o += 4) {
            uint32_t v; std::memcpy(&v, data.data() + o, 4);
            if (v >= 0x80800000u && v != 0xFFFFFFFFu) visit(TagHash(v));
            if ((o & 7) == 0 && o + 8 <= data.size()) {
                uint64_t w; std::memcpy(&w, data.data() + o, 8);
                if (w >> 63) if (auto r = pm.resolve64(w)) visit(*r);
            }
        }
    }
    std::println("{} tags reached", queue.size());
    for (auto& [c, n] : hist) std::println("  class {:08X}  {}", c, n);
    return 0;
}

int cmd_maps(PackageManager& pm) {
    for (const auto& m : list_maps(pm)) {
        auto l = load_map_layout(pm, m.bubble);
        std::println("{}  {:<32} statics {:>6}  instances {:>7}  bounds ({:.0f} {:.0f} {:.0f})-({:.0f} {:.0f} {:.0f})", m.bubble.str(),
                     m.package, l.statics.size(), l.instances, l.lo[0], l.lo[1], l.lo[2], l.hi[0], l.hi[1], l.hi[2]);
    }
    return 0;
}

// Tags that name `tag` by one of its hash64s.
int cmd_refs64(PackageManager& pm, TagHash tag) {
    std::set<uint64_t> wanted;
    for (const auto& info : pm.packages())
        for (const auto& h : pm.open(info).hash64s())
            if (h.hash32 == tag) wanted.insert(h.hash64);
    for (uint64_t w : wanted) std::println("hash64 {:016X}", w);
    size_t n = 0;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 && e.type != 16) continue;
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            for (size_t o = 0; o + 8 <= d.size(); o += 4) {
                uint64_t v; std::memcpy(&v, d.data() + o, 8);
                if (!wanted.count(v)) continue;
                std::println("{}  class {:08X}  +0x{:X}  size {}  {}", TagHash::make(info.pkg_id, uint16_t(i)).str(), e.reference, o, d.size(), info.name);
                n++;
            }
        }
    }
    std::println("{} references", n);
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

// Component tags (8080BADB) by the classes of their two resources (0x10, 0x18).
int cmd_components(PackageManager& pm, uint32_t want) {
    std::map<std::pair<uint32_t, uint32_t>, size_t> hist;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassComponent) continue;
            const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
            std::vector<uint8_t> d;
            try { d = pm.read(t); } catch (...) { continue; }
            BinaryReader r(d);
            auto cls = [&](size_t o) -> uint32_t {
                const uint64_t v = r.u64_at(o);
                return v && o + v >= 4 && o + v < d.size() ? r.u32_at(o + size_t(v) - 4) : 0;
            };
            const uint32_t a = cls(0x10), b = cls(0x18);
            hist[{a, b}]++;
            if (want && (a == want || b == want)) std::println("{}  {:08X} {:08X}  size {}", t.str(), a, b, d.size());
        }
    }
    if (!want)
        for (const auto& [k, n] : hist) std::println("{:08X} {:08X}  {}", k.first, k.second, n);
    return 0;
}

// ---- skeletons / animation -----------------------------------------------------------------------
int cmd_skeleton(PackageManager& pm, TagHash tag) {
    Skeleton sk = load_skeleton(pm, tag);
    std::println("skeleton {} bones {}", tag.str(), sk.size());
    for (size_t i = 0; i < sk.size(); ++i) {
        const auto& b = sk.bind[i];
        const auto& l = sk.local_bind[i];
        std::println("{:3} {:08X} parent {:3}  object pos ({:8.4f},{:8.4f},{:8.4f}) rot ({:6.3f},{:6.3f},{:6.3f},{:6.3f}) scale {:.3f}"
                     "  local pos ({:7.4f},{:7.4f},{:7.4f})",
                     i, sk.hashes[i], sk.parents[i], b.translation[0], b.translation[1], b.translation[2], b.rotation[0],
                     b.rotation[1], b.rotation[2], b.rotation[3], b.scale, l.translation[0], l.translation[1], l.translation[2]);
    }
    return 0;
}

// Skinned models with the skeleton and rig of their entity, and how many clips have the rig's bone count.
int cmd_skeletons(PackageManager& pm) {
    auto materials = index_dynamic_model_materials(pm);
    auto skeletons = index_model_skeletons(pm, materials);
    std::map<uint32_t, size_t> clips_by_bones;
    for (const auto& c : index_animation_clips(pm)) clips_by_bones[c.bones]++;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> by_skeleton;
    for (const auto& [model, mr] : skeletons) by_skeleton[{mr.skeleton.value, mr.rig.value}].push_back(model);
    for (auto& [key, models] : by_skeleton) {
        size_t bones = 0, rig_bones = 0;
        try { bones = load_skeleton(pm, TagHash(key.first)).size(); } catch (...) {}
        try { if (!TagHash(key.second).is_none()) rig_bones = load_animation_rig(pm, TagHash(key.second)).size(); } catch (...) {}
        std::sort(models.begin(), models.end());
        std::print("{}  bones {:3}  rig {} ({:3})  clips {:4}  models", TagHash(key.first).str(), bones, TagHash(key.second).str(),
                   rig_bones, rig_bones ? clips_by_bones[uint32_t(rig_bones)] : 0);
        for (uint32_t m : models) std::print(" {}", TagHash(m).str());
        std::println("");
    }
    std::println("{} models, {} skeletons", skeletons.size(), by_skeleton.size());
    return 0;
}

int cmd_clip(PackageManager& pm, TagHash tag, int frame) {
    AnimationClip c = load_animation_clip(pm, tag);
    std::println("clip {} codec {} frames {} bones {} name {:08X}  tracks: rotation {} translation {} scale {}", tag.str(),
                 clip_codec_name(c.codec), c.frame_count, c.bone_count, c.name_hash, c.rotations.size(),
                 c.translations.size(), c.scales.size());
    auto at = [&](size_t n) { return std::min<size_t>(size_t(std::max(frame, 0)), n - 1); };
    for (const auto& t : c.rotations) {
        const auto& q = t.frames[at(t.frames.size())];
        std::println("  rot   bone {:3} {:3} frames  ({:7.4f},{:7.4f},{:7.4f},{:7.4f})", t.bone, t.frames.size(), q[0], q[1], q[2], q[3]);
    }
    for (const auto& t : c.translations) {
        const auto& p = t.frames[at(t.frames.size())];
        std::println("  pos   bone {:3} {:3} frames  ({:8.4f},{:8.4f},{:8.4f})", t.bone, t.frames.size(), p[0], p[1], p[2]);
    }
    for (const auto& t : c.scales) std::println("  scale bone {:3} {:3} frames  {:.4f}", t.bone, t.frames.size(), t.frames[at(t.frames.size())]);
    return 0;
}

// Decodes every clip; codec histogram and failures.
int cmd_clips(PackageManager& pm, uint32_t bones) {
    std::map<std::string, size_t> codecs;
    size_t failed = 0;
    for (const auto& info : index_animation_clips(pm)) {
        if (bones && info.bones != bones) continue;
        try {
            AnimationClip c = load_animation_clip(pm, info.tag);
            codecs[clip_codec_name(c.codec)]++;
            if (bones) std::println("{}  {:7} frames {:4} tracks {}/{}/{}", info.tag.str(), clip_codec_name(c.codec), c.frame_count,
                                    c.rotations.size(), c.translations.size(), c.scales.size());
        } catch (const std::exception& ex) {
            std::println("{}  error: {}", info.tag.str(), ex.what());
            ++failed;
        }
    }
    for (const auto& [k, n] : codecs) std::println("{:8} {}", k, n);
    std::println("{} failed", failed);
    return failed ? 1 : 0;
}

// Skin weights of a dynamic model's meshes.
int cmd_skin(PackageManager& pm, TagHash tag) {
    DynamicModel m = load_dynamic_model(pm, tag);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const auto& d = m.meshes[i];
        if (d.skinning.is_none()) {
            std::println(" mesh {} rigid", i);
            continue;
        }
        VertexBuffer vb0 = load_vertex_buffer(pm, d.vertex0), sk = load_vertex_buffer(pm, d.skinning);
        SkinWeights w = decode_skin_weights(vb0, sk);
        size_t single = 0, two = 0, four = 0;
        for (size_t v = 0; v < w.bones.size(); ++v) {
            int16_t pw;
            std::memcpy(&pw, vb0.data.data() + v * vb0.stride + 6, 2);
            (std::abs(int(pw)) < 0x800 ? single : pw < 0 ? four : two)++;
        }
        std::println(" mesh {} vertices {} (single {}, two {}, four {}) skin buffer {} B stride {}  max bone {}", i, w.bones.size(),
                     single, two, four, sk.data.size(), sk.stride, w.max_bone);
    }
    return 0;
}

int cmd_rig(PackageManager& pm, TagHash tag) {
    AnimationRig rig = load_animation_rig(pm, tag);
    std::println("rig {} bones {}", tag.str(), rig.size());
    for (size_t i = 0; i < rig.size(); ++i) {
        const auto& t = rig.default_pose[i];
        std::println("{:3} -> skeleton {:3}  {:6}  rot ({:6.3f},{:6.3f},{:6.3f},{:6.3f}) pos ({:7.4f},{:7.4f},{:7.4f})", i,
                     rig.skeleton_bone[i], rig.object_space[i] ? "object" : "local", t.rotation[0], t.rotation[1], t.rotation[2],
                     t.rotation[3], t.translation[0], t.translation[1], t.translation[2]);
    }
    return 0;
}

// Poses a model's skeleton with every clip of its rig's bone count and reports, per codec, how
// far bones move from the bind pose and how low the lowest bone goes (a wrong decoder or rig
// mapping shows up as large displacements and bones far below the ground).
int cmd_clipfit(PackageManager& pm, TagHash model) {
    auto skeletons = index_model_skeletons(pm, index_dynamic_model_materials(pm));
    auto f = skeletons.find(model.value);
    if (f == skeletons.end() || f->second.rig.is_none()) {
        std::println("{}: no skeleton with a rig", model.str());
        return 1;
    }
    Skeleton sk = load_skeleton(pm, f->second.skeleton);
    AnimationRig rig = load_animation_rig(pm, f->second.rig);
    std::println("model {} skeleton {} ({} bones) rig {} ({} bones)", model.str(), sk.component.str(), sk.size(), rig.component.str(),
                 rig.size());
    std::map<std::string, std::vector<float>> moved, lowest, to_default;
    size_t additive = 0;
    std::vector<BoneTransform> object;
    for (const auto& info : index_animation_clips(pm)) {
        if (info.bones != rig.size()) continue;
        AnimationClip c;
        try { c = load_animation_clip(pm, info.tag); } catch (...) { continue; }
        float sum = 0, low = 1e9f;
        size_t count = 0;
        std::vector<float> ang;
        for (const auto& t : c.rotations) {
            if (t.bone >= rig.size() || t.frames.size() < 2) continue;
            const auto& q = t.frames[0];
            const auto& d = rig.default_pose[t.bone].rotation;
            ang.push_back(2 * std::acos(std::min(1.0f, std::fabs(q[0] * d[0] + q[1] * d[1] + q[2] * d[2] + q[3] * d[3]))) * 57.29578f);
        }
        if (!ang.empty()) {
            std::sort(ang.begin(), ang.end());
            to_default[clip_codec_name(c.codec)].push_back(ang[ang.size() / 2]);
            if (is_additive(rig, c)) to_default["additive"].push_back(ang[ang.size() / 2]);
        }
        for (uint32_t fr = 0; fr < c.frame_count; fr += std::max(1u, c.frame_count / 8)) {
            pose_skeleton(sk, rig, c, float(fr), object);
            for (size_t b = 0; b < sk.size(); ++b) {
                const auto& p = object[b].translation;
                const auto& q = sk.bind[b].translation;
                sum += std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
                low = std::min(low, p[2]);
                ++count;
            }
        }
        if (is_additive(rig, c)) ++additive;
        moved[clip_codec_name(c.codec)].push_back(sum / float(std::max<size_t>(1, count)));
        lowest[clip_codec_name(c.codec)].push_back(low);
    }
    auto pct = [](std::vector<float>& v, int p) { return v.empty() ? 0.0f : v[v.size() * size_t(p) / 100]; };
    std::println("additive clips {}", additive);
    for (auto& [k, a] : to_default) {
        if (moved.count(k)) continue;
        std::sort(a.begin(), a.end());
        std::println("{:12} clips {:4}  vs rig default p25 {:.1f} p50 {:.1f} p90 {:.1f}", k, a.size(), pct(a, 25), pct(a, 50), pct(a, 90));
    }
    for (auto& [k, v] : moved) {
        auto& l = lowest[k];
        std::sort(v.begin(), v.end());
        std::sort(l.begin(), l.end());
        std::println("{:8} clips {:4}  mean bone displacement p25 {:.3f} p50 {:.3f} p90 {:.3f}   lowest bone z p10 {:.3f} p50 {:.3f}", k,
                     v.size(), pct(v, 25), pct(v, 50), pct(v, 90), pct(l, 10), pct(l, 50));
        auto& a = to_default[k];
        std::sort(a.begin(), a.end());
        std::println("{:8}             animated rotations vs rig default pose, median angle p25 {:.1f} p50 {:.1f} p90 {:.1f} deg", "", pct(a, 25),
                     pct(a, 50), pct(a, 90));
    }
    return 0;
}

const char* kStageNames[] = {"VS", "HS", "DS", "GS", "PS", "CS"};

// Tags of class `cls` (type 8) whose bytes hold `tag` at a 4-byte aligned offset.
int cmd_refs(PackageManager& pm, TagHash tag, uint32_t cls) {
    size_t n = 0;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || (cls && e.reference != cls)) continue;
            const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
            std::vector<uint8_t> d;
            try { d = pm.read(t); } catch (...) { continue; }
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                if (v == tag.value) {
                    std::println("{}  class {:08X}  +0x{:X}  size {}", t.str(), e.reference, o, d.size());
                    ++n;
                }
            }
        }
    }
    std::println("{} references", n);
    return 0;
}

int cmd_dynamic(PackageManager& pm, TagHash tag) {
    pm.build_hash64_index();
    DynamicModel m = load_dynamic_model(pm, tag);
    std::println("dynamic {} scale ({:.4f},{:.4f},{:.4f},{:.4f}) offset ({:.4f},{:.4f},{:.4f},{:.4f}) uv ({:.3f},{:.3f})+({:.3f},{:.3f})",
                 m.tag.str(), m.model_scale[0], m.model_scale[1], m.model_scale[2], m.model_scale[3], m.model_offset[0],
                 m.model_offset[1], m.model_offset[2], m.model_offset[3], m.texcoord_scale[0], m.texcoord_scale[1],
                 m.texcoord_offset[0], m.texcoord_offset[1]);
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        const auto& d = m.meshes[i];
        std::println(" mesh {} vb0 {} vb1 {} ib {} color {} skin {} parts {} gbuffer [{}, {}) layout {}", i, d.vertex0.str(),
                     d.vertex1.str(), d.index.str(), d.color.str(), d.skinning.str(), d.parts.size(), d.part_range[0],
                     d.part_range[1], d.input_layout[0]);
        for (size_t k = 0; k < d.parts.size(); ++k) {
            const auto& p = d.parts[k];
            std::string stages;
            for (size_t s = 0; s < kRenderStageCount; ++s)
                if (k >= d.part_range[s] && k < d.part_range[s + 1]) stages += std::format("{} ", s);
            std::println("   part {:2} tech {} variant {:5} prim {} idx {}+{} id {} lod {:02X} stages {}", k, p.technique.str(),
                         p.variant_shader_index, p.primitive_type, p.index_start, p.index_count, p.external_identifier,
                         p.lod_category, stages);
        }
    }
    auto layouts = load_input_layouts(pm);
    std::set<int> used;
    for (const auto& d : m.meshes) used.insert(d.input_layout[0]);
    for (int l : used) {
        std::print("  layout {}:", l);
        for (const auto& e : layouts[l])
            std::print(" [{}]{}{} fmt{} ({}B){}", e.buffer, e.semantic, e.semantic_index, e.dxgi_format, e.size,
                       e.instanced ? " inst" : "");
        std::println("");
    }
    return 0;
}

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

// Writes every entry of one type (e.g. 41 = shader bytecode) in a package to OUT_DIR/TAG.bin.
int cmd_extract(PackageManager& pm, uint16_t pkg_id, int type, const std::string& out_dir) {
    std::filesystem::create_directories(out_dir);
    Package& p = pm.open(pkg_id);
    size_t n = 0;
    for (uint32_t i = 0; i < p.entries().size(); ++i) {
        const Entry& e = p.entries()[i];
        if (e.type != type) continue;
        auto bytes = p.read_entry(i);
        TagHash tag = TagHash::make(pkg_id, uint16_t(i));
        std::ofstream(std::filesystem::path(out_dir) / std::format("{}_{}.bin", tag.str(), e.subtype), std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        ++n;
    }
    std::println("wrote {} entries of type {} to {}", n, type, out_dir);
    return 0;
}

int cmd_globals(PackageManager& pm) {
    pm.build_hash64_index();
    RenderGlobals g = load_render_globals(pm);
    std::println("render globals {}: {} scopes, {} pipelines, {} global channels", g.tag.str(), g.scopes.size(),
                 g.pipelines.size(), g.channel_ids.size());
    std::println("lookup textures: tint {} lobe {} lobe3d {} iridescence {}", g.specular_tint_lookup.str(),
                 g.specular_lobe_lookup.str(), g.specular_lobe_3d_lookup.str(), g.iridescence_lookup.str());
    for (const auto& [n, t] : g.scopes) {
        Scope s = load_scope(pm, t);
        std::string slots;
        for (size_t i = 0; i < s.stages.size(); ++i)
            if (s.stages[i].cbuffer_slot >= 0)
                slots += std::format(" {}:cb{}({}r,{}B)", "PVGHCD"[i], s.stages[i].cbuffer_slot,
                                     s.stages[i].cbuffer_init.size(), s.stages[i].bytecode.size());
        std::println("  scope    {:<40} {}{}", n, t.str(), slots);
    }
    for (const auto& [n, t] : g.pipelines) std::println("  pipeline {:<40} {}", n, t.str());
    for (size_t i = 0; i < g.channel_ids.size(); ++i) {
        auto v = i < g.channel_defaults.size() ? g.channel_defaults[i] : std::array<float, 4>{};
        std::println("  channel {:3} {:08X} = ({}, {}, {}, {})", i, g.channel_ids[i], v[0], v[1], v[2], v[3]);
    }
    return 0;
}

// Disassembles a scope's stages (the bytecode that builds a shared cbuffer from externs).
int cmd_fonts(PackageManager& pm, const char* out_dir) {
    auto fonts = load_game_fonts(pm);
    for (const auto& f : fonts) {
        std::println("{}  {:>8} B  {} / {} ({})", f.tag.str(), f.data.size(), f.family, f.subfamily, f.postscript);
        if (out_dir) {
            fs::create_directories(out_dir);
            std::ofstream(fs::path(out_dir) / (f.postscript.empty() ? f.tag.str() : f.postscript) += ".otf",
                          std::ios::binary)
                .write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        }
    }
    std::println("{} fonts", fonts.size());
    return 0;
}

int cmd_render_resources(PackageManager& pm, std::optional<TagHash> requested) {
    pm.build_hash64_index();
    const auto globals = load_render_globals(pm);
    const auto pipelines = requested ? std::vector<std::pair<std::string, TagHash>>{{"requested", *requested}} : globals.pipelines;
    std::map<uint32_t, std::vector<ShaderBinding>> binding_cache;
    std::map<uint32_t, std::vector<ShaderInput>> output_cache;
    size_t failures = 0;
    std::println("pipeline\ttechnique\tstage\tshader\tkind\tslot\tsource");
    for (const auto& [name, tag] : pipelines) {
        try {
            if (tag.is_none()) {
                std::println("{}\t{}\t-\t-\tunassigned\t-\tNo technique assigned by this game build", name, tag.str());
                continue;
            }
            const auto technique = load_technique(pm, tag);
            auto scopes = load_technique_scopes(pm, globals, technique);
            std::vector<const Scope*> scope_ptrs;
            for (const auto& scope : scopes) scope_ptrs.push_back(&scope);
            for (size_t i = 0; i < size_t(ShaderStage::Count); ++i) {
                const auto stage = ShaderStage(i);
                const auto& core = technique.stage(stage);
                if (core.shader.is_none()) continue;
                auto row = [&](std::string_view kind, uint32_t slot, const std::string& source) {
                    std::println("{}\t{}\t{}\t{}\t{}\t{}\t{}", name, tag.str(), "VHDGPC"[i], core.shader.str(), kind, slot, source);
                };
                const auto resources = resolve_stage_resources(technique, stage, scope_ptrs);
                if (!binding_cache.contains(core.shader.value)) {
                    const auto code = load_shader_bytecode(pm, core.shader);
                    binding_cache[core.shader.value] = shader_bindings(code);
                    output_cache[core.shader.value] = shader_outputs(code);
                }
                for (const auto& binding : binding_cache.at(core.shader.value)) {
                    if (binding.upper - binding.lower > 4096) throw std::runtime_error("Unbounded shader binding range requires explicit handling");
                    for (uint32_t slot = binding.lower; slot <= binding.upper; ++slot) {
                        std::string source = "unresolved";
                        switch (binding.type) {
                        case BindingType::Sampler:
                            if (auto it = resources.samplers.find(slot); it != resources.samplers.end()) source = "tag:" + it->second.str();
                            row("sampler", slot, source);
                            break;
                        case BindingType::Cbv:
                            if (int32_t(slot) == core.cbuffer_slot) source = "material:" + tag.str();
                            else if (auto it = resources.cbuffer_scopes.find(slot); it != resources.cbuffer_scopes.end()) source = "scope:" + it->second.str();
                            row("cbuffer", slot, source);
                            break;
                        case BindingType::Srv: case BindingType::Uav: {
                            const auto& external = binding.type == BindingType::Srv ? resources.extern_textures : resources.extern_uavs;
                            if (binding.type == BindingType::Srv)
                                if (auto it = resources.textures.find(slot); it != resources.textures.end()) source = "tag:" + it->second.str();
                            if (auto it = external.find(slot); it != external.end())
                                source = std::format("extern:{}:0x{:X}:kind{}", it->second.extern_id, uint32_t(it->second.offset) * 8, int(binding.kind));
                            row(binding.type == BindingType::Srv ? "srv" : "uav", slot, source);
                            break;
                        }
                        default: break;
                        }
                    }
                }
                for (const auto& output : output_cache.at(core.shader.value)) row("output", output.index, output.semantic);
            }
        } catch (const std::exception& ex) {
            ++failures;
            std::println("{}\t{}\t-\t-\terror\t-\t{}", name, tag.str(), ex.what());
        }
    }
    std::println(stderr, "Original game resource index: {} techniques, {} shaders, {} errors. Extern producers and execution order are not inferred.",
                 pipelines.size(), binding_cache.size(), failures);
    return failures ? 1 : 0;
}

int cmd_scope(PackageManager& pm, TagHash tag) {
    pm.build_hash64_index();
    Scope s = load_scope(pm, tag);
    std::println("scope {} \"{}\"", tag.str(), s.name);
    for (size_t i = 0; i < s.stages.size(); ++i) {
        const auto& st = s.stages[i];
        if (st.cbuffer_slot < 0 && st.bytecode.empty() && st.textures.empty()) continue;
        std::println(" stage {} cbuffer slot {} regs {} constants {} textures {} samplers {}", "PVGHCD"[i],
                     st.cbuffer_slot, st.cbuffer_init.size(), st.constants.size(), st.textures.size(), st.samplers.size());
        for (size_t reg = 0; reg < st.cbuffer_init.size(); ++reg) {
            const auto& v = st.cbuffer_init[reg];
            std::println("   init[{}] = ({:.9g}, {:.9g}, {:.9g}, {:.9g})", reg, v[0], v[1], v[2], v[3]);
        }
        for (const auto& tex : st.textures) std::println("   texture t{} {}", tex.slot, tex.texture.str());
        for (size_t n = 0; n < st.samplers.size(); ++n) std::println("   sampler[{}] {}", n, st.samplers[n].str());
        std::print("{}", disassemble_tfx(st.bytecode));
    }
    return 0;
}

int cmd_technique(PackageManager& pm, TagHash tag) {
    pm.build_hash64_index();
    Technique t = load_technique(pm, tag);
    std::println("technique {} bind mode {} scopes {:016X} states {:08X}", t.tag.str(), t.bind_mode, t.used_scopes, t.states);
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
            for (size_t i = 0; i < st.cbuffer_init.size(); ++i)
                if (st.cbuffer_init[i] != float4{})
                    std::println("   init[{}] = ({:.4f}, {:.4f}, {:.4f}, {:.4f})", i, st.cbuffer_init[i][0],
                                 st.cbuffer_init[i][1], st.cbuffer_init[i][2], st.cbuffer_init[i][3]);
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
        if (cmd == "refs" && args.size() >= 2)
            return cmd_refs(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? parse_hex(args[2]) : 0);
        if (cmd == "dynamic" && args.size() >= 2) return cmd_dynamic(pm, TagHash(parse_hex(args[1])));
        if (cmd == "static" && args.size() >= 2) return cmd_static(pm, TagHash(parse_hex(args[1])));
        if (cmd == "shader" && args.size() >= 2)
            return cmd_shader(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "extract" && args.size() >= 4)
            return cmd_extract(pm, uint16_t(parse_hex(args[1])), std::stoi(args[2]), args[3]);
        if (cmd == "globals") return cmd_globals(pm);
        if (cmd == "fonts") return cmd_fonts(pm, args.size() >= 2 ? args[1].c_str() : nullptr);
        if (cmd == "render-resources") return cmd_render_resources(pm, args.size() >= 2 ? std::optional{TagHash(parse_hex(args[1]))} : std::nullopt);
        if (cmd == "scope" && args.size() >= 2) return cmd_scope(pm, TagHash(parse_hex(args[1])));
        if (cmd == "technique" && args.size() >= 2) return cmd_technique(pm, TagHash(parse_hex(args[1])));
        if (cmd == "skeleton" && args.size() >= 2) return cmd_skeleton(pm, TagHash(parse_hex(args[1])));
        if (cmd == "clipfit" && args.size() >= 2) return cmd_clipfit(pm, TagHash(parse_hex(args[1])));
        if (cmd == "rig" && args.size() >= 2) return cmd_rig(pm, TagHash(parse_hex(args[1])));
        if (cmd == "skeletons") return cmd_skeletons(pm);
        if (cmd == "clip" && args.size() >= 2) return cmd_clip(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? std::stoi(args[2]) : 0);
        if (cmd == "clips") return cmd_clips(pm, args.size() >= 2 ? uint32_t(std::stoul(args[1])) : 0);
        if (cmd == "skin" && args.size() >= 2) return cmd_skin(pm, TagHash(parse_hex(args[1])));
        if (cmd == "components") return cmd_components(pm, args.size() >= 2 ? parse_hex(args[1]) : 0);
        if (cmd == "crawl" && args.size() >= 2)
            return cmd_crawl(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? std::stoi(args[2]) : 8, args.size() >= 4 ? parse_hex(args[3]) : 0);
        if (cmd == "maps") return cmd_maps(pm);
        if (cmd == "refs64" && args.size() >= 2) return cmd_refs64(pm, TagHash(parse_hex(args[1])));
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
