#pragma once
#include "nim/core/package.hpp"
#include "nim/core/tag_hash.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace nim {

struct PackageInfo {
    std::filesystem::path newest_path;
    std::string name;
    uint16_t pkg_id = 0;
    uint16_t newest_patch = 0;
    uint16_t language = 0;
};

// Indexes every package in an install and resolves TagHash / TagHash64 to bytes.
// Packages are opened lazily: scanning the 650+ files only reads their headers.
class PackageManager {
public:
    // `install_dir` is the Marathon root (the folder with Marathon.exe).
    explicit PackageManager(const std::filesystem::path& install_dir);

    static std::optional<std::filesystem::path> find_install();

    const std::vector<PackageInfo>& packages() const { return open_->infos; }
    // Localized packages (e.g. *_en / *_jpn audio) share a pkg_id; this picks which one resolves.
    // Language ids seen in Marathon: 0 = neutral, 1 = English, 6 = Japanese.
    void set_language(uint16_t language);
    uint16_t language() const { return language_; }
    const PackageInfo* info(uint16_t pkg_id) const;
    Package& open(uint16_t pkg_id);
    Package& open(const PackageInfo& info);

    const Entry* entry(TagHash h);
    std::vector<uint8_t> read(TagHash h);
    std::vector<uint8_t> read_range(TagHash h, uint64_t offset, size_t size);
    std::optional<TagHash> resolve64(uint64_t hash64);

    // Builds the global hash64 table by opening every package (header + tables only).
    void build_hash64_index();

    // Another manager over the same install for a worker thread: the package directory and
    // hash64 indexes, open packages and block caches are shared
    // (Package reads are thread-safe), so threads do not decompress the same blocks again.
    std::unique_ptr<PackageManager> fork() const;

private:
    PackageManager() = default;
    using Hash64Index = std::unordered_map<uint64_t, TagHash>;
    struct OpenSlot {
        std::once_flag once;
        std::unique_ptr<Package> package;
    };
    struct OpenPackages {
        // Populated by the install scan, then read by all forks.
        std::vector<PackageInfo> infos;
        std::unordered_map<uint16_t, std::vector<size_t>> by_id;
        std::once_flag hash64_once;
        std::shared_ptr<const Hash64Index> hash64;
        // Fixed after the install scan; each package can be opened independently.
        std::vector<std::unique_ptr<OpenSlot>> slots;
    };
    std::shared_ptr<OpenPackages> open_;
    uint16_t language_ = 1;
    std::shared_ptr<const Hash64Index> h64_;
};

} // namespace nim
