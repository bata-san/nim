#pragma once
// Running the engine's own full-screen pipelines (render_globals: global_lighting_and_shading,
// final_combine, ...) the way the game does: each technique's TFX bytecode reads "externs"
// (engine-owned blocks of per-frame state such as the view, the G-buffer textures and the sun),
// and the shared cbuffers (cb12 view, cb13 frame) come from the render globals' scopes.
#include "nim/content/map.hpp"
#include "nim/render/scene.hpp"
#include "nim/render/dlss.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <unordered_set>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>

namespace nim {

// Extern block ids (TFX extern index, the first operand of push_extern_*).
namespace ext {
constexpr uint8_t Frame = 1, View = 2, Deferred = 3, DeferredLight = 4, Atmosphere = 7, RigidModel = 8,
                  Particle = 27, Postprocess = 38, Transparent = 40, GlobalLighting = 42, ShadowMask = 43,
                  VolumeFog = 50;
}

// Per-draw extern values over a shared ExternStore: the 4-byte words a light volume writes before
// its draw, recorded so its TFX can be evaluated on a worker thread while the store stays
// unchanged. Reads of other words fall through to the store.
class ExternOverlay {
public:
    void set(uint8_t id, uint32_t byte_offset, float v) {
        if (byte_offset >= kBlockBytes) throw std::out_of_range("ExternOverlay: offset beyond the extern block");
        const uint32_t i = uint32_t(id) * kWords + (byte_offset >> 2);
        if (values_.empty()) values_.resize(size_t(256) * kWords), stamps_.resize(size_t(256) * kWords);
        if (stamps_[i] != epoch_) {
            stamps_[i] = epoch_;
            touched_.push_back(i);
            sorted_ = false;
        }
        values_[i] = v;
    }
    void set(uint8_t id, uint32_t byte_offset, const std::array<float, 4>& v) {
        for (uint32_t i = 0; i < 4; ++i) set(id, byte_offset + i * 4, v[i]);
    }
    void set(uint8_t id, uint32_t byte_offset, const M4& m);
    // The current values, sorted by key(), for find().
    void snapshot(std::vector<std::pair<uint32_t, float>>& out) {
        if (!sorted_) std::sort(touched_.begin(), touched_.end()), sorted_ = true;
        out.resize(touched_.size());
        for (size_t k = 0; k < touched_.size(); ++k) {
            const uint32_t i = touched_[k];
            out[k] = {key(uint8_t(i / kWords), (i % kWords) * 4), values_[i]};
        }
    }
    void clear() {
        touched_.clear();
        sorted_ = true;
        if (++epoch_ == 0) std::fill(stamps_.begin(), stamps_.end(), 0), epoch_ = 1;
    }
    const float* find(uint8_t id, uint32_t byte_offset) const {
        if (values_.empty() || byte_offset >= kBlockBytes) return nullptr;
        const uint32_t i = uint32_t(id) * kWords + (byte_offset >> 2);
        return stamps_[i] == epoch_ ? &values_[i] : nullptr;
    }
    static uint32_t key(uint8_t id, uint32_t byte_offset) { return uint32_t(id) << 16 | (byte_offset >> 2); }

private:
    // Dense words of all 256 blocks: the same words are written by every light of a pass.
    static constexpr uint32_t kBlockBytes = 0x800, kWords = kBlockBytes / 4;
    std::vector<float> values_;
    std::vector<uint32_t> stamps_;
    std::vector<uint32_t> touched_;
    uint32_t epoch_ = 1;
    bool sorted_ = true;
};

// Byte-addressed extern blocks. Unprovided values are errors (offsets match the bytecode:
// float/u32 ops address 4-byte units, vec4/mat4 ops 16-byte units, texture views 8-byte units).
class ExternStore {
public:
    static constexpr uint32_t kBlockSize = 0x800, kBlocks = 256;
    ExternStore();

    // Replace a complete block, including missing-value policy and texture views. Removes stale views.
    void copy_block_from(uint8_t id, const ExternStore& source);
    void set(uint8_t id, uint32_t byte_offset, float v);
    void set(uint8_t id, uint32_t byte_offset, float4 v);
    void set(uint8_t id, uint32_t byte_offset, const M4& m);
    float get_f(uint8_t id, uint32_t byte_offset) const;
    float4 get_v(uint8_t id, uint32_t byte_offset) const;

    // `mip`: the mip a 2D UAV writes (SRVs see the whole chain).
    struct Tex { ID3D12Resource* resource = nullptr; DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; uint32_t stride = 0, elements = 0; uint64_t first_element = 0; uint32_t mip = 0; };
    void set_texture(uint8_t id, uint32_t byte_offset, ID3D12Resource* r, DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN, uint32_t stride = 0, uint32_t elements = 0, uint64_t first_element = 0, uint32_t mip = 0);
    Tex texture(uint8_t id, uint32_t byte_offset) const;

