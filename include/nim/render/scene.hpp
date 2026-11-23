#pragma once
#include "nim/content/animation.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/render/dlss.hpp"
#include "nim/render/gpu.hpp"

#include <array>
#include <future>
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
class GameLighting;

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
    float jitter_x = 0, jitter_y = 0;  // sub-pixel offset of the projection, in pixels (x right, y down)
};
struct ViewMatrices {
    M4 vp_abs, vp_rel;  // absolute / camera-relative world -> clip
    // Absolute world -> clip of the previous frame, with this frame's jitter: the G-buffer's motion
    // vectors (View +0x3E0). make_view sets it to vp_abs (no motion).
    M4 vp_prev;
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
    M4 cached_sun_view;
    uint64_t cached_sun_generation = 0;
    float cached_sun_time = 0;
    void resize(Gpu& gpu, uint32_t size);
    // Covers the sphere (center, radius) seen from the sun (sun_dir points toward the sun),
    // snapped to whole texels so the map does not shimmer as the camera moves.
    void fit(V3 sun_dir, V3 center, float radius);
    // A small scene whole; a large one (a map) around the camera's focus, sized by its distance.
    void fit_view(V3 sun_dir, V3 eye, V3 focus, V3 scene_center, float scene_radius, bool map = false);
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
    static constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    static constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    void resize(Gpu& gpu, uint32_t w, uint32_t h, uint32_t out_w = 0, uint32_t out_h = 0);
    void release(Gpu& gpu);
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    uint32_t output_width() const { return out_w_; }
    uint32_t output_height() const { return out_h_; }

    ComPtr<ID3D12Resource> rt[4], depth, lit, decal_normals;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{}, lit_rtv{}, null_rtv{}, dsv{}, dsv_ro{};  // dsv_ro: read-only depth (transparents)
    uint32_t srv_first = UINT32_MAX;  // 6 SRVs: rt0..3, depth, lit
    static constexpr DXGI_FORMAT kFormats[4] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT};

