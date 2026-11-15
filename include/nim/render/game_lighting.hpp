#pragma once
// Running the engine's own full-screen pipelines (render_globals: global_lighting_and_shading,
// final_combine, ...) the way the game does: each technique's TFX bytecode reads "externs"
// (engine-owned blocks of per-frame state such as the view, the G-buffer textures and the sun),
// and the shared cbuffers (cb12 view, cb13 frame) come from the render globals' scopes.
#include "nim/content/map.hpp"
#include "nim/render/scene.hpp"

#include <functional>
#include <map>
#include <unordered_map>

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

    // Replace a complete block, including missing-value policy and texture views. Removes stale views.
    void copy_block_from(uint8_t id, const ExternStore& source);
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
    std::map<uint32_t, float> world_channels; // script registry slots, never render-globals indices
    void set_channel(std::string_view name, float4 v);
    TfxEnvironment environment() const;
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
    // Light volumes: the pipeline is shared by every technique with the same shaders; each draw
    // brings its own technique's TFX, constants and cbuffer (textures and samplers must match), or
    // the pipeline's own with `materials` null. `ps_textures`: views the engine binds itself
    // (slots no TFX fills, e.g. a cubemap volume's cubes). With an empty ibv, `indices` is the
    // vertex count of an authored non-indexed triangle list.
    void draw_volume(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                     const Technique* materials, const D3D12_VERTEX_BUFFER_VIEW& vbv, const D3D12_INDEX_BUFFER_VIEW& ibv,
                     uint32_t indices, std::span<const std::pair<uint32_t, ID3D12Resource*>> ps_textures = {});
    void dispatch(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                  uint32_t groups_x, uint32_t groups_y, uint32_t groups_z);

    std::vector<std::string> notes;  // binding diagnostics, for the UI / nimshade
    const std::vector<float4>& stage_constants(size_t i) const { return stages_.at(i).constants; }  // last draw's cbuffer

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
    };
    void execute(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ExternStore& ext, const ScopeBufferFn& scopes,
                 bool validated, uint32_t groups_x, uint32_t groups_y, uint32_t groups_z, const Geometry* geometry = nullptr);
    bool compute_ = false;
    uint32_t cbuffer_floor_ = 256;
    TagHash tag_;
    std::vector<Stage> stages_;
    ComPtr<ID3D12RootSignature> root_sig_;
    ComPtr<ID3D12PipelineState> pso_;
};

