#include "nim/core/package_manager.hpp"
#include "nim/core/oodle.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <windows.h>

namespace nim {
namespace fs = std::filesystem;

namespace {

std::optional<fs::path> steam_root() {
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, buf, &size) != ERROR_SUCCESS)
        return std::nullopt;
    return fs::path(buf);
}

} // namespace

std::optional<fs::path> PackageManager::find_install() {
    std::vector<fs::path> libraries;
    if (auto root = steam_root()) {
        libraries.push_back(*root);
        std::ifstream vdf(*root / "steamapps" / "libraryfolders.vdf");
        std::string line;
        std::regex re(R"rx("path"\s+"([^"]+)")rx");
        while (std::getline(vdf, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re)) {
                std::string p = m[1];
                p = std::regex_replace(p, std::regex(R"(\\\\)"), "\\");
                libraries.emplace_back(p);
            }
        }
    }
    for (const auto& lib : libraries) {
        auto dir = lib / "steamapps" / "common" / "Marathon";
        if (fs::exists(dir / "Marathon.exe") && fs::exists(dir / "packages")) return dir;
    }
    return std::nullopt;
}

PackageManager::PackageManager(const fs::path& install_dir) : pkg_dir_(install_dir / "packages") {
    if (!fs::is_directory(pkg_dir_))
        throw std::runtime_error("no packages directory in " + install_dir.string());
    auto oodle = install_dir / "bin" / "x64" / "oo2core_9_win64.dll";
    if (!Oodle::load(oodle)) throw std::runtime_error("failed to load " + oodle.string());

    // Keep only the newest patch of each package.
    std::unordered_map<std::string, std::pair<int, fs::path>> newest;
    for (const auto& de : fs::directory_iterator(pkg_dir_)) {
        if (de.path().extension() != ".pkg") continue;
        std::string stem = de.path().stem().string();
        auto pos = stem.rfind('_');
        if (pos == std::string::npos) continue;
        std::string base = stem.substr(0, pos);
        int patch = std::stoi(stem.substr(pos + 1));
        auto it = newest.find(base);
        if (it == newest.end() || it->second.first < patch) newest[base] = {patch, de.path()};
    }

    for (auto& [base, pp] : newest) {
        std::ifstream f(pp.second, std::ios::binary);
        uint8_t hdr[0x40]{};
        f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
        PackageInfo info;
        info.newest_path = pp.second;
        info.name = base;
        info.newest_patch = uint16_t(pp.first);
        std::memcpy(&info.pkg_id, hdr + 0x10, 2);
        std::memcpy(&info.language, hdr + 0x32, 2);
        infos_.push_back(std::move(info));
    }
    std::ranges::sort(infos_, {}, &PackageInfo::pkg_id);
    for (size_t i = 0; i < infos_.size(); ++i) by_id_[infos_[i].pkg_id].push_back(i);
}

void PackageManager::set_language(uint16_t language) { language_ = language; }

const PackageInfo* PackageManager::info(uint16_t pkg_id) const {
    auto it = by_id_.find(pkg_id);
    if (it == by_id_.end()) return nullptr;
    const PackageInfo* fallback = &infos_[it->second.front()];
    for (size_t i : it->second) {
        const PackageInfo& p = infos_[i];
        if (p.language == 0 || p.language == language_) return &p;
        if (p.language == 1) fallback = &p;  // English when the preferred language is not installed
    }
    return fallback;
}

Package& PackageManager::open(const PackageInfo& info) {
    if (auto it = open_.find(&info); it != open_.end()) return *it->second;
    return *open_.emplace(&info, std::make_unique<Package>(info.newest_path)).first->second;
}

Package& PackageManager::open(uint16_t pkg_id) {
    const PackageInfo* i = info(pkg_id);
    if (!i) throw std::runtime_error(std::format("unknown package 0x{:04X}", pkg_id));
    return open(*i);
}

const Entry* PackageManager::entry(TagHash h) {
    if (!h.is_pkg_ref() || !info(h.pkg_id())) return nullptr;
    auto& entries = open(h.pkg_id()).entries();
    return h.entry_index() < entries.size() ? &entries[h.entry_index()] : nullptr;
}

std::vector<uint8_t> PackageManager::read(TagHash h) {
    if (!entry(h)) throw std::runtime_error("unresolvable tag " + h.str());
    return open(h.pkg_id()).read_entry(h.entry_index());
}

void PackageManager::build_hash64_index() {
    for (const auto& i : infos_)
        for (const auto& e : open(i).hash64s()) h64_.emplace(e.hash64, e.hash32);
}

std::unique_ptr<PackageManager> PackageManager::fork() const {
    std::unique_ptr<PackageManager> m(new PackageManager());
    m->pkg_dir_ = pkg_dir_;
    m->infos_ = infos_;
    m->by_id_ = by_id_;
    m->language_ = language_;
    m->h64_ = h64_;
    return m;
}

std::optional<TagHash> PackageManager::resolve64(uint64_t hash64) {
    auto it = h64_.find(hash64);
    if (it == h64_.end()) return std::nullopt;
    return it->second;
}

} // namespace nim
