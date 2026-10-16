#pragma once
// Running the engine's own full-screen pipelines (render_globals: global_lighting_and_shading,
// final_combine, ...) the way the game does: each technique's TFX bytecode reads "externs"
// (engine-owned blocks of per-frame state such as the view, the G-buffer textures and the sun),
// and the shared cbuffers (cb12 view, cb13 frame) come from the render globals' scopes.
#include "nim/render/scene.hpp"

#include <functional>
#include <map>

namespace nim {

// Extern block ids (TFX extern index, the first operand of push_extern_*).
namespace ext {
constexpr uint8_t Frame = 1, View = 2, Deferred = 3, DeferredLight = 4, Atmosphere = 7, RigidModel = 8,
                  Particle = 27, Postprocess = 38, Transparent = 40, GlobalLighting = 42, ShadowMask = 43,
                  VolumeFog = 50;
}

// Byte-addressed extern blocks. Unprovided values are errors (offsets match the bytecode:
// float/u32 ops address 4-byte units, vec4/mat4 ops 16-byte units, texture views 8-byte units).
class ExternStore {
public:
    static constexpr uint32_t kBlockSize = 0x800, kBlocks = 256;
    ExternStore();

    void set(uint8_t id, uint32_t byte_offset, float v);
    void set(uint8_t id, uint32_t byte_offset, float4 v);
    void set(uint8_t id, uint32_t byte_offset, const M4& m);
    float get_f(uint8_t id, uint32_t byte_offset) const;
    float4 get_v(uint8_t id, uint32_t byte_offset) const;

    struct Tex { ID3D12Resource* resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; uint32_t stride = 0, elements = 0; uint64_t first_element = 0; };
    void set_texture(uint8_t id, uint32_t byte_offset, ID3D12Resource* r, DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN, uint32_t stride = 0, uint32_t elements = 0, uint64_t first_element = 0);
    Tex texture(uint8_t id, uint32_t byte_offset) const;

    std::vector<float4> channels;          // global channels by index (render-globals defaults)
    std::vector<uint32_t> channel_ids;     // fnv1 of the channel name, same order
    void set_channel(std::string_view name, float4 v);
    TfxEnvironment environment() const;
    // Engine-produced cb13, including its temporal seeds and exposure state.
    void set_frame_constants(const std::array<float4, 16>& constants) { frame_ = constants; has_frame_ = true; }
    bool has_frame_constants() const { return has_frame_; }
    bool has(uint8_t id, uint32_t byte_offset, uint32_t size) const;  // every byte supplied
    const std::array<float4, 16>& frame_constants() const;
    void require(uint8_t id, uint32_t offset, uint32_t size) const;

    // Values the engine produces at run time that no game file holds and no shader analysis has
    // pinned down. They are filled with an explicit stand-in, and every one is listed (what,
    // value, why) so the UI and logs can show that the image depends on it.
    void estimate(uint8_t id, uint32_t byte_offset, float4 v, std::string_view why);
    void estimate(uint8_t id, uint32_t byte_offset, const M4& m, std::string_view why);
    void estimate_texture(uint8_t id, uint32_t byte_offset, ID3D12Resource* r, std::string_view why);
    void estimate_frame_constants(const std::array<float4, 16>& constants, std::string_view why);
    void note_estimate(std::string what, std::string_view why) const;
    // World state blocks (atmosphere, overhead visibility ...) that only a loaded map would
    // supply: unsupplied reads give zeros (texture views a null SRV, which reads zero) and are
    // recorded as estimates instead of failing.
    void zero_when_missing(uint8_t id, std::string why) { zero_policy_[id] = std::move(why); }
    bool zero_policy(uint8_t id) const { return zero_policy_.contains(id); }
    // A missing texture view under a zero policy: records it and returns true.
    bool estimate_null_texture(uint8_t id, uint32_t byte_offset) const;
    const std::map<std::string, std::string>& estimates() const { return estimates_; }
    void set_scope_constants(uint32_t reg, std::span<const float4> values);
    const std::vector<float4>& scope_constants(uint32_t reg) const;
    void set_scope_constants(ShaderStage stage, uint32_t reg, std::span<const float4> values);
    const std::vector<float4>& scope_constants(ShaderStage stage, uint32_t reg) const;
    bool has_scope_constants(ShaderStage stage, uint32_t reg) const { return stage_scopes_.contains({stage, reg}); }

private:
    std::vector<uint8_t> data_, present_;
    std::array<float4, 16> frame_{};
    bool has_frame_ = false;
    std::map<uint32_t, std::vector<float4>> scopes_;
    std::map<std::pair<ShaderStage, uint32_t>, std::vector<float4>> stage_scopes_;
    std::map<uint64_t, Tex> textures_;
    mutable std::map<std::string, std::string> estimates_;  // what -> why
    std::map<uint8_t, std::string> zero_policy_;
};

// A null view of the kind (reads zero): for estimated, unsupplied world-state resources only.
void write_null_srv(ID3D12Device* device, BindingKind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle);
// Writes only a supplied original resource view; missing metadata is an error.
void write_game_srv(ID3D12Device* device, const ExternStore::Tex& view, BindingKind kind,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle);

// Shared view inputs for model materials and the deferred lighting passes.
void fill_view_externs(ExternStore& ext, uint32_t width, uint32_t height, const ViewMatrices& vm);
std::array<float4, 16> frame_constants(float time, float delta, const ExternStore& ext);
// The engine's per-frame state that no file holds (exposure, wind, temporal seeds): cb13 and
// the Frame extern, laid out per goriath's FrameScope with its defaults, recorded as estimates.
// Time is real. Keeps constants the caller already supplied.
void estimate_engine_frame(ExternStore& ext, float time, float delta);

// Shared-cbuffer provider: (stage, register) -> GPU address, or 0 when not provided.
using ScopeBufferFn = std::function<D3D12_GPU_VIRTUAL_ADDRESS(ShaderStage, uint32_t)>;

// One engine technique drawn as a full-screen 4-vertex strip with its own DXIL.
class GamePipeline {
public:
    struct Options {
        DXGI_FORMAT rtv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bool additive = false;
        std::vector<DXGI_FORMAT> additional_rtv_formats; // exact attachment formats for SV_Target1..7
    };
    // `scopes` supply samplers the technique does not (the frame scope's s0, ...).
    void load(PackageManager& pm, Gpu& gpu, TagHash technique, std::span<const Scope* const> scopes, Options opt);
    void release(Gpu& gpu);
    bool loaded() const { return pso_ != nullptr; }
    TagHash tag() const { return tag_; }

