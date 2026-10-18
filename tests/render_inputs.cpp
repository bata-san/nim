// CPU regression tests for render-input handoff; no GPU or game install needed.
#include "nim/render/game_lighting.hpp"
#include <cstdio>
#include <cmath>
#include <stdexcept>
using namespace nim;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "%s\n", what); ++failures; } };
    // Descriptor identities survive all draws in one frame, but never cross a recycled
    // frame region or an immediate-list rewind (which would alias overwritten views).
    Gpu tables;
    const std::vector<uint64_t> table_key{12, 0, 3, 42};
    tables.begin_frame(0);
    const auto table0 = tables.transient_descriptors(12);
    tables.cache_transient_table(table_key, table0, {});
    check(tables.find_transient_table(table_key) == table0, "identical view table was not shared");
    check(!tables.find_transient_table({12, 0, 3, 43}), "different view metadata aliased a table");
    tables.begin_frame(1);
    check(!tables.find_transient_table(table_key), "previous frame table leaked into new frame");
    const auto table1 = tables.transient_descriptors(12);
    tables.cache_transient_table(table_key, table1, {});
    check(table0 != table1, "in-flight frames share mutable descriptors");
    tables.transient_rewind(table1);
    check(!tables.find_transient_table(table_key), "rewound table survived in cache");
    tables.begin_frame(0);
    check(!tables.find_transient_table(table_key), "recycled frame retained stale views");
    ExternStore scene, frame;
    int token = 0;  // opaque identity only: the CPU store never dereferences texture pointers
    auto* texture = reinterpret_cast<ID3D12Resource*>(&token);
    // Replacing a block must remove old textures, not just overwrite provided keys.
    scene.set_texture(ext::Transparent, 0x10, texture, DXGI_FORMAT_R32_FLOAT, 4, 8, 12);
    scene.set(ext::Transparent, 0x80, float4{9, 9, 9, 9});
    scene.set(ext::View, 0x20, float4{1, 2, 3, 1});
    scene.channels = {float4{7, 0, 0, 0}};
    frame.set_texture(ext::Transparent, 0x20, texture, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, 3);
    frame.set(ext::Transparent, 0x90, float4{4, 5, 6, 7});
    frame.zero_when_missing(ext::Transparent, "unproduced slices");
    frame.note_estimate("fog input", "unknown engine state");
    scene.copy_block_from(ext::Transparent, frame);
    check(scene.texture(ext::Transparent, 0x10).format == DXGI_FORMAT_UNKNOWN, "stale texture metadata survived replacement");
    check(scene.texture(ext::Transparent, 0x20).first_element == 3, "texture view metadata lost");
    check(!scene.has(ext::Transparent, 0x80, 16), "old constant survived replacement");
    check(scene.get_v(ext::Transparent, 0x80) == float4{}, "missing bytes were not cleared");
    check(scene.get_v(ext::Transparent, 0x90) == float4{4, 5, 6, 7}, "current frame constants not copied");
    check(scene.get_v(ext::View, 0x20) == float4{1, 2, 3, 1}, "unrelated view block changed");
    check(scene.channels[0][0] == 7, "material channels changed");
    check(scene.estimates().contains("fog input"), "source estimates were lost");
    scene.copy_block_from(ext::Transparent, scene);
    check(scene.get_v(ext::Transparent, 0x90)[0] == 4, "self copy destroyed inputs");
    ExternStore empty;
    scene.copy_block_from(ext::Transparent, empty);
    check(!scene.zero_policy(ext::Transparent), "old missing-value policy survived replacement");
    bool threw = false;
    try { (void)scene.get_f(ext::Transparent, 0x90); } catch (const std::runtime_error&) { threw = true; }
    check(threw, "missing original input did not fail after block clear");
    // The infinite projection maps device depth zero to infinity. Sky motion uses a finite
    // reconstructed point so that the untouched game PS never divides by zero.
    View view;
    view.eye = {155, -60, -12}; view.target = {125, -28, -26}; view.width = 640; view.height = 360;
    auto camera = make_view(view);
    fill_view_externs(scene, 640, 360, camera);
    const auto far_point = scene.get_v(ext::View, 0x210);  // fourth column of +0x1E0
    check(far_point[3] > 0 && std::isfinite(1 / far_point[3]), "sky far point is at infinity");
    const V3 point{far_point[0] / far_point[3], far_point[1] / far_point[3], far_point[2] / far_point[3]};
    check(std::abs(dot(point - camera.eye, camera.fwd) - 50000) < 100, "sky far reconstruction distance is wrong");
    std::printf("render input tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
