#pragma once
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace nim {

// Tiger "TFX" material / mesh structures, verified against Marathon data
// (layouts are close to Destiny 2's; see docs/ARCHITECTURE.md).
constexpr uint32_t kClassTechnique = 0x808031D8;
constexpr uint32_t kClassStaticMesh = 0x80808635;
constexpr uint32_t kClassStaticMeshData = 0x80808620;
constexpr uint32_t kClassRenderGlobals = 0x8080B61C;

enum class ShaderStage : uint8_t { Vertex, Hull, Domain, Geometry, Pixel, Compute, Count };

struct TextureBinding {
    uint32_t slot = 0;
    TagHash texture;  // resolved (hash32 or hash64 -> TagHash)
};

struct TechniqueStage {
    TagHash shader;                        // shader header (type 33)
    std::vector<TextureBinding> textures;
    std::vector<uint8_t> bytecode;         // TFX expression bytecode filling the cbuffer
    std::vector<std::array<float, 4>> constants;     // constants the bytecode reads (push_const etc.)
    std::vector<std::array<float, 4>> cbuffer_init;  // initial cbuffer contents the bytecode then patches
    std::vector<TagHash> samplers;
    int32_t cbuffer_slot = -1;
    TagHash cbuffer;
};

struct Technique {
    TagHash tag;
    uint32_t bind_mode = 0;  // 1 = VS+PS, 2 = VS only, 6 = CS ...
    std::array<TechniqueStage, size_t(ShaderStage::Count)> stages;
    const TechniqueStage& stage(ShaderStage s) const { return stages[size_t(s)]; }
};

std::optional<TagHash> resolve_wide_hash(PackageManager& pm, uint32_t hash32, uint64_t hash64);
Technique load_technique(PackageManager& pm, TagHash tag);

// DXBC container (DXIL, SM6) for a shader header tag (the type-41 entry whose reference is the header).
std::vector<uint8_t> load_shader_bytecode(PackageManager& pm, TagHash shader_header);

// ---- Static meshes ------------------------------------------------------------------------
struct StaticMeshPart {
    uint32_t index_start = 0, index_count = 0;
    uint8_t buffer_index = 0, lod_category = 0, primitive_type = 0;
};
struct StaticMeshGroup {
    uint16_t part_index = 0;
    uint8_t render_stage = 0;  // 0 = GenerateGbuffer, 4 = ShadowGenerate, 13 = DepthPrepass ...
    uint8_t input_layout = 0;
};
struct StaticMeshBuffers {
    TagHash index, vertex0, vertex1, color;
};
struct StaticMesh {
    TagHash tag, data_tag;
    std::vector<TagHash> techniques;  // one per group
    std::vector<StaticMeshGroup> groups;
    std::vector<StaticMeshPart> parts;
    std::vector<StaticMeshBuffers> buffers;
    std::array<float, 3> mesh_offset{};
    float mesh_scale = 1;
    float texcoord_scale = 1;
    std::array<float, 2> texcoord_offset{};
};
StaticMesh load_static_mesh(PackageManager& pm, TagHash tag);

// ---- Buffers / textures ---------------------------------------------------------------------
struct VertexBuffer { uint32_t stride = 0; uint16_t vtype = 0; std::vector<uint8_t> data; };
struct IndexBuffer { bool is_32bit = false; std::vector<uint8_t> data; };
VertexBuffer load_vertex_buffer(PackageManager& pm, TagHash header);
IndexBuffer load_index_buffer(PackageManager& pm, TagHash header);

struct Texture {
    uint32_t dxgi_format = 0;
    uint16_t width = 0, height = 0, depth = 1, array_size = 1;
    uint16_t mip_count = 1;
    std::vector<uint8_t> data;  // full mip chain, D3D subresource order
};
Texture load_texture(PackageManager& pm, TagHash header);

// ---- Vertex input layouts (from the game's render globals) ---------------------------------
struct InputElement {
    const char* semantic = "";
    uint32_t semantic_index = 0;
    uint32_t dxgi_format = 0;
    uint32_t size = 0;
    uint32_t buffer = 0;
    bool instanced = false;
};
// Index = the input_layout byte on mesh groups.
std::vector<std::vector<InputElement>> load_input_layouts(PackageManager& pm);

} // namespace nim
