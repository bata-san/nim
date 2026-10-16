#pragma once
#include "nim/content/animation.hpp"
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/render/gpu.hpp"

#include <array>
#include <map>
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
    V3 eye, fwd, right, up;
    float fx = 1, fy = 1;
};
ViewMatrices make_view(const View& v);
// A view that frames a sphere from a pleasant three-quarter angle.
View frame_sphere(V3 center, float radius, uint32_t w, uint32_t h, float yaw = -0.93f, float pitch = 0.41f);

// ---- G-buffer ---------------------------------------------------------------------------------
// RT0 albedo, RT1 packed normal+smoothness, RT2 material params, RT3 motion; all RGBA16F as
// the game's PS writes them. `lit` is the lighting pass output (RGBA8, sRGB-encoded values).
class GBuffer {
public:
    void resize(Gpu& gpu, uint32_t w, uint32_t h);
    void release(Gpu& gpu);
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }

    ComPtr<ID3D12Resource> rt[4], depth, lit;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{}, lit_rtv{}, dsv{}, dsv_ro{};  // dsv_ro: read-only depth (transparents)
    uint32_t srv_first = UINT32_MAX;  // 6 SRVs: rt0..3, depth, lit
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;

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
        int blend = -1;     // engine blend state index (transparents)
        std::vector<float4> vs_cb0;                // the VS stage's own cb0
        std::map<uint32_t, std::shared_ptr<const Texture>> vs_textures;
        TechniqueStage vs_material, ps_material;  // original TFX, retained for frame updates
        std::vector<Scope> scopes;
        StageResourceSources vs_sources, ps_sources;
        bool vs_live = false, ps_live = false;
        std::map<uint32_t, SamplerDesc> vs_samplers, ps_samplers;
    };
    TagHash tag;
    bool dynamic = false;  // dynamic model (entity mesh) rather than a static mesh
    std::vector<Group> groups;
    std::vector<Buffers> buffers;
    std::vector<uint8_t> instance;  // static meshes: t2 instance data
    // Dynamic models: the rigid_model scope cbuffer (cb1). c0-c3 mesh-to-world, c4 position scale, c5 offset, c6 texcoord scale/offset.
    std::array<float4, 20> model_cb{};
    V3 center;
    float radius = 1;
    uint8_t lod = 0;
    std::vector<std::string> notes;  // parts that could not be drawn, and why
    std::map<uint32_t, ChannelUse> channels;  // object channels the materials read
    ChannelValues channel_overrides;
    ChannelValues channel_defaults;
    std::vector<float4> global_channels;
    std::vector<Scope> scopes;
    TagHash component;               // dynamic models: the component the materials came from
    uint32_t permutation = 0, permutation_count = 0;
};
// Throws on failure.
PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash mesh, const ChannelValues* channels = nullptr);
// Dynamic model (class 8080881C), GenerateGbuffer parts at the highest LOD, without the entity's
// material variants. Skinned meshes are drawn in their bind pose until StaticMeshScene::set_skin_pose.
// `materials` (from index_dynamic_model_materials) supplies the parts with no technique of their
// own; permutation < 0 = the component's default.
PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash model, const DynamicModelMaterials* materials = nullptr,
                                   int permutation = -1, const ChannelValues* channels = nullptr);
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
    void release(Gpu& gpu);  // caller must make sure the GPU is idle
    bool loaded() const { return !items_.empty(); }

    // Clears the G-buffer and draws every G-buffer group. Leaves targets in RENDER_TARGET.
    void render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);
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
    void set_render_inputs(const ExternStore& inputs);
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
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> root_sig;
        ComPtr<ID3D12Resource> cb0, vs_cb0;
        TechniqueStage vs_material, ps_material;
        std::vector<Scope> scopes;
        StageResourceSources vs_sources, ps_sources;
        std::map<uint32_t, BindingKind> vs_kinds, ps_kinds;
        std::vector<uint32_t> vs_cbvs, ps_cbvs;
        std::map<std::pair<ShaderStage, uint32_t>, D3D12_GPU_VIRTUAL_ADDRESS> scope_addresses;
        std::vector<float4> vs_constants, ps_constants;
        D3D12_GPU_VIRTUAL_ADDRESS vs_address = 0, ps_address = 0;
        TagHash technique;
        bool vs_live = false, ps_live = false;
        uint32_t srv_first = UINT32_MAX;  // kItemSrvs descriptors: PS t0-t31, then VS t0-t31
        std::map<uint32_t, ComPtr<ID3D12Resource>> textures, vs_textures;
    };
    static constexpr uint32_t kPsSrvs = 32, kVsSrvs = 32, kItemSrvs = kPsSrvs + kVsSrvs;
    static constexpr uint32_t kSrvCopies = Gpu::kFramesInFlight + 1;
    uint32_t srv_frame_ = 0;
    void bind_common(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint32_t w, uint32_t h, const ViewMatrices& vm,
                     ID3D12RootSignature* root = nullptr);
    void draw_items(Gpu& gpu, ID3D12GraphicsCommandList* cl, uint8_t stage, uint32_t w, uint32_t h, const ViewMatrices& vm);
    void update_materials(Gpu& gpu, uint32_t w, uint32_t h, const ViewMatrices& vm);
    std::unique_ptr<ExternStore> material_externs_;
    ChannelValues channel_overrides_;
    ChannelValues channel_defaults_;
    float material_time_ = 0, material_delta_ = 0;
    std::unordered_map<uint64_t, D3D12_GPU_VIRTUAL_ADDRESS> material_addresses_;
    TagHash tag_;
    size_t transparent_count_ = 0;
    std::map<uint32_t, ChannelUse> channels_;
    std::vector<Item> items_;
    std::vector<SceneGroupInfo> info_;
    ComPtr<ID3D12RootSignature> root_sig_, root_sig_dynamic_;
    struct BufferSet {
        ComPtr<ID3D12Resource> ib, vb0, vb1, skin_pos, skin_normal, color;
        D3D12_INDEX_BUFFER_VIEW ibv{};
        D3D12_VERTEX_BUFFER_VIEW vbv[4]{};
        // CPU skinning: bind-pose vertices and weights; each frame's streams go through its own
        // upload buffer (positions, then normals) into the default-heap skin_pos / skin_normal.
        std::vector<uint8_t> vb0_cpu;
        SkinWeights skin;
        std::array<ComPtr<ID3D12Resource>, Gpu::kFramesInFlight> skin_upload;
        bool skin_readable = false;  // skin_pos / skin_normal in NON_PIXEL_SHADER_RESOURCE
    };
    void update_skin(ID3D12GraphicsCommandList* cl);
    std::vector<BufferSet> sets_;
    std::vector<std::string> notes_;
    ComPtr<ID3D12Resource> inst_vb_, t2_;
    std::array<float4, 20> model_cb_{};
    bool dynamic_ = false;
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
    // Height fog (game lighting only; evaluated by the game's own shading pass). Density is the
    // game's unit: optical depth per kilometre at the base height, falling off with height.
    bool fog = false;
    float fog_density = 6.0f, fog_falloff = 8.0f, fog_height = 0.0f, fog_start = 0.0f;
    V3 fog_color{0.55f, 0.62f, 0.70f};
    float fog_sun_glow = 1.5f;
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
