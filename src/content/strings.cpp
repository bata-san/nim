#include "nim/content/strings.hpp"
#include "nim/core/tag_reader.hpp"

#include <format>
#include <unordered_set>

namespace nim {
namespace {

constexpr uint32_t kFnvEmpty = 0x811C9DC5;  // FNV-1 of "", used as "no hash"

// Placeholder parts carry no bytes; the game substitutes them at runtime.
std::string placeholder(uint16_t kind, uint32_t hash, StringDecodeStats* stats) {
    switch (kind >> 8) {
    case 0x01: if (stats) stats->arguments++; return std::format("{{{}}}", kind & 0xFF);
    case 0x02: if (stats) stats->variables++; return std::format("{{var:{:08X}}}", hash);
    case 0x03: if (stats) stats->input_glyphs++; return std::format("{{input:{:08X}}}", hash);
    case 0x05: if (stats) stats->values++; return std::format("{{value:{:08X}}}", hash);
    default: if (stats) stats->unknown_kinds++; return std::format("{{kind_{:04X}:{:08X}}}", kind, hash);
    }
}

} // namespace

std::string_view language_code(TextLanguage l) {
    static constexpr std::string_view codes[] = {"en", "ja", "de", "fr", "es", "es-mx", "it",
                                                 "ko", "zh-cht", "zh-chs", "pt-br", "pl", "ru"};
    return codes[size_t(l)];
}

std::vector<std::string> decode_string_data(std::span<const uint8_t> tag, StringDecodeStats* stats) {
    TagReader r(tag);
    auto parts = r.vec_at(0x08);
    auto combos = r.vec_at(0x38);
    std::vector<std::string> out;
    out.reserve(size_t(combos.count));
    for (uint64_t c = 0; c < combos.count; ++c) {
        size_t co = combos.data + size_t(c) * 0x10;
        size_t first_part = r.pointer_at(co);
        int64_t part_count = int64_t(r.u64_at(co + 8));
        std::string s;
        for (int64_t p = 0; p < part_count; ++p) {
            size_t po = first_part + size_t(p) * 0x20;
            if (po + 0x20 > parts.data + parts.count * 0x20)
                throw std::runtime_error("string part out of range");
            uint16_t kind = r.u16_at(po + 0x18);
            if (stats) stats->parts++;
            if (kind != kStringPartText) {
                uint32_t hash = r.u32_at(po + 0x10);
                s += placeholder(kind, hash == kFnvEmpty ? 0 : hash, stats);
                continue;
            }
            size_t data = r.pointer_at(po + 0x08);
            uint16_t byte_length = r.u16_at(po + 0x14);
            if (data + byte_length > r.size()) throw std::runtime_error("string bytes out of range");
            s.append(reinterpret_cast<const char*>(r.span().data() + data), byte_length);
        }
        out.push_back(std::move(s));
    }
    return out;
}

void StringTable::load_all(PackageManager& pm) {
    strings_.clear();
    by_hash_.clear();
    stats_ = {};
    std::unordered_set<TagHash> referenced;
    for (const auto& info : pm.packages()) {
        if (info.language != 0) continue;  // text lives in language-neutral packages
        Package& pkg = pm.open(info);
        for (uint32_t i = 0; i < pkg.entries().size(); ++i) {
            const Entry& e = pkg.entries()[i];
            if (e.type != 8 || e.reference != kClassLocalizedStrings) continue;
            stats_.containers++;
            TagHash container = TagHash::make(info.pkg_id, uint16_t(i));
            try {
                auto bytes = pkg.read_entry(i);
                TagReader r(bytes);
                auto hashes = r.vec_at(0x08);
                size_t base = strings_.size();
                strings_.resize(base + size_t(hashes.count));
                for (uint64_t h = 0; h < hashes.count; ++h) {
                    auto& ls = strings_[base + size_t(h)];
                    ls.hash = r.u32_at(hashes.data + size_t(h) * 4);
                    ls.container = container;
                }
                for (size_t lang = 0; lang < kTextLanguageCount; ++lang) {
                    TagHash data_tag = r.tag_at(0x18 + lang * 4);
                    if (data_tag.is_none() || !pm.entry(data_tag)) continue;
                    stats_.data_tags_referenced++;
                    referenced.insert(data_tag);
                    auto texts = decode_string_data(pm.read(data_tag), &stats_.decode);
                    for (size_t k = 0; k < texts.size() && k < hashes.count; ++k)
                        strings_[base + k].text[lang] = std::move(texts[k]);
                }
            } catch (const std::exception&) {
                stats_.containers_failed++;
            }
        }
    }
    // Completeness check: every string-data tag in the install should be reachable from a container.
    for (const auto& info : pm.packages()) {
        Package& pkg = pm.open(info);
        for (uint32_t i = 0; i < pkg.entries().size(); ++i) {
            const Entry& e = pkg.entries()[i];
            if (e.type != 8 || e.reference != kClassStringData) continue;
            stats_.data_tags_total++;
            if (!referenced.contains(TagHash::make(info.pkg_id, uint16_t(i)))) {
                stats_.orphan_data_tags.push_back(TagHash::make(info.pkg_id, uint16_t(i)));
            }
        }
    }
    for (size_t i = 0; i < strings_.size(); ++i) {
        by_hash_.emplace(strings_[i].hash, i);
        for (size_t l = 0; l < kTextLanguageCount; ++l)
            if (!strings_[i].text[l].empty()) stats_.non_empty[l]++;
    }
    stats_.strings = strings_.size();
    stats_.unique_hashes = by_hash_.size();
}

const LocalizedString* StringTable::find(uint32_t hash) const {
    auto it = by_hash_.find(hash);
    return it == by_hash_.end() ? nullptr : &strings_[it->second];
}

} // namespace nim