    std::vector<float4> channels;          // global channels by index (render-globals defaults)
    std::vector<uint32_t> channel_ids;     // fnv1 of the channel name, same order
    std::map<uint32_t, float> world_channels; // script registry slots, never render-globals indices
    void set_channel(std::string_view name, float4 v);
    TfxEnvironment environment() const;
    // The same environment with `overlay` (sorted ExternOverlay words or live ExternOverlay) over this store's values.
    TfxEnvironment environment(const std::vector<std::pair<uint32_t, float>>* overlay) const;
    TfxEnvironment environment(const ExternOverlay* overlay) const;
    // Engine-produced cb13, including its temporal seeds and exposure state.
    void set_frame_constants(const std::array<float4, 16>& constants) { frame_ = constants; has_frame_ = true; frame_estimated_ = false; }
    bool has_frame_constants() const { return has_frame_; }
    bool frame_constants_estimated() const { return frame_estimated_; }  // refreshed every frame
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
    // True the first time `key` is passed: repeated per-frame reports skip formatting their text.
    bool first_note(uint64_t key) const;
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
    bool has_frame_ = false, frame_estimated_ = false;
    std::map<uint32_t, std::vector<float4>> scopes_;
    std::map<std::pair<ShaderStage, uint32_t>, std::vector<float4>> stage_scopes_;
    std::map<uint64_t, Tex> textures_;
    mutable std::map<std::string, std::string> estimates_;  // what -> why
    mutable std::unordered_set<uint64_t> noted_;              // zero-policy reports already made
    // Fixed reports of the TFX environment (registry, per-channel and per-texture notes).
    struct NoteBits {
        std::array<std::atomic<uint32_t>, 32> v{};
        NoteBits() = default;
        NoteBits(const NoteBits& o) { *this = o; }
        NoteBits& operator=(const NoteBits& o) {
            for (size_t i = 0; i < v.size(); ++i) v[i].store(o.v[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };
    mutable NoteBits note_bits_;
    bool first_bit(uint32_t bit) const {
        const uint32_t mask = 1u << (bit & 31);
        auto& word = note_bits_.v[bit >> 5];
        if (word.load(std::memory_order_relaxed) & mask) return false;
        return !(word.fetch_or(mask) & mask);
    }
    std::map<uint8_t, std::string> zero_policy_;
};

// A null view of the kind (reads zero): for estimated, unsupplied world-state resources only.
void write_null_srv(ID3D12Device* device, BindingKind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle);
// Writes only a supplied original resource view; missing metadata is an error.
void write_game_srv(ID3D12Device* device, const ExternStore::Tex& view, BindingKind kind,
                    D3D12_CPU_DESCRIPTOR_HANDLE handle);

// Shared view inputs for model materials and the deferred lighting passes.
void fill_view_externs(ExternStore& ext, uint32_t width, uint32_t height, const ViewMatrices& vm);
// cb11[19..20] of the relighting cubemap variants and the probe alpha exponent cb11[15].z, by the
// D2 engine's formula (see game_lighting.cpp). Channels: 0x8B, 0x8A, 0x89, 0x08, 0x09, 0x64.
struct CubemapRelightingInputs {
    float bounce_scale = 1, sky_intensity = 1, sun_scale = 1, sun_intensity = 10;
    float4 sun_color{1, 1, 1, 1}, sun_light_direction{0, 0, 1, 0};
};
std::array<float4, 2> cubemap_relighting(const MapCubemap& volume, const CubemapRelightingInputs& in);
float cubemap_alpha_exponent(float z);
// The depth view of a shadowing light, from its own projective matrix (+0x60: cube -> light
// local, forward +X): reversed depth, 1 at the near plane and 0 at the far plane. nullopt when the
// matrix is not a frustum along +X.
std::optional<ViewMatrices> light_shadow_view(const MapLight& light);
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
        // Light volume: a float3 POSITION0 vertex stream, indexed triangles, faces of one winding
        // culled (`cull_front`), every target accumulated additively.
        bool volume = false;
        bool cull_front = true;
        uint32_t cbuffer_floor = 256;  // float4s allocated per stage cbuffer (TFX outputs beyond fail)
        int blend_state = -1;          // >= 0: the engine's blend state (every target), instead of `additive`
        // Depth test against a D32_FLOAT depth buffer (no writes); 0 = no depth buffer.
        D3D12_COMPARISON_FUNC depth_func = D3D12_COMPARISON_FUNC(0);
        // PS targets beyond the attachments are written to nothing (e.g. the sky's motion vectors).
        bool drop_extra_targets = false;
        // Per-target write masks (D3D12_COLOR_WRITE_ENABLE_*), when not blending; empty = all.
        std::vector<uint8_t> write_masks;
        // Original volume VS only, with winding counts behind the scene depth; or apply where
        // that count is nonzero. No replacement pixel shader is used for the geometry mask.
        bool stencil_volume = false, stencil_test = false;
        // Instanced quads: a per-instance stream of this many float4s (TEXCOORD0..n-1), each
        // instance drawn as a 4-vertex strip (draw_instances). 0 = none.
        uint32_t instance_float4s = 0;
        // Mesh particles: with instance_float4s, the VS's per-vertex POSITION0..n come from a model's
        // vertex stream (slot 0: DXGI format, byte offset), the instances from slot 1, and each
        // instance is the model's indexed triangle list (draw_mesh_instances).
        std::vector<std::pair<uint32_t, uint32_t>> mesh_elements;
        bool depth_write = false;
        // A graphics technique that also carries a compute stage: load that stage instead.
        bool compute_stage = false;
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
    // With PS textures the engine binds itself (slots no TFX fills, e.g. a blur's source). Not
    // validated: the caller supplies those slots.
    void draw(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
              std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures);
    void validate(const ExternStore& ext, const ScopeBufferFn& scopes) const;
    void validate(const ExternStore& ext, const ScopeBufferFn& scopes, std::vector<std::vector<float4>>* keep) const;
    // Light volumes: the pipeline is shared by every technique with the same shaders; each draw
    // brings its own technique's TFX, constants and cbuffer (textures and samplers must match), or
    // the pipeline's own with `materials` null. `ps_textures`: views the engine binds itself
    // (slots no TFX fills, e.g. a cubemap volume's cubes). With an empty ibv, `indices` is the
    // vertex count of an authored non-indexed triangle list.
    struct VolumeBatch {
        const GamePipeline* pipeline = nullptr;
        const D3D12_VERTEX_BUFFER_VIEW* vertices = nullptr;
        const D3D12_INDEX_BUFFER_VIEW* indices = nullptr;
        std::array<uint64_t, 64> root_bindings{};
        std::unordered_map<const GamePipeline*, std::array<std::optional<uint32_t>, 2>> stable_tables;
    };
    // The per-stage resources must stay identical within a particle batch.
    void draw_instances(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        const D3D12_VERTEX_BUFFER_VIEW& vbv, uint32_t instances, VolumeBatch& batch);
    // Mesh particles: `instances` copies of the indexed triangle list `mesh` / `ibv` (`indices`),
    // the per-instance stream `vbv` in slot 1.
    void draw_mesh_instances(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                             const D3D12_VERTEX_BUFFER_VIEW& mesh, const D3D12_INDEX_BUFFER_VIEW& ibv, uint32_t indices,
                             const D3D12_VERTEX_BUFFER_VIEW& vbv, uint32_t instances, VolumeBatch& batch);
    void dispatch_batch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                        uint32_t groups_x, VolumeBatch& batch);
    // Each stage's evaluated cbuffer, in stage order (evaluate_stages).
    using StageConstants = std::array<std::vector<float4>, 2>;
    void draw_volume(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                     const Technique* materials, const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv,
                     uint32_t indices, std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures = {}, VolumeBatch* batch = nullptr,
                     const StageConstants* constants = nullptr, bool stable_views = false);
    // The TFX of each stage (with `materials`' programs when given) against `env`: what draw_volume
    // would evaluate. Thread-safe; draw_volume then takes the result instead of evaluating again.
    void evaluate_stages(const TfxEnvironment& env, const Technique* materials, StageConstants& out) const;
    // Whether a stage of `which` binds shared cbuffer b`reg` (not its own constants).
    bool binds_shared_cbuffer(ShaderStage which, uint32_t reg) const {
        for (const auto& st : stages_)
            if (st.which == which)
                for (const auto& [r, param] : st.cbvs)
                    if (r == reg && int32_t(r) != st.tfx.cbuffer_slot) return true;
        return false;
    }
    void dispatch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                  uint32_t groups_x, uint32_t groups_y, uint32_t groups_z,
                  std::span<const std::pair<uint32_t, ID3D12Resource*>> textures = {});
    // The group counts come from the first three words of `args` (INDIRECT_ARGUMENT state).
    void dispatch_indirect(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                           ID3D12CommandSignature* signature, ID3D12Resource* args);

