#include "nim/content/codex.hpp"
#include "nim/core/tag_reader.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace nim {

std::vector<CodexDefinition> parse_codex_definitions(std::span<const uint8_t> tag) {
    TagReader r(tag);
    const auto defs = r.vec_at(0x08);
    if (defs.count && defs.element_class != kClassCodexDefinition)
        throw std::runtime_error(std::format("codex: unexpected element class {:08X}", defs.element_class));
    std::vector<CodexDefinition> out;
    out.reserve(size_t(defs.count));
    for (uint64_t i = 0; i < defs.count; ++i) {
        const size_t e = defs.data + size_t(i) * 0x18;
        CodexDefinition d;
        d.hash = r.u32_at(e);
        d.kind = r.u32_at(e + 4);
        const auto comps = r.vec_at(e + 8);
        for (uint64_t k = 0; k < comps.count; ++k) {
            const size_t p = comps.data + size_t(k) * 8;
            const size_t target = r.pointer_at(p);
            if (target < 4 || target >= tag.size()) throw std::runtime_error("codex: component pointer out of range");
            d.components.push_back({r.u32_at(target - 4), target});
        }
        out.push_back(std::move(d));
    }
    return out;
}

namespace {

std::optional<TagHash> find_single(PackageManager& pm, uint32_t cls) {
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type == 8 && e.reference == cls) return TagHash::make(info.pkg_id, uint16_t(i));
        }
    }
    return std::nullopt;
}

std::vector<TagHash> find_all(PackageManager& pm, uint32_t cls) {
    std::vector<TagHash> out;
    for (const auto& info : pm.packages()) {
        Package& p = pm.open(info);
        for (uint32_t i = 0; i < p.entries().size(); ++i) {
            const Entry& e = p.entries()[i];
            if (e.type == 8 && e.reference == cls) out.push_back(TagHash::make(info.pkg_id, uint16_t(i)));
        }
    }
    return out;
}

// The display component's first u32 indexes the codex image list (class 808071C0): Vec at 0x08
// of 0x20-byte elements {u32 name hash, ..., 0x10 tag of an 80805335 image entry}. The entry holds
// its 80805350 image at 0x18, whose nested Vec (0x20 -> Vec -> tags) lists the texture headers,
// largest first. Returns the largest texture, or none when the index is 0xFFFF or does not resolve.
TagHash illustration_of(PackageManager& pm, const std::vector<TagHash>& images, uint32_t index) {
    if (index >= images.size() || images[index].is_none()) return {};
    try {
        const auto entry = pm.read(images[index]);
        if (entry.size() < 0x1C) return {};
        const auto image = pm.read(TagReader(entry).tag_at(0x18));
        TagReader ir(image);
        const auto outer = ir.vec_at(0x20);
        if (!outer.count) return {};
        const auto inner = ir.vec_at(outer.data);
        if (!inner.count) return {};
        return ir.tag_at(inner.data);
    } catch (...) {
        return {};
    }
}

uint32_t field(const TagReader& r, const CodexDefinition& d, uint32_t cls, size_t off, size_t size) {
    for (const auto& c : d.components)
        if (c.cls == cls && c.offset + off + 4 <= size) {
            const uint32_t v = r.u32_at(c.offset + off);
            return v == 0x811C9DC5 ? 0 : v;
        }
    return 0;
}

}  // namespace

