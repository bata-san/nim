// Portable regression tests for the optimized core and viewer LOD policy.
#include "nim/core/binary.hpp"
#include "nim/core/block_cache.hpp"
#include "nim/core/parallel.hpp"
#include "nim/render/visibility_lod.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <latch>
#include <thread>
#include <stdexcept>
#include <vector>

using namespace nim;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static unsigned original_lod(uint32_t levels, float px, bool near_camera) {
    constexpr std::array<float, 7> detail_radii{16, 6, 3, 1.5f, 1, 0.5f, 0.25f};
    int step = 0;
    while (!near_camera && step < int(detail_radii.size()) && px < detail_radii[size_t(step)]) ++step;
    levels = levels ? levels : 1u;
    int level = std::countr_zero(levels);
    for (; step > 0 && (levels & (levels - 1)); --step) {
        levels &= levels - 1;
        level = std::countr_zero(levels);
    }
    return unsigned(level);
}

int main() {
    // Validate all eight-bit authored LOD masks, boundaries and their adjacent float values.
    std::vector<float> radii{0, -1, 100, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()};
    for (float x : {16.f, 6.f, 3.f, 1.5f, 1.f, 0.5f, 0.25f}) {
        radii.push_back(std::nextafter(x, 0.f));
        radii.push_back(x);
        radii.push_back(std::nextafter(x, std::numeric_limits<float>::infinity()));
    }
    size_t lod_cases = 0;
    for (uint32_t mask = 0; mask < 256; ++mask)
        for (float px : radii)
            for (bool near_camera : {false, true}) {
                CHECK(visibility_lod(mask, px, near_camera) == original_lod(mask, px, near_camera));
                ++lod_cases;
            }

    // Overflowed offsets must fail before pointer arithmetic or memcpy.
    const std::array<uint8_t, 8> bytes{};
    const BinaryReader reader(bytes);
    const size_t bad_offset = std::numeric_limits<size_t>::max();
    bool caught = false;
    try { (void)reader.u32_at(bad_offset); } catch (const std::out_of_range&) { caught = true; }
    CHECK(caught);
    std::array<uint8_t, 4> dst{};
    caught = false;
    try { reader.copy_at(bad_offset, dst); } catch (const std::out_of_range&) { caught = true; }
    CHECK(caught);

    // Eight overlapping requests for the same block must share exactly one decode.
    DecodedBlockCache cache(2);
    std::atomic<unsigned> decodes{0};
    constexpr size_t readers = 8;
    std::barrier start{std::ptrdiff_t(readers)};
    std::array<DecodedBlockCache::Data, readers> results;
    std::array<bool, readers> hits{};
    std::vector<std::thread> threads;
    auto decode = [&](uint32_t index) {
        ++decodes;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return std::vector<uint8_t>(64, uint8_t(index));
    };
    for (size_t i = 0; i < readers; ++i)
        threads.emplace_back([&, i] { start.arrive_and_wait(); results[i] = cache.get(7, decode, &hits[i]); });
    for (auto& t : threads) t.join();
    CHECK(decodes == 1);
    CHECK(std::count(hits.begin(), hits.end(), true) == readers - 1);
    for (const auto& data : results) CHECK(data == results[0] && data->at(0) == 7);
    cache.clear();
    CHECK(results[0]->at(0) == 7); // active readers survive cache clear
    CHECK(cache.get(7, decode) != results[0]);
    CHECK(decodes == 2);
    (void)cache.get(8, decode);
    (void)cache.get(9, decode);
    unsigned resident = 0;
    // With capacity 2 at least one of these keys must be absent (no mutation on lookup).
    for (uint32_t i : {7u, 8u, 9u}) {
        try { (void)cache.get(i, [](uint32_t) -> std::vector<uint8_t> { throw std::runtime_error("miss"); }); ++resident; }
        catch (const std::runtime_error&) {}
    }
    CHECK(resident == 2);
    // A failed decode must not poison the cache.
    caught = false;
    try { (void)cache.get(99, [](uint32_t) -> std::vector<uint8_t> { throw std::runtime_error("decode error"); }); }
    catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);
    CHECK(cache.get(99, decode)->at(0) == 99);
    // Clearing during a decode must not let that old decode re-populate the cache.
    DecodedBlockCache inflight;
    std::latch entered{1}, resume{1};
    std::thread pending([&] { (void)inflight.get(1, [&](uint32_t) {
        entered.count_down(); resume.wait(); return std::vector<uint8_t>{1};
    }); });
    entered.wait();
    inflight.clear();
    resume.count_down();
    pending.join();
    bool hit = true;
    CHECK(inflight.get(1, [](uint32_t) { return std::vector<uint8_t>{2}; }, &hit)->at(0) == 2);
    CHECK(!hit);

    auto& pool = WorkerPool::instance();
    // No dropped/duplicated indices; also covers chunk boundaries and zero grain.
    for (size_t grain : {0u, 1u, 8u, 256u}) {
        std::vector<std::atomic<unsigned>> seen(8193);
        for (int round = 0; round < 20; ++round) {
            pool.parallel_for(seen.size(), [&](size_t i) { ++seen[i]; }, grain);
        }
        for (const auto& n : seen) CHECK(n == 20);
    }
    // Worker exceptions propagate to the caller; the next job still completes.
    caught = false;
    try { pool.parallel_for(1024, [](size_t i) { if (i == 0) throw std::runtime_error("worker error"); }, 1); }
    catch (const std::runtime_error&) { caught = true; }
    CHECK(caught);
    std::atomic<unsigned> nested{0};
    pool.parallel_for(64, [&](size_t) { pool.parallel_for(16, [&](size_t) { ++nested; }); });
    CHECK(nested == 1024);
    // Two submitters must never replace each other's in-flight job state.
    std::array<std::atomic<unsigned>, 2> calls{};
    std::barrier simultaneous{2};
    std::thread a([&] { simultaneous.arrive_and_wait(); for (int k=0;k<40;++k) pool.parallel_for(257, [&](size_t) { ++calls[0]; }); });
    std::thread b([&] { simultaneous.arrive_and_wait(); for (int k=0;k<40;++k) pool.parallel_for(257, [&](size_t) { ++calls[1]; }); });
    a.join(); b.join();
    CHECK(calls[0] == 40 * 257 && calls[1] == 40 * 257);
    std::printf("LOD equivalence: %zu cases; concurrent cache: %zu requests / 1 decode; worker pool: %zu lanes\n", lod_cases, readers, pool.threads());
    if (failures) return 1;
    std::puts("all core optimization tests passed");
    return 0;
}