    std::vector<std::string> notes;  // binding diagnostics, for the UI / nimshade
    const std::vector<float4>& stage_constants(size_t i) const { return stages_.at(i).constants; }  // last draw's cbuffer (not for draws given evaluated constants)

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
        std::vector<float4> constants;
    };
    struct Geometry {
        const Technique* materials;
        const D3D12_VERTEX_BUFFER_VIEW* vbv;
        const D3D12_INDEX_BUFFER_VIEW* ibv;
        uint32_t indices;
        std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures;
        VolumeBatch* batch = nullptr;
        const StageConstants* constants = nullptr;
        uint32_t instances = 0;  // instanced 4-vertex strips over vbv
        const D3D12_VERTEX_BUFFER_VIEW* mesh = nullptr;  // mesh particles: per-vertex slot 0 (vbv: slot 1, ibv: the mesh's)
        bool stable_views = false;
    };
    void execute(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                 bool validated, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, const Geometry* geometry = nullptr);
    bool compute_ = false;
    ID3D12CommandSignature* indirect_signature_ = nullptr;  // set for the span of dispatch_indirect
    ID3D12Resource* indirect_args_ = nullptr;
    uint32_t cbuffer_floor_ = 256;
    std::vector<std::vector<float4>> validated_constants_;
    TagHash tag_;
    std::vector<Stage> stages_;
    struct PendingView { uint32_t slot; BindingKind kind; ExternStore::Tex view; bool uav; };
    std::vector<PendingView> pending_views_;
    std::vector<uint64_t> view_key_;
    std::vector<ID3D12Resource*> view_resources_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
};

// Runs a scope's bytecode for one stage into a cbuffer (cbuffer_init patched by the program).
std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext);
std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const TfxEnvironment& env);

// Original game DXIL only. Engine-owned inputs must be supplied through externs();
// missing producers are reported instead of being approximated.
// The sun direction (toward the sun) of nim's light set-up.
V3 sun_direction(const LightingParams& p);
// With a map atmosphere (and p.atmosphere): its sun path at the time of day.
V3 sun_direction(const LightingParams& p, const MapAtmosphere* atmosphere);

