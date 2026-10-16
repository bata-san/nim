#pragma once
#include "nim/content/tfx.hpp"
#include "nim/content/tfx_vm.hpp"
#include "nim/render/gpu.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace nim {

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
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{}, lit_rtv{}, dsv{};
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
struct PreparedMesh {
    struct Buffers {
        IndexBuffer ib;
        VertexBuffer vb0, vb1;
        // Skinned meshes: the packed streams the game's compute skinning writes for the vertex
        // shader (t2/t4 positions, 21/21/22-bit snorm pairs; t3 octahedral normal + tangent).
        std::vector<uint32_t> skin_pos, skin_normal;
    };
    struct Group {
        StaticMeshPart part;
        uint8_t input_layout = 0;
        uint32_t buffers = 0;  // index into PreparedMesh::buffers
        SceneGroupInfo info;
        std::vector<uint8_t> vs, ps;
        std::vector<float4> cb0;
        std::map<uint32_t, Texture> textures;
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
    TagHash component;               // dynamic models: the component the materials came from
    uint32_t permutation = 0, permutation_count = 0;
};
// Throws on failure.
PreparedMesh prepare_static_mesh(PackageManager& pm, TagHash mesh);
// Dynamic model (class 8080881C), GenerateGbuffer parts at the highest LOD, without the entity's
// material variants. Skinned meshes are drawn in their bind pose.
// `materials` (from index_dynamic_model_materials) supplies the parts with no technique of their
// own; permutation < 0 = the component's default.
PreparedMesh prepare_dynamic_model(PackageManager& pm, TagHash model, const DynamicModelMaterials* materials = nullptr,
                                   int permutation = -1);
// Either of the above, by the tag's class.
PreparedMesh prepare_mesh(PackageManager& pm, TagHash tag, const DynamicModelMaterials* materials = nullptr);

class StaticMeshScene {
public:
    void load(PackageManager& pm, Gpu& gpu, TagHash mesh, const std::vector<std::vector<InputElement>>& layouts) {
        upload(gpu, prepare_mesh(pm, mesh), layouts);
    }
    void upload(Gpu& gpu, PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts);
    void release(Gpu& gpu);  // caller must make sure the GPU is idle
    bool loaded() const { return !items_.empty(); }

    // Clears the G-buffer and draws every G-buffer group. Leaves targets in RENDER_TARGET.
    void render_gbuffer(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);

    TagHash tag() const { return tag_; }
    V3 center() const { return center_; }
    float radius() const { return radius_; }
    uint8_t lod() const { return lod_; }
    bool dynamic() const { return dynamic_; }
    const std::vector<SceneGroupInfo>& groups() const { return info_; }
    const std::vector<std::string>& notes() const { return notes_; }

private:
    struct Item {
        StaticMeshPart part;
        uint32_t buffers = 0;
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12Resource> cb0;
        uint32_t srv_first = 0;
        std::map<uint32_t, ComPtr<ID3D12Resource>> textures;
    };
    TagHash tag_;
    std::vector<Item> items_;
    std::vector<SceneGroupInfo> info_;
    ComPtr<ID3D12RootSignature> root_sig_, root_sig_dynamic_;
    struct BufferSet {
        ComPtr<ID3D12Resource> ib, vb0, vb1, skin_pos, skin_normal;
        uint32_t srv_first = UINT32_MAX;  // t2, t3, t4 (dynamic models)
        D3D12_INDEX_BUFFER_VIEW ibv{};
        D3D12_VERTEX_BUFFER_VIEW vbv[4]{};
    };
    std::vector<BufferSet> sets_;
    std::vector<std::string> notes_;
    ComPtr<ID3D12Resource> inst_vb_, zero_vb_, t2_, fallback_;
    std::array<float4, 20> model_cb_{};
    bool dynamic_ = false;
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
