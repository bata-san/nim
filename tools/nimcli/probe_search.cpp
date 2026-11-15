// Independent install-wide probe search. Reports candidates rather than declaring unknown
// textures to be probes. No texture pixels, game processes, or GPU captures are read.
#include "nim/content/tfx.hpp"
#include "nim/core/tag_reader.hpp"
#include "nim/content/map.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/weather.hpp"
#include "nim/content/object_channels.hpp"
#include <fstream>
#include <sstream>
#include <set>
#include <print>

using namespace nim;
namespace fs = std::filesystem;

int cmd_world_channel_names(PackageManager& pm, const std::string& output) {
    const auto registry = load_world_channel_registry(pm);
    const auto raw = pm.read(registry.tag);
    TagReader rr(raw);
    std::map<uint32_t, std::vector<std::pair<const char*, uint32_t>>> targets;
    for (const auto [field, bank] : {std::pair{size_t(8), "first"}, std::pair{size_t(0x18), "second"}}) {
        const auto v = rr.vec_at(field);
        for (uint64_t i = 0; i < v.count; ++i)
            targets[rr.u32_at(v.data + size_t(i) * 8)].push_back({bank, rr.u32_at(v.data + size_t(i) * 8 + 4)});
    }
    std::ofstream report(output);
    if (!report) throw std::runtime_error("cannot create world channel name report");
    report << "bank\tslot\thash\tname_candidate\tsource\tclass\toffset\n";
    std::set<std::pair<uint32_t, std::string>> seen;
    for (const auto& info : pm.packages()) {
        if (info.language != 0) continue;
        auto& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const auto& e = p.entries()[i];
            if (e.type != 8) continue;
            const auto data = p.read_entry(i);
            size_t start = 0;
            auto emit = [&](size_t end) {
                if (end - start < 3 || end - start > 256) return;
                const std::string word(reinterpret_cast<const char*>(data.data() + start), end - start);
                const uint32_t hash = fnv1(word);
                const auto t = targets.find(hash);
                if (t == targets.end() || !seen.insert({hash, word}).second) return;
                for (const auto& [bank, slot] : t->second)
                    report << std::format("{}\t{}\t{:08X}\t{}\t{}\t{:08X}\t{:X}\n", bank, slot, hash, word,
                        TagHash::make(info.pkg_id, uint16_t(i)).str(), e.reference, start);
            };
            for (size_t o = 0; o <= data.size(); ++o) {
                const auto c = o < data.size() ? data[o] : 0;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') continue;
                emit(o);
                start = o + 1;
            }
        }
        p.clear_block_cache();
    }
    report.flush();
    if (!report) throw std::runtime_error("world channel report write failed");
    std::println("world channel names: {} hash/name candidates matched against {} registry hashes", seen.size(), targets.size());
    return 0;
}

int cmd_probe_gpu_audit(PackageManager& pm, const std::string& output) {
    std::ofstream report(output);
    if (!report) throw std::runtime_error("cannot create GPU probe report");
    report.exceptions(std::ios::badbit | std::ios::failbit);
    report << "bubble\tpackage\tauthored\tprepared\tdiffuse\tsingle_z\trelighting\ttextures\tfiltered_cubes\n";
    Gpu gpu;
    gpu.init(false);
    GameLighting lighting;
    lighting.init(pm, gpu);
    size_t total = 0;
    auto maps = list_maps(pm);
    // Revisit a map after every other map to exercise resource replacement, not just first load.
    if (!maps.empty()) maps.push_back(maps.front());
    for (const auto& entry : maps) {
        const auto layout = load_map_layout(pm, entry.bubble);
        const auto probes = load_map_cubemaps(pm, layout);
        std::vector<const MapCubemap*> ptrs;
        std::set<uint32_t> textures, cubes;
        size_t diffuse = 0, single_z = 0, relighting = 0;
        for (const auto& p : probes) {
            ptrs.push_back(&p);
            for (auto t : {p.specular, p.alpha, p.diffuse}) if (!t.is_none()) textures.insert(t.value);
            if (!p.specular.is_none()) cubes.insert(p.specular.value);
            diffuse += p.use_probes();
            single_z += p.use_probes() && p.probes[2] == 1;
            relighting += p.use_relighting();
        }
        lighting.set_cubemaps(pm, gpu, ptrs);
        if (lighting.cubemap_count() != probes.size() || lighting.cubemaps_requested() != probes.size()) {
            for (const auto& why : lighting.cubemap_notes()) std::println("{}", why);
            throw std::runtime_error("GPU audit: authored probe omitted during preparation");
        }
        if (lighting.probe_texture_count() != textures.size() || lighting.filtered_cube_count() != cubes.size() || lighting.probe_shape_count())
            throw std::runtime_error("GPU audit: resources from another map survived replacement");
        report << std::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", entry.bubble.str(), entry.package,
            probes.size(), lighting.cubemap_count(), diffuse, single_z, relighting, lighting.probe_texture_count(), lighting.filtered_cube_count());
        report.flush();
        std::println("GPU probe audit {}: {}/{} ready, {} diffuse grids ({} single Z), {} relighting, {} current textures",
            entry.bubble.str(), lighting.cubemap_count(), probes.size(), diffuse, single_z, relighting, lighting.probe_texture_count());
        std::fflush(stdout);
        total += probes.size();
        for (const auto& info : pm.packages()) pm.open(info).clear_block_cache();
    }
    lighting.clear_cubemaps(gpu);
    if (lighting.probe_texture_count() || lighting.filtered_cube_count() || lighting.probe_shape_count() || lighting.cubemap_count())
        throw std::runtime_error("GPU audit: clear retained probe resources");
    gpu.check_device();
    lighting.release(gpu);
    std::println("GPU probe audit passed: {} map selections, {} prepared occurrences; final cache empty", maps.size(), total);
    return 0;
}

