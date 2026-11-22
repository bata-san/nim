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
//   nimcli [--install DIR] wem-audit [LIMIT]          decode every dialogue stream (NIM_LANGUAGE=6: Japanese)
//   nimcli [--install DIR] movies [-v]                every USM movie: name, size, video/audio streams
//   nimcli [--install DIR] movie-audio USM OUT.wav [CH]  decode a movie's audio track (ADX)
//   nimcli [--install DIR] movie-frames USM OUT_DIR [START_S] [COUNT]  decode frames (BMP) and list subtitles
//   nimcli [--install DIR] wem TAG|EVENT OUT.wav      decode a Wwise stream (or a dialogue audio event)
//   nimcli [--install DIR] xref-strings                which tag classes reference string hashes
//   nimcli [--install DIR] xref-tags T/S|class:HASH    which tag classes reference tags of that kind
//   nimcli [--install DIR] static TAG                  static mesh: groups, techniques, shaders, layouts
//   nimcli [--install DIR] shader TAG [OUT.asm]         disassemble a shader header's DXBC
//   nimcli [--install DIR] render-resources [TAG]      original pipeline/scope resource sources (TSV)
//   nimcli [--install DIR] technique TAG               technique stages, textures (with headers), constants
//   nimcli [--install DIR] decorators TAG             audit a decorator placement group or map bubble
//   nimcli [--install DIR] find CLASS                   list every tag of a class
//   nimcli [--install DIR] probe-audit BUBBLE [OUT.tsv]  exhaustive map-family probe inventory and cross-check
//   nimcli [--install DIR] environment BUBBLE [ACTIVITY] [-v]  every global channel expression, by entity
//   nimcli [--install DIR] probe-search OUT_DIR          install-wide texture/reference/layout/channel census
//   nimcli [--install DIR] lookup HEX...                what a 32-bit value is: TagHash, string hash, audio event id
//   nimcli [--install DIR] fonts [OUT_DIR]               the game's UI fonts (names; optionally write the files)
//   nimcli [--install DIR] h64 [HEX64...]               hash64 table summary by target class, or resolve values
//   nimcli [--install DIR] components [CLASS]           component tags by resource class (or those holding CLASS)
//   nimcli [--install DIR] skeleton COMPONENT           bones of a skeleton component (80809FB7)
//   nimcli [--install DIR] skeletons                    skinned models -> their entity's skeleton and rig, clip counts
//   nimcli [--install DIR] rig COMPONENT                animation rig (8080AD7E): rig -> skeleton bones, spaces
//   nimcli [--install DIR] clip TAG [FRAME]             decode an animation clip (8080AE01), print one frame
//   nimcli [--install DIR] clips [BONES]                decode every clip (or those with BONES bones)
//   nimcli [--install DIR] animaudit                    skinned models without skeleton / rig / clips; rigs whose clips stretch bones
//   nimcli [--install DIR] clipfit MODEL [-v]           pose the model's skeleton with every clip of its rig (-v: per clip)
//   nimcli [--install DIR] skin MODEL                   skin weight summary of a dynamic model
//   nimcli [--install DIR] survey-entities               prepare every ENTITIES entry and histogram outcomes
//   nimcli [--install DIR] xref-values CLASS OFFSET     which classes contain the u32 found at OFFSET of CLASS tags
//                                                       (also reports how many of those also hold string hashes)
#include "nim/content/animation.hpp"
#include "nim/content/dialogue.hpp"
#include "nim/content/codex.hpp"
#include "nim/content/movies.hpp"
#include "nim/content/hca.hpp"
#include "media.hpp"
#include "nim/content/audio.hpp"
#include "nim/content/fonts.hpp"
#include "nim/content/map.hpp"
#include "nim/content/weather.hpp"
#include "nim/content/strings.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/core/binary.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_reader.hpp"
#include "nim/sim/ai.hpp"
#include "nim/render/scene.hpp"

#include <algorithm>
#include <set>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <unordered_set>
#include <print>
#include <string>
#include <vector>
#include <map>

using namespace nim;
namespace fs = std::filesystem;
int cmd_probe_search(PackageManager& pm, const std::string& output);
int cmd_probe_gpu_audit(PackageManager& pm, const std::string& output);
int cmd_world_channel_names(PackageManager& pm, const std::string& output);
int cmd_environment(PackageManager& pm, TagHash bubble, TagHash activity, bool verbose);

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

// Match the ENTITIES browser: every type-8 entry whose resource class is a dynamic model.
// Preparation is the same CPU path the GUI worker uses before GPU upload.
int cmd_survey_entities(PackageManager& pm) {
    pm.build_hash64_index();
    const auto materials = index_dynamic_model_materials(pm);
    struct Failure { size_t count = 0; std::vector<TagHash> examples; };
    std::map<std::string, Failure> failures;
    size_t total = 0, drawable = 0, no_drawable = 0;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassDynamicModel) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            ++total;
            try {
                const auto it = materials.find(tag.value);
                PreparedMesh mesh = prepare_dynamic_model(pm, tag, it == materials.end() ? nullptr : &it->second);
                if (mesh.groups.empty()) {
                    ++no_drawable;
                } else {
                    ++drawable;
                    for (const auto& note : mesh.notes) {
                        auto& f = failures["partial: " + note.substr(0, 120)];
                        ++f.count;
                        if (f.examples.size() < 3) f.examples.push_back(tag);
                    }
                }
            } catch (const std::exception& ex) {
                std::string reason = ex.what();
                if (reason.size() > 160) reason.resize(160);
                auto& f = failures[reason];
                ++f.count;
                if (f.examples.size() < 5) f.examples.push_back(tag);
            } catch (...) {
                auto& f = failures["unknown non-standard exception"];
                ++f.count;
                if (f.examples.size() < 5) f.examples.push_back(tag);
            }
        }
    }
    std::println("entities {} drawable {} no-drawable {} failures {} material-components {}", total, drawable,
                 no_drawable, total - drawable - no_drawable, materials.size());
    for (const auto& [reason, f] : failures) {
        std::print("{}  {}", f.count, reason);
        for (TagHash tag : f.examples) std::print(" {}", tag.str());
        std::println("");
    }
    return 0;
}

// A texture's top mip (all slices / faces), raw, for inspection.
int cmd_texraw(PackageManager& pm, TagHash h, const char* out) {
    const Texture t = load_texture(pm, h);
    std::println("{} format {} {}x{}x{} array {} mips {} bytes {}", h.str(), t.dxgi_format, t.width, t.height, t.depth, t.array_size, t.mip_count, t.data.size());
    if (out) std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(t.data.data()), std::streamsize(t.data.size()));
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
    if (values[0] == "-r") {  // reverse: the 64-bit hashes that name these tags
        std::set<uint32_t> want;
        for (size_t i = 1; i < values.size(); ++i) want.insert(uint32_t(std::stoul(values[i], nullptr, 16)));
        for (const auto& info : pm.packages())
            for (const auto& h : pm.open(info).hash64s())
                if (want.contains(h.hash32.value)) std::println("{} <- {:016X}", h.hash32.str(), h.hash64);
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

// Codex definition components: class census, then a dump of each component of the first N entries
// with every u32 that names a tag and every u64 that resolves through the hash64 index.
int cmd_codex_probe(PackageManager& pm, size_t n) {
    std::optional<TagHash> defs_tag;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size() && !defs_tag; ++i)
            if (p.entries()[i].type == 8 && p.entries()[i].reference == kClassCodexDefinitions) defs_tag = TagHash::make(info.pkg_id, uint16_t(i));
    }
    if (!defs_tag) return 1;
    const auto bytes = pm.read(*defs_tag);
    const auto defs = parse_codex_definitions(bytes);
    TagReader r(bytes);
    pm.build_hash64_index();
    std::map<uint32_t, size_t> census;
    std::map<uint32_t, std::map<uint32_t, size_t>> by_kind;
    std::vector<size_t> offs;
    for (const auto& d : defs)
        for (const auto& c : d.components) { census[c.cls]++; by_kind[d.kind][c.cls]++; offs.push_back(c.offset); }
    std::sort(offs.begin(), offs.end());
    std::println("definitions {}  in {}", defs.size(), defs_tag->str());
    for (auto& [k, m] : by_kind) {
        std::print("kind {:08X}:", k);
        for (auto& [c, cnt] : m) std::print("  {:08X} x{}", c, cnt);
        std::println("");
    }
    {  // 80805322 icon definitions: does their key hash name codex definitions?
        std::map<uint32_t, uint32_t> icon_of;  // hash -> tag
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            for (uint32_t i = 0; i < p.entries().size(); ++i)
                if (p.entries()[i].type == 8 && p.entries()[i].reference == 0x80805322) {
                    const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
                    const auto b = pm.read(t);
                    if (b.size() >= 0x10) icon_of[TagReader(b).u32_at(8)] = t.value;
                }
        }
        std::map<uint32_t, size_t> hit_kind, all_kind;
        for (const auto& d : defs) {
            all_kind[d.kind]++;
            if (icon_of.contains(d.hash)) hit_kind[d.kind]++;
        }
        std::println("icon definitions (80805322): {}", icon_of.size());
        // 80805335 (type 16): u32 hash at +0x10, icon tag at +0x18. Find those hashes in the table.
        std::map<uint32_t, uint32_t> icon35;
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            for (uint32_t i = 0; i < p.entries().size(); ++i)
                if (p.entries()[i].reference == 0x80805335) {
                    const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
                    try {
                        const auto b = pm.read(t);
                        if (b.size() >= 0x1C) icon35[TagReader(b).u32_at(0x10)] = TagReader(b).u32_at(0x18);
                    } catch (...) {}
                }
        }
        std::map<std::pair<uint32_t, size_t>, size_t> where;  // (component class, offset in it) -> hits
        size_t hits = 0;
        for (size_t o = 0; o + 4 <= bytes.size(); o += 4)
            if (icon35.contains(r.u32_at(o))) {
                ++hits;
                auto it = std::upper_bound(offs.begin(), offs.end(), o);
                const size_t base = it == offs.begin() ? 0 : *(it - 1);
                where[{r.u32_at(base - 4), o - base}]++;
            }
        std::println("icon hashes (80805335): {}  found in codex table: {}", icon35.size(), hits);
        for (auto& [k, c] : where) std::println("  component {:08X} +{:X}: {}", k.first, k.second, c);
        // Any tag at all whose u32s include an icon hash next to a codex definition hash.
        std::set<uint32_t> def_hashes;
        for (const auto& d : defs) def_hashes.insert(d.hash);
        std::map<uint32_t, size_t> classes;
        size_t shown_t = 0;
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            for (uint32_t i = 0; i < p.entries().size(); ++i) {
                const Entry& e = p.entries()[i];
                if (e.type != 8 && e.type != 16) continue;
                const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
                std::vector<uint8_t> b;
                try { b = pm.read(t); } catch (...) { continue; }
                TagReader tr(b);
                size_t ni = 0, nd = 0;
                for (size_t o = 0; o + 4 <= b.size(); o += 4) {
                    const uint32_t v = tr.u32_at(o);
                    ni += icon35.contains(v);
                    nd += def_hashes.contains(v);
                }
                if (ni && nd && e.reference == 0x80806CAC) {
                    size_t k = 0;
                    for (size_t o = 0; o + 4 <= b.size() && k < 12; o += 4)
                        if (icon35.contains(tr.u32_at(o))) {
                            std::print("    icon @{:X}:", o);
                            for (int q = -8; q < 8; ++q) std::print(" {:08X}", tr.u32_at(o + q * 4));
                            std::println("");
                            ++k;
                        }
                }
                if (ni && nd) {
                    classes[e.reference]++;
                    if (shown_t++ < 12) std::println("  both: {} class {:08X} size {} icons {} defs {}", t.str(), e.reference, b.size(), ni, nd);
                }
            }
        }
        for (auto& [c, k] : classes) std::println("  class {:08X}: {} tags", c, k);
        for (auto& [k, c] : all_kind) std::println("  kind {:08X}: {} of {} have an icon definition", k, hit_kind[k], c);
    }
    size_t shown = 0;
    for (const auto& d : defs) {
        if (d.kind != kCodexKindEntry || shown++ >= n) continue;
        std::println("entry {:08X}", d.hash);
        for (const auto& c : d.components) {
            auto it = std::upper_bound(offs.begin(), offs.end(), c.offset);
            const size_t end = std::min({bytes.size(), it == offs.end() ? bytes.size() : *it - 4, c.offset + 0x140});
            std::println("  comp {:08X} @{:X} size {:X}", c.cls, c.offset, end - c.offset);
            for (size_t o = c.offset; o + 4 <= end; o += 4) {
                const uint32_t v = r.u32_at(o);
                std::string note;
                if (const Entry* e = pm.entry(TagHash(v)); e && v != 0 && v != 0xFFFFFFFF && (v & 0x80000000))
                    note = std::format(" tag type {} sub {} ref {:08X}", e->type, e->subtype, e->reference);
                if (o + 8 <= end && (o & 7) == 0) {
                    const uint64_t q = r.u64_at(o);
                    if (q && q != ~0ull)
                        if (auto t = pm.resolve64(q)) {
                            const Entry* e = pm.entry(*t);
                            note += std::format(" h64->{} type {} sub {} ref {:08X}", t->str(), e ? e->type : 0, e ? e->subtype : 0, e ? e->reference : 0);
                        }
                }
                if (v || !note.empty()) std::println("    +{:03X} {:08X}{}", o - c.offset, v, note);
            }
        }
    }
    return 0;
}

// The codex illustrations: how many collections and records resolve to a texture, and which.
int cmd_codex_art(PackageManager& pm) {
    auto lines = load_all_dialogue(pm);
    Codex codex = load_codex(pm, lines);
    size_t cols = 0, col_art = 0;
    std::set<uint32_t> distinct;
    for (const auto& c : codex.collections) {
        ++cols;
        if (!c.illustration.is_none()) ++col_art, distinct.insert(c.illustration.value);
        for (const auto& e : c.entries) {
            if (e.illustration.is_none()) continue;
            distinct.insert(e.illustration.value);
            const Texture t = load_texture(pm, e.illustration);
            std::println("{:08X} {}  fmt {} {}x{}", e.hash, e.illustration.str(), t.dxgi_format, t.width, t.height);
        }
    }
    std::println("collections {} with art {}  entries {} with art {}  distinct textures {}", cols, col_art, codex.entries,
                 codex.with_illustration, distinct.size());
    return 0;
}

