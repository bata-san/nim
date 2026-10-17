#pragma once
#include "nim/content/animation.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/render/gpu.hpp"

#include <array>
#include <map>
#include <mutex>
#include <span>
#include <shared_mutex>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace nim {

class ExternStore;

// ---- small math (column vectors; M4 stores 4 columns) -----------------------------------------
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b);
V3 cross(V3 a, V3 b);
V3 normalize(V3 a);
struct M4 { float4 c[4]{}; };
M4 mul(const M4& a, const M4& b);

// Camera in Tiger world space (right-handed, Z up). Reversed-Z infinite projection.
struct View {
    V3 eye, target{0, 0, 0};
    float fov_y = 0.7f;  // radians
    float near_z = 0.05f;
    uint32_t width = 1, height = 1;
};
struct ViewMatrices {
    M4 vp_abs, vp_rel;  // absolute / camera-relative world -> clip
    M4 proj;            // camera space (x right, y up, z forward) -> clip
    V3 eye, fwd, right, up;
    float fx = 1, fy = 1;
    float ortho = 0;  // > 0: orthographic, half the view's extent (the sun's shadow view)
};
ViewMatrices make_view(const View& v);
// A view that frames a sphere from a pleasant three-quarter angle.
View frame_sphere(V3 center, float radius, uint32_t w, uint32_t h, float yaw = -0.93f, float pitch = 0.41f);

// ---- sun shadow map ------------------------------------------------------------------------------
// Not an engine pass (the game's shadow cascades are not decoded): nim draws the scene's own
// G-buffer pipelines from the sun into one orthographic depth map, which GameLighting turns into
// the shading pass's sun shadow mask. Reported as an estimate.
struct SunShadow {
    ComPtr<ID3D12Resource> depth;  // R32 typeless; DEPTH_WRITE outside GameLighting::render
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    uint32_t size = 0;
    ViewMatrices vm;
    float half = 1, range = 1;  // ortho half extent, depth range (m)
    bool valid = false;         // rendered this frame
    void resize(Gpu& gpu, uint32_t size);
    // Covers the sphere (center, radius) seen from the sun (sun_dir points toward the sun),
    // snapped to whole texels so the map does not shimmer as the camera moves.
    void fit(V3 sun_dir, V3 center, float radius);
    // A small scene whole; a large one (a map) around the camera's focus, sized by its distance.
    void fit_view(V3 sun_dir, V3 eye, V3 focus, V3 scene_center, float scene_radius);
};

// ---- rain occlusion ------------------------------------------------------------------------------
// The engine's overhead visibility map (material extern 80): a top-down depth of the map that
// rain-aware materials compare against (LESS_EQUAL, standard depth: the top surface is the
// smallest value) to keep covered surfaces dry. Not an engine pass here either: nim draws the
// scene's depth from straight above (SunShadow, reversed depth) and stores 1 - depth.
struct RainOcclusion {
    SunShadow top;                  // straight down, reversed depth
    ComPtr<ID3D12Resource> mask;    // R32_FLOAT standard depth: extern 80 +0x0
    float4 scale{}, offset{};       // extern 80 +0x10 / +0x20: world xyz -> (u, v, depth)
    size_t items = 0;               // scene item count it was drawn with (redraw when it changes)
    bool valid = false;
    void resize(Gpu& gpu, uint32_t size);
    void release(Gpu& gpu);
    // Converts `top` into `mask` (top in DEPTH_WRITE before and after).
    void resolve(Gpu& gpu, ID3D12GraphicsCommandList* cl);

private:
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pso_;
    uint32_t srv_ = UINT32_MAX;
};

// One render target of the engine's blend state `index` (goriath blend_states.bin, 90 entries).
D3D12_RENDER_TARGET_BLEND_DESC engine_blend_desc(int index, int rt = 0);

// ---- G-buffer ---------------------------------------------------------------------------------
// RT0 albedo, RT1 packed normal+smoothness, RT2 material params, RT3 motion, in the engine's
// formats: the material shaders rely on the UNORM targets clamping what they write (a remap
// like `x * -10 + 5.5` is a hard mask only after the clamp). `lit` is the lighting pass output
// (RGBA8, sRGB-encoded values).
class GBuffer {
public:
    void resize(Gpu& gpu, uint32_t w, uint32_t h);
    void release(Gpu& gpu);
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

