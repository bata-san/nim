// Unit tests that do not need a game install.
#include "nim/core/binary.hpp"
#include "nim/core/tag_hash.hpp"

#include <cstdio>
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

    if (g_failures) return 1;
    std::puts("all tests passed");
    return 0;
}