// The in-game codex: collections and their intel entries, with the recording lines of each entry.
// With OUT, writes a Markdown dump (titles and bodies in LANG, default en).
int cmd_codex(PackageManager& pm, const char* out_path, const char* lang_code) {
    auto t0 = std::chrono::steady_clock::now();
    StringTable table;
    table.load_all(pm);
    auto lines = load_all_dialogue(pm);
    Codex codex = load_codex(pm, lines);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    size_t lang = 0;
    if (lang_code)
        for (size_t l = 0; l < kTextLanguageCount; ++l)
            if (language_code(TextLanguage(l)) == lang_code) lang = l;
    auto text = [&](uint32_t h, size_t lg) -> std::string {
        const LocalizedString* s = h ? table.find(h) : nullptr;
        return s ? s->text[lg] : std::string();
    };
    size_t titled = 0, bodied = 0, line_total = 0, wem_lines = 0;
    for (const auto& c : codex.collections)
        for (const auto& e : c.entries) {
            titled += !text(e.title, 0).empty();
            bodied += !text(e.body, 0).empty();
            line_total += e.lines.size();
            for (size_t li : e.lines) wem_lines += !lines[li].wem.empty();
        }
    std::println("codex {}  collections {}  entries {}  with title {}  with body {}  in {:.1f}s", codex.definitions.str(),
                 codex.collections.size(), codex.entries, titled, bodied, secs);
    std::println("recording assets {}  entries with audio {}  lines {}  lines with WEM {}", codex.with_asset,
                 codex.with_audio, line_total, wem_lines);
    for (size_t i = 0; i < std::min<size_t>(codex.collections.size(), 12); ++i) {
        const auto& c = codex.collections[i];
        std::println("  [{}] {} ({} entries)", text(c.title, lang), text(c.title, 1), c.entries.size());
    }
    if (out_path) {
        std::ofstream f(out_path, std::ios::binary);
        for (const auto& c : codex.collections) {
            f << "# " << text(c.title, lang) << std::format("  ({:08X})\n\n", c.hash);
            for (const auto& e : c.entries) {
                f << "## " << text(e.title, lang) << std::format("  ({:08X})\n\n", e.hash);
                if (e.unlock) f << "> " << text(e.unlock, lang) << "\n\n";
                f << text(e.body, lang) << "\n\n";
                if (!e.lines.empty()) {
                    f << std::format("recording: asset {} table {} conversation {:08X}\n\n", e.asset.str(),
                                     e.dialogue_table.str(), e.conversation);
                    for (size_t li : e.lines)
                        f << "- " << text(lines[li].speaker_hash, lang) << ": " << text(lines[li].subtitle_hash, lang)
                          << (lines[li].wem.empty() ? "" : "  [" + lines[li].wem[0].str() + "]") << "\n";
                    f << "\n";
                }
            }
        }
        std::println("wrote {}", out_path);
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
    auto text = [&](uint32_t h, size_t lang, TagHash container = {}) -> std::string {
        const LocalizedString* s = table.find(h, container);
        return s ? s->text[lang] : std::string();
    };
    size_t with_text = 0, with_speaker = 0, with_wem = 0;
    for (const auto& l : lines) {
        with_text += !text(l.subtitle_hash, 0, l.subtitle_container).empty();
        with_speaker += !text(l.speaker_hash, 0).empty();
        with_wem += !l.wem.empty();
    }
    std::println("tags scanned {}  tables {}  entries {}  lines {}  other variants {}  in {:.1f}s", st.tags_scanned,
                 st.tables, st.entries, st.lines, st.other_variants, secs);
    std::println("lines with text {}  with speaker {}  audio event resolved {}  with WEM {}", with_text,
                 with_speaker, st.audio_resolved, with_wem);
    for (size_t i = 0; i < std::min<size_t>(lines.size(), 8); ++i) {
        const auto& l = lines[i * (lines.size() / 8)];
        std::println("  [{}] {}: {}\n      {}: {}", l.table.str(), text(l.speaker_hash, 0), text(l.subtitle_hash, 0, l.subtitle_container),
                     text(l.speaker_hash, 1), text(l.subtitle_hash, 1, l.subtitle_container));
    }
    if (out_path) {
        std::ofstream f(out_path, std::ios::binary);
        f << "\xEF\xBB\xBF" << "table,line_id,speaker_en,speaker_ja,subtitle_hash";
        for (size_t lg = 0; lg < kTextLanguageCount; ++lg) f << ',' << language_code(TextLanguage(lg));
        f << ",caption_en,audio_event,wem\n";
        for (const auto& l : lines) {
            f << std::format("{},{:08X},{},{},{:08X}", l.table.str(), l.line_id, csv_field(text(l.speaker_hash, 0)),
                             csv_field(text(l.speaker_hash, 1)), l.subtitle_hash);
            for (size_t lg = 0; lg < kTextLanguageCount; ++lg) f << ',' << csv_field(text(l.subtitle_hash, lg, l.subtitle_container));
            std::string caption = text(l.caption_hash, 0, l.caption_container);
            f << ',' << csv_field(caption == text(l.subtitle_hash, 0, l.subtitle_container) ? "" : caption);
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

// The node tables of a map: entries (transform, entity by hash64, inline component list) and a
// histogram of the component resource types and entity classes.
int cmd_nodes(PackageManager& pm, TagHash bubble, uint32_t show_type) {
    const MapLayout map = load_map_layout(pm, bubble);
    std::map<uint32_t, std::pair<size_t, std::string>> components, entities;
    size_t entries = 0;
    for (TagHash t : map.node_tables) {
        const auto data = pm.read(t);
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            ++entries;
            const uint64_t h64 = r.u64_at(e + 0x30);
            const uint32_t h32 = r.u32_at(e + 0x28);
            std::optional<TagHash> ent;
            if (h32 != 0xFFFFFFFFu) ent = TagHash(h32);
            else if (h64 && h64 != ~0ull) ent = pm.resolve64(h64);
            const Entry* ee = ent ? pm.entry(*ent) : nullptr;
            auto& eh = entities[ee ? ee->reference : 0];
            if (!eh.first++) eh.second = std::format("{} #{} entity {}", t.str(), i, ent ? ent->str() : "-");
            size_t pnt = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; pnt != e + 0x78 && pnt >= 4 && pnt + 8 <= data.size() && guard < 64; ++guard) {
                const uint32_t type = r.u32_at(pnt - 4);
                auto& ch = components[type];
                if (!ch.first++) ch.second = std::format("{} #{} at 0x{:X}", t.str(), i, pnt);
                if (type == show_type) {
                    std::println("{} #{} type {:08X} pos ({:.2f} {:.2f} {:.2f}) scale {:.2f} entity {}", t.str(), i, type,
                                 r.f32_at(e + 0x10), r.f32_at(e + 0x14), r.f32_at(e + 0x18), r.f32_at(e + 0x1C), ent ? ent->str() : "-");
                    std::string hex;
                    for (size_t k = 0; k < 0x1D0 && pnt + 0x10 + k + 4 <= data.size(); k += 4)
                        hex += std::format("{}{:08X}", k % 32 ? " " : std::format("\n    +{:03X} ", k), r.u32_at(pnt + 0x10 + k));
                    std::println("{}", hex);
                }
                const uint64_t next = r.u64_at(pnt);
                if (!next) break;
                pnt += size_t(next);
            }
        }
    }
    std::println("{} node tables, {} entries", map.node_tables.size(), entries);
    std::println("component resource types:");
    for (auto& [c, v] : components) std::println("  {:08X} {:6}  e.g. {}", c, v.first, v.second);
    std::println("entity classes:");
    for (auto& [c, v] : entities) std::println("  {:08X} {:6}  e.g. {}", c, v.first, v.second);
    return 0;
}

int cmd_cubemaps(PackageManager& pm, TagHash bubble) {
    const MapLayout map = load_map_layout(pm, bubble);
    const auto cubemaps = load_map_cubemaps(pm, map);
    std::map<std::string, size_t> variants;
    for (size_t i = 0; i < cubemaps.size(); ++i) {
        const auto& m = cubemaps[i];
        variants[std::format("alpha {} probes {} relighting {}", m.use_alpha(), m.use_probes(), m.use_relighting())]++;
        std::println("#{} centre ({:.1f} {:.1f} {:.1f}) extents ({:.1f} {:.1f} {:.1f}) probes {}x{}x{} +30 {:.3g} fade {:.0f}..{:.0f} params ({:.3g} {:.3g} {:.3g} {:.3g}) flags ({:.3g} {:.3g} {:.3g} {:.3g})",
                     i, m.center[0], m.center[1], m.center[2], m.extents[0], m.extents[1], m.extents[2], m.probes[0], m.probes[1], m.probes[2],
                     m.unk30, m.fade_start, m.fade_end, m.probe_params[0], m.probe_params[1], m.probe_params[2], m.probe_params[3],
                     m.flags[0], m.flags[1], m.flags[2], m.flags[3]);
        std::println("    blend -({:.3g} {:.3g} {:.3g}) +({:.3g} {:.3g} {:.3g}) shape ({:.3g} {:.3g} {:.3g} {:.3g}) +80 ({:.3g} {:.3g} {:.3g} {:.3g})",
                     m.blend_neg[0], m.blend_neg[1], m.blend_neg[2], m.blend_pos[0], m.blend_pos[1], m.blend_pos[2],
                     m.shape[0], m.shape[1], m.shape[2], m.shape[3], m.unk80[0], m.unk80[1], m.unk80[2], m.unk80[3]);
        if (i >= 12) continue;
        auto fmt = [&](TagHash t) -> std::string {
            if (t.is_none()) return "-";
            try {
                const Texture tx = load_texture(pm, t);
                return std::format("{} {}x{}x{} a{} m{} f{}", t.str(), tx.width, tx.height, tx.depth, tx.array_size, tx.mip_count, tx.dxgi_format);
            } catch (const std::exception& ex) { return std::format("{} ({})", t.str(), ex.what()); }
        };
        std::println("    specular {}  alpha {}  diffuse {}", fmt(m.specular), fmt(m.alpha), fmt(m.diffuse));
    }
    // Does quaternion +0x100 with half extents +0x10 reproduce the matrix +0xC0 (whose translation is +0x110)?
    float worst = 0, worst_origin = 0;
    size_t moved = 0;
    for (const auto& m : cubemaps) {
        const auto& q = m.rotation;
        for (int a = 0; a < 3; ++a) {
            std::array<float, 3> v{};
            v[size_t(a)] = m.extents[size_t(a)];
            const float dot = q[0] * v[0] + q[1] * v[1] + q[2] * v[2], norm = q[3] * q[3] - q[0] * q[0] - q[1] * q[1] - q[2] * q[2];
            for (size_t k = 0; k < 3; ++k) {
                const size_t b = (k + 1) % 3, c = (k + 2) % 3;
                const float w = norm * v[k] + 2 * q[k] * dot + 2 * q[3] * (q[b] * v[c] - q[c] * v[b]);
                worst = std::max(worst, std::fabs(w - m.local_to_world[size_t(a) * 4 + k]));
            }
        }
        float d2 = 0;
        for (size_t k = 0; k < 3; ++k) {
            worst_origin = std::max(worst_origin, std::fabs(m.origin[k] - m.local_to_world[12 + k]));
            d2 += (m.origin[k] - m.center[k]) * (m.origin[k] - m.center[k]);
        }
        if (d2 > 1e-4f) ++moved;
    }
    std::println("quaternion x extents vs matrix +0xC0: max |difference| {:.3g}; +0x110 vs matrix translation: {:.3g}; {} volumes with +0x20 != +0x110", worst, worst_origin, moved);
    std::println("{} cubemap volumes", cubemaps.size());
    for (const auto& [k, n] : variants) std::println("  {:5}  {}", n, k);
    return 0;
}

// Independently enumerate all node tables and authored cubemap collections in the map family.
// Do not merge activity layers into rendering: this command records their provenance instead.
int cmd_probe_audit(PackageManager& pm, TagHash bubble, const std::string& output) {
    const auto map = load_map_layout(pm, bubble);
    const auto* home = pm.info(bubble.pkg_id());
    if (!home) throw std::runtime_error("bubble package missing");
    const auto family = home->name.substr(0, home->name.rfind('_') + 1);
    using Key = std::vector<uint8_t>;
    std::set<Key> rooted, family_nodes, collections;
    std::map<Key, std::string> origins;
    struct ManifestRow { Key key; std::string prefix, suffix; };
    std::vector<ManifestRow> manifest_rows;
    std::map<uint32_t, std::vector<uint8_t>> auxiliary;
    size_t tables = 0, records = 0, collection_count = 0;
    size_t grid_points = 0, texture_failures = 0, dimension_mismatches = 0, empty_records = 0, mesh_count = 0;
    float mesh_coordinate_max = 0;
    std::ofstream manifest;
    if (!output.empty()) {
        manifest.open(output);
        if (!manifest) throw std::runtime_error("cannot open probe manifest");
        manifest << "collection\tindex\tauthored_id\tinline_source\tcenter_x\tcenter_y\tcenter_z\tgrid_x\tgrid_y\tgrid_z\tspecular\talpha\tdiffuse\tdiffuse_width\tdiffuse_height\tdiffuse_depth\tauxiliary\tauxiliary_records\tvertex_buffer";
        for (int i = 0; i < 16; ++i) manifest << std::format("\tlocal_to_world_{}", i);
        manifest << "\torigin_x\torigin_y\torigin_z\tvertex_count\n";
    }
    auto key = [](const std::vector<uint8_t>& d, size_t c) {
        if (c > d.size() || d.size() - c < 0x1D0) throw std::runtime_error("truncated cubemap");
        // Exclude only the serialized record header, compare every authored payload byte.
        return Key(d.begin() + c + 0x10, d.begin() + c + 0x1D0);
    };
    for (const auto& info : pm.packages()) {
        if (!info.name.starts_with(family)) continue;
        auto& pkg = pm.open(info);
        for (uint32_t i = 0; i < pkg.entries().size(); ++i) {
            const auto& en = pkg.entries()[i];
            if (en.type != 8 || (en.reference != kClassMapNodeTable && en.reference != 0x80807F4B)) continue;
            const auto tag = TagHash::make(info.pkg_id, uint16_t(i));
            const auto data = pm.read(tag);
            TagReader r(data);
            if (en.reference == 0x80807F4B) {
                const auto v = r.vec_at(0x10);
                if (v.count && v.element_class != 0x80807F50) throw std::runtime_error("unknown cubemap collection layout");
                ++collection_count;
                std::println("collection {} {}: {} volumes, spatial resource {}", tag.str(), info.name, v.count, r.tag_at(0x30).str());
                for (uint64_t k = 0; k < v.count; ++k) {
                    const size_t c = v.data + size_t(k) * 0x1D0;
                    const auto identity = key(data, c);
                    collections.insert(identity);
                    // Preserve source locations independently of package traversal order.
                    origins.try_emplace(identity);
                    const uint32_t gx = r.u32_at(c + 0x160), gy = r.u32_at(c + 0x164), gz = r.u32_at(c + 0x168);
                    grid_points += size_t(gx) * gy * gz;
                    const auto diffuse = r.tag_at(c + 0x1C0), aux = r.tag_at(c + 0x1B4), specular = r.tag_at(c + 0x1B8);
                    const bool empty = diffuse.is_none() && specular.is_none();
                    empty_records += empty;
                    Texture tx;
                    try {
                        if (!diffuse.is_none()) {
                            tx = load_texture(pm, diffuse);
                            if (tx.width != gx * 7 || tx.height != gy || tx.depth != gz) ++dimension_mismatches;
                        } else if (!empty && uint64_t(gx) * gy * gz > 1) throw std::runtime_error("missing required diffuse texture");
                        // Validate the cube's complete original mip payload too.
                        if (!empty) (void)load_texture(pm, specular);
                        if (!r.tag_at(c + 0x1BC).is_none()) (void)load_texture(pm, r.tag_at(c + 0x1BC));
                    } catch (const std::exception& ex) {
                        ++texture_failures;
                        std::println("texture error {}[{}]: {}", tag.str(), k, ex.what());
                    }
                    uint64_t aux_count = 0;
                    if (!aux.is_none()) {
                        auto [it, fresh] = auxiliary.try_emplace(aux.value);
                        if (fresh) it->second = pm.read(aux);
                        aux_count = TagReader(it->second).vec_at(8).count;
                    }
                    size_t vertices = 0;
                    if (!r.tag_at(c + 0x1C4).is_none()) {
                        const auto vb = load_vertex_buffer(pm, r.tag_at(c + 0x1C4));
                        if (vb.stride != 12 || vb.data.empty() || vb.data.size() % 36)
                            throw std::runtime_error("invalid authored probe boundary");
                        vertices = vb.data.size() / 12;
                        for (size_t o = 0; o < vb.data.size(); o += 4) {
                            float coordinate;
                            std::memcpy(&coordinate, vb.data.data() + o, 4);
                            if (!std::isfinite(coordinate)) throw std::runtime_error("non-finite authored probe boundary");
                            mesh_coordinate_max = std::max(mesh_coordinate_max, std::fabs(coordinate));
                        }
                        ++mesh_count;
                    }
                    if (manifest.is_open()) {
                        auto suffix = std::format("\t{:.9g}\t{:.9g}\t{:.9g}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
                            r.f32_at(c + 0x20), r.f32_at(c + 0x24), r.f32_at(c + 0x28),
                            gx, gy, gz, r.tag_at(c + 0x1B8).str(), r.tag_at(c + 0x1BC).str(), diffuse.str(),
                            tx.width, tx.height, tx.depth, aux.str(), aux_count, r.tag_at(c + 0x1C4).str());
                        for (int j = 0; j < 16; ++j) suffix += std::format("\t{:.9g}", r.f32_at(c + 0xC0 + size_t(j) * 4));
                        suffix += std::format("\t{:.9g}\t{:.9g}\t{:.9g}\t{}", r.f32_at(c + 0x110), r.f32_at(c + 0x114), r.f32_at(c + 0x118), vertices);
                        manifest_rows.push_back({identity, std::format("{}\t{}\t{:08X}\t", tag.str(), k, r.u32_at(c + 0x1C8)), std::move(suffix)});
                    }
                }
                continue;
            }
            ++tables;
            const bool reachable = std::find(map.node_tables.begin(), map.node_tables.end(), tag) != map.node_tables.end();
            const auto nodes = r.vec_at(8);
            for (uint64_t n = 0; n < nodes.count; ++n) {
                const size_t e = nodes.data + size_t(n) * 0x90;
                if (e + 0x90 > data.size()) throw std::runtime_error("truncated node table");
                size_t p = r.pointer_at(e + 0x78);
                for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                    if (r.u32_at(p - 4) == kClassCubemapComponent) {
                        const auto k = key(data, p + 0x10);
                        family_nodes.insert(k);
                        origins[k] += std::format("{}:node{}:0x{:X} ", tag.str(), n, p + 0x10);
                        if (reachable) rooted.insert(k);
                        ++records;
                        if (!reachable) std::println("unreachable probe: table {} node {} offset {:X}", tag.str(), n, p + 0x10);
                    }
                    const auto next = r.u64_at(p);
                    if (!next) break;
                    p += size_t(next);
                }
            }
        }
    }
    auto missing = [](const auto& a, const auto& b) { size_t n = 0; for (const auto& k : a) n += !b.contains(k); return n; };
    std::println("family {}: {} tables, {} inline records, {} collections", family, tables, records, collection_count);
    std::println("unique: bubble nodes {}, family nodes {}, collection records {}", rooted.size(), family_nodes.size(), collections.size());
    std::println("family absent from bubble {}; collection absent from bubble {}; bubble absent from collection {}",
                 missing(family_nodes, rooted), missing(collections, rooted), missing(rooted, collections));
    std::println("authored grid slots {}; texture read failures {}; grid/texture dimension mismatches {}", grid_points, texture_failures, dimension_mismatches);
    const auto loaded = load_map_cubemaps(pm, map);
    std::println("runtime loader: {} volumes", loaded.size());
    std::println("authored boundary meshes {}; records without any lighting texture {}", mesh_count, empty_records);
    std::println("maximum absolute authored boundary coordinate: {}", mesh_coordinate_max);
    size_t auxiliary_records = 0;
    std::map<uint32_t, size_t> operations;
    for (const auto& [tag, data] : auxiliary) {
        TagReader r(data);
        const auto shapes = r.vec_at(8);
        auxiliary_records += shapes.count;
        if (shapes.count && shapes.element_class != 0x80807F57) throw std::runtime_error("unknown CSG collection class");
        for (uint64_t k = 0; k < shapes.count; ++k) {
            const size_t c = shapes.data + size_t(k) * 0xD0;
            if (r.u64_at(c) != 0xD0 || c + 0xD0 > data.size()) throw std::runtime_error("invalid CSG record");
            ++operations[r.u32_at(c + 0x10)];
        }
    }
    std::println("auxiliary tags {}: {} CSG shape records (not extra probes)", auxiliary.size(), auxiliary_records);
    for (const auto& [operation, count] : operations) std::println("  CSG operation {}: {} records", operation, count);
    if (manifest.is_open()) {
        for (const auto& row : manifest_rows) manifest << row.prefix << origins.at(row.key) << row.suffix << '\n';
        manifest.flush();
        if (!manifest) throw std::runtime_error("probe manifest write failed");
    }
    return texture_failures || dimension_mismatches || loaded.size() + empty_records != rooted.size() || missing(family_nodes, rooted) || missing(collections, rooted) || missing(rooted, collections) ? 1 : 0;
}

int cmd_weather(PackageManager& pm, TagHash bubble) {
    const MapWeather w = load_map_weather(pm, bubble);
    std::println("world channel registry {}: {} channels", w.registry.tag.str(), w.registry.slots.size());
    if (w.heat_cascade_controller)
        std::println("Heat Cascade toggle: {} {} ({})", w.heat_cascade_controller->tag.str(),
                     w.heat_cascade_controller->name, w.heat_cascade_shared ? "shared Outpost state; estimate" : "map-authored state");
    for (const auto& c : w.controllers) {
        std::println("controller {} {}{}", c.tag.str(), c.name, c.heat_cascade() ? "  (Heat Cascade)" : "");
        for (const auto& st : c.states) {
            std::string vals;
            for (const auto& v : st.values) vals += std::format("  {}[{}]={:g}..{:g}", w.name(v.channel), v.slot, v.min, v.max);
            std::println("  state {:<12}{}", w.name(st.name), vals);
        }
    }
    return 0;
}

// Node-table components of a map: by component type, how many, their payload sizes and the
// classes of the tags they reference (survey for entities / gimmicks).
int cmd_map_components(PackageManager& pm, TagHash bubble, uint32_t detail_type, TagHash activity = {}) {
    std::vector<TagHash> extra;
    if (!activity.is_none())
        for (const auto& m : list_activity_maps(pm))
            if (m.activity == activity && m.bubble == bubble) extra = load_activity_node_tables(pm, activity, m.bubble_name, m.activity_map);
    const MapLayout map = load_map_layout(pm, bubble, extra);
    struct Stat { size_t n = 0; std::map<uint32_t, size_t> classes; std::vector<std::string> samples; };
    std::map<uint32_t, Stat> types;
    size_t nodes_total = 0;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try { data = pm.read(t); } catch (...) { continue; }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        nodes_total += nodes.count;
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                const uint32_t type = r.u32_at(p - 4);
                const uint64_t next = r.u64_at(p);
                const size_t end = next ? std::min(data.size(), p + size_t(next)) : std::min(data.size(), p + 0x200);
                Stat& s = types[type];
                ++s.n;
                std::set<uint32_t> seen;
                for (size_t o = p + 0x10; o + 4 <= end; o += 4) {
                    const TagHash h(r.u32_at(o));
                    if (!h.is_pkg_ref() || !seen.insert(h.value).second) continue;
                    if (const Entry* pe = pm.entry(h)) ++s.classes[pe->type == 8 ? pe->reference : 0xF0000000u | pe->type];
                }
                if (type == detail_type && s.samples.size() < 6) {
                    std::string hex = std::format("{} node {} @{:X} pos ({:.1f} {:.1f} {:.1f}):", t.str(), i, p, r.f32_at(e + 0x10), r.f32_at(e + 0x14), r.f32_at(e + 0x18));
                    for (size_t o = p + 0x10; o < end && o < p + 0x10 + 0x100; o += 4) hex += std::format("{}{:08X}", (o - p - 0x10) % 32 ? " " : "\n    ", r.u32_at(o));
                    s.samples.push_back(hex);
                }
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    std::println("{} node tables, {} nodes", map.node_tables.size(), nodes_total);
    for (const auto& [type, s] : types) {
        std::string cls;
        for (const auto& [c, n] : s.classes) cls += std::format(" {:08X}x{}", c, n);
        std::println("  type {:08X}  {:>6}{}", type, s.n, cls);
        for (const auto& x : s.samples) std::println("    {}", x);
    }
    return 0;
}

// The shaders a map's statics and terrain draw with: per shader, the techniques using it and its
// SRV bindings (which per-vertex / per-instance buffers the game feeds them).
int cmd_map_shaders(PackageManager& pm, TagHash bubble) {
    const MapLayout map = load_map_layout(pm, bubble);
    std::map<uint32_t, size_t> techniques;  // technique -> parts
    struct Census { size_t groups = 0, instances = 0; std::map<uint32_t, uint32_t> samples; };
    std::map<int, Census> stage_census;  // render stage -> static mesh groups
    for (const auto& st : map.statics) {
        try {
            const StaticMesh mesh = load_static_mesh(pm, st.mesh);
            for (TagHash t : mesh.techniques) if (!t.is_none()) techniques[t.value] += st.rows.size();
            for (size_t g = 0; g < mesh.groups.size(); ++g) {
                auto& c = stage_census[mesh.groups[g].render_stage];
                c.groups += 1;
                c.instances += st.rows.size();
                if (g < mesh.techniques.size() && !mesh.techniques[g].is_none() && c.samples.size() < 8 &&
                    !c.samples.contains(mesh.techniques[g].value))
                    c.samples[mesh.techniques[g].value] = st.mesh.value;
            }
        } catch (...) {}
    }
    for (const auto& [stage, c] : stage_census) {
        std::print("stage {:2} groups {:6} instances {:7}  e.g.", stage, c.groups, c.instances);
        for (const auto& [t, m] : c.samples) std::print(" {:08X}@{:08X}", t, m);
        std::println("");
    }
    for (const auto& tr : map.terrains)
        for (const auto& p : tr.parts) if (!p.technique.is_none()) techniques[p.technique.value] += 1;
    struct Use { size_t techniques = 0, weight = 0; std::string bindings; };
    std::map<std::pair<int, uint32_t>, Use> shaders;
    for (const auto& [tv, weight] : techniques) {
        Technique t;
        try { t = load_technique(pm, TagHash(tv)); } catch (...) { continue; }
        for (ShaderStage s : {ShaderStage::Vertex, ShaderStage::Pixel}) {
            const TagHash sh = t.stage(s).shader;
            if (sh.is_none()) continue;
            auto& u = shaders[{int(s), sh.value}];
            if (!u.techniques++) {
                try {
                    for (const auto& b : shader_bindings(load_shader_bytecode(pm, sh)))
                        if (b.type == BindingType::Srv || b.type == BindingType::Uav)
                            u.bindings += std::format(" {}{}-{}:k{}", b.type == BindingType::Srv ? "t" : "u", b.lower, b.upper, int(b.kind));
                } catch (const std::exception& ex) { u.bindings = std::string(" error ") + ex.what(); }
            }
            u.weight += weight;
        }
    }
    std::println("{} statics, {} terrain patches, {} techniques, {} shaders", map.statics.size(), map.terrains.size(), techniques.size(), shaders.size());
    for (const auto& [key, u] : shaders)
        std::println("{} {:08X} techniques {:>4} instances {:>7}{}", key.first == int(ShaderStage::Vertex) ? "VS" : "PS", key.second, u.techniques, u.weight, u.bindings);
    return 0;
}

// Script instances (8080BADB) of a map's package family: the classes they reference, and the
// ones that place a dynamic model (survey for entities / gimmicks).
// The placed entities (map nodes -> 8080BAAD) whose components name particle systems (808081E8),
// with the resource class that holds each reference.
int cmd_map_fx(PackageManager& pm, TagHash bubble, TagHash activity) {
    pm.build_hash64_index();
    MapLayout map;
    if (!activity.is_none()) {
        for (const auto& m : list_activity_maps(pm))
            if (m.activity == activity && m.bubble == bubble) {
                map = load_map_layout(pm, bubble, load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map));
                break;
            }
    } else {
        map = load_map_layout(pm, bubble);
    }
    std::map<uint32_t, size_t> systems;          // particle system -> placements
    std::map<uint32_t, std::set<uint32_t>> holder;  // system -> resource classes holding it
    std::unordered_map<uint32_t, std::vector<uint32_t>> entity_systems;
    size_t nodes = 0, placements = 0;
    for (const auto table : map.node_tables) {
        std::vector<uint8_t> data;
        try { data = pm.read(table); } catch (...) { continue; }
        TagReader r(data);
        const auto list = r.vec_at(0x08);
        for (uint64_t k = 0; k < list.count; ++k) {
            const size_t e = list.data + size_t(k) * 0x90;
            if (e + 0x90 > data.size()) break;
            ++nodes;
            const uint64_t h64 = r.u64_at(e + 0x30);
            if (!h64 || h64 == ~0ull) continue;
            const auto ent = pm.resolve64(h64);
            if (!ent) continue;
            auto it = entity_systems.find(ent->value);
            if (it == entity_systems.end()) {
                std::vector<uint32_t> found;
                try {
                    const auto ed = pm.read(*ent);
                    std::set<uint32_t> seen;
                    for (size_t o = 0; o + 4 <= ed.size(); o += 4) {
                        uint32_t c; std::memcpy(&c, ed.data() + o, 4);
                        if (c < 0x80800000u || c == ~0u || !seen.insert(c).second) continue;
                        const Entry* ce = pm.entry(TagHash(c));
                        if (!ce || ce->type != 8 || ce->reference != 0x8080BADB) continue;
                        const auto cd = pm.read(TagHash(c));
                        uint32_t resource = 0;
                        for (size_t q = 0; q + 8 <= cd.size(); q += 4) {
                            uint32_t v, w; std::memcpy(&v, cd.data() + q, 4); std::memcpy(&w, cd.data() + q + 4, 4);
                            if (v == c) resource = w;
                            if (v < 0x80800000u || v == ~0u) continue;
                            const Entry* pe = pm.entry(TagHash(v));
                            if (pe && pe->type == 8 && pe->reference == 0x808081E8) {
                                found.push_back(v);
                                holder[v].insert(resource);
                            }
                        }
                    }
                } catch (...) {}
                it = entity_systems.emplace(ent->value, std::move(found)).first;
            }
            if (it->second.empty()) continue;
            ++placements;
            for (uint32_t s : it->second) ++systems[s];
            const char* want = std::getenv("NIM_FX_SYSTEM");  // list every placement holding this system
            const bool listed = want && std::ranges::any_of(it->second, [&](uint32_t s) { return TagHash(s).str() == want; });
            if (placements <= 20 || listed)
                std::println("  node {} ent {} pos ({:.1f} {:.1f} {:.1f}) systems {}", table.str(), ent->str(), r.f32_at(e + 0x10),
                             r.f32_at(e + 0x14), r.f32_at(e + 0x18), it->second.size());
        }
    }
    std::println("{} node tables, {} nodes, {} placements with particle systems, {} systems", map.node_tables.size(), nodes,
                 placements, systems.size());
    for (const auto& [s, n] : systems) {
        std::print("  system {:08X} x{} in", s, n);
        for (uint32_t c : holder[s]) std::print(" {:08X}", c);
        std::println("");
    }
    return 0;
}

