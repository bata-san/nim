// CPU regression tests for render-input handoff; no GPU or game install needed.
#include "nim/render/game_lighting.hpp"
#include <cstdio>
#include <cmath>
#include <stdexcept>
using namespace nim;
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "%s\n", what); ++failures; } };
    {
        // Original PS contract: relighting uses alpha*cb11[19]*probe+cb11[20];
        // non-relighting uses 1+alpha*probe_params.x*original_boost*probe.
        MapCubemap m;
        m.probes = {2, 3, 1}; m.probe_params[0] = 0.4f;
        CubemapRelightingInputs in;
        in.bounce_scale = 0.75f;
        in.sky_intensity = 0;
        auto cb = cubemap_relighting(m, in);
        for (float alpha : {0.0f, 0.2f, 1.0f}) for (float probe : {0.0f, 0.01f, 1.0f})
            check(std::abs(alpha * cb[0][0] * probe + cb[1][0] - (1 + alpha * m.probe_params[0] * 0.75f * probe)) < 1e-6f,
                "relighting without sun or sky terms differs from the non-relighting modulation");
        m.flags = {1, 0.5f, 0, 0};
        in.sky_intensity = 2;
        in.sun_intensity = 20;
        in.sun_color = {0.5f, 1, 2, 1};
        in.sun_light_direction = {0, 0, 0.25f, 0};  // clamped to 0.5: sun scale 0.5 * 1 * 2 = 1
        cb = cubemap_relighting(m, in);
        check(std::abs(cb[0][0] - 0.75f * 0.4f * 0.5f) < 1e-6f && std::abs(cb[0][2] - 0.75f * 0.4f * 2) < 1e-6f, "relighting bounce term");
        check(std::abs(cb[1][1] - (2 * 0.5f + 1)) < 1e-6f && std::abs(cb[1][0] - (1 + 0.5f)) < 1e-6f, "relighting sky term");
        check(cubemap_alpha_exponent(0) == 0 && cubemap_alpha_exponent(0.5f) == 1 && std::abs(cubemap_alpha_exponent(1) - 61) < 1e-4f, "probe alpha exponent remap");
    }
    {
        // A shadowing light's +0x60 matrix: forward +X, 45 degree half angles, 0.5 .. 50 m. The
        // shadow view looks along the axis and stores reversed depth (near 1, far 0).
        MapLight l;
        l.node = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 4, 5, 1};
        l.light_space = {0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.99f, 1, 0, 0, 1.01f};
        const auto v = light_shadow_view(l);
        check(v.has_value(), "spot light frustum not recognised");
        if (v) {
            auto project = [&](float x, float y, float z) {
                std::array<float, 4> c{};
                const float p[4] = {x, y, z, 1};
                for (int col = 0; col < 4; ++col) for (int row = 0; row < 4; ++row) c[size_t(row)] += v->vp_abs.c[col][row] * p[col];
                return std::array<float, 3>{c[0] / c[3], c[1] / c[3], c[2] / c[3]};
            };
            const auto near_p = project(3.5f, 4, 5), far_p = project(53, 4, 5), edge_u = project(13, -6, 5), edge_v = project(13, 4, 15);
            check(std::abs(near_p[0]) < 1e-5f && std::abs(near_p[1]) < 1e-5f && std::abs(near_p[2] - 1) < 1e-4f, "light axis at the near plane is not the map centre at depth 1");
            check(std::abs(far_p[2]) < 1e-4f, "light far plane is not depth 0");
            // SSR reprojection (ssr +0xD0): this frame's (ndc, depth, 1) -> last frame's clip.
            {
                View cur;
                cur.eye = {3, -2, 1.5f}; cur.target = {10, 4, 0}; cur.width = 640; cur.height = 360;
                View old = cur;
                old.eye = {2.5f, -2.4f, 1.7f}; old.target = {10.5f, 3.5f, 0.2f};
                const ViewMatrices c = make_view(cur), o = make_view(old);
                const M4 m = mul(o.vp_abs, GameLighting::inverse_matrix(c.vp_abs));
                const float4 world{8, 2, 0.5f, 1};
                auto xf = [](const M4& a, float4 v) {
                    float4 r{};
                    for (int i = 0; i < 4; ++i) r[size_t(i)] = a.c[0][size_t(i)] * v[0] + a.c[1][size_t(i)] * v[1] + a.c[2][size_t(i)] * v[2] + a.c[3][size_t(i)] * v[3];
                    return r;
                };
                const float4 cc = xf(c.vp_abs, world), oc = xf(o.vp_abs, world);
                const float4 back = xf(m, float4{cc[0] / cc[3], cc[1] / cc[3], cc[2] / cc[3], 1});
                check(std::abs(back[0] / back[3] - oc[0] / oc[3]) < 1e-3f && std::abs(back[1] / back[3] - oc[1] / oc[3]) < 1e-3f && back[3] > 0,
                      "SSR reprojection does not land on the previous frame's pixel");
            }
            // The +0xE0 shaft PS fixes the axes: NDC x = -Y / (x tan), NDC y = +Z / (x tan) of the light.
            check(std::abs(edge_u[0] - 1) < 1e-4f && std::abs(edge_u[1]) < 1e-4f, "light -Y side is not the map's +x edge");
            check(std::abs(edge_v[1] - 1) < 1e-4f && std::abs(edge_v[0]) < 1e-4f, "light +Z side is not the map's +y edge");
        }
        l.light_space = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        check(!light_shadow_view(l).has_value(), "a box light was given a spot shadow frustum");
    }
    {
        ExternStore ext;
        ext.channels.resize(45);
        ext.channels[19] = {7, 8, 9, 10};
        ext.channels[44] = {2, 3, 4, 5};
        ext.world_channels[19] = 1;
        const uint8_t program[] = {0x63, 19, 0x53, 0, 0x64, 19, 0x53, 1, 0x64, 44, 0x53, 2};
        std::array<float4, 3> out{};
        run_tfx(program, {}, out, ext.environment());
        check(out[0] == ext.channels[19], "world weather overwrote a render-global vector");
        check(out[1] == float4{1, 1, 1, 1}, "world scalar did not reach TFX 0x64");
        check(out[2] == float4{}, "world scalar inherited an unrelated sky vector");
        ext.world_channels.clear();
        run_tfx(program, {}, out, ext.environment());
        check(out[1] == float4{}, "disabling weather retained the previous state");
        check(ext.channels[44] == float4{2, 3, 4, 5}, "weather changed sky defaults");
    }
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
    const float4 camera_back = scene.get_v(ext::View, 0x100);  // third column of +0xE0, PS cb12[6]
    check(dot(V3{camera_back[0], camera_back[1], camera_back[2]}, camera.fwd) < -0.9999f,
          "transparent PS would sample its froxel from negative forward depth");
    const auto center_ray = scene.get_v(ext::View, 0x190);
    check(center_ray[2] == -1 && center_ray[3] == 0, "screen ray Z does not match the engine camera basis");
    const auto projection_z = scene.get_v(ext::View, 0xC0);
    check(projection_z[3] == -1, "engine projection does not project negative view Z in front of the camera");
    {
        // A co-authored mechanism starts its own clip at the same frame, independent
        // of a humanoid hand or any common joints, and returns to rest on other clips.
        PreviewAnimation preview;
        preview.model = TagHash(0x80A40001);
        AnimationClip camera_event;
        camera_event.tag = TagHash(0x80A40002);
        camera_event.frame_count = 2;
        auto& part = preview.animated_parts.emplace_back();
        part.model = TagHash(0x80A40003);
        part.skeleton.parents = {-1, 0};
        part.skeleton.bind.resize(2);
        part.skeleton.local_bind.resize(2);
        part.skeleton.inverse_bind.resize(2);
        part.rig.skeleton_bone = {0, 1};
        part.rig.object_space = {0, 0};
        part.rig.default_pose.resize(2);
        auto& local = part.clips[camera_event.tag];
        local.frame_count = 2;
        local.translations.push_back({1, {{0, 0, 0}, {2, 0, 0}}});
        auto poses = preview.poses(camera_event, 0.5f);
        check(poses.size() == 2 && poses[1].model == part.model && poses[1].bones.size() == 2 &&
              std::abs(poses[1].bones[1].translation[0] - 1) < 1e-6f,
              "the co-authored part did not play its own animation at the selected frame");
        camera_event.tag = TagHash(0x80A40004);
        check(preview.poses(camera_event, 0.5f).size() == 1,
              "the co-authored part's pose survived switching to an unrelated clip");
    }
    std::printf("render input tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
