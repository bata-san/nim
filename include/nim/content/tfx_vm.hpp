#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace nim {

using float4 = std::array<float, 4>;

// Inputs the TFX expression bytecode may read. Anything left unset evaluates to the defaults
// the engine would use for an object with no animation state (ONE for channels, ZERO for externs).
struct TfxEnvironment {
    std::function<float4(uint8_t extern_id, uint8_t offset)> extern_vec4;  // offset in vec4 units
    std::function<float(uint8_t extern_id, uint8_t offset)> extern_float;  // offset in float units
    std::function<float4(uint8_t channel)> global_channel;
    std::function<float4(uint32_t hash)> object_channel;
};

struct TfxExternTexture {
    uint32_t slot = 0;        // destination texture register (PopTextureView)
    uint8_t extern_id = 0;    // extern block the texture view comes from
    uint8_t offset = 0;       // in 8-byte units (byte offset = offset * 8)
};

struct TfxSampler {
    uint32_t slot = 0;   // destination sampler register (PopSampler & 0x1F)
    uint8_t index = 0;   // index into the stage's sampler list (PushSampler)
};

struct TfxResult {
    std::vector<TfxExternTexture> extern_textures;
    std::vector<TfxSampler> samplers;
    std::vector<std::string> unsupported;  // opcodes that were stubbed, for diagnostics
    size_t instructions = 0;
};

// Runs a TFX expression program. `constants` are the stage's bytecode constants; `out` is the
// cbuffer (pre-filled with the stage's initial cbuffer contents) the program writes into.
TfxResult run_tfx(std::span<const uint8_t> bytecode, std::span<const float4> constants, std::span<float4> out,
                  const TfxEnvironment& env = {}, std::string* trace = nullptr);

// One instruction per line, for `nimcli technique`.
std::string disassemble_tfx(std::span<const uint8_t> bytecode);

} // namespace nim