class GameLighting {
public:
    void init(PackageManager& pm, Gpu& gpu);
    bool ready() const { return shading_.loaded() && final_combine_.loaded(); }
    // Resolve frame inputs before recording the G-buffer; materials and lighting share this exposure.
    void prepare_frame(Gpu& gpu, const GBuffer& gb, const LightingParams& p);
    // `transparents` runs after the shading pass with the HDR target's RTV (the G-buffer depth is
    // then in DEPTH_READ, bound through gb.dsv_ro).
    using TransparentFn = std::function<void(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE hdr)>;
    using DistortionFn = std::function<void(ID3D12GraphicsCommandList*, uint32_t, uint32_t)>;
    // `shadow`: a sun shadow map rendered this frame (StaticMeshScene::render_shadow), resolved
    // into the shading pass's sun shadow mask; without one the mask is white (unshadowed).
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p,
                const TransparentFn& transparents = {}, SunShadow* shadow = nullptr, bool needs_light_grid = true,
                const DistortionFn& distortions = {});
    void release(Gpu& gpu);

    // A map's local lights (load_map_lights), drawn each frame with their own lighting-apply
    // techniques into the light buffers the shading pass reads. Lights whose technique cannot be
    // loaded are left out and listed in light_notes().
    void set_lights(PackageManager& pm, Gpu& gpu, std::span<const MapLight* const> lights);
    void clear_lights(Gpu& gpu);  // waits for the GPU first when light shadow maps are freed
    size_t light_count() const { return lights_.size(); }
    size_t lights_drawn() const { return lights_drawn_; }
    // Shadowing lights (SShadowingLight) draw with their +0xD8 technique and a depth map of the
    // scene from the light. Returns the maps still to draw for the lights on screen (at most
    // `budget`), each with its view set: render them with StaticMeshScene::render_shadow before
    // render(). `scene_items` changing (a map still loading) discards the maps drawn so far.
    std::vector<SunShadow*> light_shadows_to_render(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, size_t scene_items, size_t budget);
    void set_light_shadows(bool on) { light_shadows_ = on; }
    // Off: the light records' clip planes, weights and fade distances are not applied (deferred_light
    // +0x100..+0x130 = (0, 0, 0, 1), +0x150 = 1, +0x160 = 1, as before section 3.95). For comparisons.
    void set_light_records(bool on) { light_records_ = on; }
    size_t light_shadows_used() const { return light_shadows_used_; }  // shadowed lights drawn last frame
    size_t volumetric_lights() const { return vol_lights_.size(); }
    size_t volumetrics_drawn() const { return vol_drawn_; }
    bool ssr_drawn() const { return ssr_drawn_; }             // reflections were traced last frame
    bool light_grid_drawn() const { return grid_ready_; }
    // Half-resolution SSR buffers (UNORDERED_ACCESS between frames): RGBA16F radiance (w: 0 no ray,
    // 1 fallback, 2 hit) and R32 hit distance (float bits).
    ID3D12Resource* ssr_source() const { return ssr_source_.Get(); }
    ID3D12Resource* ssr_radiance() const { return ssr_radiance_.Get(); }
    ID3D12Resource* ssr_hit_distance() const { return ssr_dir_.Get(); }
    static M4 inverse_matrix(const M4& m);  // general 4x4 inverse (the SSR pixel <-> world transforms)
    void drop_ssr_history() { ssr_history_valid_ = false; }  // the next frame's image is unrelated (map switch)
    ID3D12Resource* specular_ibl() const { return env_.Get(); }  // RGBA16F, PIXEL_SHADER_RESOURCE between frames
    ID3D12Resource* volumetric_layer(int i) const { return vol_blurred_[i ? 1 : 0].Get(); }  // blurred far / near shafts: RGBA16F, quarter resolution, PIXEL_SHADER_RESOURCE
    ID3D12Resource* volumetric_depth() const { return vol_minmax_.Get(); }  // RGBA32F max/min/mean/coverage, PIXEL_SHADER_RESOURCE
    size_t volumetric_shadows_used() const { return vol_shadows_used_; }  // shafts drawn with a shadow map last frame
    const std::vector<std::string>& light_notes() const { return light_notes_; }
    ID3D12Resource* light_buffer(int i) const { return i ? light_specular_.Get() : light_diffuse_.Get(); }  // RGBA16F, PIXEL_SHADER_RESOURCE

    // A map's reflection / ambient probe volumes (load_map_cubemaps), drawn each frame with the
    // engine's cubemap_apply pipelines into the diffuse light buffer and the specular IBL buffer,
    // before the local lights. Volumes that cannot be loaded are left out and listed.
    void set_cubemaps(PackageManager& pm, Gpu& gpu, std::span<const MapCubemap* const> cubemaps, bool diagnostic_csg = false);
    void clear_cubemaps(Gpu& gpu);
    bool set_entity_preview(PackageManager& pm, Gpu& gpu);
    size_t filtered_cube_count() const { return filtered_cubes_.size(); }
    size_t cubemap_count() const { return cubemaps_.size(); }
    size_t cubemaps_drawn() const { return cubemaps_drawn_; }
    size_t cubemaps_failed() const { return cubemaps_failed_; }
    size_t cubemaps_requested() const { return cubemaps_requested_; }
    size_t probe_texture_count() const { return probe_textures_.size(); }
    size_t probe_shape_count() const { return probe_shapes_.size(); }
    const std::vector<std::string>& cubemap_notes() const { return cubemap_notes_; }

    // Weather: global channel values by index (world channel controllers), over the defaults.
    void set_global_channels(const std::map<uint32_t, float>& values);

    // A map's atmosphere (load_map_atmosphere): each frame the engine's sky_lookup_generate_near /
    // _far pipelines render the in-scatter seen along every view ray for the time of day, the
    // shading pass fogs with them and the `sky` pipeline draws the background. nullptr clears.
    void set_atmosphere(PackageManager& pm, Gpu& gpu, const MapAtmosphere* atmosphere);
    void clear_atmosphere() {
        atm_ = {};
        atm_notes_.clear();
        hemi_inputs_.reset();
        grid_key_.reset();
        ae_valid_ = taa_valid_ = false;
        ae_fence_ = {};
    }
    // The map seen from straight above (StaticMeshScene::render_rain_occlusion) for the sky
    // ambient's overhead occlusion; must outlive the frames rendered with it. nullptr clears.
    void set_overhead_occlusion(const RainOcclusion* top) { overhead_ = top; }
    bool has_atmosphere() const { return atm_.valid; }
    const std::vector<std::string>& atmosphere_notes() const { return atm_notes_; }
    float default_time_of_day() const { return atm_.valid ? atm_.data.default_time : 0.5f; }
    float time_of_day(const LightingParams& p) const { return atm_.valid && p.time_of_day >= 0 ? p.time_of_day : default_time_of_day(); }
    // The global channels for these parameters: the render-globals defaults, then (with the map's
    // atmosphere on) the map environment's channel expressions at the time of day, then the
    // weather. Runs every frame in render(); call it first to give the materials the same values.
    const std::vector<float4>& update_global_channels(const LightingParams& p);
    // The world (script) channels of the last update_global_channels, for the materials.
    const std::map<uint32_t, float>& world_channels() const { return ext_.world_channels; }
    size_t environment_channels() const { return env_channels_; }  // channels the map environment set
    // The sun direction (toward the sun) for these parameters: the map's sun path with an atmosphere.
    V3 sun_toward(const LightingParams& p) const { return sun_direction(p, atm_.valid ? &atm_.data : nullptr); }

    ExternStore& externs() { return ext_; }
    const ExternStore& externs() const { return ext_; }
    const std::vector<std::string>& notes() const { return shading_.notes; }
    TagHash shading_technique() const { return shading_.tag(); }
    ID3D12Resource* shadow_mask() const { return shadow_mask_.Get(); }  // R8, PIXEL_SHADER_RESOURCE
    ID3D12Resource* ambient_occlusion() const { return ao_.Get(); }  // RGBA16F, PIXEL_SHADER_RESOURCE (x = AO)
    ID3D12Resource* material_occlusion() const { return material_ao_.Get(); }  // R8, copied from G-buffer
    ID3D12Resource* linear_depth() const { return linear_depth_.Get(); }
    struct TransparentReflection {
        ID3D12Resource* cube = nullptr;
        std::array<float4, 37> constants{};
    };
    // Probe index zero means sky. Other indices belong to the current map or preview.
    size_t transparent_probe_at(V3 position) const;
    uint64_t cubemap_generation() const { return cubemap_generation_; }
    TransparentReflection transparent_reflection(size_t probe) const;
    ID3D12Resource* distortion_view_depth() const { return distortion_view_depth_.Get(); }
    // The exposure scale of the last frame (cb13 [1].x): auto exposure x LightingParams::exposure.
    // Material passes (emissive) read the same value: StaticMeshScene::set_exposure.
    float exposure() const { return exposure_; }
    float scene_luminance() const { return ae_luminance_; }  // auto exposure's measured average (unexposed)
    bool post_ready() const { return post_tonemap_.loaded() && grading_fill_.loaded() && grading_convert_.loaded(); }
    ID3D12Resource* grading_lut() const { return grading_lut_.Get(); }
    // Temporal anti-aliasing: this frame's view, jittered by a sub-pixel offset when resolve_taa
    // runs, with the previous frame's view (same jitter) for the G-buffer's motion vectors. Call
    // once per rendered frame and use the result for the G-buffer and render().
    ViewMatrices temporal_view(const View& v, const LightingParams& p);
    const std::string& dlaa_status() const { return dlss_.status(); }
    bool temporal_aa_on(const LightingParams& p) const;