private:
    ComPtr<ID3D12DescriptorHeap> rtv_heap_, dsv_heap_;
    uint32_t w_ = 0, h_ = 0, out_w_ = 0, out_h_ = 0;
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
        // vb0 holds 48-byte float vertices (position, normal, tangent; input layout 13), already
        // unpacked to model space. Their vertex shader reads vb0 directly, so posing rewrites vb0.
        bool float_positions = false;
    };
    struct Group {
        StaticMeshPart part;
        uint8_t input_layout = 0;
        uint32_t buffers = 0;  // index into PreparedMesh::buffers
        SceneGroupInfo info;
        std::vector<uint8_t> vs, ps;
        std::vector<float4> cb0;
        std::map<uint32_t, std::shared_ptr<const Texture>> textures;
        uint8_t stage = 0;  // render stage: 0 = G-buffer, 4 = shadow, 8 = transparents
        uint8_t lod_mask = 1;  // static mesh LOD levels this part belongs to (bit n = LOD n)
        int blend = -1;     // engine blend state index (transparents)
        std::vector<float4> vs_cb0;                // the VS stage's own cb0
        std::map<uint32_t, std::shared_ptr<const Texture>> vs_textures;
        TechniqueStage vs_material, ps_material;  // original TFX, retained for frame updates
        std::shared_ptr<const std::vector<Scope>> scopes;
        StageResourceSources vs_sources, ps_sources;
        bool vs_live = false, ps_live = false;
        std::array<uint8_t, 2> preview_sky_weights{};
        std::map<uint32_t, SamplerDesc> vs_samplers, ps_samplers;
        // Constant buffers the object supplies itself (terrain: VS b11 = position offset and
        // texcoord transform), by (stage, register).
        std::map<std::pair<ShaderStage, uint32_t>, std::vector<float4>> own_cbuffers;
        // Texture-space bakes (export tools): a DXIL geometry shader placed between the original VS and
        // PS that moves each triangle into an atlas (its own PSO, never shared).
        std::vector<uint8_t> bake_gs;
    };
    TagHash tag;
    TagHash assembly_root;
    bool dynamic = false;  // dynamic model (entity mesh) rather than a static mesh
    bool no_drawable = false;  // dynamic model has no parts supported by the connected passes
    bool inferred_dynamic_lod = false;  // alternates of one piece: only the lowest external identifier drawn
    size_t region_hidden_parts = 0;     // parts of other region permutations left out (3.96)
    bool distance_field_preview = false; // preview-only material state and camera framing
    bool inferred_transparent_stage = false;
    bool animation_equipment = false;  // visible when the active clip supplies a placement
    std::vector<Group> groups;
    std::vector<Buffers> buffers;
    std::vector<PreparedMesh> attachments;  // authored equipment parts, in the same preview space
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
    bool exfil_device = false;  // placed by an exfil device: unset object state is its inactive state (0)
    std::shared_ptr<const std::vector<float4>> global_channels;
    std::shared_ptr<const std::vector<Scope>> scopes;
    TagHash component;               // dynamic models: the component the materials came from
    uint32_t permutation = 0, permutation_count = 0;
    uint32_t bone_count = 0;  // declared by the dynamic model header
};
struct ModelSkinPose {
    TagHash model;
    std::vector<BoneTransform> bones;
    std::optional<BoneTransform> placement;
};
struct PreviewAnimation {
    struct Follower {
        TagHash model;
        std::vector<int32_t> bone_map;
    };
    struct AnimatedPart {
        TagHash model;
        Skeleton skeleton;
        AnimationRig rig;
        std::map<TagHash, AnimationClip> clips;
    };
    struct Equipment {
        TagHash source_clip, model;
        Skeleton skeleton;
        AnimationRig rig;
        AnimationClip clip;
    };
    TagHash model;
    Skeleton skeleton;
    AnimationRig rig;
    std::vector<ClipInfo> clips;
    std::vector<Follower> followers;
    bool inferred_binding = false;
    bool authored_clips = false;
    std::vector<AnimatedPart> animated_parts;
    std::vector<Equipment> equipment;
    std::optional<uint16_t> equipment_socket;
    std::optional<uint16_t> equipment_wrist;
    std::vector<ModelSkinPose> poses(const AnimationClip& clip, float frame, TagHash equipment_model = {}) const;
};
// Shared across many prepare_static_mesh calls (maps): render globals / scopes read once,
// textures shared by tag and capped at max_texture_dim (0 = full size), G-buffer parts only.
struct PrepareCache {
    uint32_t max_texture_dim = 0;
    bool keep_static_lods_for_map = false;  // the model view keeps only the highest authored LOD
    bool ready = false;
    std::shared_ptr<const std::vector<float4>> global_channels;
    std::shared_ptr<const std::vector<Scope>> scopes;
    std::unordered_map<uint32_t, std::shared_future<std::shared_ptr<const Texture>>> textures;
    // Shared by the threads preparing a map's meshes in parallel (each with its own PackageManager).
    std::shared_ptr<std::mutex> mutex = std::make_shared<std::mutex>();
    std::shared_ptr<std::shared_mutex> texture_mutex = std::make_shared<std::shared_mutex>();
};
// Throws on failure.
PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash mesh, const ChannelValues* channels = nullptr,
                                 PrepareCache* cache = nullptr);
struct Terrain;
struct MapDecal;
// A map's terrain patch (high detail parts) with its own techniques, dyemaps and VS constants;
// `layouts` picks the game input layout matching its vertex streams. Throws on failure.
PreparedMesh prepare_terrain(PackageManager& pm, const Terrain& terrain, const std::vector<std::vector<InputElement>>& layouts,
                             PrepareCache* cache = nullptr);
PreparedMesh prepare_map_decal(PackageManager& pm, const MapDecal& decal,
                               const std::vector<std::vector<InputElement>>& layouts, PrepareCache* cache = nullptr);
// Dynamic model (class 8080881C), GenerateGbuffer parts at the highest LOD, without the entity's
// material variants. Skinned meshes are drawn in their bind pose until StaticMeshScene::set_skin_poses.
// `materials` (from index_dynamic_model_materials) supplies the parts with no technique of their
// own; permutation < 0 = the component's default.
// `regions` (an entity's spawned permutations, 3.96) leaves out the part groups of other permutations.
PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash model, const DynamicModelMaterials* materials = nullptr,
                                   int permutation = -1, const ChannelValues* channels = nullptr, PrepareCache* cache = nullptr,
                                   const RegionPermutations* regions = nullptr,
                                   std::span<const std::pair<uint32_t, uint32_t>> spawn_tags = {});