    ComPtr<ID3D12Resource> rt[4], depth, lit;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{}, lit_rtv{}, dsv{}, dsv_ro{};  // dsv_ro: read-only depth (transparents)
    uint32_t srv_first = UINT32_MAX;  // 6 SRVs: rt0..3, depth, lit
    static constexpr DXGI_FORMAT kFormats[4] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};

private:
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
    uint32_t w_ = 0, h_ = 0;
};

// ---- Static mesh rendered with the game's own G-buffer shaders -------------------------------
struct SceneTextureInfo { uint32_t slot; TagHash tag; uint32_t format, width, height, mips; std::string error; };
struct SceneGroupInfo {
    uint16_t part = 0;
    uint32_t index_count = 0;
    TagHash technique, vs, ps;
    size_t cbuffer_regs = 0, vm_instructions = 0;
    std::vector<std::string> stubbed_ops;
    std::vector<TfxExternTexture> extern_textures;
    std::vector<SceneTextureInfo> textures;
};

// Everything a StaticMeshScene needs, read and evaluated on the CPU (safe on a worker thread with
// its own PackageManager); StaticMeshScene::upload() then only creates GPU objects.
// Object channel values to evaluate a mesh's materials with (hash -> value), and what the
// materials read: the value used and from which stage.
using ChannelValues = std::map<uint32_t, float4>;
struct ChannelUse {
    float4 value{};
    bool vs = false, ps = false;
};

struct PreparedMesh {
    struct Buffers {
        IndexBuffer ib;
        VertexBuffer vb0, vb1;
        // Skinned meshes: the packed streams the game's compute skinning writes for the vertex
        // shader (t2/t4 positions, 21/21/22-bit snorm pairs; t3 octahedral normal + tangent).
        std::vector<uint32_t> skin_pos, skin_normal;
        std::vector<uint8_t> color;  // dynamic models: per-vertex RGBA8 the VS reads as t0
        SkinWeights skin;            // skinned meshes: per-vertex bones and weights (CPU skinning)
    };
    struct Group {
        StaticMeshPart part;
        uint8_t input_layout = 0;
        uint32_t buffers = 0;  // index into PreparedMesh::buffers
        SceneGroupInfo info;
        std::vector<uint8_t> vs, ps;
        std::vector<float4> cb0;
        std::map<uint32_t, std::shared_ptr<const Texture>> textures;
        uint8_t stage = 0;  // render stage: 0 = G-buffer, 8 = transparents
        uint8_t lod_mask = 1;  // static mesh LOD levels this part belongs to (bit n = LOD n)
        int blend = -1;     // engine blend state index (transparents)
        std::vector<float4> vs_cb0;                // the VS stage's own cb0
        std::map<uint32_t, std::shared_ptr<const Texture>> vs_textures;
        TechniqueStage vs_material, ps_material;  // original TFX, retained for frame updates
        std::shared_ptr<const std::vector<Scope>> scopes;
        StageResourceSources vs_sources, ps_sources;
        bool vs_live = false, ps_live = false;
        std::map<uint32_t, SamplerDesc> vs_samplers, ps_samplers;
        // Constant buffers the object supplies itself (terrain: VS b11 = position offset and
        // texcoord transform), by (stage, register).
        std::map<std::pair<ShaderStage, uint32_t>, std::vector<float4>> own_cbuffers;
    };
    TagHash tag;
    bool dynamic = false;  // dynamic model (entity mesh) rather than a static mesh
    bool no_drawable = false;  // dynamic model has no GenerateGbuffer parts
    std::vector<Group> groups;
    std::vector<Buffers> buffers;
    std::vector<uint8_t> instance;  // static meshes: t2 instance data
    // Dynamic models: the rigid_model scope cbuffer (cb1). c0-c3 mesh-to-world, c4 position scale, c5 offset, c6 texcoord scale/offset.
    std::array<float4, 20> model_cb{};
    V3 center;
    float radius = 1;
    uint8_t lod = 0;
    uint8_t lod_levels = 1;  // static meshes prepared with all LODs: bit n = LOD n present
    std::vector<std::string> notes;  // parts that could not be drawn, and why
    std::map<uint32_t, ChannelUse> channels;  // object channels the materials read
    ChannelValues channel_overrides;
    ChannelValues channel_defaults;
    std::shared_ptr<const std::vector<float4>> global_channels;
    std::shared_ptr<const std::vector<Scope>> scopes;
    TagHash component;               // dynamic models: the component the materials came from
    uint32_t permutation = 0, permutation_count = 0;
};
// Shared across many prepare_static_mesh calls (maps): render globals / scopes read once,
// textures shared by tag and capped at max_texture_dim (0 = full size), G-buffer parts only.
struct PrepareCache {
    uint32_t max_texture_dim = 0;
    bool gbuffer_only = false;
    bool all_lods = false;  // every LOD level (maps pick one per instance by screen size)
    bool ready = false;
    std::shared_ptr<const std::vector<float4>> global_channels;
    std::shared_ptr<const std::vector<Scope>> scopes;
    std::unordered_map<uint32_t, std::shared_ptr<const Texture>> textures;
    // Shared by the threads preparing a map's meshes in parallel (each with its own PackageManager).
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::shared_mutex> texture_mutex = std::make_shared<std::shared_mutex>();
};
// Throws on failure.
PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash mesh, const ChannelValues* channels = nullptr,
                                 PrepareCache* cache = nullptr);