// Runs a scope's bytecode for one stage into a cbuffer (cbuffer_init patched by the program).
std::vector<float4> evaluate_scope(const Scope& scope, ScopeStage stage, const ExternStore& ext);

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
    // `transparents` runs after the shading pass with the HDR target's RTV (the G-buffer depth is
    // then in DEPTH_READ, bound through gb.dsv_ro).
    using TransparentFn = std::function<void(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE hdr)>;
    // `shadow`: a sun shadow map rendered this frame (StaticMeshScene::render_shadow), resolved
    // into the shading pass's sun shadow mask; without one the mask is white (unshadowed).
    void render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const LightingParams& p,
                const TransparentFn& transparents = {}, SunShadow* shadow = nullptr);
    void release(Gpu& gpu);

    // A map's local lights (load_map_lights), drawn each frame with their own lighting-apply
    // techniques into the light buffers the shading pass reads. Lights whose technique cannot be
    // loaded are left out and listed in light_notes().
    void set_lights(PackageManager& pm, Gpu& gpu, std::span<const MapLight* const> lights);
    void clear_lights() { lights_.clear(); vol_lights_.clear(); }
    size_t light_count() const { return lights_.size(); }
    size_t lights_drawn() const { return lights_drawn_; }
    size_t volumetric_lights() const { return vol_lights_.size(); }
    size_t volumetrics_drawn() const { return vol_drawn_; }
    const std::vector<std::string>& light_notes() const { return light_notes_; }
    ID3D12Resource* light_buffer(int i) const { return i ? light_specular_.Get() : light_diffuse_.Get(); }  // RGBA16F, PIXEL_SHADER_RESOURCE

    // A map's reflection / ambient probe volumes (load_map_cubemaps), drawn each frame with the
    // engine's cubemap_apply pipelines into the diffuse light buffer and the specular IBL buffer,
    // before the local lights. Volumes that cannot be loaded are left out and listed.
    void set_cubemaps(PackageManager& pm, Gpu& gpu, std::span<const MapCubemap* const> cubemaps, bool diagnostic_csg = false);
    void clear_cubemaps(Gpu& gpu);
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
    void clear_atmosphere() { atm_ = {}; atm_notes_.clear(); }
    // The map seen from straight above (StaticMeshScene::render_rain_occlusion) for the sky
    // ambient's overhead occlusion; must outlive the frames rendered with it. nullptr clears.
    void set_overhead_occlusion(const RainOcclusion* top) { overhead_ = top; }
    bool has_atmosphere() const { return atm_.valid; }
    const std::vector<std::string>& atmosphere_notes() const { return atm_notes_; }
    float default_time_of_day() const { return atm_.valid ? atm_.data.default_time : 0.5f; }
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
    const std::vector<std::string>& notes() const { return shading_.notes; }
    TagHash shading_technique() const { return shading_.tag(); }
    ID3D12Resource* shadow_mask() const { return shadow_mask_.Get(); }  // R8, PIXEL_SHADER_RESOURCE
    ID3D12Resource* ambient_occlusion() const { return ao_.Get(); }  // RGBA16F, PIXEL_SHADER_RESOURCE (x = AO)
    ID3D12Resource* linear_depth() const { return linear_depth_.Get(); }
    // The exposure scale of the last frame (cb13 [1].x): auto exposure x LightingParams::exposure.
    // Material passes (emissive) read the same value: StaticMeshScene::set_exposure.
    float exposure() const { return exposure_; }
    float scene_luminance() const { return ae_luminance_; }  // auto exposure's measured average (unexposed)
    bool post_ready() const { return post_tonemap_.loaded(); }
    // Temporal anti-aliasing: this frame's view, jittered by a sub-pixel offset when resolve_taa
    // runs, with the previous frame's view (same jitter) for the G-buffer's motion vectors. Call
    // once per rendered frame and use the result for the G-buffer and render().
    ViewMatrices temporal_view(const View& v, const LightingParams& p);
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
    GamePipeline shading_, final_combine_;
    // SSAO: the engine's hdao pass over the depth buffer, read by the shading pass (extern 93).
    GamePipeline hdao_, apply_ssao_;
    ExternStore hdao_ext_;
    ComPtr<ID3D12Resource> ao_, linear_depth_;
    ComPtr<ID3D12PipelineState> linear_depth_pso_;
    void render_ssao(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm, const ScopeBufferFn& scopes);
    void render_linear_depth(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm);

    // Volumetrics (docs/PROGRESS.md 3.3): each local light's volumetrics technique is marched at
    // quarter resolution into a near / far layer pair (plus the transparents' 8 intensity slices),
    // then volumetrics_blur_results and volumetrics_upres_1 add the shafts to the HDR target.
    void render_volumetrics(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const ViewMatrices& vm,
                            D3D12_CPU_DESCRIPTOR_HANDLE hdr, const ScopeBufferFn& scopes);
    Scope rigid_scope_, transparent_scope_, transparent_adv_scope_;
    GamePipeline vol_downsample_, vol_blur_, vol_upres_;
    struct VolumetricLight {
        uint32_t pipeline = 0;  // pair: both windings
        std::shared_ptr<const Technique> technique;
        size_t light = 0;       // index into lights_
    };
    std::vector<std::unique_ptr<GamePipeline>> vol_pipelines_;
    std::vector<VolumetricLight> vol_lights_;
    size_t vol_drawn_ = 0;
    // Quarter resolution: min/max depth, the light pass targets (far, near, slices 0-3, 4-7), the
    // blurred far / near layers.
    ComPtr<ID3D12Resource> vol_minmax_, vol_layer_[4], vol_blurred_[2];
    ComPtr<ID3D12DescriptorHeap> vol_rtv_heap_;
    uint32_t vw_ = 0, vh_ = 0;
    ComPtr<ID3D12Resource> tint_lookup_, lobe_lookup_, lobe3d_lookup_, iridescence_lookup_, atmosphere_noise_;
    ComPtr<ID3D12Resource> no_atmosphere_;  // 1x1x2 (1, 1, 0, 0): clear air while no map atmosphere is loaded
    struct Atmosphere {
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

    void draw_lights(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, const ScopeBufferFn& scopes);
    struct LocalLight {
        uint32_t pipeline = 0;
        std::shared_ptr<const Technique> technique;
        MapLight light;
    };
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
    std::unordered_map<uint32_t, ComPtr<ID3D12Resource>> probe_textures_;  // current selection's resources only
    std::unordered_map<uint32_t, ComPtr<ID3D12Resource>> probe_shapes_;    // keep in-flight boundary buffers alive across map changes

    // Post-processing (docs/PROGRESS.md 3.5, src/render/game_post.cpp): the bloom pyramid
    // (bloom_initial_downsample, downsample_block_2x2, gaussian_10 blurs, weighted_add), auto
    // exposure (autoexposure_sample_columns read back to the CPU) and the tonemap / grading pass
    // screen_area_global_lut3d into the 8-bit output.
    void load_post(PackageManager& pm, Gpu& gpu, std::span<const Scope* const> scopes);
    void resize_post(Gpu& gpu, uint32_t w, uint32_t h);
    void release_post(Gpu& gpu);
    void update_exposure(Gpu& gpu, const LightingParams& p);
    void render_post(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, const LightingParams& p, const ScopeBufferFn& scopes);
    GamePipeline post_tonemap_, post_luma_, fxaa_, bloom_initial_, bloom_down_, bloom_blur_h_, bloom_blur_v_, bloom_add_, bloom_copy_, ae_columns_;
    static constexpr int kBloomLevels = 4;  // 1/2, 1/4, 1/8, 1/16
    struct BloomLevel {
        ComPtr<ID3D12Resource> down, blur, sum;  // downsampled, horizontal pass, result
        uint32_t w = 0, h = 0;
    };
    std::array<BloomLevel, kBloomLevels> bloom_;
    ComPtr<ID3D12Resource> post_ldr_;  // the tonemapped colour with its luma in alpha (the fxaa input)
    ComPtr<ID3D12Resource> bloom_final_, ae_columns_rt_, grading_lut_, black_;
    ComPtr<ID3D12DescriptorHeap> post_rtv_heap_;
    // resolve_taa: two RGBA16F history targets (written / read alternately); copy_texture_bilinear
    // puts the resolved frame into the 8-bit output.
    GamePipeline taa_, taa_copy_;
    std::array<ComPtr<ID3D12Resource>, 2> taa_history_;
    uint32_t taa_current_ = 0;
    uint64_t taa_index_ = 0;
    bool taa_valid_ = false;       // the history holds the previous frame
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
    GamePipeline hemi_sky_, hemi_copy_, hemi_down_, sky_apply_;
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
    void release_froxels(Gpu& gpu);
    bool froxels_on(const LightingParams& p) const;
    void render_froxels(Gpu& gpu, ID3D12GraphicsCommandList* cl, const ViewMatrices& vm, const LightingParams& p, const SunShadow* shadow);
    GamePipeline froxel_density_, froxel_inscatter_;
    GamePipeline froxel_density_no_sun_, froxel_inscatter_no_sun_;
    GamePipeline froxel_density_noise_, froxel_density_noise_no_sun_;
    ComPtr<ID3D12Resource> froxel_density_vol_, froxel_vol_;  // RGBA16F 3D
    uint32_t froxel_w_ = 0, froxel_h_ = 0;
    // Engine values unknown (estimates): one cascade of 64 slices, 8x8 pixels per froxel, slice
    // depth = near + (far - near) t^(1 / curve).
    static constexpr uint32_t kFroxelDepth = 64, kFroxelTile = 8;
    static constexpr float kFroxelNear = 0.5f, kFroxelFar = 256.0f, kFroxelCurve = 0.5f;
    static constexpr uint32_t kHemiSize = 256, kHemiMips = 9, kHemiFace = 128;
    ComPtr<ID3D12Resource> hemi_, hemi_face_;  // RGBA16F: the mirror-ball map (9 mips), one cube face
    ComPtr<ID3D12DescriptorHeap> hemi_rtv_heap_;  // hemisphere mips 0..8, then the face
};

} // namespace nim
