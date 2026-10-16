#pragma once
#include <cstdint>
#include <compare>
#include <format>
#include <string>

namespace nim {

// 32-bit reference to an entry inside a package.
// Layout (same as Destiny 2): 0x80800000 + (pkg_id << 13) + entry_index.
struct TagHash {
    uint32_t value = 0xFFFFFFFF;

    static constexpr uint32_t kBase = 0x80800000;

    constexpr TagHash() = default;
    constexpr explicit TagHash(uint32_t v) : value(v) {}
    static constexpr TagHash make(uint16_t pkg_id, uint16_t index) {
        return TagHash(kBase + (uint32_t(pkg_id) << 13) + (index & 0x1FFF));
    }

    constexpr bool is_none() const { return value == 0xFFFFFFFF || value == 0; }
    constexpr bool is_pkg_ref() const { return value >= kBase && value < kBase + (0x400u << 13); }
    constexpr uint16_t pkg_id() const { return uint16_t(((value - kBase) >> 13) & 0x3FF); }
    constexpr uint16_t entry_index() const { return uint16_t(value & 0x1FFF); }

    constexpr auto operator<=>(const TagHash&) const = default;

    std::string str() const { return std::format("{:08X}", value); }
};

} // namespace nim

template <>
struct std::hash<nim::TagHash> {
    size_t operator()(nim::TagHash h) const noexcept { return std::hash<uint32_t>{}(h.value); }
};