struct Terrain;
// A map's terrain patch (high detail parts) with its own techniques, dyemaps and VS constants;
// `layouts` picks the game input layout matching its vertex streams. Throws on failure.
PreparedMesh prepare_terrain(PackageManager& pm, const Terrain& terrain, const std::vector<std::vector<InputElement>>& layouts,
                             PrepareCache* cache = nullptr);
// Dynamic model (class 8080881C), GenerateGbuffer parts at the highest LOD, without the entity's
// material variants. Skinned meshes are drawn in their bind pose until StaticMeshScene::set_skin_pose.
// `materials` (from index_dynamic_model_materials) supplies the parts with no technique of their
// own; permutation < 0 = the component's default.
PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash model, const DynamicModelMaterials* materials = nullptr,
                                   int permutation = -1, const ChannelValues* channels = nullptr, PrepareCache* cache = nullptr);
// Either of the above, by the tag's class.
PreparedMesh prepare_mesh(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials = nullptr,
                          const ChannelValues* channels = nullptr);

class StaticMeshScene {
public:
    StaticMeshScene();
    ~StaticMeshScene();
    void load(PackageManager& pm, Gpu& gpu, TagHash mesh, const std::vector<std::vector<InputElement>>& layouts) {
        upload(gpu, prepare_mesh(pm, mesh), layouts);
    }
    void upload(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts);
    // Many static meshes in one scene (a map): begin, add_mesh per mesh with its world placements
    // (3x4 rows: rotation * scale | translation), then finish. Materials, scopes and textures are
    // shared across meshes; each mesh is drawn instanced.
    void begin(Gpu& gpu);
    void add_mesh(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts,
                  std::span<const std::array<float, 12>> placements);
    void finish(Gpu& gpu, TagHash tag, V3 center, float radius);
    size_t mesh_count() const { return sets_.size(); }
    size_t instance_count() const { return instance_total_; }
    void release(Gpu& gpu);  // caller must make sure the GPU is idle
    bool loaded() const { return !items_.empty(); }