// An activity's gameplay placements (load_map_gameplay, docs/PROGRESS.md 3.65): spawn points by
// archetype, loot containers and script objects with their node positions, and the census of the
// family's script components by module. -v lists every point; --json FILE writes them all.
// Every BlockScript definition (80800000) in the install with its module path; FILTER keeps paths
// containing that text.
int cmd_bs_modules(PackageManager& pm, const std::string& filter) {
    for (const auto& info : pm.packages()) {
        Package& pk = pm.open(info);
        for (uint32_t i = 0; i < pk.entries().size(); ++i) {
            const Entry& en = pk.entries()[i];
            if (en.type != 8 || en.reference != 0x80800000) continue;
            std::vector<uint8_t> d;
            try { d = pk.read_entry(i); } catch (...) { continue; }
            const std::string text(d.begin(), d.end());
            const auto at = text.find("bs\\");
            const std::string name = at == std::string::npos ? std::string() : std::string(text.c_str() + at);
            if (!filter.empty() && name.find(filter) == std::string::npos) continue;
            std::println("{}  {:>6}  {:<40} {}", TagHash::make(info.pkg_id, uint16_t(i)).str(), en.file_size, info.name, name);
        }
    }
    return 0;
}

// Exploration: 64-bit references between the type-8 tags of packages named PREFIX*.
// Prints "edge SRC +OFF DST" for every aligned u64 that is the hash64 of another tag.
int cmd_h64_graph(PackageManager& pm, const std::string& prefix) {
    std::unordered_map<uint64_t, TagHash> by64;
    for (const auto& info : pm.packages())
        for (const auto& h : pm.open(info).hash64s()) by64.try_emplace(h.hash64, h.hash32);
    for (const auto& info : pm.packages()) {
        if (!info.name.starts_with(prefix)) continue;
        Package& pk = pm.open(info);
        for (uint32_t i = 0; i < pk.entries().size(); ++i) {
            const Entry& en = pk.entries()[i];
            if (en.type != 8) continue;
            std::vector<uint8_t> d;
            try { d = pk.read_entry(i); } catch (...) { continue; }
            const TagHash self = TagHash::make(info.pkg_id, uint16_t(i));
            for (size_t o = 0; o + 8 <= d.size(); o += 4) {
                uint64_t w;
                std::memcpy(&w, d.data() + o, 8);
                if (w == 0 || w == ~0ull) continue;
                if (auto f = by64.find(w); f != by64.end()) {
                    const Entry* de = pm.entry(f->second);
                    std::println("edge {} {:08X} +{:X} {} {:08X}", self.str(), en.reference, o, f->second.str(), de ? de->reference : 0);
                }
            }
        }
    }
    return 0;
}