    // Evaluates the stages' TFX against `ext`, binds everything and draws. The caller sets the
    // render target, viewport and scissor.
    void draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
              bool validated = false);
    void validate(const ExternStore& ext, const ScopeBufferFn& scopes) const;
    void dispatch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                  uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

    std::vector<std::string> notes;  // binding diagnostics, for the UI / nimshade
    bool cache_bindings = true;

private:
    struct Stage {
        ShaderStage which = ShaderStage::Vertex;
        TechniqueStage tfx;
        std::vector<ShaderBinding> bindings;
        std::vector<TfxExternTexture> extern_textures, extern_uavs;
        StageResourceSources sources;
        std::map<uint32_t, ComPtr<ID3D12Resource>> static_textures;
        std::map<uint32_t, BindingKind> srv_kinds;
        uint32_t srv_count = 0, uav_count = 0;
        std::map<uint32_t, BindingKind> uav_kinds;
        std::vector<std::pair<uint32_t, uint32_t>> cbvs;  // (register, root parameter)
        int table_param = -1, uav_table_param = -1;
        struct BoundView { ComPtr<ID3D12Resource> resource; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; bool valid = false; uint32_t stride = 0, elements = 0; uint64_t first_element = 0; };
        std::array<std::vector<BoundView>, 3> bound_views;
        std::vector<float4> constants;
    };
    void execute(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                 bool validated, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);
    bool compute_ = false;
    TagHash tag_;
    std::vector<Stage> stages_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
    uint32_t srv_first_ = UINT32_MAX, srv_total_ = 0, ring_ = 0;
    static constexpr uint32_t kRing = 3;
};

// Runs a scope's bytecode for one stage into a cbuffer (cbuffer_init patched by the program).
std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext);

// Original game DXIL only. Engine-owned inputs must be supplied through externs();
// missing producers are reported instead of being approximated.
// The sun direction (toward the sun) of nim's light set-up.
V3 sun_direction(const LightingParams& p);

class GameLighting {
public:
    void init(PackageManager& pm, Gpu& gpu);
    bool ready() const { return shading_.loaded() && final_combine_.loaded(); }
    // `transparents` runs after the shading pass with the HDR target's RTV (the G-buffer depth is
    // then in DEPTH_READ, bound through gb.dsv_ro).
    using TransparentFn = std::function<void(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE hdr)>;
    // `shadow`: a sun shadow map rendered this frame (StaticMeshScene::render_shadow), resolved
    // into the shading pass's sun shadow mask; without one the mask is white (unshadowed).
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p,
                const TransparentFn& transparents = {}, SunShadow* shadow = nullptr);
    void release(Gpu& gpu);

    ExternStore& externs() { return ext_; }
    const std::vector<std::string>& notes() const { return shading_.notes; }
    TagHash shading_technique() const { return shading_.tag(); }
    ID3D12Resource* shadow_mask() const { return shadow_mask_.Get(); }  // R8, PIXEL_SHADER_RESOURCE

private:
    void resize(Gpu& gpu, uint32_t w, uint32_t h);
    void fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p, bool shadowed);

    RenderGlobals globals_;
    Scope frame_scope_, view_scope_;
    ExternStore ext_;
    GamePipeline shading_, final_combine_;
    ComPtr<ID3D12Resource> tint_lookup_, lobe_lookup_, lobe3d_lookup_, iridescence_lookup_;
    ComPtr<ID3D12Resource> no_atmosphere_;  // 1x1x2 (1, 1, 0, 0): clear air while no map atmosphere is loaded
    ComPtr<ID3D12Resource> unshadowed_;  // 1x1 white: "no shadow" while no shadow pass exists
    ComPtr<ID3D12Resource> hdr_;
    // Viewer environment (not game data): nim's sky as the specular-IBL light buffer and background.
    ComPtr<ID3D12Resource> env_;
    ComPtr<ID3D12RootSignature> env_rs_;
    ComPtr<ID3D12PipelineState> env_ibl_pso_, env_bg_pso_;
    ComPtr<ID3D12RootSignature> shadow_rs_;
    ComPtr<ID3D12PipelineState> shadow_pso_;
    ComPtr<ID3D12Resource> shadow_mask_;  // R8: the resolved sun shadow (viewer estimate)
    uint32_t env_srv_ = UINT32_MAX;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    uint32_t w_ = 0, h_ = 0;

};

} // namespace nim