// Either of the above, by the tag's class.
PreparedMesh prepare_mesh(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials = nullptr,
                          const ChannelValues* channels = nullptr);
PreparedMesh prepare_entity_preview(PackageManager& pm, TagHash model,
                                    const std::unordered_map<uint32_t, DynamicModelMaterials>& materials,
                                    const ModelAssemblyIndex& assemblies, int permutation = -1,
                                    const ChannelValues* channels = nullptr, PrepareCache* cache = nullptr,
                                    const RegionPermutations* regions = nullptr,
                                    std::span<const std::pair<uint32_t, uint32_t>> spawn_tags = {});
std::optional<PreviewAnimation> prepare_preview_animation(PackageManager& pm, const PreparedMesh& mesh,
                                                          const ModelRigIndex& rigs, std::span<const ClipInfo> clips,
                                                          TagHash preferred = {}, std::span<const ClipInfo> related = {},
                                                          const ModelMotionIndex* motions = nullptr);
void restrict_preview_clip(AnimationClip& clip, const ClipInfo& info);
void prepare_preview_equipment(PackageManager& pm, PreparedMesh& mesh, PreviewAnimation& animation,
                               const ModelRigIndex& rigs, std::span<const ClipInfo> clips,
                               const std::unordered_map<uint32_t, DynamicModelMaterials>& materials);

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
                  std::span<const std::array<float, 12>> placements, std::span<const float> sky_order = {});
    void finish(Gpu& gpu, TagHash tag, V3 center, float radius);
    size_t mesh_count() const { return sets_.size(); }
    size_t instance_count() const { return instance_total_; }
    void release(Gpu& gpu);  // caller must make sure the GPU is idle
    bool loaded() const { return !items_.empty(); }

    // Clears the G-buffer and draws every G-buffer group. Leaves targets in RENDER_TARGET.
    void render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);
    // Depth of the G-buffer parts from the sun (shadow.fit first); sets shadow.valid.
    // Draw bundles for large opaque lists (frame command lists only: off for one-off views
    // recorded on the immediate list, such as the AI floor probes).
    void allow_bundles(bool allow) { bundles_allowed_ = allow; }
    void render_shadow(Gpu& gpu, ID3D12GraphicsCommandList* cl, SunShadow& shadow);
    void render_sun_shadow(Gpu& gpu, ID3D12GraphicsCommandList* cl, SunShadow& shadow, bool map);
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
    // Call from GameLighting's callback: inputs belong to this frame, after its atmosphere passes.
    void render_transparent(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                            const ViewMatrices& vm, const GameLighting& lighting);
    bool has_visible_distortion() const;
    uint32_t distortion_draws() const { return distortion_draws_; }
    void render_distortion(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h,
                           const ViewMatrices& vm, const ExternStore& inputs);
    bool has_transparent() const { return !transparent_items_.empty(); }
    bool has_visible_light_grid() const;
    // CPU skinning, per model. Each pose holds pose * inverse bind in skeleton order.
    // Models without a supplied pose use the bind pose.
    bool skinned() const { return skinned_; }
    void set_skin_poses(std::vector<ModelSkinPose> poses);
    void load_animation_equipment(Gpu& gpu, const std::vector<std::vector<InputElement>>& layouts, std::span<const ModelSkinPose> poses);
    // Actors (docs/PROGRESS.md 3.70): skinned entity previews placed in a map scene and moved and
    // posed every frame (AI characters). add_actor adds one hidden copy of `mesh` and its assembled
    // parts; set_actor shows it at `rows` (3x4 object to world) with `poses` (as set_skin_poses),
    // or hides it when rows is empty.
    uint32_t add_actor(Gpu& gpu, const PreparedMesh& mesh, const std::vector<std::vector<InputElement>>& layouts);
    void set_actor(uint32_t actor, const std::optional<std::array<float, 12>>& rows, std::vector<ModelSkinPose> poses = {});
    // Run-time object state of one actor (e.g. the face indicator's channels): over its models'
    // authored channel defaults, under the scene's overrides. Its materials then evaluate apart
    // from the other actors of the same model.
    void set_actor_channels(uint32_t actor, const ChannelValues& values);
    size_t actor_count() const { return actors_.size(); }
    // The lowest z of the actor's skinned vertices (every 4th) in object space under `poses`.
    std::optional<float> actor_lowest(uint32_t actor, const std::vector<ModelSkinPose>& poses) const;
    // Every `stride`-th skinned vertex of the actor in object space under `poses`.
    std::vector<V3> actor_points(uint32_t actor, const std::vector<ModelSkinPose>& poses, size_t stride = 4) const;
    size_t transparent_count() const { return transparent_items_.size(); }
    size_t world_transparent_draws() const { return world_transparent_draws_; }
    size_t sky_transparent_draws() const { return sky_transparent_draws_; }
    size_t transparent_local_reflections() const { return transparent_local_reflections_; }
    size_t transparent_sky_reflections() const { return transparent_sky_reflections_; }
    size_t transparent_local_probes() const { return transparent_local_probes_; }
    std::array<size_t, 3> visibility_counts() const;  // visible statics, LOD0 statics, visible placed entities
    // One clock for both shader stages and all passes; channel edits update constants in place.
    void set_material_frame(float seconds, float delta_seconds = 0);
    void set_material_time_of_day(float day_fraction) { material_time_of_day_ = day_fraction; }
    // The engine exposure scale (extern 1 +0x1C) the emissive materials read: GameLighting::exposure().
    void set_exposure(float scale) { material_exposure_ = scale; }
    void set_render_inputs(const ExternStore& inputs);
    size_t item_count() const { return items_.size(); }
    void set_channels(const ChannelValues& values) { channel_overrides_ = values; ++shadow_generation_; }

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
        // Moved, not copied, when items_ grows (std::map's move is not declared noexcept).
        Item() = default;
        Item(Item&&) noexcept = default;
        Item& operator=(Item&&) noexcept = default;
        Item(const Item&) = default;
        Item& operator=(const Item&) = default;
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
        bool object_reflection = false;
        std::vector<std::pair<uint32_t, D3D12_GPU_VIRTUAL_ADDRESS>> scope_addresses;  // this frame's
        std::vector<float4> vs_constants, ps_constants;
        struct AnimatedAtlas { AtlasOperands operands; float4 uv; uint16_t frames; };
        std::vector<AnimatedAtlas> ps_atlases;
        D3D12_GPU_VIRTUAL_ADDRESS vs_address = 0, ps_address = 0;
        // Stages that read the object's own position (object channel interpolated_world_position):
        // their constants differ per placement. -1 = not scanned yet.
        int8_t vs_reads_position = -1, ps_reads_position = -1;
        std::vector<D3D12_GPU_VIRTUAL_ADDRESS> vs_placement_addresses, ps_placement_addresses;  // this frame's, per placement
        TagHash technique;
        bool vs_live = false, ps_live = false;
        std::array<uint8_t, 2> preview_sky_weights{};
        bool blended = false;  // G-buffer decal: blends into some targets, drawn after the opaque items
        bool failed = false;  // its inputs could not be evaluated: not drawn (reported in notes)
        bool light_grid = false;
        bool light_grid_ready = false;
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
    void draw_items(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint8_t stage, uint32_t w, uint32_t h, const ViewMatrices& vm,
                    bool shadow = false, const GameLighting* lighting = nullptr);
    void update_materials(Gpu& gpu, uint32_t w, uint32_t h, const ViewMatrices& vm, uint8_t render_stage = 0,
                          const ExternStore* frame_inputs = nullptr, bool shadow = false);
    std::unique_ptr<ExternStore> material_externs_;
    std::vector<float4> global_base_;               // the materials' global channels before overrides
    std::map<uint32_t, float> global_overrides_;
    std::vector<float4> global_values_;
    const RainOcclusion* rain_ = nullptr;
    ChannelValues channel_overrides_;
    ChannelValues channel_defaults_;
    float material_time_ = 0, material_delta_ = 0, material_exposure_ = 1;
    float material_time_of_day_ = 0.5f;
    std::unordered_map<uint64_t, D3D12_GPU_VIRTUAL_ADDRESS> material_addresses_;
    // update_materials' per-call working sets, kept between frames so they are not reallocated.
    struct MaterialScratch;
    static void retire_prepared(PreparedMesh&& mesh);
    std::unique_ptr<MaterialScratch> material_scratch_;
    TagHash tag_;
    size_t world_transparent_draws_ = 0;
    size_t sky_transparent_draws_ = 0;
    size_t transparent_local_reflections_ = 0, transparent_sky_reflections_ = 0, transparent_local_probes_ = 0;
    std::unordered_map<uint64_t, size_t> transparent_probe_cache_;
    uint64_t transparent_probe_gen_ = UINT64_MAX;
    uint64_t shadow_generation_ = 0;
    struct VisibilityKey {
        std::array<float, 16> view;
        uint64_t generation;
        uint32_t height;
        bool shadow;
        bool operator==(const VisibilityKey&) const = default;
    };
    // The main view and the shadow view each keep their culling result, so alternating between
    // them does not rebuild either one while the camera holds still. The active one lives in the
    // fields the draws read; the other is parked in the *_other members and swapped in when needed.
    std::optional<VisibilityKey> visibility_key_, visibility_key_other_;
    bool visibility_shadow_ = false;
    std::map<uint32_t, ChannelUse> channels_;
    std::vector<Item> items_;
    std::vector<size_t> transparent_items_, distortion_items_;
    uint32_t distortion_draws_ = 0;
    std::vector<size_t> decal_items_;
    bool standalone_decal_ = false;
    std::vector<SceneGroupInfo> info_;
    struct PlacementConstants { std::array<float4, 32> values; };
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
        TagHash model;
        V3 skin_scale, skin_offset, skin_pack_offset;
        float skin_range = 1;
        std::array<float, 12> actor_previous_rows{};
        std::array<ComPtr<ID3D12Resource>, Gpu::kFramesInFlight> skin_upload;
        bool skin_readable = false;  // skin_pos / skin_normal in NON_PIXEL_SHADER_RESOURCE
        bool skin_posed = false;
        bool float_vertices = false;  // PreparedMesh::Buffers::float_positions: posed into vb0 itself
        // Dynamic models: the object constants (rigid_model / skinning scope, local_to_world in
        // rows 0-3) and the placements; this frame's constants per placement.
        bool dynamic = false;
        V3 model_center{};
        std::array<float4, 7> model_parameters{};
        std::shared_ptr<const ChannelValues> model_channels;
        std::shared_ptr<const ChannelValues> actor_channels;  // set_actor_channels: model defaults + the actor's state
        bool exfil_device = false;
        std::vector<std::array<float, 12>> placements;
        std::vector<float> sky_order;
        GpuBuffer object_cb;
        struct EquipmentPlacement {
            std::array<float4, 20> constants;
            std::optional<BoneTransform> current, previous;
            D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        };
        std::unique_ptr<EquipmentPlacement> equipment;
        int32_t actor = -1;                  // actors_ index: moved and posed per frame
        PlacementConstants actor_constants{};  // the model's own constants (rows 0-3 replaced per frame)
    };
    struct Actor {
        std::vector<uint32_t> sets;
        std::optional<std::array<float, 12>> rows;
        std::vector<ModelSkinPose> poses;
    };
    std::vector<Actor> actors_;
    int32_t adding_actor_ = -1;
    void bind_actor_constants(Gpu& gpu);
    void update_skin(ID3D12GraphicsCommandList* cl);
    void rebuild_visibility(const ViewMatrices& vm, uint32_t height, bool shadow);
    void add_mesh_parts(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts,
                        std::span<const std::array<float, 12>> placements, std::span<const float> sky_order);
    std::vector<BufferSet> sets_;
    std::vector<std::string> notes_;
    size_t instance_total_ = 0;
    // View culling of static instances (a viewer choice, not the game's visibility system):
    // world bounding spheres, and per frame the indices that survive into the instance stream.
    struct CullGroup {
        std::vector<std::array<float, 4>> spheres;
        float4 bounds{};
        std::vector<uint32_t> visible_placements;
        size_t first_item = 0, end_item = 0;
        uint32_t first_set = 0, set_count = 0;
        uint32_t stream_offset = 0;
        bool dynamic = false;
        bool authored_shadow = false;
        bool has_transparency = false;
        std::vector<float> visible_depths;  // follows the compacted instance stream, including LOD order
        uint8_t levels = 1;  // LOD levels the mesh has (bit n = LOD n)
        uint32_t visible = 0;
        std::array<uint32_t, 8> first{}, count{};  // visible instances per LOD level, in that order
        std::vector<uint32_t> visible_indices;  // this view's compacted instance stream (static groups)
        uint8_t visible_lods = 0;
        struct Parked {
            std::vector<uint32_t> visible_placements, visible_indices;
            std::vector<float> visible_depths;
            std::array<uint32_t, 8> first{}, count{};
            uint32_t visible = 0, stream_offset = 0;
            uint8_t visible_lods = 0;
        } other;
        void swap_view() {
            std::swap(visible_placements, other.visible_placements);
            std::swap(visible_indices, other.visible_indices);
            std::swap(visible_depths, other.visible_depths);
            std::swap(first, other.first);
            std::swap(count, other.count);
            std::swap(visible, other.visible);
            std::swap(stream_offset, other.stream_offset);
            std::swap(visible_lods, other.visible_lods);
        }
    };
    std::array<std::vector<uint32_t>, 8> cull_scratch_;
    std::vector<uint32_t> visible_static_indices_, visible_static_groups_;
    std::vector<CullGroup> cull_groups_;
    std::vector<size_t> uncullable_opaque_, visible_opaque_, visible_blended_;
    std::vector<uint32_t> visible_static_indices_other_, visible_static_groups_other_;
    std::vector<size_t> visible_opaque_other_, visible_blended_other_;
    struct DrawBundle {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t fence = 0;
    };
    // Opaque draws are recorded into kDrawBundles bundles in parallel (a few draw calls per frame).
    static constexpr size_t kDrawBundles = 8;
    std::array<DrawBundle, kDrawBundles * 4 * Gpu::kFramesInFlight> draw_bundles_;
    bool bundles_allowed_ = true;
    void cull(Gpu& gpu, const ViewMatrices& vm, uint32_t height, bool shadow = false);
    uint32_t item_instances(const Item& it) const;  // this frame's, after cull()
    struct ScopeDef { const Scope* scope; ShaderStage stage; uint32_t reg; };
    std::vector<ScopeDef> scope_defs_;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> scope_frame_;
    bool scopes_resolved_ = false;
    GpuBuffer white_color_;
    BufferArena arena_;  // geometry and fixed constants of this scene  // 1 x RGBA8 white: VS colour stream of meshes without one
    std::map<uint32_t, ComPtr<ID3D12Resource>> texture_cache_;
    std::unordered_map<uint32_t, GpuBuffer> geometry_cache_;
    std::map<uint64_t, ComPtr<ID3D12RootSignature>> root_cache_;
    std::map<std::vector<uint64_t>, uint32_t> table_cache_;  // identical static descriptor tables
    std::map<std::tuple<uint64_t, uint8_t, uint8_t, uint8_t, bool>, ComPtr<ID3D12PipelineState>> pipeline_cache_;
    std::array<float4, 20> model_cb_{};
    bool dynamic_ = false;          // models in a single entity preview
    bool assembled_ = false;
    bool dynamic_parts_ = false;    // dynamic models placed among static meshes (map entities)
    bool skinned_ = false, skin_dirty_ = false;
    std::vector<ModelSkinPose> skin_poses_;
    std::vector<PreparedMesh> pending_equipment_;
    uint32_t skin_frame_ = 0;
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
    float frame_delta_time = 1.0f / 60.0f;  // real frame interval for camera exposure
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
    bool entity_preview = false;
    bool froxel_noise = true;  // original spatial density-noise permutation; source/selection are estimates
    float time_of_day = -1.0f;
    float day_night_state = -1.0f;  // world channel day_night_state; < 0: 1 while the sun is below the horizon (estimate)
    // The map environment (its global channel expressions: sun, ambient, fog over the day) with the atmosphere.
    bool environment = true;
    // Sky ambient with the atmosphere: the sky hemisphere map and cubemap_apply_sky_copy_ao.
    bool sky_ambient = true;
    // With the sky ambient: cubemap_apply_sky_copy_ao_with_overhead_occlusion, the sky's light
    // faded under cover (GameLighting::set_overhead_occlusion supplies the top-down map).
    bool overhead_occlusion = true;
    // Froxel interior classification (extern 81) from the same top-down depth: froxels under geometry
    // take fog_density_interior. The engine's interior visibility map producer is unknown (estimate).
    bool interior_fog = true;
    bool authored_interior_fog = false;  // keep the interior mask but suppress its density by default
    // Light state channels: render-global channels that map lights read, that default to 0 and that
    // no map data sets (script state: the light group dimmers ECA86879..ECA86870, Marathon Deck's
    // blue fill 07806276, ...). On: each is set to whichever of 0 / 1 lights more of the map's
    // lights (Outpost's lights go dark at 1, Marathon Deck's at 0). Estimate; off keeps the defaults.
    bool assume_lights_on = true;
    // With the atmosphere: the engine's froxel volume (cascade_raymarch_atmosphere_volume and
    // its _inscatter pass) for the height fog's density, sun shadowing and in-scatter.
    bool froxel_fog = true;
    float fog_density_scale = 0.25f;  // viewer visibility adjustment, 1 = map density
    bool froxel_temporal = true;  // original density reprojection; history weight and reset policy are estimates
    // Full-map viewing keeps every authored probe active at its own spatial bounds. The
    // optional camera-distance fade is an unverified interpretation of +0x3C/+0x40.
    bool probe_distance_fade = false;
    // Screen-space ambient occlusion with the game's hdao pass (game lighting only).
    bool ssao = true;
    // Light shafts: the local lights' volumetrics techniques and the engine's blur / upres passes.
    bool volumetrics = true;
    float near_shaft_strength = 0.25f;  // fades to original strength for distant volumes
    // Reconstructed radial density from the froxels (extern 0x58). Off by default: differentiating
    // the 3D noise-modulated interior froxel transmittance multiplies 5 m noise blobs by ~15x over
    // the authored base shaft density (cb0[30].x), producing blotchy clouds in interior maps.
    bool shaft_fog_density = false;
    // Screen-space reflections: the engine's hi_z_generate_* / ssr_* compute passes (PROGRESS 3.12).
    bool ssr = true;
    // Post-processing (game lighting only): bloom, auto exposure and the engine's tonemap /
    // grading pass (screen_area_global_lut3d) instead of final_combine. `exposure` then scales the
    // auto exposure (as a compensation); without auto exposure it is the exposure itself.
    bool post = true;
    bool auto_exposure = true;
    bool bloom = true;
    // Bloom weight of bright pixels (3.111, estimate): the initial downsample's luminance curve
    // weights a pixel by 1 + bloom_highlight * (its luminance after exposure), so light sources and
    // glowing FX spread while the dim surfaces keep the linear bloom. 0 = linear only.
    float bloom_highlight = 1.35f;
    // Anti-aliasing with the engine's fxaa pass over the tonemapped image (with post only).
    bool antialias = true;
    // Temporal anti-aliasing (with post only): a jittered view each frame (GameLighting::temporal_view)
    // resolved against the previous frames with the engine's resolve_taa. Replaces fxaa when it runs.
    bool temporal_aa = true;
    bool dlaa = false;
    DlssMode dlss_mode = DlssMode::Off;
    // Cinematic depth of field (with post only): the engine's v2_dof_cinematic_* passes, focused at
    // dof_focus metres of view depth (> 0). The focus ranges and blur size are estimates.
    // Vignette (with post only): the engine's draw_vignette; its strength and radii are estimates.
    bool vignette = false;
    bool depth_of_field = false;
    bool dof_edges_only = false;
    float chromatic_aberration = 0.0f;
    float emissive_gain = 1.0f;
    float output_gamma = 1.0f;  // exponent applied to the final SDR image
    float output_sharpen = 0.0f;
    float output_grain = 0.0f;
    float dof_focus = 0;
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
