// Unit tests that do not need a game install.
#include "nim/content/animation.hpp"
#include "nim/content/audio.hpp"
#include "nim/content/map_audio.hpp"
#include "nim/content/codex.hpp"
#include "nim/content/usm.hpp"
#include "nim/content/map.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/content/weather.hpp"
#include "nim/core/binary.hpp"
#include "nim/core/tag_hash.hpp"
#include "nim/core/tag_reader.hpp"
#include "nim/sim/ai.hpp"
#include "ui_text.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <cmath>
#include <cstring>
#include <vector>

using namespace nim;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main() {
    // TagHash seen in w64_sr_globals_0120: 0x80A51CAA -> pkg 0x128, entry 0x1CAA
    TagHash h(0x80A51CAA);
    CHECK(h.is_pkg_ref());
    CHECK(h.pkg_id() == 0x128);
    CHECK(h.entry_index() == 0x1CAA);
    CHECK(TagHash::make(0x128, 0x1CAA) == h);
    CHECK(TagHash().is_none());

    std::vector<uint8_t> buf = {0x35, 0x00, 0x02, 0x00, 0x78, 0x56, 0x34, 0x12, 'h', 'i', 0};
    BinaryReader r(buf);
    CHECK(r.u16_at(0) == 0x35);
    CHECK(r.u16_at(2) == 2);
    CHECK(r.u32_at(4) == 0x12345678);
    CHECK(r.cstr_at(8) == "hi");
    bool threw = false;
    try { r.u64_at(8); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);

    // Vec layout taken from string container 80A40282: count at 0x08, rel offset at 0x10 -> header at 0x60.
    std::vector<uint8_t> tag(0x80, 0);
    auto put32 = [&](size_t o, uint32_t v) { std::memcpy(tag.data() + o, &v, 4); };
    auto put64 = [&](size_t o, uint64_t v) { std::memcpy(tag.data() + o, &v, 8); };
    put64(0x08, 2);
    put64(0x10, 0x50);
    put64(0x60, 2);
    put32(0x68, 0x80800070);
    put32(0x70, 0xAAAA);
    put32(0x74, 0xBBBB);
    TagReader tr(tag);
    auto v = tr.vec_at(0x08);
    CHECK(v.count == 2);
    CHECK(v.data == 0x70);
    CHECK(v.element_class == 0x80800070);
    CHECK(tr.u32_at(v.data + 4) == 0xBBBB);
    put64(0x60, 3);  // header count mismatch must be rejected
    threw = false;
    try { tr.vec_at(0x08); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);

    // TFX VM: operand order (a - b with b on top), outputs, temps, permute, cubic.
    {
        const float4 k[] = {{1, 2, 3, 4}, {10, 20, 30, 40}, {2, 0, 0, 1}, {3, -4, 0, 0}};
        std::vector<float4> out(8);
        const uint8_t bc[] = {
            0x42, 0x01, 0x42, 0x00, 0x02, 0x53, 0x00,  // out0 = k1 - k0
            0x42, 0x00, 0x29, 0xE4, 0x53, 0x01,        // out1 = k0.wzyx (selectors 11 10 01 00)
            0x42, 0x00, 0x58, 0x03, 0x57, 0x03, 0x42, 0x01, 0x03, 0x53, 0x02,  // temp3 = k0; out2 = temp3 * k1
            0x42, 0x02, 0x42, 0x00, 0x0F, 0x53, 0x03,  // out3 = cubic(k2, k0)
            0x42, 0x03, 0x2D, 0x53, 0x04,              // out4 = broadcast length(k3)
            0x80, 0x42, 0x00, 0x53, 0x05,              // return stops execution
        };
        auto res = run_tfx(bc, k, out);
        CHECK(res.unsupported.empty());
        CHECK((out[0] == float4{9, 18, 27, 36}));
        CHECK((out[1] == float4{4, 3, 2, 1}));
        CHECK((out[2] == float4{10, 40, 90, 160}));
        CHECK((out[3] == float4{3, 17, 55, 129}));
        CHECK((out[4] == float4{5, 5, 5, 5}));
        CHECK((out[5] == float4{0, 0, 0, 0}));
    }

    // Sequencer (map environment channel expressions): 0x4A n pushes input n, 0x49 is the
    // eight-stop gradient, 0x4C n pops to output n.
    {
        float4 k[11]{};
        k[1] = {1, 0, 0, 0};           // red delta at stop 0
        k[5] = {0, 0, 0, 2};           // red delta at stop 7
        k[9] = {0.2f, 0.4f, 0.6f, 0.7f};
        k[10] = {0.8f, 0.9f, 0.95f, 0.97f};
        const uint8_t bc[] = {0x4A, 0x00, 0x29, 0x00, 0x49, 0x00, 0x4C, 0x01};
        auto at = [&](float t) {
            std::vector<float4> out(2);
            TfxEnvironment env;
            env.sequencer_input = [t](uint8_t i) { return i == 0 ? float4{t, 0, 0, 0} : float4{}; };
            run_tfx(bc, k, out, env);
            return out[1][0];
        };
        CHECK(std::fabs(at(0.3f) - 0.5f) < 1e-5f);    // half way through segment 0
        CHECK(std::fabs(at(0.985f) - 2.0f) < 1e-4f);  // segment 0 full, half way through 7 (ends at 1)
        CHECK(at(0.1f) == 0);
    }

    // Material frame evaluation: time-driven UVs, edited object state, real global defaults and
    // an output that reads its initial value. Rewinding must reproduce the original frame.
    {
        const float4 initial[] = {{5, 5, 5, 5}, {7, 8, 9, 10}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        const float4 constants[] = {{0.04f, 0.04f, 0.04f, 0.04f}};
        const uint8_t program[] = {
            0x51, 0x00, 0x4A, 0x01, 0x00, 0x01, 0x53, 0x00, // initial[0] + frame.time
            0x4A, 0x01, 0x00, 0x42, 0x00, 0x03, 0x1D, 0x53, 0x02, // frac(time * .04)
            0x62, 0xEA, 0xA8, 0xE3, 0xCF, 0x63, 0xCA, 0x03, 0x53, 0x03,
        };
        float time = 0, gate = 1;
        TfxEnvironment env;
        env.extern_float = [&](uint8_t id, uint8_t off) { return id == 1 && off == 0 ? time : 0; };
        env.object_channel = [&](uint32_t h) { CHECK(h == 0xEAA8E3CF); return float4{gate, gate, gate, gate}; };
        env.global_channel = [&](uint8_t ch) { CHECK(ch == 202); return float4{0, 16, 16, 0.5f}; };
        std::vector<float4> out;
        const auto result = evaluate_tfx(program, constants, initial, out, env);
        CHECK(result.unsupported.empty());
        CHECK(result.live_inputs);
        CHECK(out[0] == initial[0]);
        CHECK(out[1] == initial[1]);
        CHECK((out[3] == float4{0, 16, 16, 0.5f}));
        const auto first = out;
        time = 2.5f;
        gate = 0;
        evaluate_tfx(program, constants, initial, out, env);
        CHECK((out[0] == float4{7.5f, 7.5f, 7.5f, 7.5f}));
        CHECK(std::fabs(out[2][0] - 0.1f) < 1e-6f);
        CHECK(out[1] == initial[1]);
        CHECK((out[3] == float4{}));
        time = 0;
        gate = 1;
        evaluate_tfx(program, constants, initial, out, env);
        CHECK(out == first);
        const uint8_t globals_only[] = {0x63, 0xCA, 0x53, 0x00};
        CHECK(!evaluate_tfx(globals_only, {}, initial, out, env).live_inputs);
    }

    {
        const uint8_t atlas[] = {0x68, 0, 0xBB, 0x53, 0, 0x69, 4, 0, 0x53, 1, 0x68, 5, 0x11, 0x53, 2};
        const auto sky_atlas = tfx_animated_atlas_operands(atlas);
        CHECK(sky_atlas.size() == 1 && sky_atlas.front().count == 4);
        CHECK(tfx_animated_atlas_operands(std::span(atlas).first(10)).empty());
        const uint8_t object_atlas[] = {0x68, 3, 0x11, 0x68, 4, 0xBB, 0x62, 0xF2, 0x17, 0x53, 0xD4,
                                        0x29, 0x00, 0x69, 5, 0x00};
        const auto material = tfx_animated_atlas_operands(object_atlas);
        CHECK(material.size() == 1 && material.front().count == 5);
        CHECK(tfx_animated_atlas_operands(std::span(object_atlas).subspan(3)).empty());
        const uint8_t dual_atlas[] = {0x68, 0, 0xBB, 0x69, 4, 0, 0x68, 5, 0x11,
                                       0x68, 6, 0x11, 0x68, 7, 0xBB, 0x69, 8, 0};
        const auto dual = tfx_animated_atlas_operands(dual_atlas);
        CHECK(dual.size() == 2 && dual[0].step == 0 && dual[0].count == 4 && dual[0].scale == 5);
        CHECK(dual.size() == 2 && dual[1].step == 7 && dual[1].count == 8 && dual[1].scale == 6);
        const uint8_t layers[] = {0x68, 0, 0xBB, 0x68, 1, 0xBB, 0x68, 2, 0xBB,
                                  0x69, 5, 0, 0x69, 6, 0, 0x69, 7, 0,
                                  0x68, 8, 0x11, 0x68, 9, 0x11, 0x68, 10, 0x11};
        const auto mapped = tfx_animated_atlas_operands(layers);
        CHECK(mapped.size() == 3 && mapped[2].step == 2 && mapped[2].count == 7 && mapped[2].scale == 10);
        std::vector<uint8_t> shared{0x68, 3, 0x11, 0x68, 4, 0x11};
        for (uint8_t scale = 5; scale <= 29; scale += 3)
            shared.insert(shared.end(), {0x68, scale, 0x11, 0x68, uint8_t(scale + 1), 0xBB, 0x69, uint8_t(scale + 2), 0});
        const auto shared_slots = tfx_animated_atlas_operands(shared);
        const uint8_t expected_slots[]{1, 2, 1, 1, 1, 2, 2, 2, 1, 2, 2};
        CHECK(shared_slots.size() == std::size(expected_slots));
        for (size_t i = 0; i < shared_slots.size() && i < std::size(expected_slots); ++i)
            CHECK(shared_slots[i].texture_slot == expected_slots[i]);
        CHECK(shared_slots.size() == 11 && shared_slots[0].count == AtlasOperands::kUnspecified &&
              shared_slots[8].count == 25 && shared_slots[10].count == 31);
        TfxEnvironment env;
        env.texture_layer_count = [](uint8_t) { return float4{0.125f, 0.25f, 0.125f, 0.03125f}; };
        env.atlas_frame_count = [](uint8_t operand, uint8_t texture) {
            CHECK(operand == 4 && texture == 0);
            return 32.0f;
        };
        std::array<float4, 3> out{};
        run_tfx(atlas, {}, out, env);
        CHECK((out[0] == float4{0.125f, 0.03125f, 0.125f, 0.03125f}));
        CHECK((out[1] == float4{32, 32, 32, 32}));
        CHECK((out[2] == float4{0.125f, 0.25f, 0.125f, 0.25f}));
    }

    {
        // Weather: a state's channels at the middle of their range; unregistered channels skipped.
        WeatherController c;
        c.name = "exp_outpost/hz_emberwave";
        c.states = {{1, {{0x798A4399, 19, 0, 0}, {0xB, 17, 0, 0}}}, {2, {{0x798A4399, 19, 1, 1}, {0xB, 17, 1, 1}, {0xC, -1, 5, 5}}}};
        CHECK(c.heat_cascade());
        c.name = "stripped-name";
        CHECK(c.heat_cascade());
        WeatherController unrelated;
        unrelated.name = "emberwave-but-no-heat-channel";
        unrelated.states = {{1, {{0xB60A35EC, 44, 1, 1}}}};
        CHECK(!unrelated.heat_cascade());
        CHECK(c.strongest_state() == &c.states[1]);
        WeatherState rain{3, {{0xD, 7, 50, 100}}};
        const auto v = weather_values(rain);
        CHECK(v.size() == 1 && v[0].first == 7 && v[0].second == 75);
        CHECK(weather_values(c.states[1]).size() == 2);
    }

    {
        // Atmosphere: lookup slice coordinates between time keys, sun path interpolation.
        MapAtmosphere a;
        for (size_t i = 0; i < 16; ++i) a.slice_times[i] = 0.1f + 0.05f * float(i);
        CHECK(std::fabs(a.slice_coordinate(0.0f) - 0.5f / 16) < 1e-6f);
        CHECK(std::fabs(a.slice_coordinate(0.125f) - 1.0f / 16) < 1e-5f);  // half way from key 0 to key 1
        CHECK(std::fabs(a.slice_coordinate(0.99f) - 15.5f / 16) < 1e-6f);
        a.paths[0] = {{1, 0, 0}, {0, 1, 0}};
        const auto d = a.path_direction(0, 0.25f);  // half way between the two entries
        CHECK(std::fabs(d[0] - d[1]) < 1e-6f && std::fabs(d[0] * d[0] + d[1] * d[1] - 1) < 1e-5f);
        CHECK(a.path_direction(1, 0.5f)[2] == 1);  // an empty path points up
    }

    {
        // A ground-level IK target (the robots' feet, 3.82): thigh and knee turn so the ankle reaches
        // the target node's translation; the limb keeps its lengths and the knee its bend side.
        Skeleton leg;
        leg.parents = {-1, 0, 1, 2, 3};
        leg.hashes = {1, 2, 3, 4, 5};
        leg.bind.resize(5);
        leg.bind[1].translation = {0, 0, 1};
        leg.bind[2].translation = {0, 0.1f, 1};
        leg.bind[3].translation = {0.05f, 0.1f, 0.55f};
        leg.bind[4].translation = {0, 0.1f, 0.1f};
        leg.local_bind.resize(5);
        for (size_t i = 0; i < 5; ++i)
            leg.local_bind[i] = leg.parents[i] < 0 ? leg.bind[i] : inverse(leg.bind[size_t(leg.parents[i])]) * leg.bind[i];
        AnimationRig rig;
        rig.skeleton_bone = {0, 2, 3, 4};
        rig.object_space = {0, 1, 1, 1};
        rig.default_pose.resize(4);
        rig.default_pose[3].translation = {0, 0.1f, 0.1f};
        rig.ik_root = {-1, -1, -1, 2};
        AnimationClip clip;
        clip.frame_count = 1;
        clip.translations = {{3, {{0.12f, 0.1f, 0.12f}}}};
        std::vector<BoneTransform> posed;
        pose_skeleton(leg, rig, clip, 0, posed);
        auto dist = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
        };
        CHECK(dist(posed[4].translation, {0.12f, 0.1f, 0.12f}) < 1e-3f);
        CHECK(std::fabs(dist(posed[2].translation, posed[3].translation) - dist(leg.bind[2].translation, leg.bind[3].translation)) < 1e-3f);
        CHECK(std::fabs(dist(posed[3].translation, posed[4].translation) - dist(leg.bind[3].translation, leg.bind[4].translation)) < 1e-3f);
        // Still bent forward: the knee is in front of the hip-ankle line.
        CHECK(posed[3].translation[0] > 0.12f * (1 - posed[3].translation[2]) / 0.88f + 0.02f);
        CHECK(std::fabs(posed[4].rotation[3]) > 0.9999f);  // the ankle keeps its own object rotation
        clip.translations = {{3, {{0.6f, 0.1f, 0.1f}}}};  // far from the FK ankle: not a target
        pose_skeleton(leg, rig, clip, 0, posed);
        CHECK(dist(posed[4].translation, leg.bind[4].translation) < 1e-4f);
    }

    {
        // Clips of a rig family rebind by bone group (3.83): the shared first group keeps its nodes,
        // a group the target rig also has moves to its position there, others are dropped.
        AnimationRig rig;
        rig.skeleton_bone = {0, 1, 2, 3, 4};
        rig.bone_groups = {{0xA, 2}, {0xD, 1}, {0xB, 2}};
        AnimationClip clip;
        clip.bone_count = 5;
        clip.bone_groups = {{0xA, 2}, {0xC, 1}, {0xB, 2}};
        clip.rotations = {{0, {{0, 0, 0, 1}}}, {1, {{0, 0, 0, 1}}}, {2, {{0, 0, 0, 1}}}, {3, {{0, 0, 0, 1}}}, {4, {{0, 0, 0, 1}}}};
        clip.translations = {{2, {{1, 2, 3}}}, {4, {{4, 5, 6}}}};
        const auto mapped = map_clip_groups(clip, rig);
        CHECK(mapped && mapped->bone_count == 5 && mapped->rotations.size() == 4);
        CHECK(mapped->rotations[0].bone == 0 && mapped->rotations[1].bone == 1 && mapped->rotations[2].bone == 3 && mapped->rotations[3].bone == 4);
        CHECK(mapped->translations.size() == 1 && mapped->translations[0].bone == 4 && mapped->translations[0].frames[0][0] == 4);
        clip.bone_groups = {{0xE, 2}, {0xC, 1}, {0xB, 2}};  // the first group is not the rig's
        CHECK(!map_clip_groups(clip, rig));
        clip.bone_groups = {{0xA, 2}};  // groups that do not cover the tracks
        CHECK(!map_clip_groups(clip, rig));
    }

    {
        // A local mechanical joint slides and scales. Its child follows the resulting transform.
        Skeleton mechanism;
        mechanism.parents = {-1, 0, 1};
        mechanism.hashes = {1, 2, 3};
        mechanism.bind.resize(3);
        mechanism.bind[1].translation = {0, 0, 1};
        mechanism.bind[2].translation = {0, 0, 2};
        mechanism.local_bind = mechanism.bind;
        mechanism.local_bind[2].translation = {0, 0, 1};
        AnimationRig rig;
        rig.skeleton_bone = {0, 1};
        rig.object_space = {0, 0};
        rig.default_pose.assign(mechanism.local_bind.begin(), mechanism.local_bind.begin() + 2);
        AnimationClip clip;
        clip.frame_count = 2;
        clip.translations = {{1, {{0, 0, 1}, {0, 0, 2}}}};
        clip.scales = {{1, {1, 2}}};
        std::vector<BoneTransform> posed;
        pose_skeleton(mechanism, rig, clip, 1, posed);
        CHECK(posed[1].translation[2] == 2);
        CHECK(posed[2].translation[2] == 4);
        CHECK(posed[2].scale == 2);
        rig.default_pose[1].translation[2] = 3;
        pose_skeleton(mechanism, rig, clip, 1, posed);
        CHECK(posed[1].translation[2] == 1);
        rig.bind_relative = {0, 1};
        rig.default_pose[1] = {};
        clip.translations[0].frames = {{{0, 0, 0}, {0, 0, 1}}};
        pose_skeleton(mechanism, rig, clip, 0, posed);
        CHECK(posed[1] == mechanism.bind[1]);
        pose_skeleton(mechanism, rig, clip, 1, posed);
        CHECK(posed[1].translation[2] == 2);

        // A matching joint follows by name and bind transform even when the part's bone order differs.
        Skeleton attachment;
        attachment.parents = {-1, 0, 1, 1, -1};
        attachment.hashes = {1, 3, 4, 2, 5};
        attachment.bind.resize(5);
        attachment.bind[1] = mechanism.bind[2];
        CHECK((assembly_pose_map(mechanism, attachment) == std::vector<int32_t>{0, 2, 2, 2, -1}));

        Skeleton variant = mechanism;
        variant.bind[2].translation[2] += 0.1f;
        CHECK((assembly_pose_map(mechanism, variant) == std::vector<int32_t>{0, 1, 2}));
    }

    {
        // Identity-rest finger channels retain the bind axes, and curls use those local axes.
        constexpr float q = 0.70710678f;
        Skeleton hand;
        hand.hashes = {1, 0x67CB838C, 3, 4, 5};
        hand.parents = {-1, 0, 1, 2, 0};
        hand.local_bind.resize(5);
        hand.local_bind[2].rotation = {0, 0, q, q};
        hand.local_bind[3].translation = {0, 1, 0};
        hand.local_bind[4].rotation = hand.local_bind[2].rotation;
        hand.bind = hand.local_bind;
        hand.bind[3] = hand.bind[2] * hand.local_bind[3];

        AnimationRig rig;
        rig.skeleton_bone = {0, 1, 2, 4};
        rig.object_space = {1, 1, 0, 0};
        rig.bind_relative = {0, 0, 1, 0};
        rig.default_pose.resize(rig.size());
        AnimationClip clip;
        clip.frame_count = 2;
        clip.rotations = {{2, {{{0, 0, 0, 1}, {q, 0, 0, q}}}}};

        std::vector<BoneTransform> posed;
        pose_skeleton(hand, rig, clip, 0, posed);
        CHECK(std::fabs(posed[3].translation[0] + 1) < 1e-5f);
        pose_skeleton(hand, rig, clip, 1, posed);
        CHECK(std::fabs(posed[3].translation[2] - 1) < 1e-5f);
        CHECK(posed[1].rotation == BoneTransform{}.rotation);
        CHECK(posed[4].rotation == BoneTransform{}.rotation);
    }

    {
        // The FFT inverse MDCT matches the direct Vorbis formula.
        for (size_t n : {size_t(256), size_t(2048)}) {
            std::vector<float> in(n / 2), fast(n), slow(n);
            for (size_t k = 0; k < in.size(); ++k) in[k] = float(std::sin(double(k) * 0.37) + 0.25 * std::cos(double(k * k) * 0.011));
            inverse_mdct(in, fast);
            double worst = 0;
            for (size_t i = 0; i < n; ++i) {
                double acc = 0;
                for (size_t k = 0; k < n / 2; ++k)
                    acc += in[k] * std::cos(2.0 * 3.14159265358979323846 / double(n) * (double(i) + 0.5 + double(n) / 4) * (double(k) + 0.5));
                worst = std::max(worst, std::abs(acc - double(fast[i])));
            }
            CHECK(worst < 1e-2);
        }
    }

    {
        // ADX: a two-channel stream with one frame of zero samples, then the end marker.
        std::vector<uint8_t> adx = {0x80, 0x00, 0x00, 0x10, 3, 18, 4, 2, 0, 0, 0xBB, 0x80, 0, 0, 0, 32, 0x01, 0xF4, 4, 0};
        adx.resize(0x14, 0);
        for (int ch = 0; ch < 2; ++ch) {
            adx.push_back(0), adx.push_back(9);
            for (int i = 0; i < 16; ++i) adx.push_back(ch ? 0x11 : 0x00);
        }
        adx.push_back(0x80), adx.push_back(0x01);
        adx.resize(adx.size() + 34, 0);
        AdxDecoder dec;
        std::vector<int16_t> pcm;
        dec.feed(std::span(adx).first(10), pcm);  // split mid-header
        dec.feed(std::span(adx).subspan(10), pcm);
        CHECK(dec.sample_rate == 48000 && dec.channels == 2);
        CHECK(pcm.size() == 64);
        CHECK(pcm[0] == 0 && pcm[1] == 10);
        CHECK(dec.ended);

        // @UTF: one per-row u32 column and a constant string column.
        std::vector<uint8_t> t = {'@', 'U', 'T', 'F', 0, 0, 0, 0};
        std::vector<uint8_t> body = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 2};
        body.push_back(0x54), body.insert(body.end(), {0, 0, 0, 6});           // "value", per row, u32
        body.push_back(0x3A), body.insert(body.end(), {0, 0, 0, 12});          // "name", constant string
        body.insert(body.end(), {0, 0, 0, 0});                                // constant -> "abc"
        const uint16_t rows = uint16_t(body.size());
        body.insert(body.end(), {0, 0, 0, 7, 0, 0, 0, 9});
        const uint32_t strings = uint32_t(body.size());
        for (const char* str : {"TBL", "value", "name", "abc"}) body.insert(body.end(), str, str + std::strlen(str) + 1);
        const uint32_t data = uint32_t(body.size());
        auto put32 = [&](size_t o, uint32_t v) { body[o] = uint8_t(v >> 24), body[o + 1] = uint8_t(v >> 16), body[o + 2] = uint8_t(v >> 8), body[o + 3] = uint8_t(v); };
        body[2] = uint8_t(rows >> 8), body[3] = uint8_t(rows);
        put32(4, strings), put32(8, data), put32(12, 0);
        put32(25, 4), put32(30, 10), put32(34, 15);
        t[4] = 0, t[5] = 0, t[6] = uint8_t(body.size() >> 8), t[7] = uint8_t(body.size());
        t.insert(t.end(), body.begin(), body.end());
        auto table = parse_utf_table(t);
        CHECK(table.name == "TBL");
        CHECK(table.rows.size() == 2);
        CHECK(table.integer(1, "value") == 9);
        CHECK(table.text(0, "name") == "abc");
    }

    // UI text: every key has a non-empty text in each interface language, and the
    // languages keep the same printf / std::format placeholders.
    for (size_t i = 0; i < size_t(ui::S::Count); ++i) {
        const auto key = ui::S(i);
        const std::string ja = ui::tr_in(key, ui::Lang::Ja);
        CHECK(!ja.empty());
        CHECK(!std::string(ui::tr_in(key, ui::Lang::En)).empty());
        CHECK(!std::string(ui::tr_in(key, ui::Lang::Ko)).empty());
        auto count = [](const std::string& s, char c) { return std::count(s.begin(), s.end(), c); };
        for (auto l : {ui::Lang::En, ui::Lang::Ko}) {
            const std::string t = ui::tr_in(key, l);
            if (count(t, '{') != count(ja, '{') || count(t, '%') != count(ja, '%')) {
                std::fprintf(stderr, "UI key %zu: placeholders differ in language %d\n", i, int(l));
                ++g_failures;
            }
        }
    }
    CHECK(std::string(ui::tr_in(ui::S::EstimatesHeader, ui::Lang::En)).find("Estimated") != std::string::npos);
    CHECK(std::string(ui::tr_in(ui::S::EstimatesHeader, ui::Lang::Ko)).find("추정") != std::string::npos);
    CHECK(std::string(ui::tr_in(ui::S::EstimatesHeader, ui::Lang::Ja)).find("推定") != std::string::npos);
    CHECK(std::string(ui::trf(ui::S::LogModelLoaded, "m", 2, 1.0, 2.0, "")).size() > 0);
    ui::Lang parsed = ui::Lang::Ja;
    CHECK(ui::parse_lang("ko", parsed) && parsed == ui::Lang::Ko);
    CHECK(!ui::parse_lang("fr", parsed));

    // Codex definition table: one entry with one display component.
    {
        std::vector<uint8_t> b(0x80, 0);
        auto w32 = [&](size_t o, uint32_t v) { std::memcpy(&b[o], &v, 4); };
        auto w64 = [&](size_t o, uint64_t v) { std::memcpy(&b[o], &v, 8); };
        w64(0x00, b.size());
        w64(0x08, 1); w64(0x10, 0x10);                        // Vec -> header at 0x20
        w64(0x20, 1); w32(0x28, nim::kClassCodexDefinition);
        w32(0x30, 0x11111111); w32(0x34, nim::kCodexKindEntry);
        w64(0x38, 1); w64(0x40, 0x10);                        // components -> header at 0x50
        w64(0x50, 1); w32(0x58, 0x808028EB);
        w64(0x60, 0x0C);                                      // pointer -> 0x6C
        w32(0x68, nim::kClassCodexDisplay);
        w32(0x74, 0xCAFEF00D);                                // title hash at +8
        auto defs = nim::parse_codex_definitions(b);
        CHECK(defs.size() == 1);
        CHECK(defs[0].hash == 0x11111111 && defs[0].kind == nim::kCodexKindEntry);
        CHECK(defs[0].components.size() == 1);
        CHECK(defs[0].components[0].cls == nim::kClassCodexDisplay && defs[0].components[0].offset == 0x6C);
        w64(0x60, 0x1000);                                    // pointer out of range
        bool threw = false;
        try { nim::parse_codex_definitions(b); } catch (...) { threw = true; }
        CHECK(threw);
    }

    // AI simulation (3.69): motion choice, patrol inside the firing area, awareness, invisibility.
    {
        nim::MotionSet ms;
        ms.locomotion = {{nim::TagHash(1), 1, 1.2f, {1, 0}}, {nim::TagHash(2), 1, 2.5f, {1, 0}}, {nim::TagHash(3), 1, 1.0f, {-1, 0}}};
        ms.idle = nim::MotionClip{nim::TagHash(4), 3, 0, {}};
        CHECK(!nim::select_motion(ms, {0.1f, 0}).locomotion);
        CHECK(nim::select_motion(ms, {1.2f, 0}).locomotion == size_t(0));
        CHECK(nim::select_motion(ms, {2.6f, 0}).locomotion == size_t(1));
        CHECK(nim::select_motion(ms, {-0.9f, 0}).locomotion == size_t(2));
        CHECK(std::abs(nim::select_motion(ms, {1.8f, 0}).rate - 1.5f) < 0.01f);  // nearest is the 1.2 m/s walk

        nim::MapGameplay g;
        g.scripts.resize(2);
        g.characters.resize(1);
        g.characters[0].name_estimate = "ue_elite_guard";
        nim::MapFiringArea fa;
        fa.outline = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
        fa.triangles = {{0, 1, 2}, {0, 2, 3}};
        g.firing_areas.push_back(fa);
        g.objectives.push_back({0, {0}});
        nim::MapSquad sq;
        sq.objective = 0;
        sq.members = {0, 0};
        nim::MapGameplayPoint pt;
        pt.position = {5, 5, 0};
        sq.positions = {pt};
        g.squads.push_back(sq);
        auto run = [&](bool invisible, std::array<float, 3> player, float seconds) {
            nim::AiParams prm;
            prm.player_invisible = invisible;
            nim::AiSimulation sim(g, prm, 7);
            for (int i = 0; i < int(seconds * 10); ++i) sim.step(0.1f, player);
            return sim.agents();
        };
        const auto a = run(true, {1000, 0, 0}, 60), b = run(true, {1000, 0, 0}, 60);
        CHECK(a.size() == 2 && a[0].sight == 40.0f);
        CHECK(a[0].position == b[0].position && a[1].position == b[1].position);  // deterministic for a seed
        bool inside = true, moved = false;
        for (const auto& x : a) {
            inside = inside && x.position[0] >= -0.01f && x.position[0] <= 10.01f && x.position[1] >= -0.01f && x.position[1] <= 10.01f;
            moved = moved || std::abs(x.position[0] - 5) > 0.5f || std::abs(x.position[1] - 5) > 0.5f;
        }
        CHECK(inside && moved);
        CHECK(a[0].awareness == 0 && a[0].alert == nim::AiAlert::Unaware);
        // The player standing next to them: awareness rises (hearing at least), but an invisible
        // player changes nothing about where they go.
        const auto c = run(true, {5, 6, 0}, 5), d = run(true, {1000, 0, 0}, 5);
        CHECK(c[0].awareness > 0.5f && c[0].alert != nim::AiAlert::Unaware);
        CHECK(c[0].position == d[0].position && c[1].position == d[1].position);
        const auto e = run(false, {5, 6, 0}, 5);
        CHECK(e[0].alert == nim::AiAlert::Alerted);
        CHECK(!nim::ai_estimates(nim::AiParams{}).empty());

        // Walk grids (3.72): room to the nearest blocked cell, paths that follow the floor up
        // steps of at most 0.45 m, and bodies kept apart by their radii.
        nim::WalkGrid wg;
        wg.x0 = 0, wg.y0 = 0, wg.cell = 0.25f, wg.w = wg.h = 40;
        wg.height.assign(1600, 0.0f);
        std::vector<uint8_t> free(1600, 1);
        free[20 * 40 + 20] = 0;
        wg.set_free(free);
        CHECK(std::abs(wg.room[20 * 40 + 21] - 0.125f) < 1e-4f && std::abs(wg.room[20 * 40 + 24] - 0.875f) < 1e-4f);
        CHECK(!wg.walkable(5.3f, 5.1f, 0.3f) && wg.walkable(6.2f, 5.1f, 0.5f) && !wg.walkable(6.2f, 5.1f, 0.9f));
        nim::AiSimulation ws(g, nim::AiParams{}, 7);
        auto stepped = wg;
        for (uint32_t r = 0; r < 40; ++r)
            for (uint32_t c = 30; c < 40; ++c) stepped.height[r * 40 + c] = 1.0f;  // a 1 m ledge from x = 7.5
        ws.set_walk_grid(0, stepped);
        CHECK(ws.path_clear({2, 2, 0}, {7, 2, 0}, 0.3f) && !ws.path_clear({2, 2, 0}, {9, 2, 1}, 0.3f));
        CHECK(ws.ground(8, 2, 0.8f) == 1.0f && ws.ground(8, 2, -1.0f) == std::nullopt);
        ws.set_character_radius(0, 0.6f);
        for (int i = 0; i < 20; ++i) ws.step(0.1f, {1000, 0, 0});
        const auto& sa = ws.agents();
        CHECK(std::hypot(sa[0].position[0] - sa[1].position[0], sa[0].position[1] - sa[1].position[1]) > 1.1f);

        // Path search (3.73): around a wall from y = 0 to 7.5 m at x = 5 m.
        nim::AiSimulation ps(g, nim::AiParams{}, 7);
        auto walled = wg;
        std::vector<uint8_t> open_floor(1600, 1);
        for (uint32_t r = 0; r < 30; ++r) open_floor[r * 40 + 20] = 0;
        walled.set_free(open_floor);
        ps.set_walk_grid(0, walled);
        CHECK(!ps.path_clear({2, 2, 0}, {8, 2, 0}, 0.3f));
        const auto route = ps.find_path({2, 2, 0}, {8, 2, 0}, 0.3f);
        CHECK(route && !route->empty() && route->back() == (std::array<float, 3>{8, 2, 0}));
        if (route) {
            std::array<float, 3> from{2, 2, 0};
            bool walkable = true, around = false;
            for (const auto& w : *route) {
                walkable = walkable && ps.path_clear(from, w, 0.3f);
                around = around || w[1] > 7.5f;
                from = w;
            }
            CHECK(walkable && around);
        }
        CHECK(!ps.find_path({2, 2, 0}, {8, 2, 3}, 0.3f));  // no floor at that height
    }

    // Map ambient audio emitter geometry (point, polyline segment, global).
    {
        MapAudioEmitter pt;
        pt.position = {10, 0, 0};
        CHECK(std::abs(pt.distance_to({13, 4, 0}) - 5.0f) < 1e-4f);

        MapAudioEmitter poly;
        poly.polyline = true;
        poly.points = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
        const auto cp = poly.closest_point({5, 3, 0});
        CHECK(std::abs(cp[0] - 5.0f) < 1e-4f && std::abs(cp[1]) < 1e-4f && std::abs(cp[2]) < 1e-4f);
        CHECK(std::abs(poly.distance_to({5, 3, 0}) - 3.0f) < 1e-4f);
        CHECK(std::abs(poly.distance_to({14, 5, 0}) - 4.0f) < 1e-4f);

        MapAudioEmitter glob;
        glob.global = true;
        glob.position = {999, 999, 999};
        CHECK(glob.distance_to({0, 0, 0}) == 0.0f);
    }

    if (g_failures) return 1;
    std::puts("all tests passed");
    return 0;
}