private:
    void resize(Gpu& gpu, uint32_t w, uint32_t h);
    void fill_externs(GBuffer& gb, const ViewMatrices& vm, const LightingParams& p, bool shadowed);

    RenderGlobals globals_;
    Scope frame_scope_, view_scope_;
    ExternStore ext_;
    std::map<uint32_t, float> global_overrides_;
    size_t env_channels_ = 0;
    int day_night_slot_ = -1;  // world channel registry slot of day_night_state
    int time_of_day_slot_ = -1;
    GamePipeline shading_, final_combine_;
    // SSAO: the engine's hdao pass over the depth buffer, read by the shading pass (extern 93).
    GamePipeline hdao_, apply_ssao_, copy_material_ao_;
    ExternStore hdao_ext_;
    ExternStore atmo_lut_;  // the atmosphere density pass's inputs
    static constexpr UINT kMaterialAoRtv = 13;
    ComPtr<ID3D12Resource> ao_, material_ao_, linear_depth_;
    ComPtr<ID3D12PipelineState> linear_depth_pso_;
    void render_ssao(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes);
    void render_linear_depth(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);

    // Volumetrics (docs/PROGRESS.md 3.3): each local light's volumetrics technique is marched at
    // quarter resolution into a near / far layer pair (plus the transparents' 8 intensity slices),
    // then volumetrics_blur_results and volumetrics_upres_1 add the shafts to the HDR target.
    void render_volumetrics(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                            D3D12_CPU_DESCRIPTOR_HANDLE hdr, const ScopeBufferFn& scopes, bool transparent_inputs,
                            bool use_fog_density, float strength);
    void pack_shaft_fog(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm);
    Scope rigid_scope_, transparent_scope_, transparent_adv_scope_;
    GamePipeline vol_downsample_, vol_blur_, vol_upres_, vol_assemble_;
    struct VolumetricLight {
        uint32_t pipeline = 0;  // pair: both windings
        std::shared_ptr<const Technique> technique;
        size_t light = 0;       // index into lights_
        // Shadowing lights: the +0xE0 technique, which looks the march samples up in the light's depth map.
        uint32_t shadow_pipeline = UINT32_MAX;
        std::shared_ptr<const Technique> shadow_technique;
    };
    std::vector<std::unique_ptr<GamePipeline>> vol_pipelines_;
    std::vector<VolumetricLight> vol_lights_;
    size_t vol_drawn_ = 0, vol_shadows_used_ = 0;
    // Quarter resolution: min/max depth, the light pass targets (far, near, slices 0-3, 4-7), the
    // blurred far / near layers.
    ComPtr<ID3D12Resource> vol_minmax_, vol_layer_[4], vol_blurred_[2];
    ComPtr<ID3D12Resource> vol_intensity_;
    std::array<ComPtr<ID3D12Resource>, 2> shaft_density_;
    ComPtr<ID3D12RootSignature> shaft_density_root_;
    ComPtr<ID3D12PipelineState> shaft_density_pso_;
    static constexpr UINT kShaftDensityRtv = 15;
    ComPtr<ID3D12DescriptorHeap> vol_rtv_heap_;
    uint32_t vw_ = 0, vh_ = 0;
    ComPtr<ID3D12Resource> tint_lookup_, lobe_lookup_, lobe3d_lookup_, iridescence_lookup_, atmosphere_noise_;
    ComPtr<ID3D12Resource> no_atmosphere_;  // 1x1x2 (1, 1, 0, 0): clear air while no map atmosphere is loaded
    struct Atmosphere {
        GamePipeline lens_glare, lens_upsample;
        MapAtmosphere data;
        std::array<ComPtr<ID3D12Resource>, 4> lookups;
        ComPtr<ID3D12Resource> density_lut;
        bool valid = false;
    } atm_;
    std::vector<std::string> atm_notes_;
    GamePipeline sky_near_, sky_far_, sky_, atmo_density_;
    static constexpr uint32_t kAtmoDensitySize = 256;  // engine resolution unknown (estimate)
    ComPtr<ID3D12Resource> atmo_density_rt_;  // sqrt(distance/50km) x elevation
    void render_atmosphere_density(Gpu& gpu, ID3D12GraphicsCommandList* cl);
    ComPtr<ID3D12Resource> sky_near_rt_, sky_far_rt_;  // RGBA16F, full resolution: ext 7 +0xF0 / +0xE0
    void fill_atmosphere(const LightingParams& p);
    void render_sky_lookups(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& scopes);
    ComPtr<ID3D12Resource> unshadowed_;  // 1x1 white: "no shadow" while no shadow pass exists
    ComPtr<ID3D12Resource> hdr_;

