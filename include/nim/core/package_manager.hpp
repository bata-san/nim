#pragma once
#include "nim/core/package.hpp"
#include "nim/core/tag_hash.hpp"

#include <filesystem>
#include <memory>
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
    std::optional<TagHash> resolve64(uint64_t hash64);

    // Builds the global hash64 table by opening every package (header + tables only).
    void build_hash64_index();

private:
    std::filesystem::path pkg_dir_;
    std::vector<PackageInfo> infos_;
    std::unordered_map<uint16_t, std::vector<size_t>> by_id_;
    std::unordered_map<const PackageInfo*, std::unique_ptr<Package>> open_;
    uint16_t language_ = 1;
    std::unordered_map<uint64_t, TagHash> h64_;
};

} // namespace nim
