// Exercise newest- and older-patch mappings without a game install or Oodle DLL.
#include "nim/core/package.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto base = std::filesystem::temp_directory_path();
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 64; ++i) {
            auto candidate = base / ("nim-pkg-test-" + std::to_string(ticks) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Could not create package fixture directory");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("Could not write package fixture");
}

} // namespace

int main() {
    TempDir temp;
    constexpr size_t blocks_at = 0x140, data_at = 0x200;
    const std::vector<uint8_t> expected_new{1, 2, 3, 4}, expected_old{5, 6, 7, 8};
    std::vector<uint8_t> newest(data_at + expected_new.size()), older(expected_old);
    const auto put = [&]<class T>(size_t at, T value) { std::memcpy(newest.data() + at, &value, sizeof(T)); };
    put(0, uint16_t(0x35));
    put(0x30, uint16_t(2));
    put(0x68, uint32_t(3));
    put(0x6C, uint32_t(blocks_at));
    put(0x120, uint32_t(newest.size()));
    const auto block = [&](size_t n, uint32_t offset, uint16_t patch, uint16_t flags) {
        const size_t at = blocks_at + n * 0x30;
        put(at, offset);
        put(at + 4, uint32_t(4));
        put(at + 8, patch);
        put(at + 10, flags);
    };
    block(0, data_at, 2, 0);
    block(1, 0, 1, 0);
    block(2, data_at, 2, nim::kBlockRedacted);
    std::copy(expected_new.begin(), expected_new.end(), newest.begin() + data_at);
    write_file(temp.path / "fixture_2.pkg", newest);
    write_file(temp.path / "fixture_1.pkg", older);
    std::vector<uint8_t> owned;
    {
        nim::Package pkg(temp.path / "fixture_2.pkg");
        std::array<bool, 8> matches{};
        std::vector<std::jthread> workers;
        for (size_t i = 0; i < matches.size(); ++i) {
            workers.emplace_back([&, i] {
                try {
                    for (unsigned repeat = 0; repeat < 16; ++repeat) {
                        if (pkg.read_block(0) != expected_new || pkg.read_block(1) != expected_old) return;
                    }
                    matches[i] = true;
                } catch (const std::exception&) {}
            });
        }
        workers.clear();
        if (!std::ranges::all_of(matches, [](bool result) { return result; })) return 1;
        owned = pkg.read_block(0);
        try {
            (void)pkg.read_block(2);
            return 2;
        } catch (const std::runtime_error& ex) {
            if (std::string(ex.what()).find("redacted") == std::string::npos) return 3;
        }
    }
    return owned == expected_new ? 0 : 4;
}
