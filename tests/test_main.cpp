// Unit tests that do not need a game install.
#include "nim/core/binary.hpp"
#include "nim/core/tag_hash.hpp"
#include "nim/core/tag_reader.hpp"

#include <cstdio>
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

    if (g_failures) return 1;
    std::puts("all tests passed");
    return 0;
}
