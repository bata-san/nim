#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace nim {
// Existing viewer LOD policy. Keep the same strict threshold comparisons and authored bits.
inline unsigned visibility_lod(uint32_t levels, float px, bool near_camera) noexcept {
    levels = levels ? levels : 1u;
    if (near_camera || std::has_single_bit(levels)) return std::countr_zero(levels);
    constexpr std::array<float, 7> detail_radii{16, 6, 3, 1.5f, 1, 0.5f, 0.25f};
    // Advance only while another authored level exists; no work after the coarsest level.
    for (size_t step = 0; step < detail_radii.size() && (levels & (levels - 1)) &&
         px < detail_radii[step]; ++step)
        levels &= levels - 1;
    return std::countr_zero(levels);
}
} // namespace nim