    // Clears the G-buffer and draws every G-buffer group. Leaves targets in RENDER_TARGET.
    void render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);
    // Depth of the G-buffer parts from the sun (shadow.fit first); sets shadow.valid.
    void render_shadow(Gpu& gpu, ID3D12GraphicsCommandList* cl, SunShadow& shadow);
    // Draws the overhead visibility map over the scene (radius around `center`) and hands it to
    // the materials (extern 80) from then on.
    void render_rain_occlusion(Gpu& gpu, ID3D12GraphicsCommandList* cl, RainOcclusion& rain, V3 center, float radius);
    void set_rain_occlusion(const RainOcclusion* rain) { rain_ = rain; }
    // Global channel values set by the map's world channel controllers (weather), by index:
    // replace the render-globals defaults' x for the materials.
    void set_global_channels(std::map<uint32_t, float> values) { global_overrides_ = std::move(values); }
    // All global channels (GameLighting::update_global_channels: the map environment), replacing
    // the render-globals defaults; empty restores them.
    void set_global_channel_values(std::vector<float4> values) { global_values_ = std::move(values); }
    // Transparent parts (render stage 8), blended into the HDR target `rtv` (R16G16B16A16_FLOAT)
    // against the G-buffer depth read-only. The depth must be in DEPTH_READ.
    void render_transparent(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                            const ViewMatrices& vm);
    bool has_transparent() const { return transparent_count_ > 0; }
    // Skinned dynamic models: poses the meshes on the CPU. `bones` (skeleton order) take bone space
    // to object space, i.e. pose * inverse bind; empty = bind pose. Uploaded by the next render_gbuffer.
    bool skinned() const { return skinned_; }
    void set_skin_pose(std::vector<BoneTransform> bones);
    size_t transparent_count() const { return transparent_count_; }
    // One clock for both shader stages and all passes; channel edits update constants in place.
    void set_material_frame(float seconds, float delta_seconds = 0);
    // The engine exposure scale (extern 1 +0x1C) the emissive materials read: GameLighting::exposure().
    void set_exposure(float scale) { material_exposure_ = scale; }
    void set_render_inputs(const ExternStore& inputs);
    size_t item_count() const { return items_.size(); }
    void set_channels(const ChannelValues& values) { channel_overrides_ = values; }

    TagHash tag() const { return tag_; }
    V3 center() const { return center_; }
    float radius() const { return radius_; }
    uint8_t lod() const { return lod_; }
    bool dynamic() const { return dynamic_; }
    const std::vector<SceneGroupInfo>& groups() const { return info_; }
    const std::map<uint32_t, ChannelUse>& channels() const { return channels_; }
    const std::vector<std::string>& notes() const { return notes_; }
    // Engine inputs the materials read that were estimated (what -> why); see ExternStore.
    const std::map<std::string, std::string>& estimates() const;

