#include "nim/core/oodle.hpp"

#include <windows.h>

namespace nim {
namespace {

using OodleLZ_Decompress_t = intptr_t(__stdcall*)(
    const void* comp, intptr_t comp_len, void* raw, intptr_t raw_len,
    int fuzz_safe, int check_crc, int verbosity, void* dec_buf_base, intptr_t dec_buf_size,
    void* fp_callback, void* callback_user, void* decoder_memory, intptr_t decoder_memory_size,
    int thread_phase);

OodleLZ_Decompress_t g_decompress = nullptr;

} // namespace

bool Oodle::load(const std::filesystem::path& dll) {
    if (g_decompress) return true;
    HMODULE m = LoadLibraryW(dll.c_str());
    if (!m) return false;
    g_decompress = reinterpret_cast<OodleLZ_Decompress_t>(GetProcAddress(m, "OodleLZ_Decompress"));
    return g_decompress != nullptr;
}

bool Oodle::loaded() { return g_decompress != nullptr; }

size_t Oodle::decompress(std::span<const uint8_t> src, std::span<uint8_t> dst) {
    if (!g_decompress) return 0;
    // thread_phase 3 = unthreaded; fuzz_safe on so corrupt input cannot crash us.
    intptr_t r = g_decompress(src.data(), intptr_t(src.size()), dst.data(), intptr_t(dst.size()),
                              1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    return r > 0 ? size_t(r) : 0;
}

} // namespace nim