// An entity's motion-bank clips and the gait read from their feet (fps 30, 推定).
int cmd_motion_probe(PackageManager& pm, TagHash entity) {
    pm.build_hash64_index();
    const auto bytes = pm.read(entity);
    TagReader r(bytes);
    const auto comps = r.vec_at(0x08);
    TagHash sk, rg;
    for (uint64_t k = 0; k < comps.count; ++k) {
        const TagHash c(r.u32_at(comps.data + size_t(k) * 12));
        if (is_skeleton_component(pm, c)) sk = c;
        else try { if (load_animation_rig(pm, c).size()) rg = c; } catch (...) {}
    }
    if (sk.is_none() || rg.is_none()) throw std::runtime_error("entity has no skeleton and rig");
    const auto skeleton = load_skeleton(pm, sk);
    const auto rig = load_animation_rig(pm, rg);
    const auto feet = find_feet(skeleton);
    if (!feet) throw std::runtime_error("no ankle pair");
    std::println("skeleton {} rig {} feet {} {}", sk.str(), rg.str(), (*feet)[0], (*feet)[1]);
    for (const auto clip : entity_motion_clips(pm, entity)) {
        try {
            const auto c = load_animation_clip(pm, clip);
            if (c.bone_count != rig.size()) continue;
            const auto g = measure_clip_gait(skeleton, rig, c, *feet, 30.0f);
            std::println("{} {:4} frames {:5.2f}s cyclic {} lift {:.2f} speed {:5.2f} dir ({:5.2f} {:5.2f})", clip.str(), c.frame_count, g.seconds,
                         g.cyclic ? 1 : 0, g.lift, g.speed, g.direction[0], g.direction[1]);
        } catch (const std::exception& e) {
            std::println("{} {}", clip.str(), e.what());
        }
    }
    return 0;
}

// Estimated AI simulation (3.69): squads patrol their objective's firing areas, drones fly their
// splines, and every enemy keeps an awareness of the player (the camera). Rules are 推定.
int cmd_ai_sim(PackageManager& pm, TagHash activity, float seconds, float dt, std::array<float, 3> player, bool visible, uint64_t seed,
               const char* json) {
    pm.build_hash64_index();
    const MapGameplay g = load_map_gameplay(pm, activity);
    std::vector<MotionSet> motions;
    std::map<uint32_t, size_t> by_entity;
    for (const auto& ch : g.characters) {
        if (const auto it = by_entity.find(ch.entity.value); it != by_entity.end()) {
            const MotionSet copy = motions[it->second];
            motions.push_back(copy);
            continue;
        }
        by_entity[ch.entity.value] = motions.size();
        motions.push_back(build_motion_set(pm, ch.entity, 30.0f));
    }
    for (size_t c = 0; c < g.characters.size(); ++c) {
        const auto& m = motions[c];
        std::println("  motion {} {:<28} {} locomotion clips, idle {}{}", g.characters[c].entity.str(), g.characters[c].name_estimate,
                     m.locomotion.size(), m.idle ? m.idle->clip.str() : "-", m.error.empty() ? "" : "  (" + m.error + ")");
    }
    AiParams params;
    params.player_invisible = !visible;
    std::println("estimates (推定):");
    for (const auto& e : ai_estimates(params)) std::println("  {:<16} {:>6.2f}  {}", e.name, e.value, e.note);
    AiSimulation sim(g, params, seed, motions);
    std::println("agents {} drones {} player ({:.1f} {:.1f} {:.1f}) {}", sim.agents().size(), sim.drones().size(), player[0], player[1], player[2],
                 visible ? "visible" : "invisible to enemies");
    std::string j = "{\"frames\":[";
    const int steps = int(seconds / dt);
    std::array<size_t, 3> peak{};
    float walked = 0;
    for (int i = 0; i <= steps; ++i) {
        if (i) sim.step(dt, player);
        std::array<size_t, 3> count{};
        for (const auto& a : sim.agents()) ++count[size_t(a.alert)], walked += std::hypot(a.velocity[0], a.velocity[1]) * (i ? dt : 0);
        for (const auto& d : sim.drones()) ++count[size_t(d.alert)];
        for (size_t k = 0; k < 3; ++k) peak[k] = std::max(peak[k], count[k]);
        if (json && i % std::max(1, int(0.5f / dt)) == 0) {
            j += std::format("{}{{\"t\":{:.2f},\"agents\":[", j.back() == '[' ? "" : ",", sim.time());
            for (size_t k = 0; k < sim.agents().size(); ++k) {
                const auto& a = sim.agents()[k];
                std::string clip = "idle";
                if (a.character < motions.size() && a.motion.locomotion) clip = motions[a.character].locomotion[*a.motion.locomotion].clip.str();
                j += std::format("{}[{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{},{},\"{}\",{:.2f}]", k ? "," : "", a.position[0], a.position[1], a.position[2],
                                 a.yaw, a.awareness, int(a.alert), a.sees_player ? 1 : 0, clip, a.motion_time);
            }
            j += "],\"drones\":[";
            for (size_t k = 0; k < sim.drones().size(); ++k) {
                const auto& d = sim.drones()[k];
                j += std::format("{}[{:.2f},{:.2f},{:.2f},{:.3f},{:.3f},{},{}]", k ? "," : "", d.position[0], d.position[1], d.position[2], d.yaw,
                                 d.awareness, int(d.alert), d.sees_player ? 1 : 0);
            }
            j += "]}";
        }
    }
    std::println("after {:.1f}s: walked {:.0f} m in total; peak unaware {} suspicious {} alerted {}", sim.time(), walked, peak[0], peak[1], peak[2]);
    for (const auto& a : sim.agents())
        if (a.alert != AiAlert::Unaware)
            std::println("  {} at ({:.1f} {:.1f} {:.1f}) awareness {:.2f} {}", g.characters[a.character].name_estimate, a.position[0], a.position[1],
                         a.position[2], a.awareness, ai_alert_name(a.alert));
    if (json) {
        j += std::format("],\"player\":[{:.2f},{:.2f},{:.2f}],\"squads\":[", player[0], player[1], player[2]);
        for (size_t k = 0; k < sim.agents().size(); ++k) {
            const auto& a = sim.agents()[k];
            j += std::format("{}[{},{},{:.1f}]", k ? "," : "", a.squad, a.character, a.sight);
        }
        j += "],\"names\":[";
        for (size_t c = 0; c < g.characters.size(); ++c) j += std::format("{}\"{}\"", c ? "," : "", g.characters[c].name_estimate);
        j += "]}";
        std::ofstream(json, std::ios::binary) << j;
        std::println("wrote {}", json);
    }
    return 0;
}

int cmd_map_gameplay(PackageManager& pm, TagHash activity, bool verbose, const char* json) {
    pm.build_hash64_index();
    const MapGameplay g = load_map_gameplay(pm, activity);
    auto labels = [](const MapGameplayPoint& p) {
        std::string l;
        for (const auto h : p.labels) l += std::format("{}{:08X}", l.empty() ? "" : " ", h);
        return l;
    };
    auto group = [&](const char* title, const std::vector<MapGameplayPoint>& points) {
        std::map<uint32_t, std::vector<const MapGameplayPoint*>> by_entity;
        for (const auto& p : points) by_entity[p.entity.value].push_back(&p);
        std::println("{}: {} points, {} archetypes", title, points.size(), by_entity.size());
        for (const auto& [e, list] : by_entity) {
            std::map<std::string, size_t> by_label;
            for (const auto* p : list) ++by_label[labels(*p)];
            std::println("  entity {} model {}: {} points", TagHash(e).str(), list.front()->model.is_none() ? "-" : list.front()->model.str(), list.size());
            if (verbose) {
                for (const auto* p : list)
                    std::println("    ({:.1f} {:.1f} {:.1f}) [{}] id {:016X}", p->position[0], p->position[1], p->position[2], labels(*p), p->id);
            } else {
                std::string l;
                for (const auto& [k, n] : by_label) l += std::format(" [{}]x{}", k, n);
                std::println("   {}", l);
            }
        }
    };
    std::println("{}: {} nodes", activity.str(), g.nodes);
    group("spawn points (entity has the start component 80802D32)", g.spawns);
    group("loot containers (labelled nodes with a dynamic model)", g.loot);
    auto char_name = [&](size_t c) {
        const auto& ch = g.characters[c];
        return ch.name_estimate.empty() ? ch.item.str() : ch.name_estimate;
    };
    std::println("characters (8080B1A7 descriptors squads spawn; names 推定 from squad slot names): {}", g.characters.size());
    for (const auto& ch : g.characters) {
        std::string l;
        for (const auto h : ch.labels) l += std::format(" {:08X}", h);
        std::println("  {} entity {} labels{}  {}", ch.item.str(), ch.entity.str(), l, ch.name_estimate.empty() ? "-" : ch.name_estimate);
    }
    {
        std::map<std::string, std::pair<size_t, size_t>> by_char;  // name -> (squads, members)
        size_t placed = 0, members = 0;
        for (const auto& sq : g.squads) {
            std::set<size_t> seen;
            for (const auto m : sq.members) {
                ++by_char[char_name(m)].second;
                if (seen.insert(m).second) ++by_char[char_name(m)].first;
            }
            members += sq.members.size();
            placed += !sq.positions.empty();
        }
        std::println("squads (native {:08X}): {}, {} with spawn positions, {} members", kClassAiSquad, g.squads.size(), placed, members);
        for (const auto& [n, v] : by_char) std::println("  {:4} squads {:4} members  {}", v.first, v.second, n);
        for (const auto& sq : g.squads) {
            if (!verbose) break;
            const auto& o = g.scripts[sq.script];
            std::string who;
            for (const auto m : sq.members) who += " " + char_name(m);
            std::string at;
            for (const auto& pt : sq.positions) at += std::format(" ({:.1f} {:.1f} {:.1f})", pt.position[0], pt.position[1], pt.position[2]);
            std::println("  {} group {}:{} @{}", o.component.str(), sq.group ? g.scripts[*sq.group].component.str() : "-", who, at.empty() ? " -" : at);
        }
    }
    {
        std::map<std::string, size_t> by_name;
        float area = 0;
        for (const auto& fa : g.firing_areas) {
            ++by_name[fa.name];
            for (const auto& tri : fa.triangles) {
                const auto &a = fa.outline[tri[0]], &b = fa.outline[tri[1]], &c = fa.outline[tri[2]];
                area += std::abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) * 0.5f;
            }
        }
        size_t with_obj = 0;
        for (const auto& sq : g.squads) with_obj += sq.objective.has_value();
        std::println("firing areas (native {:08X}): {} ({:.0f} m2 of floor summed, areas overlap), objectives (native {:08X}): {}, squads with an objective: {}",
                     kClassAiFiringArea, g.firing_areas.size(), area, kClassAiObjective, g.objectives.size(), with_obj);
        if (verbose) {
            for (const auto& fa : g.firing_areas) {
                float cx = 0, cy = 0, cz = 0;
                for (const auto& v : fa.outline) cx += v[0], cy += v[1], cz += v[2];
                const float n = float(std::max<size_t>(fa.outline.size(), 1));
                std::println("  {} {:<28} {} vertices, floor ({:.1f} {:.1f} {:.1f}), height {:.1f}", g.scripts[fa.script].component.str(), fa.name,
                             fa.outline.size(), cx / n, cy / n, cz / n, fa.height);
            }
            for (const auto& ob : g.objectives) {
                std::string l;
                for (const auto a : ob.firing_areas) l += " " + g.firing_areas[a].name;
                std::println("  objective {}:{}", g.scripts[ob.script].component.str(), l);
            }
        }
        std::println("point groups (native {:08X}): {}", kClassPointGroup, g.point_groups.size());
        for (const auto& pg : g.point_groups) {
            if (!verbose && pg.spline.empty()) continue;
            float len = 0;
            for (size_t i = 1; i < pg.spline.size(); ++i)
                len += std::hypot(pg.spline[i][0] - pg.spline[i - 1][0], pg.spline[i][1] - pg.spline[i - 1][1], pg.spline[i][2] - pg.spline[i - 1][2]);
            std::println("  {} owner {}: {} points{}", g.scripts[pg.script].component.str(), pg.owner ? g.scripts[*pg.owner].module : "-",
                         pg.points.size(), pg.spline.empty() ? std::string() : std::format(", spline {} samples {:.0f} m", pg.spline.size(), len));
        }
    }
    std::println("script objects: {} ({} place nodes)", g.scripts.size(),
                 std::ranges::count_if(g.scripts, [](const auto& o) { return !o.placements.empty(); }));
    for (const auto& o : g.scripts) {
        if (o.placements.empty()) continue;
        std::string l;
        for (size_t i = 0; i < o.placements.size() && (verbose || i < 4); ++i) {
            const auto& [off, p] = o.placements[i];
            l += std::format(" +0x{:X}=({:.1f} {:.1f} {:.1f})", off, p.position[0], p.position[1], p.position[2]);
        }
        if (!verbose && o.placements.size() > 4) l += std::format(" ... {} in all", o.placements.size());
        std::println("  {} {}:{}", o.component.str(), o.module, l);
    }
    std::println("script components of the package family by module:");
    for (const auto& [m, n] : g.modules) std::println("  {:5} {}", n, m);
    if (json) {
        std::string j = std::format("{{\"activity\":\"{}\",\"spawns\":[", activity.str());
        auto point = [&](const MapGameplayPoint& p) {
            std::string l;
            for (const auto h : p.labels) l += std::format("{}\"{:08X}\"", l.empty() ? "" : ",", h);
            return std::format("{{\"p\":[{:.3f},{:.3f},{:.3f}],\"q\":[{:.5f},{:.5f},{:.5f},{:.5f}],\"entity\":\"{}\",\"model\":\"{}\",\"id\":\"{:016X}\",\"labels\":[{}]}}",
                               p.position[0], p.position[1], p.position[2], p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3],
                               p.entity.str(), p.model.is_none() ? "" : p.model.str(), p.id, l);
        };
        for (size_t i = 0; i < g.spawns.size(); ++i) j += (i ? "," : "") + point(g.spawns[i]);
        j += "],\"loot\":[";
        for (size_t i = 0; i < g.loot.size(); ++i) j += (i ? "," : "") + point(g.loot[i]);
        j += "],\"scripts\":[";
        for (size_t i = 0; i < g.scripts.size(); ++i) {
            const auto& o = g.scripts[i];
            std::string module;
            for (const char c : o.module) module += c == '\\' ? std::string("\\\\") : std::string(1, c);
            j += std::format("{}{{\"component\":\"{}\",\"module\":\"{}\",\"points\":[", i ? "," : "", o.component.str(), module);
            for (size_t k = 0; k < o.placements.size(); ++k) j += (k ? "," : "") + point(o.placements[k].second);
            j += std::format("],\"slot\":\"{:016X}\",\"names\":[", o.slot);
            for (size_t k = 0; k < o.names.size(); ++k) j += std::format("{}\"{}\"", k ? "," : "", o.names[k].second);
            j += "],\"characters\":[";
            for (size_t k = 0; k < o.characters.size(); ++k) j += std::format("{}{}", k ? "," : "", o.characters[k].second);
            j += "],\"links\":[";
            for (size_t k = 0; k < o.links.size(); ++k) j += std::format("{}{}", k ? "," : "", o.links[k].second);
            j += "]}";
        }
        j += "],\"characters\":[";
        for (size_t i = 0; i < g.characters.size(); ++i) {
            const auto& c = g.characters[i];
            std::string l;
            for (const auto h : c.labels) l += std::format("{}\"{:08X}\"", l.empty() ? "" : ",", h);
            j += std::format("{}{{\"item\":\"{}\",\"entity\":\"{}\",\"name\":\"{}\",\"labels\":[{}]}}", i ? "," : "", c.item.str(),
                             c.entity.str(), c.name_estimate, l);
        }
        j += "],\"squads\":[";
        for (size_t i = 0; i < g.squads.size(); ++i) {
            const auto& sq = g.squads[i];
            j += std::format("{}{{\"component\":\"{}\",\"group\":\"{}\",\"objective\":{},\"members\":[", i ? "," : "",
                             g.scripts[sq.script].component.str(), sq.group ? g.scripts[*sq.group].component.str() : "",
                             sq.objective ? std::to_string(*sq.objective) : std::string("null"));
            for (size_t k = 0; k < sq.members.size(); ++k) j += std::format("{}{}", k ? "," : "", sq.members[k]);
            j += "],\"points\":[";
            for (size_t k = 0; k < sq.positions.size(); ++k) j += (k ? "," : "") + point(sq.positions[k]);
            j += "]}";
        }
        auto vec3s = [](const std::vector<std::array<float, 3>>& v) {
            std::string r;
            for (size_t k = 0; k < v.size(); ++k) r += std::format("{}[{:.2f},{:.2f},{:.2f}]", k ? "," : "", v[k][0], v[k][1], v[k][2]);
            return r;
        };
        j += "],\"firing_areas\":[";
        for (size_t i = 0; i < g.firing_areas.size(); ++i) {
            const auto& fa = g.firing_areas[i];
            std::string tris;
            for (size_t k = 0; k < fa.triangles.size(); ++k)
                tris += std::format("{}[{},{},{}]", k ? "," : "", fa.triangles[k][0], fa.triangles[k][1], fa.triangles[k][2]);
            j += std::format("{}{{\"component\":\"{}\",\"name\":\"{}\",\"height\":{:.2f},\"outline\":[{}],\"triangles\":[{}]}}", i ? "," : "",
                             g.scripts[fa.script].component.str(), fa.name, fa.height, vec3s(fa.outline), tris);
        }
        j += "],\"objectives\":[";
        for (size_t i = 0; i < g.objectives.size(); ++i) {
            std::string l;
            for (size_t k = 0; k < g.objectives[i].firing_areas.size(); ++k) l += std::format("{}{}", k ? "," : "", g.objectives[i].firing_areas[k]);
            j += std::format("{}{{\"component\":\"{}\",\"firing_areas\":[{}]}}", i ? "," : "", g.scripts[g.objectives[i].script].component.str(), l);
        }
        j += "],\"point_groups\":[";
        for (size_t i = 0; i < g.point_groups.size(); ++i) {
            const auto& pg = g.point_groups[i];
            std::string owner;
            if (pg.owner)
                for (const char c : g.scripts[*pg.owner].module) owner += c == '\\' ? std::string("\\\\") : std::string(1, c);
            j += std::format("{}{{\"component\":\"{}\",\"owner\":\"{}\",\"points\":[{}],\"spline\":[{}]}}", i ? "," : "",
                             g.scripts[pg.script].component.str(), owner, vec3s(pg.points), vec3s(pg.spline));
        }
        // The map's static placements (positions only): context for overview plots.
        j += "],\"statics\":[";
        bool first = true;
        for (const auto& m : list_activity_maps(pm)) {
            if (m.activity != activity) continue;
            const auto layout = load_map_layout(pm, m.bubble, load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map));
            for (const auto& st : layout.statics)
                for (const auto& row : st.rows) {
                    j += std::format("{}[{:.1f},{:.1f},{:.1f}]", first ? "" : ",", row[3], row[7], row[11]);
                    first = false;
                }
        }
        j += "]}\n";
        std::ofstream(json, std::ios::binary) << j;
        std::println("wrote {}", json);
    }
    return 0;
}