private:
    struct Item {
        StaticMeshPart part;
        uint32_t buffers = 0;
        uint8_t stage = 0;
        uint8_t lod_mask = 1;
        ComPtr<ID3D12PipelineState> shadow_pso;  // same shaders, depth only, from the sun
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> root_sig;
        GpuBuffer cb0, vs_cb0;
        TechniqueStage vs_material, ps_material;
        std::shared_ptr<const std::vector<Scope>> scopes;
        StageResourceSources vs_sources, ps_sources;
        std::map<uint32_t, BindingKind> vs_kinds, ps_kinds;
        std::vector<uint32_t> vs_cbvs, ps_cbvs;
        std::vector<std::pair<uint32_t, GpuBuffer>> own_cbvs;  // (root parameter, constants)
        std::map<std::pair<ShaderStage, uint32_t>, std::vector<float4>> own_regs;
        std::vector<std::pair<uint32_t, uint32_t>> scope_slots;                   // (root parameter, scope_defs_ slot)
        std::vector<std::pair<uint32_t, D3D12_GPU_VIRTUAL_ADDRESS>> scope_addresses;  // this frame's
        std::vector<float4> vs_constants, ps_constants;
        D3D12_GPU_VIRTUAL_ADDRESS vs_address = 0, ps_address = 0;
        TagHash technique;
        bool vs_live = false, ps_live = false;
        bool blended = false;  // G-buffer decal: blends into some targets, drawn after the opaque items
        bool failed = false;  // its inputs could not be evaluated: not drawn (reported in notes)
        bool dynamic = false;  // a dynamic model part: drawn once per placement with its object constants
        std::vector<uint32_t> object_params;  // root parameters of its rigid_model / skinning scopes
        uint32_t srv_first = UINT32_MAX;  // kItemSrvs descriptors: PS t0-t31, then VS t0-t31
        uint32_t srv_copies = 1;          // kSrvCopies when engine textures are rebound per frame
        bool owns_srv = true;             // false: a table shared with an item with the same views
        // Single-copy tables: the engine views last written (resource, format) per table slot, so
        // a slot is only rewritten when the view changes.
        std::map<uint32_t, std::pair<const void*, uint32_t>> written;
        std::map<uint32_t, ComPtr<ID3D12Resource>> textures, vs_textures;
    };
    static constexpr uint32_t kPsSrvs = 32, kVsSrvs = 32, kItemSrvs = kPsSrvs + kVsSrvs;
    static constexpr uint32_t kSrvCopies = Gpu::kFramesInFlight + 1;
    uint32_t srv_frame_ = 0;
    void bind_common(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h, const ViewMatrices& vm,
                     ID3D12RootSignature* root = nullptr);
    void draw_items(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint8_t stage, uint32_t w, uint32_t h, const ViewMatrices& vm,
                    bool shadow = false);
    void update_materials(Gpu& gpu, uint32_t w, uint32_t h, const ViewMatrices& vm);
    std::unique_ptr<ExternStore> material_externs_;
    std::vector<float4> global_base_;               // the materials' global channels before overrides
    std::map<uint32_t, float> global_overrides_;
    std::vector<float4> global_values_;
    const RainOcclusion* rain_ = nullptr;
    ChannelValues channel_overrides_;
    ChannelValues channel_defaults_;
    float material_time_ = 0, material_delta_ = 0, material_exposure_ = 1;
    std::unordered_map<uint64_t, D3D12_GPU_VIRTUAL_ADDRESS> material_addresses_;
    TagHash tag_;
    size_t transparent_count_ = 0;
    std::map<uint32_t, ChannelUse> channels_;
    std::vector<Item> items_;
    std::vector<SceneGroupInfo> info_;
    ComPtr<ID3D12RootSignature> root_sig_, root_sig_dynamic_;
    struct BufferSet {
        GpuBuffer ib, vb0, vb1, color;
        ComPtr<ID3D12Resource> skin_pos, skin_normal;
        GpuBuffer t2, inst;  // static meshes: instance records + mesh block, instance index stream
        uint32_t instances = 1;
        uint32_t cull = UINT32_MAX;  // cull_groups_ index: this frame's visible instances
        D3D12_INDEX_BUFFER_VIEW ibv{};
        D3D12_VERTEX_BUFFER_VIEW vbv[4]{};
        // CPU skinning: bind-pose vertices and weights; each frame's streams go through its own
        // upload buffer (positions, then normals) into the default-heap skin_pos / skin_normal.
        std::vector<uint8_t> vb0_cpu;
        SkinWeights skin;
        std::array<ComPtr<ID3D12Resource>, Gpu::kFramesInFlight> skin_upload;
        bool skin_readable = false;  // skin_pos / skin_normal in NON_PIXEL_SHADER_RESOURCE
        // Dynamic models: the object constants (rigid_model / skinning scope, local_to_world in
        // rows 0-3) and the placements; this frame's constants per placement.
        bool dynamic = false;
        std::array<float4, 20> model_cb{};
        std::vector<std::array<float, 12>> placements;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> object_cb;
    };
    void update_skin(ID3D12GraphicsCommandList* cl);
    void add_mesh_parts(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts,
                        std::span<const std::array<float, 12>> placements);
    std::vector<BufferSet> sets_;
    std::vector<std::string> notes_;
    size_t instance_total_ = 0;
    // View culling of static instances (a viewer choice, not the game's visibility system):
    // world bounding spheres, and per frame the indices that survive into the instance stream.
    struct CullGroup {
        std::vector<std::array<float, 4>> spheres;
        uint8_t levels = 1;  // LOD levels the mesh has (bit n = LOD n)
        D3D12_GPU_VIRTUAL_ADDRESS visible_address = 0;
        uint32_t visible = 0;
        std::array<uint32_t, 4> first{}, count{};  // visible instances per LOD level, in that order
    };
    std::array<std::vector<uint32_t>, 4> cull_scratch_;
    std::vector<CullGroup> cull_groups_;
    void cull(Gpu& gpu, const ViewMatrices& vm, uint32_t height);
    uint32_t item_instances(const Item& it) const;  // this frame's, after cull()
    struct ScopeDef { const Scope* scope; ShaderStage stage; uint32_t reg; };
    std::vector<ScopeDef> scope_defs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> scope_frame_;
    bool scopes_resolved_ = false;
    GpuBuffer white_color_;
    BufferArena arena_;  // geometry and fixed constants of this scene  // 1 x RGBA8 white: VS colour stream of meshes without one
    std::map<uint32_t, ComPtr<ID3D12Resource>> texture_cache_;
    std::map<uint32_t, ComPtr<ID3D12RootSignature>> root_cache_;
    std::map<std::vector<uint64_t>, uint32_t> table_cache_;  // identical static descriptor tables
    std::map<std::tuple<uint32_t, uint8_t, uint8_t, uint8_t, bool>, ComPtr<ID3D12PipelineState>> pipeline_cache_;
    std::array<float4, 20> model_cb_{};
    bool dynamic_ = false;          // the scene is one dynamic model (entity view)
    bool dynamic_parts_ = false;    // dynamic models placed among static meshes (map entities)
    bool skinned_ = false, skin_dirty_ = false;
    std::vector<BoneTransform> skin_pose_;
    uint32_t skin_frame_ = 0;
    V3 skin_scale_, skin_offset_;
    float skin_range_ = 1;  // packed positions are (p - offset) / range
    uint32_t srv_count_ = 0;
    V3 center_;
    float radius_ = 1;
    uint8_t lod_ = 0;
};

