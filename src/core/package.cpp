#include "nim/core/package.hpp"
#include "nim/core/binary.hpp"
#include "nim/core/oodle.hpp"
#include "nim/core/profile.hpp"

#include <cstring>
#include <format>
#include <stdexcept>

namespace nim {
namespace {

// "w64_sr_globals_0120_3" -> ("w64_sr_globals_0120", 3)
std::pair<std::string, int> split_patch(const std::string& stem) {
    auto pos = stem.rfind('_');
    if (pos == std::string::npos) return {stem, 0};
    return {stem.substr(0, pos), std::stoi(stem.substr(pos + 1))};
}

} // namespace

Package::Package(const std::filesystem::path& path) : dir_(path.parent_path()), newest_file_(path) {
    auto [base, patch] = split_patch(path.stem().string());
    name_ = base;
    newest_patch_ = uint16_t(patch);
    parse();
}

void Package::parse() {
    BinaryReader r(newest_file_.bytes());
    PackageHeader& h = header_;
    h.version = r.u16_at(0x00);
    if (h.version != 0x35)
        throw std::runtime_error(std::format("{}: unsupported package version 0x{:X}", name_, h.version));
    h.platform = r.u16_at(0x02);
    h.group_id = r.u64_at(0x08);
    h.pkg_id = r.u16_at(0x10);
    h.unpatchable = r.u8_at(0x12) != 0;
    h.build_time = r.u64_at(0x20);
    h.patch_id = r.u16_at(0x30);
    h.language = r.u16_at(0x32);
    h.entry_count = r.u32_at(0x60);
    h.entry_offset = r.u32_at(0x64);
    h.block_count = r.u32_at(0x68);
    h.block_offset = r.u32_at(0x6C);
    h.named_tag_count = r.u32_at(0x78);
    h.named_tag_offset = r.u32_at(0x7C);
    h.h64_count = r.u32_at(0xB8);
    h.h64_offset = r.u32_at(0xBC);
    h.file_size = r.u32_at(0x120);

    entries_.resize(h.entry_count);
    for (uint32_t i = 0; i < h.entry_count; ++i) {
        size_t o = h.entry_offset + size_t(i) * 16;
        uint32_t type_info = r.u32_at(o + 4);
        uint64_t bi = r.u64_at(o + 8);
        Entry& e = entries_[i];
        e.reference = r.u32_at(o);
        e.type = uint8_t((type_info >> 9) & 0x7F);
        e.subtype = uint8_t((type_info >> 6) & 0x7);
        e.starting_block = uint32_t(bi & 0x3FFF);
        e.starting_block_offset = uint32_t(((bi >> 14) & 0x3FFF) << 4);
        e.file_size = uint32_t(bi >> 28);
    }

    blocks_.resize(h.block_count);
    for (uint32_t i = 0; i < h.block_count; ++i) {
        size_t o = h.block_offset + size_t(i) * 0x30;
        Block& b = blocks_[i];
        b.offset = r.u32_at(o);
        b.size = r.u32_at(o + 4);
        b.patch_id = r.u16_at(o + 8);
        b.flags = r.u16_at(o + 10);
        r.copy_at(o + 12, b.sha1);
        r.copy_at(o + 32, b.gcm_tag);
    }

    named_tags_.resize(h.named_tag_count);
    for (uint32_t i = 0; i < h.named_tag_count; ++i) {
        size_t o = h.named_tag_offset + 0x30 + size_t(i) * 16;
        NamedTag& t = named_tags_[i];
        t.hash = TagHash(r.u32_at(o));
        t.class_hash = r.u32_at(o + 4);
        t.name = r.cstr_at(o + 8 + size_t(r.u64_at(o + 8)));
    }

    if (h.h64_count) {
        size_t base = h.h64_offset + 0x20 + size_t(int64_t(r.u64_at(h.h64_offset + 0x10)));
        hash64s_.resize(h.h64_count);
        for (uint32_t i = 0; i < h.h64_count; ++i) {
            size_t o = base + size_t(i) * 16;
            hash64s_[i] = {r.u64_at(o), TagHash(r.u32_at(o + 8)), r.u32_at(o + 12)};
        }
    }
}

const MappedFile& Package::file_for_patch(uint16_t patch) const {
    // The newest patch already has an immutable mapping; no older-file lookup is needed.
    if (patch == newest_patch_) return newest_file_;
    std::lock_guard lock(files_mutex_);
    auto it = files_.find(patch);
    if (it != files_.end()) return *it->second;
    auto path = dir_ / std::format("{}_{}.pkg", name_, patch);
    return *files_.emplace(patch, std::make_unique<MappedFile>(path)).first->second;
}

std::vector<uint8_t> Package::read_block(uint32_t index) const {
    ProfileScope profile("package.block_read_decode");
    Profile::instance().count("package.block_reads");
    const Block& b = blocks_.at(index);
    auto src = file_for_patch(b.patch_id).bytes();
    if (size_t(b.offset) + b.size > src.size())
        throw std::runtime_error(std::format("{}: block {} out of file bounds", name_, index));

    if (b.flags & kBlockRedacted)
        throw std::runtime_error(std::format("{}: block {} is redacted (needs group key)", name_, index));
    // The decompressor only reads its input. Copy when decryption needs writable bytes, or
    // when returning an uncompressed block that must own its data beyond this mapping.
    auto payload = src.subspan(b.offset, b.size);
    std::vector<uint8_t> raw;
    if (b.flags & kBlockEncrypted) {
        // Index/metadata scans open packages without reading encrypted payloads.
        std::call_once(cipher_once_, [this] { cipher_ = std::make_unique<PackageCipher>(header_.pkg_id); });
        raw.assign(payload.begin(), payload.end());
        if (!cipher_->decrypt(b.flags & kBlockAltKey, b.gcm_tag, raw))
            throw std::runtime_error(std::format("{}: block {} failed GCM authentication", name_, index));
        if (!(b.flags & kBlockCompressed)) return raw;
        payload = raw;
    } else if (!(b.flags & kBlockCompressed)) {
        return {payload.begin(), payload.end()};
    }
    std::vector<uint8_t> out(kBlockSize);
    size_t n = Oodle::decompress(payload, out);
    if (n == 0) throw std::runtime_error(std::format("{}: block {} failed to decompress", name_, index));
    out.resize(n);
    return out;
}

std::shared_ptr<const std::vector<uint8_t>> Package::cached_block(uint32_t index) const {
    ProfileScope profile("package.block_cache_lookup");
    bool hit = false;
    auto data = cache_.get(index, [this, &profile](uint32_t i) {
        profile.stop();  // keep decode time separate from cache lookup
        Profile::instance().count("package.block_cache_misses");
        return read_block(i);
    }, &hit);
    if (hit) Profile::instance().count("package.block_cache_hits");
    return data;
}

std::vector<uint8_t> Package::read_entry(uint32_t index) const {
    const Entry& e = entries_.at(index);
    std::vector<uint8_t> out;
    out.reserve(e.file_size);
    uint32_t block = e.starting_block;
    size_t offset = e.starting_block_offset;
    while (out.size() < e.file_size) {
        auto block_data = cached_block(block++);
        const auto& data = *block_data;
        if (offset >= data.size()) throw std::runtime_error("entry offset beyond block");
        size_t n = std::min(data.size() - offset, size_t(e.file_size) - out.size());
        out.insert(out.end(), data.begin() + offset, data.begin() + offset + n);
        offset = 0;
    }
    return out;
}

std::vector<uint8_t> Package::read_entry_range(uint32_t index, uint64_t offset, size_t size) const {
    const Entry& e = entries_.at(index);
    if (offset >= e.file_size) return {};
    size = size_t(std::min<uint64_t>(size, e.file_size - offset));
    std::vector<uint8_t> out;
    out.reserve(size);
    uint64_t pos = e.starting_block_offset + offset;
    uint32_t block = e.starting_block + uint32_t(pos / kBlockSize);
    size_t in_block = size_t(pos % kBlockSize);
    while (out.size() < size) {
        const auto data = read_block(block++);
        if (in_block >= data.size()) throw std::runtime_error("entry offset beyond block");
        const size_t n = std::min(data.size() - in_block, size - out.size());
        out.insert(out.end(), data.begin() + in_block, data.begin() + in_block + n);
        in_block = 0;
    }
    return out;
}

void Package::clear_block_cache() const {
    cache_.clear();
}

} // namespace nim