int cmd_map_entities(PackageManager& pm, TagHash bubble) {
    const PackageInfo* home = pm.info(bubble.pkg_id());
    if (!home) return 1;
    const std::string family = home->name.substr(0, home->name.rfind('_') + 1);
    std::map<uint32_t, size_t> classes;  // referenced class -> instances referencing it
    std::map<uint32_t, size_t> scripts;  // referenced 80800000 tag -> instances
    size_t total = 0, with_dyn = 0;
    std::vector<std::string> lines;
    for (const auto& info : pm.packages()) {
        if (!info.name.starts_with(family)) continue;
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != 0x8080BADB) continue;
            const TagHash self = TagHash::make(info.pkg_id, uint16_t(i));
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            ++total;
            std::set<uint32_t> cls, dyn, nodes, scr;
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                uint32_t v; std::memcpy(&v, d.data() + o, 4);
                const TagHash h(v);
                if (!h.is_pkg_ref() || h == self) continue;
                const Entry* pe = pm.entry(h);
                if (!pe || pe->type != 8) continue;
                cls.insert(pe->reference);
                if (pe->reference == 0x8080881C) dyn.insert(v);
                if (pe->reference == 0x8080B1A7) nodes.insert(v);
                if (pe->reference == 0x80800000) scr.insert(v);
            }
            for (uint32_t c : cls) ++classes[c];
            for (uint32_t s : scr) ++scripts[s];
            if (dyn.empty()) continue;
            ++with_dyn;
            std::string l = std::format("{} size {:>6} dyn", self.str(), d.size());
            for (uint32_t v : dyn) l += " " + TagHash(v).str();
            l += "  nodes";
            for (uint32_t v : nodes) l += " " + TagHash(v).str();
            l += "  scripts";
            for (uint32_t v : scr) l += " " + TagHash(v).str();
            lines.push_back(l);
        }
    }
    std::println("{} script instances, {} with dynamic models", total, with_dyn);
    for (const auto& [c, n] : classes) if (n >= 20) std::println("  class {:08X} in {}", c, n);
    std::println("scripts:");
    for (const auto& [s, n] : scripts) if (n >= 10) std::println("  {} in {}", TagHash(s).str(), n);
    for (const auto& l : lines) std::println("{}", l);
    return 0;
}

// Node placements of a map's package family: the entity each node names by hash64 (+0x30),
// and the dynamic models those entities' components carry.
int cmd_map_placements(PackageManager& pm, TagHash bubble) {
    pm.build_hash64_index();
    const PackageInfo* home = pm.info(bubble.pkg_id());
    if (!home) return 1;
    const std::string family = home->name.substr(0, home->name.rfind('_') + 1);
    auto tag_refs = [&](const std::vector<uint8_t>& d, uint32_t cls, TagHash self) {
        std::set<uint32_t> out;
        for (size_t o = 0; o + 4 <= d.size(); o += 4) {
            uint32_t v; std::memcpy(&v, d.data() + o, 4);
            const TagHash h(v);
            if (!h.is_pkg_ref() || h == self) continue;
            if (const Entry* pe = pm.entry(h); pe && pe->type == 8 && pe->reference == cls) out.insert(v);
        }
        return out;
    };
    std::map<uint32_t, std::set<uint32_t>> entity_models;  // entity -> dynamic models
    std::map<uint32_t, size_t> classes, model_uses;
    size_t nodes = 0, tables = 0, placed = 0, unresolved = 0;
    std::vector<std::string> samples;
    for (const auto& info : pm.packages()) {
        if (!info.name.starts_with(family)) continue;
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& en = p.entries()[i];
            if (en.type != 8 || en.reference != 0x8080B1A7) continue;
            std::vector<uint8_t> data;
            try { data = p.read_entry(i); } catch (...) { continue; }
            ++tables;
            TagReader r(data);
            const auto vec = r.vec_at(0x08);
            for (uint64_t k = 0; k < vec.count; ++k) {
                const size_t e = vec.data + size_t(k) * 0x90;
                if (e + 0x90 > data.size()) break;
                ++nodes;
                const uint64_t h64 = r.u64_at(e + 0x30);
                if (!h64 || h64 == ~0ull) continue;
                const auto ent = pm.resolve64(h64);
                if (!ent) { ++unresolved; continue; }
                const Entry* ee = pm.entry(*ent);
                if (!ee) continue;
                ++classes[ee->type == 8 ? ee->reference : 0xF0000000u | ee->type];
                if (ee->reference != 0x8080BAAD) continue;
                auto it = entity_models.find(ent->value);
                if (it == entity_models.end()) {
                    std::set<uint32_t> models;
                    try {
                        const auto ed = pm.read(*ent);
                        for (uint32_t c : tag_refs(ed, 0x8080BADB, *ent))
                            for (uint32_t m : tag_refs(pm.read(TagHash(c)), 0x8080881C, TagHash(c))) models.insert(m);
                    } catch (...) {}
                    it = entity_models.emplace(ent->value, std::move(models)).first;
                }
                if (it->second.empty()) continue;
                ++placed;
                for (uint32_t m : it->second) ++model_uses[m];
                if (samples.size() < 30)
                    samples.push_back(std::format("{} node {} entity {} pos ({:.1f} {:.1f} {:.1f}) scale {:.2f} models {}", TagHash::make(info.pkg_id, uint16_t(i)).str(), k,
                                                  ent->str(), r.f32_at(e + 0x10), r.f32_at(e + 0x14), r.f32_at(e + 0x18), r.f32_at(e + 0x1C), it->second.size()));
            }
        }
    }
    size_t with_models = 0;
    for (const auto& [e, m] : entity_models) with_models += !m.empty();
    std::println("{} node tables, {} nodes, {} unresolved hash64", tables, nodes, unresolved);
    for (const auto& [c, n] : classes) std::println("  entity class {:08X}: {}", c, n);
    std::println("{} entities ({} with dynamic models), {} placements with models, {} distinct models", entity_models.size(), with_models, placed, model_uses.size());
    for (const auto& s : samples) std::println("  {}", s);
    for (const auto& [m, n] : model_uses) std::println("  model {} x{}", TagHash(m).str(), n);
    return 0;
}

// Every static mesh and dynamic model in the install with parts in one render stage
// (e.g. 20 = Volumetrics): count, examples and their techniques.
int cmd_stage_survey(PackageManager& pm, size_t stage) {
    size_t statics = 0, dynamics = 0, hits_s = 0, hits_d = 0;
    std::map<uint32_t, std::pair<size_t, uint32_t>> techs;  // technique -> (uses, first model)
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            try {
                if (e.reference == kClassStaticMesh) {
                    ++statics;
                    const StaticMesh m = load_static_mesh(pm, tag);
                    bool hit = false;
                    for (const auto& sp : m.special_parts)
                        if (sp.render_stage == stage) {
                            hit = true;
                            auto& u = techs[sp.technique.value];
                            if (!u.first++) u.second = tag.value;
                        }
                    for (size_t g = 0; g < m.groups.size(); ++g)
                        if (m.groups[g].render_stage == stage) {
                            hit = true;
                            const uint32_t t = g < m.techniques.size() ? m.techniques[g].value : 0;
                            auto& u = techs[t];
                            if (!u.first++) u.second = tag.value;
                        }
                    hits_s += hit;
                } else if (e.reference == kClassDynamicModel) {
                    ++dynamics;
                    const DynamicModel dm = load_dynamic_model(pm, tag);
                    bool hit = false;
                    for (const auto& mesh : dm.meshes)
                        for (size_t k = mesh.part_range[stage]; k < std::min<size_t>(mesh.part_range[stage + 1], mesh.parts.size()); ++k) {
                            hit = true;
                            auto& u = techs[mesh.parts[k].technique.value];
                            if (!u.first++) u.second = tag.value;
                        }
                    hits_d += hit;
                }
            } catch (...) {}
        }
    }
    std::println("stage {}: {} of {} statics, {} of {} dynamic models, {} techniques", stage, hits_s, statics, hits_d, dynamics, techs.size());
    size_t shown = 0;
    for (const auto& [t, u] : techs)
        if (shown++ < 40) std::println("  technique {:08X} parts {:5} first in {:08X}", t, u.first, u.second);
    return 0;
}

// Every record of a map's sky collections (80808378 components -> 8080837C): kind (+0x70),
// authored distance (+0x64), model, position and the largest axis scale, and the model's
// render stages.
int cmd_map_sky(PackageManager& pm, TagHash bubble) {
    const MapLayout map = load_map_layout(pm, bubble);
    std::set<uint32_t> collections;
    for (TagHash t : map.node_tables) {
        std::vector<uint8_t> data;
        try { data = pm.read(t); } catch (...) { continue; }
        TagReader r(data);
        const auto nodes = r.vec_at(0x08);
        for (uint64_t i = 0; i < nodes.count; ++i) {
            const size_t e = nodes.data + size_t(i) * 0x90;
            if (e + 0x90 > data.size()) break;
            size_t p = e + 0x78 + size_t(r.u64_at(e + 0x78));
            for (int guard = 0; p != e + 0x78 && p >= 4 && p + 0x18 <= data.size() && guard < 64; ++guard) {
                if (r.u32_at(p - 4) == 0x80808378) collections.insert(r.u32_at(p + 0x10));
                const uint64_t next = r.u64_at(p);
                if (!next) break;
                p += size_t(next);
            }
        }
    }
    for (uint32_t c : collections) {
        std::vector<uint8_t> data;
        try { data = pm.read(TagHash(c)); } catch (...) { continue; }
        TagReader r(data);
        const auto objects = r.vec_at(0x08);
        std::println("collection {:08X}: {} records", c, objects.count);
        for (uint64_t i = 0; i < objects.count; ++i) {
            const size_t o = objects.data + size_t(i) * 0x90;
            if (o + 0x90 > data.size()) break;
            const TagHash ref = r.tag_at(o + 0x60);
            TagHash model;
            try { model = TagReader(pm.read(ref)).tag_at(0x08); } catch (...) {}
            float sc = 0;
            for (int k = 0; k < 3; ++k) {
                const float x = r.f32_at(o + k * 16), y = r.f32_at(o + k * 16 + 4), z = r.f32_at(o + k * 16 + 8);
                sc = std::max(sc, std::sqrt(x * x + y * y + z * z));
            }
            std::string stages;
            try {
                const StaticMesh m = load_static_mesh(pm, model);
                std::set<int> st;
                for (const auto& g : m.groups) st.insert(g.render_stage);
                for (int s : st) stages += std::format(" {}", s);
            } catch (const std::exception& ex) { stages = std::string(" (") + ex.what() + ")"; }
            std::println("  [{:3}] kind {} dist {:9.1f} model {} pos ({:.0f} {:.0f} {:.0f}) scale {:.1f} words +0x68 {:08X} +0x6C {:08X} +0x74 {:08X} stages{}", i,
                         r.u32_at(o + 0x70), r.f32_at(o + 0x64), model.str(), r.f32_at(o + 0x30), r.f32_at(o + 0x34), r.f32_at(o + 0x38), sc,
                         r.u32_at(o + 0x68), r.u32_at(o + 0x6C), r.u32_at(o + 0x74), stages);
        }
    }
    return 0;
}