// ---- Deferred lighting over the game's G-buffer ------------------------------------------------
enum class ViewMode : uint32_t { Lit, Albedo, Normal, Smoothness, Metalness, Material, Motion, Count };
const char* view_mode_name(ViewMode m);

struct LightingParams {
    ViewMode mode = ViewMode::Lit;
    float time = 0, delta_time = 0;  // seconds, shared with the model's material clock
    bool optimize = true;  // reference path for pixel comparisons / timestamp benchmarks
    float exposure = 1.0f;
    float sun_azimuth = 0.9f, sun_elevation = 0.75f;  // radians
    float sun_intensity = 3.2f;
    V3 sun_color{1.0f, 0.95f, 0.88f};
    float sky_intensity = 1.0f;
    V3 sky_top{0.42f, 0.55f, 0.75f}, sky_horizon{0.80f, 0.82f, 0.84f}, ground{0.28f, 0.26f, 0.24f};
    V3 background_top{0.20f, 0.21f, 0.23f}, background_bottom{0.11f, 0.11f, 0.12f};
    // A map's atmosphere (game lighting only, GameLighting::set_atmosphere): sky lookups, height
    // fog and the sky background with the engine's pipelines. time_of_day in days (0..1), < 0 =
    // the map's own; the sun then follows the map's sun path instead of sun_azimuth/elevation.
    bool atmosphere = true;
    float time_of_day = -1.0f;
    // The map environment (its global channel expressions: sun, ambient, fog over the day) with the atmosphere.
    bool environment = true;
    // Screen-space ambient occlusion with the game's hdao pass (game lighting only).
    bool ssao = true;
    // Light shafts: the local lights' volumetrics techniques and the engine's blur / upres passes.
    bool volumetrics = true;
    // Post-processing (game lighting only): bloom, auto exposure and the engine's tonemap /
    // grading pass (screen_area_global_lut3d) instead of final_combine. `exposure` then scales the
    // auto exposure (as a compensation); without auto exposure it is the exposure itself.
    bool post = true;
    bool auto_exposure = true;
    bool bloom = true;
    // Anti-aliasing with the engine's fxaa pass over the tonemapped image (with post only).
    bool antialias = true;
};

class LightingPass {
public:
    void init(Gpu& gpu);
    // Reads the G-buffer, writes gb.lit. Leaves lit in PIXEL_SHADER_RESOURCE and the G-buffer
    // targets back in RENDER_TARGET / DEPTH_WRITE.
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p);

private:
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
};

} // namespace nim
