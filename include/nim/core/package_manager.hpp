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

    const std::vector<PackageInfo>& packages() const { return infos_; }
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

    // Another manager over the same install for a worker thread: the package list and hash64
    // index are copied (no directory scan); open packages and their block caches are shared
    // (Package reads are thread-safe), so threads do not decompress the same blocks again.
    std::unique_ptr<PackageManager> fork() const;

private:
    PackageManager() = default;
    std::filesystem::path pkg_dir_;
    std::vector<PackageInfo> infos_;
    std::unordered_map<uint16_t, std::vector<size_t>> by_id_;
    struct OpenPackages {
        std::mutex mutex;
        std::unordered_map<size_t, std::unique_ptr<Package>> by_index;  // index into infos_
    };
    std::shared_ptr<OpenPackages> open_ = std::make_shared<OpenPackages>();
    uint16_t language_ = 1;
    std::unordered_map<uint64_t, TagHash> h64_;
};

} // namespace nim