// The statics and entity models placed within R of (X, Y) (horizontal), largest first:
// position, scale, the render stages and the techniques of their groups.
int cmd_map_near(PackageManager& pm, TagHash bubble, float x, float y, float radius) {
    pm.build_hash64_index();
    const MapLayout map = load_map_layout(pm, bubble);
    struct Hit { std::string line; float size; };
    std::vector<Hit> hits;
    auto scale_of = [](const std::array<float, 12>& r) {
        float s = 0;
        for (int a = 0; a < 3; ++a) s = std::max(s, std::sqrt(r[a] * r[a] + r[4 + a] * r[4 + a] + r[8 + a] * r[8 + a]));
        return s;
    };
    for (const auto& st : map.statics) {
        StaticMesh m;
        try { m = load_static_mesh(pm, st.mesh); } catch (...) { continue; }
        for (const auto& row : st.rows) {
            // The mesh's centre (its offset) through the placement, and its half extent.
            const auto& o = m.mesh_offset;
            const float cx = row[0] * o[0] + row[1] * o[1] + row[2] * o[2] + row[3];
            const float cy = row[4] * o[0] + row[5] * o[1] + row[6] * o[2] + row[7];
            const float cz = row[8] * o[0] + row[9] * o[1] + row[10] * o[2] + row[11];
            const float half = m.mesh_scale * scale_of(row);
            const float dx = cx - x, dy = cy - y;
            if (std::sqrt(dx * dx + dy * dy) > radius + half * 0.5f || half > radius * 8) continue;
            std::map<uint32_t, std::set<int>> tech;
            for (size_t g = 0; g < m.groups.size(); ++g)
                if (g < m.techniques.size()) tech[m.techniques[g].value].insert(m.groups[g].render_stage);
            std::string info;
            for (const auto& [t, ss] : tech) {
                info += std::format(" {:08X}:", t);
                for (int s2 : ss) info += std::format("{},", s2);
            }
            hits.push_back({std::format("static {} centre ({:.0f} {:.0f} {:.0f}) half {:.1f} x{} techs{}", st.mesh.str(), cx, cy, cz, half, st.rows.size(), info), half});
            break;
        }
    }
    const auto materials = index_dynamic_model_materials(pm);
    const MapEntities me = load_map_entities(pm, map, materials);
    for (const auto& m : me.models)
        for (const auto& row : m.rows) {
            const float dx = row[3] - x, dy = row[7] - y;
            if (dx * dx + dy * dy > radius * radius) continue;
            hits.push_back({std::format("entity {} at ({:.0f} {:.0f} {:.0f}) scale {:.1f} x{}", m.mesh.str(), row[3], row[7], row[11], scale_of(row), m.rows.size()), scale_of(row) * 10});
            break;
        }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.size > b.size; });
    for (size_t i = 0; i < hits.size() && i < 60; ++i) std::println("{}", hits[i].line);
    return 0;
}

int cmd_map_entity_models(PackageManager& pm, TagHash bubble) {
    pm.build_hash64_index();
    const auto materials = index_dynamic_model_materials(pm);
    const MapEntities me = load_map_entities(pm, load_map_layout(pm, bubble), materials);
    std::println("{} node tables, {} nodes, {} entities, {} placements with a model, {} models", me.node_tables, me.nodes, me.entities,
                 me.placements, me.models.size());
    std::map<size_t, std::pair<size_t, std::string>> stage_models;  // render stage -> (models, samples)
    for (const auto& m : me.models) {
        std::print("  {} x{}  first at ({:.1f} {:.1f} {:.1f})", m.mesh.str(), m.rows.size(), m.rows[0][3], m.rows[0][7], m.rows[0][11]);
        try {
            const DynamicModel dm = load_dynamic_model(pm, m.mesh);
            std::set<size_t> stages;
            for (const auto& mesh : dm.meshes)
                for (size_t s = 0; s < kRenderStageCount; ++s)
                    if (mesh.part_range[s] < std::min<size_t>(mesh.part_range[s + 1], mesh.parts.size())) stages.insert(s);
            std::print("  stages");
            for (size_t s : stages) std::print(" {}", s);
            for (size_t s : stages) {
                auto& e = stage_models[s];
                if (e.first++ < 6) e.second += std::format(" {}x{}", m.mesh.str(), m.rows.size());
            }
        } catch (...) {}
        std::println("");
    }
    for (const auto& [s, e] : stage_models) std::println("stage {:2} models {:4}{}", s, e.first, e.second);
    return 0;
}

int cmd_lights(PackageManager& pm, TagHash bubble) {
    const MapLayout map = load_map_layout(pm, bubble);
    const auto lights = load_map_lights(pm, map);
    std::map<uint32_t, size_t> techniques;
    size_t shadowing = 0;
    for (const auto& l : lights) ++techniques[l.technique.value], shadowing += l.shadowing;
    std::println("{} lights ({} shadowing), {} distinct lighting-apply techniques", lights.size(), shadowing, techniques.size());
    {
        std::map<uint32_t, size_t> shadow_techniques;
        for (const auto& l : lights)
            if (l.shadowing && !l.shadow_technique.is_none()) ++shadow_techniques[l.shadow_technique.value];
        for (const auto& [t, n] : shadow_techniques) std::println("  shadow technique {} x{}", TagHash(t).str(), n);
    }
    std::vector<const MapLight*> shown;
    for (const auto& l : lights)
        if (shown.size() < 8 && l.shadowing) shown.push_back(&l);
    for (const auto& l : lights)
        if (shown.size() < 12 && !l.shadowing) shown.push_back(&l);
    for (const MapLight* lp : shown) {
        const auto& l = *lp;
        std::println("  {} {} tech {} pos ({:.1f} {:.1f} {:.1f}) bounds ({:.1f} {:.1f} {:.1f})-({:.1f} {:.1f} {:.1f})", l.source.str(),
                     l.shadowing ? "S" : "C", l.technique.str(), l.node[12], l.node[13], l.node[14], l.lo[0], l.lo[1], l.lo[2], l.hi[0], l.hi[1], l.hi[2]);
    }
    // Which run-time channels the lights' own expressions read (their colour and intensity
    // follow these): world channels (TFX 0x64) and render-global channels (0x63), in lights.
    const auto registry = load_world_channel_registry(pm);
    std::map<uint32_t, uint32_t> slot_hash;
    for (const auto& [hash, slot] : registry.slots) slot_hash[slot] = hash;
    std::map<uint8_t, size_t> world, global;
    std::map<uint8_t, TagHash> global_example;  // one technique per render-global channel
    std::map<uint32_t, std::array<size_t, 2>> light_floats;  // deferred_light (extern 4) float offset -> lights reading it (all, shadowing)
    size_t failed = 0, shadow_world3 = 0;
    for (const auto& l : lights) {
        try {
            const Technique t = load_technique(pm, l.technique);
            const auto& ps = t.stage(ShaderStage::Pixel);
            std::set<uint8_t> w, g, f;
            TfxEnvironment env;
            env.extern_vec4 = [](uint8_t, uint8_t) { return float4{1, 1, 1, 1}; };
            env.extern_float = [&](uint8_t e, uint8_t i) { if (e == 4) f.insert(i); return 1.0f; };
            env.global_channel = [&](uint8_t c) { g.insert(c); return float4{1, 1, 1, 1}; };
            env.world_channel = [&](uint8_t c) { w.insert(c); return float4{1, 1, 1, 1}; };
            env.object_channel = [](uint32_t) { return float4{1, 1, 1, 1}; };
            std::vector<float4> out;
            evaluate_tfx(ps.bytecode, ps.constants, ps.cbuffer_init, out, env);
            for (auto c : w) ++world[c];
            for (auto c : g) {
                ++global[c];
                global_example.try_emplace(c, l.technique);
            }
            for (auto i : f) { ++light_floats[uint32_t(i) * 4][0]; light_floats[uint32_t(i) * 4][1] += l.shadowing; }
            if (l.shadowing && w.contains(3)) ++shadow_world3;
        } catch (const std::exception&) { ++failed; }
    }
    std::println("channels read by the lights' pixel expressions ({} lights failed):", failed);
    for (const auto& [c, n] : world) {
        const auto it = slot_hash.find(c);
        std::println("  world channel {:2} ({:08X}): {} lights", c, it == slot_hash.end() ? 0u : it->second, n);
    }
    for (const auto& [c, n] : global)
        std::println("  render-global channel {:02X}: {} lights, e.g. technique {}", c, n, global_example[c].str());
    std::println("  shadowing lights reading world channel 3: {}", shadow_world3);
    for (const auto& [off, n] : light_floats) std::println("  deferred_light +0x{:X} (float): {} lights, {} of them shadowing", off, n[0], n[1]);
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

int cmd_movies(PackageManager& pm, bool verbose) {
    auto movies = list_movies(pm);
    size_t subtitles = 0, multi_audio = 0;
    for (const auto& m : movies) {
        const auto& i = m.info;
        std::string audio;
        for (const auto& a : i.audio)
            audio += std::format(" ch{}:{} {}Hz x{}", a.channel, usm_audio_codec_name(a.codec), a.sample_rate, a.channels);
        subtitles += !i.subtitle_channels.empty();
        multi_audio += i.audio.size() > 1;
        std::println("{}  {:>10}  {:<40} {} {}x{} {:>5} frames {:.3f} fps {:>7.1f}s audio{}{}{}", m.usm.str(), m.size,
                     i.filename, usm_video_codec_name(i.video_codec), i.width, i.height, i.total_frames, i.frame_rate,
                     i.duration(), audio.empty() ? " -" : audio, i.subtitle_channels.empty() ? "" : " subtitles",
                     m.error.empty() ? "" : "  ERROR " + m.error);
        if (verbose)
            for (auto d : m.descriptors) std::println("    descriptor {}", d.str());
    }
    std::println("{} movies, {} with subtitle chunks in the header area, {} with several audio tracks", movies.size(),
                 subtitles, multi_audio);
    return 0;
}

bool write_wav(const char* path, const PcmAudio& pcm) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t data = uint32_t(pcm.samples.size() * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(pcm.channels);
    u32(pcm.sample_rate); u32(pcm.sample_rate * pcm.channels * 2); u16(uint16_t(pcm.channels * 2)); u16(16);
    f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(pcm.samples.data()), data);
    return bool(f);
}

int cmd_movie_audio(PackageManager& pm, TagHash usm, const char* out, int channel) {
    const Entry* e = pm.entry(usm);
    if (!e) throw std::runtime_error("no such entry");
    AdxDecoder adx;
    HcaDecoder hca;
    bool is_hca = false;
    PcmAudio pcm;
    std::vector<uint8_t> buf;
    uint64_t pos = 0;
    size_t subs = 0;
    while (pos < e->file_size || !buf.empty()) {
        if (pos < e->file_size) {
            auto more = pm.read_range(usm, pos, 0x400000);
            pos += more.size();
            buf.insert(buf.end(), more.begin(), more.end());
        }
        size_t used = for_each_usm_chunk(buf, [&](const UsmChunk& c) {
            if (c.data_type != 0) return;
            if (c.stream == UsmStream::Audio && c.channel == channel) {
                if (!hca.ready() && !adx.channels && c.payload.size() >= 8 && (c.payload[0] & 0x7F) == 'H' &&
                    (c.payload[1] & 0x7F) == 'C' && (c.payload[2] & 0x7F) == 'A') {
                    hca.parse_header(c.payload);
                    is_hca = true;
                    if (c.payload.size() > hca.header_size()) hca.feed(c.payload.subspan(hca.header_size()), pcm.samples);
                } else if (is_hca) {
                    hca.feed(c.payload, pcm.samples);
                } else {
                    adx.feed(c.payload, pcm.samples);
                }
            }
            if (c.stream == UsmStream::Subtitle) {
                if (auto s = parse_usm_subtitle(c.payload); s && subs++ < 20)
                    std::println("subtitle ch{} lang {} {}ms +{}: {}", c.channel, s->language, s->start, s->duration, s->text);
            }
        });
        if (used == 0 && pos >= e->file_size) break;
        buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(used));
    }
    if (is_hca) {
        pcm.sample_rate = hca.sample_rate();
        pcm.channels = uint16_t(hca.channels());
        int peak = 0;
        double sum = 0;
        for (int16_t v : pcm.samples) peak = std::max(peak, std::abs(int(v))), sum += double(v) * v;
        std::println("HCA {} Hz x{}, {} samples ({} frames, {} errors), peak {}, rms {:.0f}", hca.sample_rate(), hca.channels(),
                     pcm.samples.size() / std::max(1u, hca.channels()), hca.frame_count(), hca.frame_errors(), peak,
                     std::sqrt(sum / std::max<size_t>(1, pcm.samples.size())));
        return write_wav(out, pcm) ? 0 : 1;
    }
    pcm.sample_rate = adx.sample_rate;
    pcm.channels = uint16_t(adx.channels);
    std::println("ADX {} Hz x{}, {} samples decoded of {}{}", adx.sample_rate, adx.channels,
                 adx.channels ? pcm.samples.size() / adx.channels : 0, adx.total_samples, adx.encrypted ? " (encrypted)" : "");
    if (subs) std::println("{} subtitle chunks", subs);
    return write_wav(out, pcm) ? 0 : 1;
}

void write_bmp(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    std::vector<uint8_t> b(size, 0);
    auto put32 = [&](size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); };
    b[0] = 'B', b[1] = 'M';
    put32(2, size), put32(10, 54), put32(14, 40), put32(18, w), put32(22, h);
    b[26] = 1, b[28] = 24;
    put32(34, row * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            uint8_t* q = &b[54 + size_t(h - 1 - y) * row + x * 3];
            q[0] = p[2], q[1] = p[1], q[2] = p[0];
        }
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

// Writes the raw video elementary stream (the concatenated video chunk payloads) of a USM.
int cmd_movie_video(PackageManager& pm, TagHash usm, const char* out, uint64_t limit) {
    const Entry* e = pm.entry(usm);
    if (!e) throw std::runtime_error("no such entry");
    std::ofstream f(out, std::ios::binary);
    std::vector<uint8_t> buf;
    uint64_t pos = 0, written = 0;
    size_t chunks = 0;
    while ((pos < e->file_size || !buf.empty()) && written < limit) {
        if (pos < e->file_size) {
            auto more = pm.read_range(usm, pos, 0x400000);
            pos += more.size();
            buf.insert(buf.end(), more.begin(), more.end());
        }
        size_t used = for_each_usm_chunk(buf, [&](const UsmChunk& c) {
            if (c.data_type != 0 || c.stream != UsmStream::Video || written >= limit) return;
            f.write(reinterpret_cast<const char*>(c.payload.data()), std::streamsize(c.payload.size()));
            written += c.payload.size();
            ++chunks;
        });
        if (used == 0 && pos >= e->file_size) break;
        buf.erase(buf.begin(), buf.begin() + std::ptrdiff_t(used));
    }
    std::println("{} video chunks, {} bytes", chunks, written);
    return 0;
}

int cmd_movie_frames(PackageManager& pm, TagHash usm, const fs::path& dir, double start, int count) {
    auto movies = list_movies(pm);
    auto it = std::find_if(movies.begin(), movies.end(), [&](const MovieEntry& m) { return m.usm == usm; });
    if (it == movies.end()) throw std::runtime_error("not a movie");
    fs::create_directories(dir);
    media::MoviePlayer player(pm, nullptr, *it, 0, start);
    int got = 0;
    double first = -1, last = 0;
    uint64_t checksum = 0;
    while (got < count) {
        auto f = player.take_next_frame(std::chrono::seconds(20));
        if (!f) break;
        if (first < 0) first = f->pts;
        last = f->pts;
        uint64_t mean = 0;
        for (size_t i = 0; i < f->rgba.size(); i += 4) mean += f->rgba[i] + f->rgba[i + 1] + f->rgba[i + 2];
        checksum += mean;
        if (got == 0 || got == count - 1 || got % 30 == 0)
            write_bmp(dir / std::format("frame_{:05}.bmp", int(std::lround(f->pts * it->info.frame_rate))), f->width, f->height, f->rgba);
        ++got;
    }
    std::println("{} frames {:.3f}..{:.3f}s ({}x{}){}", got, first, last, it->info.display_width, it->info.display_height,
                 player.error().empty() ? "" : "  ERROR " + player.error());
    StringTable strings;
    bool loaded = false;
    for (const auto& s : player.subtitles()) {
        if (!loaded) strings.load_all(pm), loaded = true;
        const LocalizedString* ls = strings.find(s.string_hash);
        std::println("  {:7.2f}s +{:.2f}  {}  {}  /  {}", s.start, s.duration, s.key, ls ? ls->text[0] : "?", ls ? ls->text[1] : "?");
    }
    return got > 0 && player.error().empty() ? 0 : 1;
}

