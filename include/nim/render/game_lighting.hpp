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

// Byte-addressed extern blocks, filled with the engine defaults (offsets match the bytecode:
// float/u32 ops address 4-byte units, vec4/mat4 ops 16-byte units, texture views 8-byte units).
class ExternStore {
public:
    static constexpr uint32_t kBlockSize = 0x800, kBlocks = 64;
    ExternStore();

    void set(uint8_t id, uint32_t byte_offset, float v);
    void set(uint8_t id, uint32_t byte_offset, float4 v);
    void set(uint8_t id, uint32_t byte_offset, const M4& m);
    float get_f(uint8_t id, uint32_t byte_offset) const;
    float4 get_v(uint8_t id, uint32_t byte_offset) const;

    struct Tex { ID3D12Resource* resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; };
    void set_texture(uint8_t id, uint32_t byte_offset, ID3D12Resource* r, DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN);
    Tex texture(uint8_t id, uint32_t byte_offset) const;

    std::vector<float4> channels;          // global channels by index (render-globals defaults)
    std::vector<uint32_t> channel_ids;     // fnv1 of the channel name, same order
    void set_channel(std::string_view name, float4 v);
    TfxEnvironment environment() const;

private:
    std::vector<uint8_t> data_;
    std::map<uint64_t, Tex> textures_;
};

// 1x1 textures standing in for engine textures nobody rendered (an empty shadow mask is white...).
class FillerTextures {
public:
    ID3D12Resource* get(Gpu& gpu, BindingKind kind, float4 value);
    void release() { cache_.clear(); }

private:
    std::map<std::pair<int, std::array<float, 4>>, ComPtr<ID3D12Resource>> cache_;
};

// Shared-cbuffer provider: (stage, register) -> GPU address, or 0 when not provided.
using ScopeBufferFn = std::function<D3D12_GPU_VIRTUAL_ADDRESS(ShaderStage, uint32_t)>;

// One engine technique drawn as a full-screen 4-vertex strip with its own DXIL.
class GamePipeline {
public:
    struct Options {
        DXGI_FORMAT rtv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        bool additive = false;
    };
    // `scopes` supply samplers the technique does not (the frame scope's s0, ...).
    void load(PackageManager& pm, Gpu& gpu, TagHash technique, std::span<const Scope* const> scopes, Options opt);
    void release(Gpu& gpu);
    // Creates the filler textures draw() will need, so their uploads can be flushed up front.
    void prewarm(Gpu& gpu, FillerTextures& fillers);
    bool loaded() const { return pso_ != nullptr; }
    TagHash tag() const { return tag_; }

    // Evaluates the stages' TFX against `ext`, binds everything and draws. The caller sets the
    // render target, viewport and scissor.
    void draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
              FillerTextures& fillers);

    std::vector<std::string> notes;  // binding diagnostics, for the UI / nimshade
    // Pixel-stage cb0 registers forced after the TFX ran (inputs whose engine passes nim lacks).
    std::map<uint32_t, float4> ps_overrides;

private:
    struct Stage {
        ShaderStage which = ShaderStage::Vertex;
        TechniqueStage tfx;
        std::vector<ShaderBinding> bindings;
        std::vector<TfxExternTexture> extern_textures;
        std::map<uint32_t, ComPtr<ID3D12Resource>> static_textures;
        std::map<uint32_t, BindingKind> srv_kinds;
        uint32_t srv_count = 0;
        std::vector<std::pair<uint32_t, uint32_t>> cbvs;  // (register, root parameter)
        int table_param = -1;
    };
    TagHash tag_;
    std::vector<Stage> stages_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
    uint32_t srv_first_ = UINT32_MAX, srv_total_ = 0, ring_ = 0;
    static constexpr uint32_t kRing = 3;
};

// Runs a scope's bytecode for one stage into a cbuffer (cbuffer_init patched by the program).
std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext);

// The deferred shading the game itself runs over the G-buffer: global_lighting_and_shading with
// the sun / ambient fed through the GlobalLighting extern, a sky-reflection stand-in written to
// light_specular_ibl and a fog in-scatter stand-in for the atmosphere lookups (the height fog
// itself is the game shader's), then the game's final_combine film curve to gb.lit.
class GameLighting {
public:
    void init(PackageManager& pm, Gpu& gpu);
    bool ready() const { return shading_.loaded(); }
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p);
    void release(Gpu& gpu);

    ExternStore& externs() { return ext_; }
    const std::vector<std::string>& notes() const { return shading_.notes; }
    TagHash shading_technique() const { return shading_.tag(); }

private:
    void resize(Gpu& gpu, uint32_t w, uint32_t h);
    void fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p);

    RenderGlobals globals_;
    Scope frame_scope_, view_scope_;
    ExternStore ext_;
    FillerTextures fillers_;
    GamePipeline shading_, final_combine_;
    ComPtr<ID3D12Resource> tint_lookup_, lobe_lookup_, lobe3d_lookup_, iridescence_lookup_;
    // light_diffuse, light_specular, light_specular_ibl, hdr, fog in-scatter, exposed hdr
    static constexpr int kTargets = 6;
    ComPtr<ID3D12Resource> targets_[kTargets];
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    uint32_t srv_first_ = UINT32_MAX;  // hdr, depth, rt1 (for nim's passes)
    uint32_t w_ = 0, h_ = 0;
    ComPtr<ID3D12RootSignature> util_rs_;
    ComPtr<ID3D12PipelineState> ibl_pso_, tonemap_pso_, fog_pso_, post_pso_;
    float time_ = 0;
};

} // namespace nim