public:
    ID3D12Resource* hdr_target() const { return hdr_.Get(); }  // diagnostics (PIXEL_SHADER_RESOURCE after render)

private:
    GamePipeline distortion_depth_downsample_, distortion_resolve_;
    ComPtr<ID3D12Resource> distortion_, distortion_depth_, distortion_view_depth_, distorted_hdr_;
    ComPtr<ID3D12DescriptorHeap> distortion_dsv_;
    void render_distortions(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb,
                            const DistortionFn& draw, const ScopeBufferFn& scopes);
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

    void draw_lights(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes);
    // A light volume draw of this frame: its extern writes (recorded in order, so each sees the
    // values every earlier draw left), and its TFX evaluated ahead on the worker pool.
    struct VolumeDraw {
        size_t light = 0;
        bool shadowed = false;
        GamePipeline* pipeline = nullptr;
        const Technique* technique = nullptr;
        std::vector<std::pair<uint32_t, float>> overlay;
        GamePipeline::StageConstants constants;
        std::string error;
        // The volumetrics pass's per-draw object scopes (rigid, transparent, transparent_advanced
        // x VS / PS), evaluated ahead with the draw's writes; an error is raised only if bound.
        std::array<std::vector<float4>, 6> scopes;
        std::array<std::string, 6> scope_errors;
    };
    std::vector<VolumeDraw> volume_draws_;
    ExternOverlay volume_overlay_;
    void evaluate_volume_draws(size_t count, std::span<const Scope* const> scopes = {});
    struct LocalLight {
        uint32_t pipeline = 0;
        std::shared_ptr<const Technique> technique;
        MapLight light;
        // Shadowing lights: the +0xD8 technique and this light's depth map (drawn by the caller).
        uint32_t shadow_pipeline = UINT32_MAX;
        std::shared_ptr<const Technique> shadow_technique;
        std::unique_ptr<SunShadow> shadow;
        bool shadow_readable = false;  // depth map in PIXEL_SHADER_RESOURCE (else DEPTH_WRITE)
        M4 world_volume, world_box, world_to_box;
        bool mirrored = false;
        // Registers of the light PS read for the transparent light grid (-1: not found):
        // falloff (a, b) of saturate(a * d + b)^2, the colour at falloff - 1, the spot cone
        // (scale, bias, exponent) at falloff - 3, and the diffuse weight (+0x150 product).
        int grid_falloff = -1, grid_weight = -1;
    };
    // One local light as light_sample_points_instance_grid reads it (extern 68).
    struct GridLight {
        std::array<float, 3> position{}, axis{}, colour{};
        float start = 0, end = 0;               // +0xA0.xy: full light up to start, none past end
        float inner = -1, outer = -1;           // +0xA0.zw: cone angles in radians, -1 for a point light
        float intensity = 0;                    // +0x80.w
    };
    template <class Sink> void write_light(Sink& sink, const LocalLight& ll, const ViewMatrices& vm, bool shadowed) const;
    std::vector<GridLight> grid_lights(const ViewMatrices& vm, const float4& center, float radius);
    size_t grid_light_misses_ = 0, grid_light_count_ = 0;
    size_t light_shadow_items_ = 0, light_shadows_used_ = 0;
    // Hands a light's depth map to the deferred_shadow extern (6) for its +0xD8 / +0xE0 technique.
    void bind_light_shadow(ID3D12GraphicsCommandList* cl, LocalLight& ll, const ViewMatrices& vm);
    bool light_shadows_ = true;
    bool light_records_ = true;
    // The record's light / shaft fade at the camera (1 with light_records_ off).
    float light_fade(const MapLight& l, const V3& eye) const;
    float shaft_fade(const MapLight& l, const V3& eye) const;
    std::vector<std::unique_ptr<GamePipeline>> light_pipelines_;  // two per shader pair: both windings
    std::vector<LocalLight> lights_;
    std::vector<std::string> light_notes_;
    size_t lights_drawn_ = 0;
    ComPtr<ID3D12Resource> light_diffuse_, light_specular_;  // Deferred +0xC0 / +0xC8
    ComPtr<ID3D12Resource> cube_;
    D3D12_VERTEX_BUFFER_VIEW cube_vbv_{};
    D3D12_INDEX_BUFFER_VIEW cube_ibv_{};
    void make_cube(Gpu& gpu);

    void draw_cubemaps(Gpu& gpu, ID3D12GraphicsCommandList* cl, const GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes, bool distance_fade);
    struct CubemapVolume {
        uint32_t pipeline = 0;  // index of the variant's pipeline pair (both windings)
        MapCubemap volume;
        ComPtr<ID3D12Resource> specular, alpha, diffuse;
        ComPtr<ID3D12Resource> shape;
        D3D12_VERTEX_BUFFER_VIEW shape_vbv{};
        std::optional<M4> world_to_local;
        uint32_t shape_vertices = 0;
        float max_mip = 0;      // the specular cube's last mip
        bool squared = false;   // specular cube stored gamma-2 (UNORM): the PS squares its samples
    };
    // The engine keeps two mips of a specular cube on disc and GGX-filters the rest at load with
    // its cubemap_face_filter_ggx pipeline; this does the same for the cubes not filtered yet.
    void filter_specular_cubes(PackageManager& pm, Gpu& gpu, std::span<const uint32_t> tags);
    std::unique_ptr<GamePipeline> cube_filter_;
    std::unordered_map<uint32_t, ComPtr<ID3D12Resource>> filtered_cubes_;  // by specular tag
    std::vector<std::unique_ptr<GamePipeline>> cubemap_pipelines_;
    std::unique_ptr<GamePipeline> cubemap_stencil_;
    bool cubemap_csg_diagnostic_ = false;
    std::map<std::string, uint32_t> cubemap_variants_;  // pipeline name -> pipeline pair
    std::vector<CubemapVolume> cubemaps_;
    std::vector<std::string> cubemap_notes_;
    size_t cubemaps_drawn_ = 0;
    size_t cubemaps_failed_ = 0;
    size_t cubemaps_requested_ = 0;
    uint64_t cubemap_generation_ = 0;
    std::unordered_map<uint32_t, ComPtr<ID3D12Resource>> probe_textures_;  // current selection's resources only
    std::unordered_map<uint32_t, ComPtr<ID3D12Resource>> probe_shapes_;    // keep in-flight boundary buffers alive across map changes

    // Post-processing (docs/PROGRESS.md 3.5, src/render/game_post.cpp): the bloom pyramid
    // (bloom_initial_downsample, downsample_block_2x2, gaussian_10 blurs, weighted_add), auto
    // exposure (autoexposure_sample_columns read back to the CPU) and the tonemap / grading pass
    // screen_area_global_lut3d into the 8-bit output.
    void load_post(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void resize_post(Gpu& gpu, uint32_t w, uint32_t h);
    void release_post(Gpu& gpu);
    void generate_grading_lut(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& scopes);
    void update_exposure(Gpu& gpu, const LightingParams& p);
    void render_post(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const LightingParams& p,
                     const ScopeBufferFn& scopes, bool distorted);
    // Cinematic depth of field with the engine's v2_dof_cinematic_* passes (docs/PROGRESS.md 3.96):
    // returns the blurred HDR image, or `image` when the passes are missing.
    ID3D12Resource* render_dof(Gpu& gpu, ID3D12GraphicsCommandList* cl, ID3D12Resource* image, const LightingParams& p,
                               const ScopeBufferFn& scopes);
    bool dof_ready() const;
    // The engine's draw_vignette into the overlay the tonemap multiplies by (docs/PROGRESS.md 3.99).
    ID3D12Resource* render_vignette(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& scopes);
    GamePipeline vignette_;
    ComPtr<ID3D12Resource> screen_overlay_;
    GamePipeline output_gamma_, output_sharpen_;
    Dlss dlss_;
    GamePipeline dlaa_copy_depth_, dlaa_copy_motion_;
    ComPtr<ID3D12Resource> dlaa_depth_, dlaa_motion_;
    float dlaa_jitter_x_ = 0, dlaa_jitter_y_ = 0;
    bool dlaa_reset_ = true;
    ComPtr<ID3D12Resource> output_sdr_;
    // prepare (full-res CoC), 12-texel tile max (horizontal, vertical), neighbourhood max,
    // half-res downsample (colour + CoC), gather, fill blur, full-res combine.
    GamePipeline dof_prepare_, dof_tile_h_, dof_tile_v_, dof_neighbor_, dof_down_, dof_apply_, dof_blur_, dof_combine_;
    ComPtr<ID3D12Resource> dof_coc_, dof_tile_h_rt_, dof_tile_v_rt_, dof_neighbor_rt_, dof_half_, dof_half_coc_, dof_apply_rt_,
        dof_blur_rt_, dof_out_;
    uint32_t dof_tw_ = 0, dof_th_ = 0, dof_hw_ = 0, dof_hh_ = 0;
    GamePipeline post_tonemap_, post_tonemap_luma_, post_pack_luma_, fxaa_, bloom_initial_, bloom_down_, bloom_down_finite_, bloom_blur_h_, bloom_blur_v_, bloom_add_, ae_columns_;
    GamePipeline bloom_line_down_, bloom_line_blur_, bloom_line_combine_;
    GamePipeline grading_fill_, grading_convert_;
    static constexpr int kBloomLevels = 4;  // 1/3, 1/6, 1/12, 1/24
    struct BloomLevel {
        ComPtr<ID3D12Resource> down, blur, sum;  // downsampled, horizontal pass, result
        uint32_t w = 0, h = 0;
    };
    std::array<BloomLevel, kBloomLevels> bloom_;
    std::array<ComPtr<ID3D12Resource>, 3> bloom_line_;  // half width, quarter width, filter scratch
    ComPtr<ID3D12Resource> post_ldr_;  // the tonemapped colour with its luma in alpha (the fxaa input)
    ComPtr<ID3D12Resource> ae_columns_rt_, grading_strip_, grading_lut_, black_, lens_blur_;
    ComPtr<ID3D12DescriptorHeap> post_rtv_heap_;
    // resolve_taa: two RGBA16F history targets (written / read alternately); copy_texture_bilinear
    // puts the resolved frame into the 8-bit output.
    GamePipeline taa_, taa_copy_, taa_seed_;
    std::array<ComPtr<ID3D12Resource>, 2> taa_history_;
    uint32_t taa_current_ = 0;
    uint64_t taa_index_ = 0;
    bool taa_valid_ = false;       // the history holds the previous frame
    float taa_history_exposure_ = 1;
    View taa_prev_view_;
    bool taa_prev_valid_ = false;
    std::array<ComPtr<ID3D12Resource>, 3> ae_readback_;
    std::array<uint64_t, 3> ae_fence_{};
    uint64_t ae_frame_ = 0;
    float exposure_ = 1, ae_luminance_ = 0, ae_auto_ = 1;  // ae_auto_: the adapted auto exposure
    bool ae_valid_ = false;

    // Sky ambient (docs/PROGRESS.md 3.7, src/render/game_sky_ambient.cpp): the atmosphere drawn
    // into six cube faces (hemisphere_sky_color_generate), folded into a mirror-ball map
    // (sky_hemisphere_copy_frustum) with a mip chain (sky_hemisphere_downsample_filter_ggx) at
    // Deferred +0xF8; cubemap_apply_sky_copy_ao then lights every pixel with it into the diffuse
    // light buffer and the specular IBL buffer, under the cubemap volumes.
    void load_sky_ambient(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void release_sky_ambient(Gpu& gpu);
    bool sky_ambient_on(const LightingParams& p) const;
    void render_sky_hemisphere(Gpu& gpu, ID3D12GraphicsCommandList* cl);
    GamePipeline hemi_sky_, hemi_copy_, hemi_tint_, hemi_down_, sky_apply_;
    // Overhead occlusion: extern 82 +0x08, the variance map of the top-down depth
    // (sample_and_blur_variance_map_9x9), read by cubemap_apply_sky_copy_ao_with_overhead_occlusion.
    bool overhead_occlusion_on(const LightingParams& p) const;
    void prepare_overhead_occlusion(Gpu& gpu, ID3D12GraphicsCommandList* cl);
    GamePipeline sky_apply_overhead_, overhead_vsm_;
    ExternStore overhead_ext_;
    ComPtr<ID3D12Resource> overhead_vsm_tex_;  // R32G32F (mean depth, mean depth^2), 1/4 of the source
    const RainOcclusion* overhead_ = nullptr;
    size_t overhead_items_ = 0;
    float4 overhead_scale_{}, overhead_offset_{};
    // Froxel atmosphere volume (docs/PROGRESS.md 3.2.1, src/render/game_froxels.cpp):
    // cascade_raymarch_atmosphere_volume writes per froxel (density, 0.5, sun visibility, covered)
    // and cascade_raymarch_atmosphere_volume_inscatter integrates it front to back into
    // (transmittance, fog transmittance, in-scatter): extern 7 +0x108 / extern 40 +0x78.
    void load_froxels(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void load_light_grid(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void release_light_grid(Gpu& gpu);
    void render_light_grid(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm,
                           const ScopeBufferFn& scopes, bool sky, bool overhead, const CubemapVolume* preview,
                           const SunShadow* shadow);
    // light_sample_points_shadow_compute: the sun visibility of each grid cell from the sun shadow
    // map (extern 6), read by the transparent shaders as extern 40 +0x40.
    // Render-global channel slot -> value chosen for the map's lights (LightingParams::assume_lights_on).
    std::map<size_t, float> light_state_;
    void choose_light_state();
    void prepare_transparent_reflection(const ViewMatrices& vm, const CubemapVolume* preview);
    bool transparent_probe_preview_ = false;
    void render_grid_sun(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ScopeBufferFn& grid_scopes, const SunShadow& shadow);
    GamePipeline grid_sky_, grid_probe_, grid_blend_, grid_encode_, grid_points_, grid_sun_;
    std::array<ComPtr<ID3D12Resource>, 2> grid_sun_vis_;  // [0] result, [1] the "previous" input, all 1
    bool grid_sun_cleared_ = false;
    ExternStore grid_ext_;
    std::array<ComPtr<ID3D12Resource>, 3> grid_basis_, grid_sh_;
    ComPtr<ID3D12DescriptorHeap> grid_clear_heap_;
    std::optional<std::array<float4, 10>> grid_key_;
    bool grid_ready_ = false;
    // Screen-space reflections (game_ssr.cpp, docs/PROGRESS.md 3.12).
    void load_ssr(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void release_ssr(Gpu& gpu);
    void resize_ssr(Gpu& gpu);
    bool render_ssr(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p);
    void keep_ssr_history(ID3D12GraphicsCommandList* cl, const ViewMatrices& vm);  // the lit image, for the next frame's rays
    GamePipeline hiz_from_texture_, hiz_from_target_, ssr_source_gen_, ssr_init_, ssr_extract_, ssr_args_, ssr_trace_,
        ssr_resolve_, ssr_hole_, ssr_hblur_down_, ssr_hblur_, ssr_vblur_, ssr_apply_;
    ComPtr<ID3D12Resource> hiz_, ssr_source_, ssr_radiance_, ssr_dir_, ssr_color_, ssr_weight_, ssr_args_buf_, ssr_rays_buf_,
        ssr_stencil_, ssr_history_;
    ComPtr<ID3D12CommandSignature> dispatch_sig_;
    uint32_t ssr_w_ = 0, ssr_h_ = 0, ssr_mips_ = 0, hiz_mips_ = 0;
    bool ssr_loaded_ = false, ssr_history_valid_ = false, ssr_drawn_ = false;
    M4 ssr_history_vp_;
    float ssr_history_exposure_ = 1;
    GamePipeline ssr_scale_history_;
    std::unique_ptr<ExternStore> ssr_copy_inputs_;
    void release_froxels(Gpu& gpu);
    bool froxels_on(const LightingParams& p) const;
    void render_froxels(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, const LightingParams& p, const SunShadow* shadow);
    GamePipeline froxel_density_, froxel_inscatter_;
    GamePipeline froxel_density_no_sun_, froxel_inscatter_no_sun_;
    GamePipeline froxel_density_noise_, froxel_density_noise_no_sun_;
    std::array<ComPtr<ID3D12Resource>, 2> froxel_density_vol_;
    ComPtr<ID3D12Resource> froxel_vol_;  // RGBA16F 3D
    uint32_t froxel_w_ = 0, froxel_h_ = 0;
    uint32_t froxel_density_index_ = 0;
    bool froxel_history_valid_ = false;
    struct FroxelHistoryView {
        M4 projection;
        V3 eye, forward;
        float near_plane, time, time_of_day, day_night_state, fog_density_scale;
        bool noise, shadowed, interior, authored_interior_fog, environment;
    } froxel_history_view_{};
    // Engine values unknown (estimates): one cascade of 64 slices, 8x8 pixels per froxel, slice
    // depth = near + (far - near) t^(1 / curve).
    static constexpr uint32_t kFroxelDepth = 96, kFroxelTile = 8;
    static constexpr float kFroxelNear = 0.5f, kFroxelFar = 256.0f, kFroxelCurve = 0.5f;
    static constexpr uint32_t kHemiSize = 256, kHemiMips = 9, kHemiFace = 128;
    ComPtr<ID3D12Resource> hemi_, hemi_face_, hemi_untinted_;
    std::optional<std::array<float4, 7>> hemi_inputs_;
    ComPtr<ID3D12DescriptorHeap> hemi_rtv_heap_;  // hemisphere mips 0..8, then the face
};

} // namespace nim