int cmd_movie_playcheck(PackageManager& pm, TagHash usm, int channel, double start) {
    struct ComApartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~ComApartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result)) throw std::runtime_error("could not initialize COM for movie playback");
    const auto movies = list_movies(pm);
    const auto movie = std::ranges::find_if(movies, [&](const auto& m) { return m.usm == usm; });
    if (movie == movies.end()) throw std::runtime_error("not a movie");
    media::AudioEngine engine;
    if (!engine.ok()) throw std::runtime_error(engine.error());
    media::MoviePlayer player(pm, &engine, *movie, channel, start);
    player.set_volume(0);
    size_t frames = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < end && player.error().empty() &&
           (player.audio_frames_played() < 48000 || frames < 10)) {
        if (player.take_due_frame()) ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::println("audio supported {} played {} sample frames, PCM peak {}, {} pictures, clock {:.3f}s; {}",
                 player.audio_supported(), player.audio_frames_played(), player.audio_peak(), frames, player.position(), player.error());
    return player.audio_frames_played() > 0 && frames > 0 && player.error().empty() ? 0 : 1;
}

int cmd_wem(PackageManager& pm, TagHash tag, const char* out) {
    const Entry* e = pm.entry(tag);
    if (!e) throw std::runtime_error("no such entry");
    TagHash wem = tag;
    if (e->type == 8 && e->reference == kClassAudioEvent) {
        auto ev = pm.read(tag);
        TagReader r(ev);
        auto v = r.vec_at(0x20);
        if (v.count == 0) throw std::runtime_error("audio event has no streams");
        wem = r.tag_at(v.data);
    }
    auto bytes = pm.read(wem);
    auto info = probe_wem(bytes);
    if (!info) throw std::runtime_error("not a WEM");
    std::println("{}: codec {:04X} {} Hz x{} {} samples", wem.str(), info->codec, info->sample_rate, info->channels,
                 info->sample_count);
    auto pcm = decode_wem(bytes);
    std::println("decoded {} samples", pcm.frames());
    return write_wav(out, pcm) ? 0 : 1;
}

// Decodes every dialogue stream and reports codecs, failures and silent results.
int cmd_wem_audit(PackageManager& pm, size_t limit) {
    auto lines = load_all_dialogue(pm);
    std::map<uint16_t, size_t> codecs;
    size_t streams = 0, ok = 0, failed = 0, silent = 0, missing = 0;
    double seconds = 0;
    std::set<uint32_t> seen;
    for (const auto& l : lines)
        for (TagHash w : l.wem) {
            if (!seen.insert(w.value).second) continue;
            if (limit && streams >= limit) break;
            ++streams;
            if (!pm.entry(w)) { ++missing; continue; }
            try {
                auto bytes = pm.read(w);
                if (bytes.size() < 12) { ++missing; continue; }  // localized placeholder: no recording
                auto info = probe_wem(bytes);
                if (!info) throw std::runtime_error("not RIFF");
                codecs[info->codec]++;
                auto pcm = decode_wem(bytes);
                int peak = 0;
                for (int16_t v : pcm.samples) peak = std::max(peak, std::abs(int(v)));
                if (info->sample_count && pcm.frames() != info->sample_count)
                    throw std::runtime_error(std::format("{} of {} samples", pcm.frames(), info->sample_count));
                silent += peak < 64;
                seconds += pcm.seconds();
                ++ok;
            } catch (const std::exception& ex) {
                if (failed++ < 10) std::println("  {}: {}", w.str(), ex.what());
            }
        }
    for (auto [c, n] : codecs) std::println("codec {:04X} {:<10} {}", c, wem_codec_name(c), n);
    std::println("{} streams: {} decoded ({:.0f} s), {} failed, {} silent, {} not installed/not recorded", streams, ok, seconds, failed,
                 silent, missing);
    return failed ? 1 : 0;
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

// Skinned models with their skeleton, rig and compatible clips.
int cmd_skeletons(PackageManager& pm) {
    pm.build_hash64_index();
    auto materials = index_dynamic_model_materials(pm);
    auto skeletons = index_model_skeletons(pm, materials);
    const auto clips = index_animation_clips(pm);
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> by_skeleton;
    for (const auto& [model, mr] : skeletons) by_skeleton[{mr.skeleton.value, mr.rig.value}].push_back(model);
    for (auto& [key, models] : by_skeleton) {
        size_t bones = 0, rig_bones = 0, compatible = 0;
        try { bones = load_skeleton(pm, TagHash(key.first)).size(); } catch (...) {}
        try {
            if (!TagHash(key.second).is_none()) {
                const auto rig = load_animation_rig(pm, TagHash(key.second));
                rig_bones = rig.size();
                compatible = compatible_animation_clips(pm, skeletons, rig, clips).size();
            }
        } catch (...) {}
        std::sort(models.begin(), models.end());
        std::print("{}  bones {:3}  rig {} ({:3})  clips {:4}  models", TagHash(key.first).str(), bones, TagHash(key.second).str(),
                   rig_bones, compatible);
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
        // The authored 72-byte node: hash, i16 fields, then the rest as hex.
        const uint8_t* n = rig.node_layout.data() + i * 72;
        int16_t f[4];
        std::memcpy(f, n + 4, 8);
        std::string hex;
        for (size_t k = 12; k < 72; ++k) hex += std::format("{:02X}{}", n[k], (k % 4 == 3) ? " " : "");
        std::println("      node hash {:08X} i16 ({},{},{},{})  {}", *reinterpret_cast<const uint32_t*>(n), f[0], f[1], f[2], f[3], hex);
    }
    return 0;
}

// Largest change of a bone's distance to its parent over sampled frames (bones under the root
// move freely and are skipped). A correct pose keeps it near zero.
float clip_stretch(const Skeleton& sk, const AnimationRig& rig, const AnimationClip& c, std::vector<int>* worst_bone = nullptr) {
    float stretch = 0;
    std::vector<BoneTransform> object;
    auto len = [](const auto& a, const auto& b) {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return std::sqrt(x * x + y * y + z * z);
    };
    for (uint32_t fr = 0; fr < c.frame_count; fr += std::max(1u, c.frame_count / 8)) {
        pose_skeleton(sk, rig, c, float(fr), object);
        for (size_t b = 0; b < sk.size(); ++b) {
            const int32_t p = sk.parents[b];
            if (p < 0 || sk.parents[size_t(p)] < 0) continue;
            const float e = std::fabs(len(object[b].translation, object[size_t(p)].translation) -
                                      len(sk.bind[b].translation, sk.bind[size_t(p)].translation));
            if (e > 0.02f && worst_bone) worst_bone->push_back(int(b));
            if (e > 0.1f && std::getenv("NIM_STRETCH_DEBUG")) {
                const auto& o = object[b];
                const auto& q = object[size_t(p)];
                std::println("    frame {} bone {} ({:.3f},{:.3f},{:.3f}) s{:.3f} parent {} ({:.3f},{:.3f},{:.3f}) s{:.3f}", fr, b, o.translation[0],
                             o.translation[1], o.translation[2], o.scale, p, q.translation[0], q.translation[1], q.translation[2], q.scale);
            }
            stretch = std::max(stretch, e);
        }
    }
    return stretch;
}

// Every skinned dynamic model: whether it has a skeleton, a rig and clips, and for each rig the
// share of its clips that stretch bones (a wrong rig binding or pose composition).
int cmd_animaudit(PackageManager& pm) {
    pm.build_hash64_index();
    const auto materials = index_dynamic_model_materials(pm);
    ModelAssemblyIndex assemblies;
    const auto rigs = index_model_skeletons(pm, materials, &assemblies);
    const auto clips = index_animation_clips(pm);
    size_t skinned = 0, no_skeleton = 0, no_rig = 0, no_clips = 0, assembled = 0, fitted = 0;
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> by_rig;
    std::vector<std::string> missing;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassDynamicModel) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            DynamicModel m;
            try { m = load_dynamic_model(pm, tag); } catch (...) { continue; }
            if (std::ranges::none_of(m.meshes, [](const DynamicMesh& d) { return !d.skinning.is_none(); })) continue;
            ++skinned;
            const auto it = rigs.find(tag.value);
            if (it == rigs.end()) {
                ++no_skeleton;
                if (assemblies.contains(tag.value)) ++assembled;
                // The skeleton its bind-pose skin fits (what the viewer uses).
                std::vector<std::array<float, 3>> positions;
                std::vector<std::array<uint16_t, 4>> bones;
                std::vector<std::array<float, 4>> weights;
                for (const auto& d : m.meshes) {
                    if (d.skinning.is_none()) continue;
                    try {
                        const auto vb0 = load_vertex_buffer(pm, d.vertex0);
                        const auto skin = decode_skin_weights(vb0, load_vertex_buffer(pm, d.skinning));
                        for (size_t v = 0; v < skin.bones.size(); ++v) {
                            const uint8_t* q = vb0.data.data() + v * vb0.stride;
                            int16_t c[3];
                            std::memcpy(c, q, 6);
                            positions.push_back({c[0] / 32767.0f * m.model_scale[0] + m.model_offset[0], c[1] / 32767.0f * m.model_scale[1] + m.model_offset[1],
                                                 c[2] / 32767.0f * m.model_scale[2] + m.model_offset[2]});
                        }
                        bones.insert(bones.end(), skin.bones.begin(), skin.bones.end());
                        weights.insert(weights.end(), skin.weights.begin(), skin.weights.end());
                    } catch (...) {
                    }
                }
                std::vector<SkinFit> ranking;
                const auto fit = fit_skin_to_skeleton(pm, rigs, m.bone_count, positions, bones, weights, clips, &ranking);
                if (fit) ++fitted;
                std::string ranked;
                for (size_t k = 0; k < std::min<size_t>(3, ranking.size()); ++k)
                    ranked += std::format(" {}:{:.1f}cm", ranking[k].binding.skeleton.str(), ranking[k].error * 100);
                missing.push_back(std::format("{} bones {:3}{}  {}", tag.str(), m.bone_count, assemblies.contains(tag.value) ? " (assembly part)" : "",
                                              fit ? std::format("fits skeleton {} rig {} ({:.1f} cm)", fit->binding.skeleton.str(),
                                                                fit->binding.rig.str(), fit->error * 100)
                                                  : std::format("no fit ({} candidates{})", ranking.size(), ranked)));
                continue;
            }
            if (it->second.rig.is_none()) { ++no_rig; continue; }
            by_rig[{it->second.skeleton.value, it->second.rig.value}].push_back(tag.value);
        }
    }
    size_t rigs_bad = 0;
    for (const auto& [key, models] : by_rig) {
        try {
            const Skeleton sk = load_skeleton(pm, TagHash(key.first));
            const AnimationRig rig = load_animation_rig(pm, TagHash(key.second));
            const auto matches = compatible_animation_clips(pm, rigs, rig, clips);
            if (matches.empty()) { no_clips += models.size(); continue; }
            size_t bad = 0, tested = 0;
            std::map<int, size_t> bones;
            for (size_t k = 0; k < matches.size(); k += std::max<size_t>(1, matches.size() / 60)) {
                AnimationClip c;
                try { c = load_animation_clip(pm, matches[k].tag); } catch (...) { continue; }
                std::vector<int> worst;
                ++tested;
                if (clip_stretch(sk, rig, c, &worst) > 0.02f) ++bad;
                for (int b : worst) ++bones[b];
            }
            if (bad * 10 > tested) {
                ++rigs_bad;
                std::string list;
                for (const auto& [b, n] : bones) list += std::format(" {}x{}", b, n);
                std::println("rig {} skeleton {} ({} bones, {} models e.g. {}): {}/{} clips stretch;{}", TagHash(key.second).str(),
                             TagHash(key.first).str(), sk.size(), models.size(), TagHash(models.front()).str(), bad, tested, list);
            }
        } catch (const std::exception& ex) {
            std::println("rig {}: {}", TagHash(key.second).str(), ex.what());
        }
    }
    std::println("skinned models {}: no skeleton {} ({} in assemblies, {} fitted by skin), no rig {}, rig without clips {}, rigs with stretching clips {}/{}",
                 skinned, no_skeleton, assembled, fitted, no_rig, no_clips, rigs_bad, by_rig.size());
    std::map<TagHash, size_t> roots;
    for (const auto& [model, parts] : assemblies) roots[parts.front()] = parts.size();
    std::println("assembled previews {} (root: parts)", roots.size());
    for (const auto& [root, n] : roots) std::print(" {}:{}", root.str(), n);
    std::println("");
    for (const auto& m : missing) std::println("  no skeleton: {}", m);
    return 0;
}

