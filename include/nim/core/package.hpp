#pragma once
#include "nim/core/crypto.hpp"
#include "nim/core/mapped_file.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <vector>

namespace nim {

// Package format version 0x35 (Destiny 2 Beyond Light layout; Marathon / "Goliath" uses it too).
struct PackageHeader {
    uint16_t version = 0;
    uint16_t platform = 0;
    uint64_t group_id = 0;
    uint16_t pkg_id = 0;
    bool unpatchable = false;
    uint64_t build_time = 0;
    uint16_t patch_id = 0;
    uint16_t language = 0;
    uint32_t entry_count = 0, entry_offset = 0;
    uint32_t block_count = 0, block_offset = 0;
    uint32_t named_tag_count = 0, named_tag_offset = 0;
    uint32_t h64_count = 0, h64_offset = 0;
    uint32_t file_size = 0;
};

struct Entry {
    uint32_t reference = 0;   // usually the TagHash of the struct's type class
    uint8_t type = 0;
    uint8_t subtype = 0;
    uint32_t starting_block = 0;
    uint32_t starting_block_offset = 0;
    uint32_t file_size = 0;
};

enum BlockFlags : uint16_t {
    kBlockCompressed = 0x1,
    kBlockEncrypted = 0x2,
    kBlockAltKey = 0x4,
    kBlockRedacted = 0x8,
};

struct Block {
    uint32_t offset = 0;
    uint32_t size = 0;
    uint16_t patch_id = 0;
    uint16_t flags = 0;
    std::array<uint8_t, 20> sha1{};
    std::array<uint8_t, 16> gcm_tag{};
};

struct NamedTag {
    TagHash hash;
    uint32_t class_hash = 0;
    std::string name;
};

struct Hash64Entry {
    uint64_t hash64 = 0;
    TagHash hash32;
    uint32_t reference = 0;
};

constexpr size_t kBlockSize = 0x40000;

// One logical package: the newest patch file plus older patch files its blocks point at.
class Package {
public:
    // `path` is the newest patch, e.g. .../w64_sr_globals_0120_3.pkg
    explicit Package(const std::filesystem::path& path);

    const PackageHeader& header() const { return header_; }
    const std::vector<Entry>& entries() const { return entries_; }
    const std::vector<Block>& blocks() const { return blocks_; }
    const std::vector<NamedTag>& named_tags() const { return named_tags_; }
    const std::vector<Hash64Entry>& hash64s() const { return hash64s_; }
    const std::string& name() const { return name_; }  // e.g. "w64_sr_globals_0120"

    // Reads one decrypted, decompressed block (bypasses the cache). Thread-safe.
    std::vector<uint8_t> read_block(uint32_t index) const;
    // Same, through a small LRU of recently decoded blocks. Thread-safe.
    std::shared_ptr<const std::vector<uint8_t>> cached_block(uint32_t index) const;
    // Reads one entry's bytes. Thread-safe.
    std::vector<uint8_t> read_entry(uint32_t index) const;

private:
    const MappedFile& file_for_patch(uint16_t patch) const;
    void parse();

    std::filesystem::path dir_;
    std::string name_;
    PackageHeader header_;
    std::vector<Entry> entries_;
    std::vector<Block> blocks_;
    std::vector<NamedTag> named_tags_;
    std::vector<Hash64Entry> hash64s_;
    std::unique_ptr<PackageCipher> cipher_;

    static constexpr size_t kCacheSlots = 1024;  // decompressed blocks kept per package
    mutable std::shared_mutex cache_mutex_;
    mutable std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> cache_;

    mutable std::mutex files_mutex_;
    mutable std::map<uint16_t, std::unique_ptr<MappedFile>> files_;
};

} // namespace nim
