// Unit tests that do not need a game install.
#include "nim/content/tfx_vm.hpp"
#include "nim/core/binary.hpp"
#include "nim/core/tag_hash.hpp"
#include "nim/core/tag_reader.hpp"

#include <cstdio>
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
        const float4 k[3] = {{1, 2, 3, 4}, {10, 20, 30, 40}, {2, 0, 0, 1}};  // cubic coeffs: 2x^3 + 1
        std::vector<float4> out(8);
        const uint8_t bc[] = {
            0x42, 0x01, 0x42, 0x00, 0x02, 0x53, 0x00,  // out0 = k1 - k0
            0x42, 0x00, 0x29, 0xE4, 0x53, 0x01,        // out1 = k0.wzyx (selectors 11 10 01 00)
            0x42, 0x00, 0x58, 0x03, 0x57, 0x03, 0x42, 0x01, 0x03, 0x53, 0x02,  // temp3 = k0; out2 = temp3 * k1
            0x42, 0x02, 0x42, 0x00, 0x0F, 0x53, 0x03,  // out3 = cubic(k2, k0)
            0x80, 0x42, 0x00, 0x53, 0x04,              // return stops execution
        };
        auto res = run_tfx(bc, k, out);
        CHECK(res.unsupported.empty());
        CHECK((out[0] == float4{9, 18, 27, 36}));
        CHECK((out[1] == float4{4, 3, 2, 1}));
        CHECK((out[2] == float4{10, 40, 90, 160}));
        CHECK((out[3] == float4{3, 17, 55, 129}));
        CHECK((out[4] == float4{0, 0, 0, 0}));
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

    if (g_failures) return 1;
    std::puts("all tests passed");
    return 0;
}