Codex load_codex(PackageManager& pm, const std::vector<DialogueLine>& dialogue) {
    Codex codex;
    const auto defs_tag = find_single(pm, kClassCodexDefinitions);
    if (!defs_tag) throw std::runtime_error("codex: no definition table (class 808029D1) in the install");
    codex.definitions = *defs_tag;
    const auto bytes = pm.read(*defs_tag);
    const auto defs = parse_codex_definitions(bytes);
    TagReader r(bytes);

    // Codex image list: index -> 80805335 image entry.
    std::vector<TagHash> images;
    if (const auto list = find_single(pm, kClassCodexImageList)) {
        const auto b = pm.read(*list);
        TagReader lr(b);
        const auto v = lr.vec_at(0x08);
        for (uint64_t i = 0; i < v.count; ++i) {
            const size_t e = v.data + size_t(i) * 0x20;
            if (e + 0x14 > b.size()) break;
            images.push_back(lr.tag_at(e + 0x10));
        }
    }
    auto illustration = [&](const CodexDefinition& d) {
        for (const auto& c : d.components)
            if (c.cls == kClassCodexDisplay && c.offset + 4 <= bytes.size()) return illustration_of(pm, images, r.u32_at(c.offset));
        return TagHash{};
    };

    // Item hash -> asset key.
    std::unordered_map<uint32_t, uint32_t> asset_key;
    for (TagHash t : find_all(pm, kClassItemTable)) {
        const auto b = pm.read(t);
        TagReader ir(b);
        const auto items = ir.vec_at(0x08);
        for (uint64_t i = 0; i < items.count; ++i) {
            const size_t e = items.data + size_t(i) * 0x48;
            const uint32_t key = ir.u32_at(e + 8);
            if (key && key != 0xFFFFFFFF) asset_key.emplace(ir.u32_at(e), key);
        }
    }
    // Asset key -> hash64 of the record.
    std::unordered_map<uint32_t, uint64_t> asset64;
    for (TagHash t : find_all(pm, kClassAssetIndex)) {
        std::vector<uint8_t> b;
        try { b = pm.read(t); } catch (...) { continue; }
        TagReader ar(b);
        TagReader::Vec v;
        try { v = ar.vec_at(0x08); } catch (...) { continue; }
        for (uint64_t i = 0; i < v.count; ++i) {
            const size_t e = v.data + size_t(i) * 0x18;
            if (e + 0x18 > b.size()) break;
            asset64.emplace(uint32_t(ar.u64_at(e)), ar.u64_at(e + 0x10));
        }
    }
    // (table, conversation) -> dialogue line indices.
    std::unordered_map<uint64_t, std::vector<size_t>> by_conversation;
    for (size_t i = 0; i < dialogue.size(); ++i) {
        by_conversation[(uint64_t(dialogue[i].table.value) << 32) | dialogue[i].conversation].push_back(i);
    }

    pm.build_hash64_index();
    auto link_recording = [&](CodexEntry& e) {
        auto k = asset_key.find(e.hash);
        if (k == asset_key.end()) return;
        e.asset_key = k->second;
        auto h = asset64.find(k->second);
        if (h == asset64.end()) return;
        auto rec = pm.resolve64(h->second);
        if (!rec) return;
        const Entry* re = pm.entry(*rec);
        if (!re || re->reference != kClassAssetRecord) return;
        e.asset = *rec;
        std::vector<uint8_t> comp;
        try {
            const auto rb = pm.read(*rec);
            if (rb.size() < 0xC4) return;
            comp = pm.read(TagReader(rb).tag_at(0xC0));
        } catch (...) {
            return;
        }
        // The recording component stores the conversation key and its wide table reference.
        // Both fields matched the authored recording on all 87 installed audio logs.
        if (comp.size() < 0xAB0) return;
        TagReader cr(comp);
        const auto table = pm.resolve64(cr.u64_at(0xAA8));
        const uint32_t key = cr.u32_at(0xA98);
        if (!table || !key || key == 0xFFFFFFFF) return;
        const auto lines = by_conversation.find((uint64_t(table->value) << 32) | key);
        if (lines == by_conversation.end()) return;
        e.dialogue_table = *table;
        e.conversation = key;
        e.lines = lines->second;
    };

    codex.collections.emplace_back();  // entries that come before the first collection
    for (const auto& d : defs) {
        if (d.kind == kCodexKindCollection) {
            CodexCollection c;
            c.hash = d.hash;
            c.title = field(r, d, kClassCodexDisplay, 0x08, bytes.size());
            c.unlock = field(r, d, kClassCodexUnlock, 0x6C, bytes.size());
            c.illustration = illustration(d);
            codex.collections.push_back(std::move(c));
        } else if (d.kind == kCodexKindEntry) {
            CodexEntry e;
            e.hash = d.hash;
            e.title = field(r, d, kClassCodexDisplay, 0x08, bytes.size());
            e.body = field(r, d, kClassCodexBody, 0x2C, bytes.size());
            e.unlock = field(r, d, kClassCodexUnlock, 0x6C, bytes.size());
            e.illustration = illustration(d);
            link_recording(e);
            if (!e.illustration.is_none()) codex.with_illustration++;
            codex.entries++;
            if (!e.asset.is_none()) codex.with_asset++;
            if (!e.lines.empty()) codex.with_audio++;
            codex.collections.back().entries.push_back(std::move(e));
        }
    }
    if (codex.collections.front().entries.empty()) codex.collections.erase(codex.collections.begin());
    std::erase_if(codex.collections, [](const CodexCollection& c) { return c.entries.empty(); });
    return codex;
}

}  // namespace nim
