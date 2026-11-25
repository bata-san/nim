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

PackageManager::PackageManager(const fs::path& install_dir) : open_(std::make_shared<OpenPackages>()) {
    const auto package_dir = install_dir / "packages";
    if (!fs::is_directory(package_dir))
        throw std::runtime_error("no packages directory in " + install_dir.string());
    auto oodle = install_dir / "bin" / "x64" / "oo2core_9_win64.dll";
    if (!Oodle::load(oodle)) throw std::runtime_error("failed to load " + oodle.string());

    // Keep only the newest patch of each package.
    std::unordered_map<std::string, std::pair<int, fs::path>> newest;
    for (const auto& de : fs::directory_iterator(package_dir)) {
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
        open_->infos.push_back(std::move(info));
    }
    std::ranges::sort(open_->infos, {}, &PackageInfo::pkg_id);
    open_->slots.reserve(open_->infos.size());
    for (size_t i = 0; i < open_->infos.size(); ++i) {
        open_->by_id[open_->infos[i].pkg_id].push_back(i);
        open_->slots.push_back(std::make_unique<OpenSlot>());
    }
}

void PackageManager::set_language(uint16_t language) { language_ = language; }

const PackageInfo* PackageManager::info(uint16_t pkg_id) const {
    auto it = open_->by_id.find(pkg_id);
    if (it == open_->by_id.end()) return nullptr;
    const PackageInfo* fallback = &open_->infos[it->second.front()];
    for (size_t i : it->second) {
        const PackageInfo& p = open_->infos[i];
        if (p.language == 0 || p.language == language_) return &p;
        if (p.language == 1) fallback = &p;  // English when the preferred language is not installed
    }
    return fallback;
}

Package& PackageManager::open(const PackageInfo& info) {
    const size_t index = size_t(&info - open_->infos.data());
    auto& slot = *open_->slots.at(index);
    std::call_once(slot.once, [&] { slot.package = std::make_unique<Package>(info.newest_path); });
    return *slot.package;
}

Package& PackageManager::open(uint16_t pkg_id) {
    const PackageInfo* i = info(pkg_id);
    if (!i) throw std::runtime_error(std::format("unknown package 0x{:04X}", pkg_id));
    return open(*i);
}

const Entry* PackageManager::entry(TagHash h) {
    if (!h.is_pkg_ref()) return nullptr;
    const auto* i = info(h.pkg_id());
    if (!i) return nullptr;
    auto& entries = open(*i).entries();
    return h.entry_index() < entries.size() ? &entries[h.entry_index()] : nullptr;
}

std::vector<uint8_t> PackageManager::read(TagHash h) {
    const auto* i = h.is_pkg_ref() ? info(h.pkg_id()) : nullptr;
    if (!i) throw std::runtime_error("unresolvable tag " + h.str());
    auto& p = open(*i);
    if (h.entry_index() >= p.entries().size()) throw std::runtime_error("unresolvable tag " + h.str());
    return p.read_entry(h.entry_index());
}

std::vector<uint8_t> PackageManager::read_range(TagHash h, uint64_t offset, size_t size) {
    const auto* i = h.is_pkg_ref() ? info(h.pkg_id()) : nullptr;
    if (!i) throw std::runtime_error("unresolvable tag " + h.str());
    auto& p = open(*i);
    if (h.entry_index() >= p.entries().size()) throw std::runtime_error("unresolvable tag " + h.str());
    return p.read_entry_range(h.entry_index(), offset, size);
}

void PackageManager::build_hash64_index() {
    if (h64_) return;
    std::call_once(open_->hash64_once, [this] {
        auto index = std::make_shared<Hash64Index>();
        for (const auto& i : open_->infos)
            for (const auto& e : open(i).hash64s()) index->emplace(e.hash64, e.hash32);
        open_->hash64 = std::move(index);
    });
    h64_ = open_->hash64;
}

std::unique_ptr<PackageManager> PackageManager::fork() const {
    std::unique_ptr<PackageManager> m(new PackageManager());
    m->language_ = language_;
    m->h64_ = h64_;
    m->open_ = open_;
    return m;
}

std::optional<TagHash> PackageManager::resolve64(uint64_t hash64) {
    if (!h64_) return std::nullopt;
    auto it = h64_->find(hash64);
    if (it == h64_->end()) return std::nullopt;
    return it->second;
}

} // namespace nim
