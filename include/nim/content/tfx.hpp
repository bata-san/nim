#pragma once
#include "nim/core/package_manager.hpp"
#include "nim/core/tag_hash.hpp"
#include "nim/content/tfx_vm.hpp"

#include <array>
#include <map>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace nim {

// Tiger "TFX" material / mesh structures, verified against Marathon data
// (layouts are close to Destiny 2's; see docs/ARCHITECTURE.md).
constexpr uint32_t kClassTechnique = 0x808031D8;
constexpr uint32_t kClassStaticMesh = 0x80808635;
constexpr uint32_t kClassStaticMeshData = 0x80808620;
constexpr uint32_t kClassRenderGlobals = 0x8080B61C;
constexpr uint32_t kClassDynamicModel = 0x8080881C;

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
    uint64_t used_scopes = 0;
    // Fixed-function state selection (goriath): bytes blend / depth-stencil / rasterizer / depth
    // bias, each an index into the engine's state table with bit 7 = set.
    uint32_t states = 0;
    int blend_state() const { return (states & 0x80) ? int(states & 0x7F) : -1; }
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
// A static mesh part outside the main groups (SStaticMesh +0x20, Vec<8080861F>, 0x20 bytes):
// the transparent (8) and light shaft occlusion (10) parts, each with its own buffers.
struct StaticMeshSpecialPart {
    uint8_t render_stage = 0;  // +0x00
    uint8_t input_layout = 0;  // +0x01
    uint8_t primitive_type = 3;  // +0x02 (3 triangles, 5 strip)
    TagHash index, vertex0, vertex1, color;  // +0x04, +0x08, +0x0C, +0x10
    uint32_t index_start = 0, index_count = 0;  // +0x14, +0x18
    TagHash technique;  // +0x1C
};
struct StaticMesh {
    TagHash tag, data_tag;
    std::vector<StaticMeshSpecialPart> special_parts;
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

// ---- Dynamic models (entity meshes: rigid and skinned) -------------------------------------------
// Render stages index the per-mesh part ranges (0 = GenerateGbuffer, 8 = Transparents ...).
constexpr size_t kRenderStageCount = 25;
struct DynamicMeshPart {
    TagHash technique;
    uint16_t variant_shader_index = 0xFFFF;  // into the entity component's technique map
    uint8_t primitive_type = 3;              // 3 triangles, 5 strip
    uint32_t index_start = 0, index_count = 0;
    uint16_t external_identifier = 0;
    uint8_t lod_category = 0;                // bit 0 = highest detail
};
struct DynamicMesh {
    TagHash vertex0, vertex1, index, color, skinning;
    std::vector<DynamicMeshPart> parts;
    std::array<uint16_t, kRenderStageCount + 1> part_range{};  // stage s: [range[s], range[s+1])
    std::array<uint8_t, kRenderStageCount> input_layout{};
};
struct DynamicModel {
    TagHash tag;
    std::vector<DynamicMesh> meshes;
    uint32_t bone_count = 0;
    std::array<float, 4> model_scale{1, 1, 1, 0}, model_offset{};
    std::array<float, 2> texcoord_scale{1, 1}, texcoord_offset{};
};
DynamicModel load_dynamic_model(PackageManager& pm, TagHash tag);

// The entity component that places a dynamic model (SDynamicModelComponent, inside a component
// tag of class 8080BADB) carries the materials of parts whose own technique is none: part
// variant_shader_index -> technique_map[i] = {count, start} -> techniques[start + permutation % count].
constexpr uint32_t kClassComponent = 0x8080BADB;
struct DynamicModelMaterials {
    TagHash component;  // the component tag it came from
    struct Range { uint32_t count = 0, start = 0, unk8 = 0; };
    std::vector<Range> technique_map;
    std::vector<TagHash> techniques;
    uint32_t permutation_count = 1;
    uint32_t default_permutation = 0;  // goriath: the last permutation when there are more than two
    std::map<uint32_t, std::array<float, 4>> object_defaults;  // entity channel expressions' initial constants
    TagHash technique(uint16_t variant, uint32_t permutation) const;
};
// Model tag -> materials, from one pass over every component tag (a few seconds).
std::unordered_map<uint32_t, DynamicModelMaterials> index_dynamic_model_materials(PackageManager& pm);

// ---- Buffers / textures ---------------------------------------------------------------------
struct VertexBuffer { uint32_t stride = 0; uint16_t vtype = 0; std::vector<uint8_t> data; TagHash data_tag; };
struct IndexBuffer { bool is_32bit = false; std::vector<uint8_t> data; TagHash data_tag; };
VertexBuffer load_vertex_buffer(PackageManager& pm, TagHash header);
IndexBuffer load_index_buffer(PackageManager& pm, TagHash header);

struct Texture {
    uint32_t dxgi_format = 0;
    uint16_t width = 0, height = 0, depth = 1, array_size = 1;
    uint16_t mip_count = 1;
    uint16_t tile_count = 1;
    float4 tile_uv{};  // atlas local UV scale (xy) and per-frame UV step (zw)
    std::vector<uint8_t> data;  // full mip chain, D3D subresource order
};
Texture load_texture(PackageManager& pm, TagHash header);
// Same, without the mips larger than max_dim (2D textures; others load in full). Reads only the
// small data entry when its tail mips already cover the request.
Texture load_texture(PackageManager& pm, TagHash header, uint32_t max_dim);

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

// ---- Render globals (class 8080B61C -> data 80808070) ---------------------------------------
// The engine's own named pipelines (deferred_shading, global_lighting, final_combine, ...),
// the scopes that fill the shared cbuffers (view -> cb12, frame -> cb13, ...), the BRDF lookup
// textures, and the global channel defaults.
struct RenderGlobals {
    TagHash tag;
    std::vector<std::pair<std::string, TagHash>> pipelines;  // name -> technique
    std::vector<std::pair<std::string, TagHash>> scopes;     // name -> scope
    TagHash specular_tint_lookup, specular_lobe_lookup, specular_lobe_3d_lookup, iridescence_lookup;
    TagHash atmosphere_noise;  // render globals data +0x40, 3D scalar noise (inferred from original CS)
    std::vector<uint32_t> channel_ids;
    std::vector<std::array<float, 4>> channel_defaults;
    TagHash pipeline(std::string_view name) const;
    TagHash scope(std::string_view name) const;
};
RenderGlobals load_render_globals(PackageManager& pm);

// Scope (class 808031DC): a technique-like set of stages with no shaders, run each frame to
// build a shared cbuffer from extern values.
enum class ScopeStage : uint8_t { Pixel, Vertex, Geometry, Hull, Compute, Domain, Count };
struct Scope {
    TagHash tag;
    std::string name;
    std::array<TechniqueStage, size_t(ScopeStage::Count)> stages;
};
Scope load_scope(PackageManager& pm, TagHash tag);

// Resource inheritance follows the technique's used_scopes bits, then its own stage.
// The mapping records sources; it never manufactures a missing texture or sampler.
struct StageResourceSources {
    std::map<uint32_t, TagHash> textures, samplers;
    std::map<uint32_t, TfxExternTexture> extern_textures, extern_uavs;
    std::map<uint32_t, TagHash> cbuffer_scopes;
};
std::vector<Scope> load_technique_scopes(PackageManager& pm, const RenderGlobals& globals, const Technique& technique);
ScopeStage scope_stage(ShaderStage stage);
StageResourceSources resolve_stage_resources(const Technique& technique, ShaderStage stage,
                                             std::span<const Scope* const> scopes);

// ---- Shader bindings (DXBC PSV0 part) ---------------------------------------------------------
// What a compiled shader binds, read from the pipeline-state-validation part dxc embeds. Lets nim
// build a root signature for a game shader without reflection DLLs.
enum class BindingType : uint8_t { Invalid, Sampler, Cbv, Srv, Uav };
enum class BindingKind : uint8_t {  // DXIL resource kind
    Invalid, Texture1D, Texture2D, Texture2DMS, Texture3D, TextureCube, Texture1DArray, Texture2DArray,
    Texture2DMSArray, TextureCubeArray, TypedBuffer, RawBuffer, StructuredBuffer, CBuffer, Sampler
};
struct ShaderBinding {
    BindingType type = BindingType::Invalid;
    BindingKind kind = BindingKind::Invalid;
    uint32_t space = 0, lower = 0, upper = 0;
};
std::vector<ShaderBinding> shader_bindings(std::span<const uint8_t> dxbc);
// Vertex inputs a shader declares (DXBC ISG1 part), system values excluded.
struct ShaderInput {
    std::string semantic;
    uint32_t index = 0, reg = 0;
    bool never_written = false;  // outputs: declared, but no component is ever written
    bool never_read = false;     // inputs: declared, but no component is ever read
    uint8_t component_type = 0;  // DXBC register component type (uint32, sint32, float32)
    uint8_t component_mask = 0;
};
std::vector<ShaderInput> shader_inputs(std::span<const uint8_t> dxbc);
// Output attachments must match the original PS signature, including non-contiguous targets.
std::vector<ShaderInput> shader_outputs(std::span<const uint8_t> dxbc);

// Sampler tag -> the raw D3D12_SAMPLER_DESC it references (same layout).
struct SamplerDesc {
    uint32_t filter = 0, address_u = 1, address_v = 1, address_w = 1;
    float mip_lod_bias = 0;
    uint32_t max_anisotropy = 1, comparison_func = 1;
    float border_color[4]{};
    float min_lod = 0, max_lod = 3.402823466e+38f;
};
SamplerDesc load_sampler(PackageManager& pm, TagHash tag);

} // namespace nim
