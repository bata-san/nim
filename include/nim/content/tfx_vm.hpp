#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace nim {

using float4 = std::array<float, 4>;

// Inputs the TFX expression bytecode may read. Missing inputs and unimplemented operations fail explicitly.
struct TfxEnvironment {
    std::function<float4(uint8_t extern_id, uint8_t offset)> extern_vec4;  // offset in vec4 units
    std::function<float(uint8_t extern_id, uint8_t offset)> extern_float;  // offset in float units
    std::function<float4(uint8_t channel)> global_channel;
    std::function<float4(uint32_t hash)> object_channel;
    std::function<float4(uint8_t texture)> texture_tiling;  // push_tex_tiling (streaming state)
    std::function<float4(uint8_t texture)> texture_layer_count;  // push_tex_layer_count
    // Set: run as the sequencer VM (global channel expressions), whose 0x4A n pushes input n
    // (0 = time of day, 0..1) and 0x4C n pops to output n.
    std::function<float4(uint8_t input)> sequencer_input;
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
    std::vector<TfxExternTexture> extern_uavs;
    std::vector<TfxSampler> samplers;
    std::vector<std::string> unsupported;  // opcodes that were stubbed, for diagnostics
    size_t instructions = 0;
    bool live_inputs = false;  // extern state or editable per-object channels
};

// Inspect texture/sampler assignments without evaluating numeric expressions.
TfxResult inspect_tfx_bindings(std::span<const uint8_t> bytecode);

// Runs a TFX expression program. `constants` are the stage's bytecode constants; `out` is the
// cbuffer (pre-filled with the stage's initial cbuffer contents) the program writes into.
TfxResult run_tfx(std::span<const uint8_t> bytecode, std::span<const float4> constants, std::span<float4> out,
                  const TfxEnvironment& env = {}, std::string* trace = nullptr);

// Evaluate a material for a new frame from its original constants, never the previous frame's
// outputs. Untouched registers and opaque uint/float bit patterns retain their original values.
TfxResult evaluate_tfx(std::span<const uint8_t> bytecode, std::span<const float4> constants,
                       std::span<const float4> initial, std::vector<float4>& out, const TfxEnvironment& env = {});

// One instruction per line, for `nimcli technique`.
std::string disassemble_tfx(std::span<const uint8_t> bytecode);

} // namespace nim