int cmd_probe_search(PackageManager& pm, const std::string& output) {
    fs::create_directories(output);
    auto file = [&](const char* name, const char* header) {
        std::ofstream f(fs::path(output) / name);
        if (!f) throw std::runtime_error("cannot create probe search report");
        f << header << '\n';
        f.exceptions(std::ios::badbit | std::ios::failbit);
        return f;
    };
    auto errors = file("errors.tsv", "phase\tpackage\ttag\terror");
    auto textures = file("textures.tsv", "tag\tpackage\tsubtype\tformat\twidth\theight\tdepth\tarray\tflags\tcandidate\tknown_roles\traw_references");
    auto records = file("records.tsv", "source\tpackage\tsource_class\toffset\tgrid_x\tgrid_y\tgrid_z\tspecular\talpha\tdiffuse\tgrid_matches_texture");
    auto refs = file("references.tsv", "source\tpackage\tsource_class\toffset\tencoding\ttarget");
    auto uses = file("channel_uses.tsv", "technique\tpackage\tstage\topcode\tindex\tinstruction");
    auto classes = file("classes.tsv", "class\tentries\tbytes\tsample");
    struct Header {
        std::string package;
        uint32_t format = 0, flags = 0, subtype = 0;
        uint16_t w = 0, h = 0, d = 0, a = 0;
        bool candidate = false;
        std::set<std::string> roles;
        size_t references = 0;
    };
    std::map<uint32_t, Header> headers;
    std::unordered_map<uint64_t, uint32_t> wide;
    std::vector<const PackageInfo*> packages;
    size_t skipped_localized = 0, failed = 0, entries = 0, record_count = 0, candidates = 0, technique_count = 0;
    auto error = [&](const char* phase, const PackageInfo& info, TagHash tag, const std::exception& e) {
        errors << std::format("{}\t{}\t{}\t{}\n", phase, info.name, tag.str(), e.what());
        ++failed;
    };
    for (const auto& info : pm.packages()) {
        if (info.language != 0) { ++skipped_localized; continue; }
        packages.push_back(&info);
        auto& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const auto& e = p.entries()[i];
            if (e.type != 32 || e.subtype < 1 || e.subtype > 3) continue;
            const auto tag = TagHash::make(info.pkg_id, uint16_t(i));
            try {
                const auto data = p.read_entry(i);
                TagReader r(data);
                if (r.u16_at(0x20) != 0xCAFE) throw std::runtime_error("missing texture header marker");
                Header h;
                h.package = info.name;
                h.format = r.u32_at(4); h.flags = r.u8_at(0x2C); h.subtype = e.subtype;
                h.w = r.u16_at(0x22); h.h = r.u16_at(0x24); h.d = r.u16_at(0x26); h.a = r.u16_at(0x28);
                // Include the one-slice volumes and unrelated LUT/noise candidates too.
                h.candidate = e.subtype == 3 || h.d > 1 || h.a >= 6 || (h.w && h.w % 7 == 0);
                candidates += h.candidate;
                headers.emplace(tag.value, std::move(h));
            } catch (const std::exception& ex) { error("texture", info, tag, ex); }
        }
        p.clear_block_cache();
    }
    for (const auto* info : packages)
        for (const auto& h : pm.open(*info).hash64s())
            if (const auto it = headers.find(h.hash32.value); it != headers.end() && it->second.candidate)
                wide[h.hash64] = h.hash32.value;
    std::println("probe search: {} neutral packages, {} texture headers, {} broad candidates; {} localized packages excluded",
                 packages.size(), headers.size(), candidates, skipped_localized);
    std::fflush(stdout);
    struct Class { size_t entries = 0; uint64_t bytes = 0; TagHash sample; };
    std::map<uint32_t, Class> class_counts;
    size_t package_index = 0;
    for (const auto* info : packages) {
        auto& p = pm.open(*info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const auto& e = p.entries()[i];
            if (e.type != 8) continue;
            const auto tag = TagHash::make(info->pkg_id, uint16_t(i));
            auto& cls = class_counts[e.reference];
            ++cls.entries; cls.bytes += e.file_size; cls.sample = tag;
            try {
                const auto data = p.read_entry(i);
                TagReader r(data);
                ++entries;
                for (size_t o = 0; o + 4 <= data.size(); o += 4) {
                    auto emit = [&](uint32_t target, const char* encoding) {
                        auto& h = headers.at(target);
                        ++h.references;
                        refs << std::format("{}\t{}\t{:08X}\t{:X}\t{}\t{:08X}\n", tag.str(), info->name, e.reference, o, encoding, target);
                    };
                    const uint32_t v = r.u32_at(o);
                    if (const auto it = headers.find(v); it != headers.end() && it->second.candidate) emit(v, "raw32");
                    if (o + 8 <= data.size())
                        if (const auto it = wide.find(r.u64_at(o)); it != wide.end()) emit(it->second, "raw64");
                    // Layout signature, independent of source class or map family. A record
                    // must have its serialized size and an actual cube or matching diffuse grid.
                    if (v != 0x1D0 || o + 0x1D0 > data.size() || r.u64_at(o) != 0x1D0) continue;
                    const auto gx = r.u32_at(o + 0x160), gy = r.u32_at(o + 0x164), gz = r.u32_at(o + 0x168);
                    const auto spec = r.u32_at(o + 0x1B8), alpha = r.u32_at(o + 0x1BC), diff = r.u32_at(o + 0x1C0);
                    const auto s = headers.find(spec), d = headers.find(diff);
                    const bool match = d != headers.end() && gx && gy && gz && uint64_t(gx) * 7 == d->second.w && gy == d->second.h && gz == d->second.d;
                    if (!match && (s == headers.end() || s->second.a != 6)) continue;
                    records << std::format("{}\t{}\t{:08X}\t{:X}\t{}\t{}\t{}\t{:08X}\t{:08X}\t{:08X}\t{}\n",
                        tag.str(), info->name, e.reference, o, gx, gy, gz, spec, alpha, diff, match);
                    ++record_count;
                    if (s != headers.end()) s->second.roles.insert("specular");
                    if (d != headers.end()) d->second.roles.insert("diffuse");
                    if (auto a = headers.find(alpha); a != headers.end()) a->second.roles.insert("alpha");
                }
                if (e.reference == kClassTechnique) {
                    const auto t = load_technique(pm, tag);
                    ++technique_count;
                    for (size_t stage = 0; stage < t.stages.size(); ++stage) {
                        std::istringstream listing(disassemble_tfx(t.stages[stage].bytecode));
                        std::string line;
                        while (std::getline(listing, line)) {
                            const auto scalar = line.find(": push_global_channel_ ");
                            const auto vector = line.find(": push_global_channel ");
                            const auto at = scalar != std::string::npos ? scalar : vector;
                            if (at == std::string::npos) continue;
                            const auto value = std::stoul(line.substr(line.find_last_of(' ') + 1), nullptr, 16);
                            uses << std::format("{}\t{}\t{}\t{}\t{}\t{}\n", tag.str(), info->name, stage, scalar != std::string::npos ? "64" : "63", value, line);
                        }
                    }
                }
            } catch (const std::exception& ex) { error("structure", *info, tag, ex); }
        }
        p.clear_block_cache();
        if (++package_index % 20 == 0) {
            std::println("probe search: {}/{} packages, {} structures, {} record occurrences, {} errors", package_index, packages.size(), entries, record_count, failed);
            std::fflush(stdout);
        }
    }
    size_t unowned = 0;
    for (const auto& [tag, h] : headers) {
        std::string roles;
        for (const auto& role : h.roles) roles += (roles.empty() ? "" : ",") + role;
        unowned += h.candidate && h.roles.empty();
        textures << std::format("{:08X}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{:02X}\t{}\t{}\t{}\n",
            tag, h.package, h.subtype, h.format, h.w, h.h, h.d, h.a, h.flags, h.candidate, roles, h.references);
    }
    for (const auto& [cls, c] : class_counts) classes << std::format("{:08X}\t{}\t{}\t{}\n", cls, c.entries, c.bytes, c.sample.str());
    auto summary = file("summary.txt", "Scope: installed neutral packages, all type32/subtype1..3 texture headers and type8 structures. Raw aligned references are candidates, not validated ownership.");
    summary << std::format("packages={} excluded_localized={} textures={} candidates={} structures={} record_occurrences={} techniques={} candidates_without_known_probe_role={} errors={}\n",
        packages.size(), skipped_localized, headers.size(), candidates, entries, record_count, technique_count, unowned, failed);
    summary << "Limits: signature covers the known 0x1D0 layout under any class. Other packed buffers, runtime SH grids, and non-texture representations require separate analysis. Counts include repeated inline/collection records.\n";
    std::println("probe search complete: {} candidate textures without a known probe record role, {} errors; reports in {}", unowned, failed, output);
    return failed ? 1 : 0;
}