// Poses a model's skeleton with every compatible clip and reports, per codec, how
// far bones move from the bind pose and how low the lowest bone goes (a wrong decoder or rig
// mapping shows up as large displacements and bones far below the ground).
int cmd_clipfit(PackageManager& pm, TagHash model, bool verbose) {
    pm.build_hash64_index();
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
    std::map<std::string, std::vector<float>> moved, lowest, to_default, stretches;
    size_t additive = 0;
    std::vector<BoneTransform> object;
    for (const auto& info : compatible_animation_clips(pm, skeletons, rig, index_animation_clips(pm))) {
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
        std::vector<int> stretched;
        const float stretch = clip_stretch(sk, rig, c, &stretched);
        std::map<int, size_t> worst_count;
        for (int bone : stretched) ++worst_count[bone];
        stretches[clip_codec_name(c.codec)].push_back(stretch);
        if (verbose) {
            std::string bad;
            for (const auto& [bone, n] : worst_count) bad += std::format(" {}", bone);
            std::println("  {} {:7} frames {:4} {} max stretch {:.4f}{}", c.tag.str(), clip_codec_name(c.codec), c.frame_count,
                         is_additive(rig, c) ? "additive" : "full    ", stretch, bad);
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
        auto& st = stretches[k];
        std::sort(st.begin(), st.end());
        std::println("{:8}             max bone stretch (m) p50 {:.4f} p90 {:.4f} max {:.4f}", "", pct(st, 50), pct(st, 90),
                     st.empty() ? 0.0f : st.back());
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
            // cls FFFFFFFF: every tag of type 8 or 16 (16 holds list records such as icon entries)
            if (cls == 0xFFFFFFFF ? (e.type != 8 && e.type != 16) : (e.type != 8 || (cls && e.reference != cls))) continue;
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

int cmd_decorators(PackageManager& pm, TagHash tag) {
    const auto* entry = pm.entry(tag);
    if (!entry) throw std::runtime_error("Unknown decorator group or bubble: " + tag.str());
    std::vector<TagHash> groups;
    if (entry->reference == kClassDecoratorGroup) groups.push_back(tag);
    else if (entry->reference == kClassBubbleParent) groups = load_map_layout(pm, tag).decorator_groups;
    else throw std::runtime_error("Expected a decorator group or bubble: " + tag.str());

    for (TagHash group : groups) {
        const auto placements = load_decorator_group(pm, group);
        const auto model = load_dynamic_model(pm, placements.model);
        for (const auto& mesh : model.meshes) {
            for (const auto& part : mesh.parts)
                if (size_t(part.external_identifier) + 1 >= placements.part_offsets.size())
                    throw std::runtime_error("Missing decorator instance range for mesh part: " + placements.model.str());
        }
        const BinaryReader instances(placements.instances.data);
        float max_outside = 0;
        size_t outside = 0;
        for (const auto& cluster : placements.clusters) {
            const size_t end = size_t(cluster.first_instance) + cluster.instance_count;
            for (size_t i = cluster.first_instance; i < end; ++i) {
                float distance = 0;
                for (size_t axis = 0; axis < 3; ++axis) {
                    const float value = float(instances.read_at<int16_t>(i * 16 + 2 * axis));
                    const float position = std::max(-1.0f, value / 32767.0f) * placements.decode[0][axis] + placements.decode[1][axis];
                    if (!std::isfinite(position))
                        throw std::runtime_error("Invalid decorator instance position: " + group.str());
                    distance = std::max({distance, cluster.min[axis] - position, position - cluster.max[axis]});
                }
                max_outside = std::max(max_outside, distance);
                outside += distance > 0.03f;
            }
        }
        const auto [min_distance, max_distance] = std::ranges::minmax_element(
            placements.part_distance_thresholds, {}, [](const float4& v) { return v.back(); });
        std::println("{} model {} stream {} | {} instances, {} parts, {} clusters | final distance thresholds {:.1f} to {:.1f} | {} origins outside bounds by >3 cm (max {:.3f} m)",
            group.str(), placements.model.str(), placements.instances.data_tag.str(), placements.part_offsets.back(),
            placements.part_distance_thresholds.size(), placements.clusters.size(), min_distance->back(), max_distance->back(),
            outside, max_outside);
    }
    std::println("{} decorator groups", groups.size());
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
    for (const auto& sp : m.special_parts)
        std::println("  special stage {:>2} layout {:>3} lod {} prim {} ib {} vb0 {} vb1 {} color {} idx [{}, +{}) tech {}", sp.render_stage, sp.input_layout,
                     sp.lod_category, sp.primitive_type, sp.index.str(), sp.vertex0.str(), sp.vertex1.str(), sp.color.str(), sp.index_start, sp.index_count,
                     sp.technique.str());
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
    std::println("3D noise candidate: {} (render globals +0x40; Frame +0xD8 binding inferred)", g.atmosphere_noise.str());
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

// Techniques whose used_scopes has bit `bit` set (22 = particle_transforms), then one pass over
// every tag counting which classes reference them (the asset classes that own those techniques).
int cmd_scope_survey(PackageManager& pm, unsigned bit) {
    pm.build_hash64_index();
    std::set<uint32_t> hits;
    size_t total = 0;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference != kClassTechnique) continue;
            const TagHash tag = TagHash::make(info.pkg_id, uint16_t(i));
            ++total;
            try {
                const Technique t = load_technique(pm, tag);
                if (t.used_scopes >> bit & 1) hits.insert(tag.value);
            } catch (...) {}
        }
    }
    std::println("scope bit {}: {} of {} techniques", bit, hits.size(), total);
    size_t shown = 0;
    for (uint32_t t : hits)
        if (shown++ < 12) std::println("  technique {:08X}", t);
    std::map<uint32_t, std::pair<size_t, std::vector<uint32_t>>> classes;  // class -> (tags, examples)
    std::map<uint32_t, std::set<size_t>> offsets;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type != 8 || e.reference == kClassTechnique) continue;
            const TagHash t = TagHash::make(info.pkg_id, uint16_t(i));
            std::vector<uint8_t> d;
            try { d = pm.read(t); } catch (...) { continue; }
            bool hit = false;
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                uint32_t v;
                std::memcpy(&v, d.data() + o, 4);
                if (hits.count(v)) {
                    hit = true;
                    if (offsets[e.reference].size() < 8) offsets[e.reference].insert(o);
                }
            }
            if (hit) {
                auto& c = classes[e.reference];
                if (c.first++ < 4) c.second.push_back(t.value);
            }
        }
    }
    for (const auto& [cls, c] : classes) {
        std::print("  class {:08X} tags {:6} e.g.", cls, c.first);
        for (uint32_t t : c.second) std::print(" {:08X}", t);
        std::print("  offsets");
        for (size_t o : offsets[cls]) std::print(" +0x{:X}", o);
        std::println("");
    }
    return 0;
}

// Which classes reference tags of class `cls` (by tag32, or by hash64 when `wide`), with offsets.
int cmd_class_refs(PackageManager& pm, uint32_t cls) {
    std::set<uint32_t> tags;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i)
            if (p.entries()[i].type == 8 && p.entries()[i].reference == cls) tags.insert(TagHash::make(info.pkg_id, uint16_t(i)).value);
    }
    std::set<uint64_t> wide;
    for (const auto& info : pm.packages())
        for (const auto& h : pm.open(info).hash64s())
            if (tags.count(h.hash32.value)) wide.insert(h.hash64);
    std::println("class {:08X}: {} tags, {} with hash64", cls, tags.size(), wide.size());
    struct Use { size_t tags = 0, wide = 0; std::vector<uint32_t> examples; std::set<size_t> offsets; };
    std::map<uint32_t, Use> classes;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if ((e.type != 8 && e.type != 16) || e.reference == cls) continue;
            std::vector<uint8_t> d;
            try { d = p.read_entry(i); } catch (...) { continue; }
            bool hit = false, hit_wide = false;
            Use& u = classes[e.type == 8 ? e.reference : 0x10000000u | e.reference];
            for (size_t o = 0; o + 4 <= d.size(); o += 4) {
                uint32_t v; std::memcpy(&v, d.data() + o, 4);
                bool h = (v & 0x80000000u) && tags.count(v);
                if (!h && o + 8 <= d.size() && !wide.empty()) {
                    uint64_t w; std::memcpy(&w, d.data() + o, 8);
                    if (wide.count(w)) h = hit_wide = true;
                }
                if (h) { hit = true; if (u.offsets.size() < 6) u.offsets.insert(o); }
            }
            if (hit) { ++u.tags; u.wide += hit_wide; if (u.examples.size() < 4) u.examples.push_back(TagHash::make(info.pkg_id, uint16_t(i)).value); }
        }
    }
    for (const auto& [c, u] : classes) {
        if (!u.tags) continue;
        std::print("  class {:08X} tags {:6} (hash64 {:5}) e.g.", c, u.tags, u.wide);
        for (uint32_t t : u.examples) std::print(" {:08X}", t);
        std::print("  offsets");
        for (size_t o : u.offsets) std::print(" +0x{:X}", o);
        std::println("");
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
        if (const char* lang = std::getenv("NIM_LANGUAGE")) pm.set_language(uint16_t(std::atoi(lang)));  // localized packages
        const std::string& cmd = args[0];
        if (cmd == "pkgs") return cmd_pkgs(pm);
        if (cmd == "ls" && args.size() >= 2) return cmd_ls(pm, uint16_t(parse_hex(args[1])));
        if (cmd == "types") return cmd_types(pm);
        if (cmd == "survey-entities") return cmd_survey_entities(pm);
        if (cmd == "read" && args.size() >= 2)
            return cmd_read(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "grep" && args.size() >= 2) return cmd_grep(pm, args[1], args.size() >= 3 ? std::stoi(args[2]) : -1);
        if (cmd == "strings")
            return cmd_strings(pm, args.size() >= 2 && args[1] != "-" ? args[1].c_str() : nullptr,
                               args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "codex-probe") return cmd_codex_probe(pm, args.size() >= 2 ? size_t(std::stoul(args[1])) : 3);
        if (cmd == "codex") return cmd_codex(pm, args.size() >= 2 ? args[1].c_str() : nullptr, args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "dialogue") return cmd_dialogue(pm, args.size() >= 2 ? args[1].c_str() : nullptr);
        if (cmd == "wem-audit") return cmd_wem_audit(pm, args.size() >= 2 ? std::stoul(args[1]) : 0);
        if (cmd == "movies") return cmd_movies(pm, args.size() >= 2 && args[1] == "-v");
        if (cmd == "movie-video" && args.size() >= 3)
            return cmd_movie_video(pm, TagHash(parse_hex(args[1])), args[2].c_str(), args.size() >= 4 ? std::stoull(args[3]) << 20 : ~0ull);
        if (cmd == "movie-audio" && args.size() >= 3)
            return cmd_movie_audio(pm, TagHash(parse_hex(args[1])), args[2].c_str(), args.size() >= 4 ? std::stoi(args[3]) : 0);
        if (cmd == "movie-playcheck" && args.size() >= 2)
            return cmd_movie_playcheck(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? std::stoi(args[2]) : 0,
                                       args.size() >= 4 ? std::stod(args[3]) : 0.0);
        if (cmd == "movie-frames" && args.size() >= 3)
            return cmd_movie_frames(pm, TagHash(parse_hex(args[1])), args[2], args.size() >= 4 ? std::stod(args[3]) : 0,
                                    args.size() >= 5 ? std::stoi(args[4]) : 60);
        if (cmd == "wem" && args.size() >= 3) return cmd_wem(pm, TagHash(parse_hex(args[1])), args[2].c_str());
        if (cmd == "xref-strings") return cmd_xref_strings(pm);
        if (cmd == "codex-art") return cmd_codex_art(pm);
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
        if (cmd == "clipfit" && args.size() >= 2) return cmd_clipfit(pm, TagHash(parse_hex(args[1])), args.size() >= 3 && args[2] == "-v");
        if (cmd == "rig" && args.size() >= 2) return cmd_rig(pm, TagHash(parse_hex(args[1])));
        if (cmd == "skeletons") return cmd_skeletons(pm);
        if (cmd == "animaudit") return cmd_animaudit(pm);
        if (cmd == "clip" && args.size() >= 2) return cmd_clip(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? std::stoi(args[2]) : 0);
        if (cmd == "clips") return cmd_clips(pm, args.size() >= 2 ? uint32_t(std::stoul(args[1])) : 0);
        if (cmd == "skin" && args.size() >= 2) return cmd_skin(pm, TagHash(parse_hex(args[1])));
        if (cmd == "components") return cmd_components(pm, args.size() >= 2 ? parse_hex(args[1]) : 0);
        if (cmd == "crawl" && args.size() >= 2)
            return cmd_crawl(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? std::stoi(args[2]) : 8, args.size() >= 4 ? parse_hex(args[3]) : 0);
        if (cmd == "maps") return cmd_maps(pm);
        if (cmd == "map-near" && args.size() >= 5)
            return cmd_map_near(pm, TagHash(parse_hex(args[1])), std::stof(args[2]), std::stof(args[3]), std::stof(args[4]));
        if (cmd == "map-sky" && args.size() >= 2) return cmd_map_sky(pm, TagHash(parse_hex(args[1])));
        if (cmd == "stage-survey" && args.size() >= 2) return cmd_stage_survey(pm, std::stoul(args[1]));
        if (cmd == "scope-survey" && args.size() >= 2) return cmd_scope_survey(pm, unsigned(std::stoul(args[1])));
        if (cmd == "map-fx" && args.size() >= 2)
            return cmd_map_fx(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? TagHash(parse_hex(args[2])) : TagHash{});
        if (cmd == "class-refs" && args.size() >= 2) return cmd_class_refs(pm, parse_hex(args[1]));
        if (cmd == "activities") {
            for (const auto& m : list_activity_maps(pm)) {
                std::println("{} map[{}] bubble {} name {:08X} {}", m.activity.str(), m.activity_map, m.bubble.str(), m.bubble_name, m.name);
                if (args.size() >= 2 && args[1] == "--verify") {
                    const auto tables = load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map);
                    const auto layout = load_map_layout(pm, m.bubble, tables);
                    const auto probes = load_map_cubemaps(pm, layout);
                    std::println("  verified: {} phase/ambient tables, {} combined tables, {} instances, {} probes",
                                 tables.size(), layout.node_tables.size(), layout.instances, probes.size());
                }
                if (args.size() >= 2 && args[1] == "--starts") {
                    const auto layout = load_map_layout(pm, m.bubble, load_activity_node_tables(pm, m.activity, m.bubble_name, m.activity_map));
                    for (const auto& s : load_map_starts(pm, layout))
                        std::println("  start {:08X} ({:.1f} {:.1f} {:.1f}) facing ({:.2f} {:.2f} {:.2f})", s.name, s.position[0], s.position[1],
                                     s.position[2], s.forward()[0], s.forward()[1], s.forward()[2]);
                }
            }
            return 0;
        }
        if (cmd == "map-entity-models" && args.size() >= 2) return cmd_map_entity_models(pm, TagHash(parse_hex(args[1])));
        if (cmd == "map-placements" && args.size() >= 2) return cmd_map_placements(pm, TagHash(parse_hex(args[1])));
        if (cmd == "h64-graph" && args.size() >= 2) return cmd_h64_graph(pm, args[1]);
        if (cmd == "motion-probe" && args.size() >= 2) return cmd_motion_probe(pm, TagHash(parse_hex(args[1])));
        if (cmd == "bs-modules") return cmd_bs_modules(pm, args.size() >= 2 ? args[1] : std::string());
        if (cmd == "ai-sim" && args.size() >= 2) {
            float seconds = 60, dt = 0.1f;
            std::array<float, 3> player{};
            bool visible = false;
            uint64_t seed = 1;
            const char* json = nullptr;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--seconds" && i + 1 < args.size()) seconds = std::stof(args[++i]);
                else if (args[i] == "--dt" && i + 1 < args.size()) dt = std::stof(args[++i]);
                else if (args[i] == "--seed" && i + 1 < args.size()) seed = std::stoull(args[++i]);
                else if (args[i] == "--visible") visible = true;
                else if (args[i] == "--json" && i + 1 < args.size()) json = args[++i].c_str();
                else if (args[i] == "--player" && i + 1 < args.size()) {
                    if (std::sscanf(args[++i].c_str(), "%f,%f,%f", &player[0], &player[1], &player[2]) != 3) throw std::runtime_error("--player x,y,z");
                }
            }
            return cmd_ai_sim(pm, TagHash(parse_hex(args[1])), seconds, dt, player, visible, seed, json);
        }
        if (cmd == "map-gameplay" && args.size() >= 2) {
            bool verbose = false;
            const char* json = nullptr;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "-v") verbose = true;
                else if (args[i] == "--json" && i + 1 < args.size()) json = args[++i].c_str();
            }
            return cmd_map_gameplay(pm, TagHash(parse_hex(args[1])), verbose, json);
        }
        if (cmd == "map-entities" && args.size() >= 2) return cmd_map_entities(pm, TagHash(parse_hex(args[1])));
        if (cmd == "map-components" && args.size() >= 2)
            return cmd_map_components(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? parse_hex(args[2]) : 0,
                                      args.size() >= 4 ? TagHash(parse_hex(args[3])) : TagHash{});
        if (cmd == "map-shaders" && args.size() >= 2) return cmd_map_shaders(pm, TagHash(parse_hex(args[1])));
        if (cmd == "weather" && args.size() >= 2) return cmd_weather(pm, TagHash(parse_hex(args[1])));
        if (cmd == "texraw" && args.size() >= 2) return cmd_texraw(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2].c_str() : nullptr);
        if (cmd == "lights" && args.size() >= 2) return cmd_lights(pm, TagHash(parse_hex(args[1])));
        if (cmd == "cubemaps" && args.size() >= 2) return cmd_cubemaps(pm, TagHash(parse_hex(args[1])));
        if (cmd == "probe-audit" && args.size() >= 2) return cmd_probe_audit(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? args[2] : "");
        if (cmd == "environment" && args.size() >= 2)
            return cmd_environment(pm, TagHash(parse_hex(args[1])), args.size() >= 3 && args[2] != "-v" ? TagHash(parse_hex(args[2])) : TagHash{}, args.back() == "-v");
        if (cmd == "probe-search" && args.size() >= 2) return cmd_probe_search(pm, args[1]);
        if (cmd == "probe-gpu-audit" && args.size() >= 2) return cmd_probe_gpu_audit(pm, args[1]);
        if (cmd == "world-channel-names" && args.size() >= 2) return cmd_world_channel_names(pm, args[1]);
        if (cmd == "nodes" && args.size() >= 2)
            return cmd_nodes(pm, TagHash(parse_hex(args[1])), args.size() >= 3 ? parse_hex(args[2]) : 0);
        if (cmd == "refs64" && args.size() >= 2) return cmd_refs64(pm, TagHash(parse_hex(args[1])));
        if (cmd == "decorators" && args.size() >= 2) return cmd_decorators(pm, TagHash(parse_hex(args[1])));
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
