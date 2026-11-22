#include "nim/render/particles.hpp"

#include "nim/content/tfx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace nim {

// Extern blocks of the particle techniques (TFX extern ids; mida's TfxExtern order).
namespace {
constexpr uint8_t kGeneric = 26, kParticle = 27, kParticleCompute = 83;
// Definition record counts are also used by the original shaders to advance each record.
constexpr uint32_t kMaxStateFloat4s = 32;
constexpr uint32_t kHistoryFloat4s = 3;   // per particle: the previous frame, for the draw's motion
constexpr uint32_t kMaxInstanceFloat4s = 32; // D3D12 input-assembler limit
constexpr uint32_t kMaxCapacity = 256;  // per emitter (estimate: the spawn rate itself is an estimate)
constexpr uint32_t kGroup = 64;           // [numthreads(64,1,1)] of every particle CS
constexpr uint32_t kWarmSteps = 30;       // steps through one lifetime for an emitter coming into range
constexpr uint32_t kWarmStepsPerFrame = 4;
static_assert(kWarmSteps % 2 == 0 && kWarmStepsPerFrame % 2 == 0);
using ParticleObjectConstants = std::array<float4, 32>;

void uav_barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cl->ResourceBarrier(1, &b);
}
}  // namespace

struct ParticleScene::System {
    ParticleSystem def;
    GamePipeline init, update, instance, draw;
    uint32_t instance_float4s = 0;  // the draw VS's per-instance TEXCOORD inputs
    bool screen_collision = false;
    bool failed = false;  // an original program could not be evaluated: left out from then on
};

// Runs one original dispatch / draw of `s`; a failure (an input that cannot be produced) takes
// the system out instead of the frame.
template <class F>
void ParticleScene::guard(System& s, F&& f) {
    if (s.failed) return;
    try {
        f();
    } catch (const std::exception& ex) {
        s.failed = true;
        notes_.push_back(std::format("particle system {} left out: {}", s.def.tag.str(), ex.what()));
    }
}

ParticleScene::ParticleScene() = default;
ParticleScene::~ParticleScene() = default;

const std::map<std::string, std::string>& ParticleScene::estimates() const {
    static const std::map<std::string, std::string> none;
    return ext_ ? ext_->estimates() : none;
}

bool ParticleScene::has_emitters_near(V3 eye) const {
    return std::ranges::any_of(emitters_, [&](const auto& e) {
        const V3 d = e.position - eye;
        return !systems_[e.system]->failed && dot(d, d) <= radius * radius;
    });
}

void ParticleScene::release(Gpu&) {
    systems_.clear();
    emitters_.clear();
    for (auto& s : state_) s.Reset();
    history_.Reset();
    instances_.Reset();
    object_constants_.Reset();
    total_ = 0;
    state_total_ = 0;
    instance_total_ = 0;
    active_ = 0;
    notes_.clear();
}

void ParticleScene::load(PackageManager& pm, Gpu& gpu, std::span<const MapParticle> placements) {
    release(gpu);
    ext_ = std::make_unique<ExternStore>();
    ext_initialized_ = false;
    const RenderGlobals globals = load_render_globals(pm);
    view_scope_ = std::make_unique<Scope>(load_scope(pm, globals.scope("view")));
    transparent_scope_ = std::make_unique<Scope>(load_scope(pm, globals.scope("transparent")));
    grid_scope_ = std::make_unique<Scope>(load_scope(pm, globals.scope("player_centered_cascaded_grid")));
    std::unordered_map<uint32_t, int> index;  // system tag -> systems_ index, -1 = failed
    gated_ = 0;
    for (const auto& p : placements) {
        // The exfil device's FX component plays on exfil_activation_progress. With no activation
        // state supplied the device is inactive (as its materials, scene.cpp), so they do not run.
        if (p.gate == kChannelExfilActivationProgress) {
            ++gated_;
            continue;
        }
        auto [it, fresh] = index.try_emplace(p.system.value, -1);
        if (fresh) {
            try {
                auto s = std::make_unique<System>();
                if (const char* skip = std::getenv("NIM_FX_SKIP"); skip && std::strstr(skip, p.system.str().c_str()))
                    throw std::runtime_error("skipped (NIM_FX_SKIP)");  // diagnostics: per-system cost
                s->def = load_particle_system(pm, p.system);
                if (!s->def.state_float4s || s->def.state_float4s > kMaxStateFloat4s ||
                    !s->def.instance_float4s || s->def.instance_float4s > kMaxInstanceFloat4s)
                    throw std::runtime_error("unsupported particle record widths");
                if (!(s->def.rate > 0) || !(s->def.lifetime > 0))
                    throw std::runtime_error(std::format("rate {} / lifetime {}", s->def.rate, s->def.lifetime));
                for (TagHash t : {s->def.init, s->def.update, s->def.instance})
                    if (t.is_none()) throw std::runtime_error("missing compute technique");
                GamePipeline::Options cs;
                s->init.load(pm, gpu, s->def.init, {}, cs);
                s->update.load(pm, gpu, s->def.update, {}, cs);
                const auto update = load_technique(pm, s->def.update);
                const auto program = disassemble_tfx(update.stage(ShaderStage::Compute).bytecode);
                s->screen_collision = program.contains("push_extern_texview 03 16") && program.contains("push_extern_texview 03 13") &&
                                      s->update.binds_shared_cbuffer(ShaderStage::Compute, 12);
                s->instance.load(pm, gpu, s->def.instance, {}, cs);
                const Technique draw = load_technique(pm, s->def.draw);
                GamePipeline::Options d;
                d.rtv_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                d.blend_state = draw.blend_state();
                if (d.blend_state < 0) throw std::runtime_error(std::format("{}: draw has no blend state", s->def.draw.str()));
                d.depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed depth, read only
                d.additional_rtv_formats = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, GBuffer::kFormats[3]};
                for (const auto& in : shader_inputs(load_shader_bytecode(pm, draw.stage(ShaderStage::Vertex).shader))) {
                    if (_stricmp(in.semantic.c_str(), "TEXCOORD") == 0) s->instance_float4s = std::max(s->instance_float4s, in.index + 1);
                    else  // POSITION etc.: a model drawn per particle; its geometry reference is not decoded
                        throw std::runtime_error(std::format("{}: mesh particles (draw VS reads per-vertex {}{}), model not decoded",
                                                             s->def.draw.str(), in.semantic, in.index));
                }
                if (!s->instance_float4s || s->instance_float4s > s->def.instance_float4s)
                    throw std::runtime_error(std::format("{}: draw VS inputs exceed the authored instance record", s->def.draw.str()));
                d.instance_float4s = s->instance_float4s;
                s->draw.load(pm, gpu, s->def.draw, {}, d);
                s->instance_float4s = s->def.instance_float4s;
                it->second = int(systems_.size());
                systems_.push_back(std::move(s));
            } catch (const std::exception& ex) {
                notes_.push_back(std::format("particle system {}: {}", p.system.str(), ex.what()));
            }
        }
        if (it->second < 0) continue;
        const System& s = *systems_[size_t(it->second)];
        Emitter e;
        e.system = uint32_t(it->second);
        e.rows = p.rows;
        e.position = {p.rows[3], p.rows[7], p.rows[11]};
        e.capacity = std::clamp<uint32_t>(uint32_t(std::ceil(s.def.rate * s.def.lifetime)) + 1, 1, kMaxCapacity);
        e.offset = total_;
        e.state_base = state_total_;
        e.instance_base = instance_total_;
        state_total_ += e.capacity * s.def.state_float4s;
        instance_total_ += e.capacity * s.instance_float4s;
        e.seed = uint32_t(emitters_.size()) * 7919u + 17u;
        total_ += e.capacity;
        emitters_.push_back(e);
    }
    if (emitters_.empty()) return;
    std::vector<ParticleObjectConstants> objects(emitters_.size());
    for (size_t i = 0; i < emitters_.size(); ++i) {
        const auto& p = emitters_[i].rows;
        auto& object = objects[i];
        object[0] = {p[0], p[4], p[8], 0};
        object[1] = {p[1], p[5], p[9], 0};
        object[2] = {p[2], p[6], p[10], 0};
        object[3] = {p[3], p[7], p[11], 1};
        std::copy_n(object.begin(), 4, object.begin() + 8);
    }
    object_constants_ = gpu.upload(objects.data(), objects.size() * sizeof(ParticleObjectConstants));
    const uint64_t n = total_;
    auto uav_buffer = [&](uint64_t float4s) {
        return gpu.buffer(float4s * 16, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    };
    state_[0] = uav_buffer(state_total_);
    state_[1] = uav_buffer(state_total_);
    history_ = uav_buffer(n * kHistoryFloat4s);
    instances_ = uav_buffer(instance_total_);
    // Zero the pools: a slot spawned in the last step has no history record until the next update,
    // and the draw VS reads it (garbage there made screen-sized quads).
    {
        const uint64_t bytes = std::max({uint64_t(state_total_), uint64_t(instance_total_), n * kHistoryFloat4s}) * 16;
        auto zeros = gpu.buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        zeros->Map(0, nullptr, &p);
        std::memset(p, 0, size_t(bytes));
        zeros->Unmap(0, nullptr);
        auto* cl = gpu.immediate();
        for (ID3D12Resource* r : {state_[0].Get(), state_[1].Get(), history_.Get(), instances_.Get()}) {
            Gpu::barrier(cl, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyBufferRegion(r, 0, zeros.Get(), 0, r->GetDesc().Width);
            Gpu::barrier(cl, r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        gpu.flush();
    }
    parity_ = 0;
    notes_.push_back(std::format("particles: {} systems, {} emitters, {} particle slots", systems_.size(), emitters_.size(), total_));
}

// One simulation step of the emitters in `list` (state_[parity_] -> state_[1 - parity_]): the
// original update over every used slot, then the original init over the slots spawned into.
void ParticleScene::step(Gpu& gpu, ID3D12GraphicsCommandList* cl, const std::vector<uint32_t>& list, const std::vector<float>& dts,
                         const ScopeBufferFn& scopes, std::span<const D3D12_GPU_VIRTUAL_ADDRESS> collision_views) {
    ExternStore& ext = *ext_;
    ID3D12Resource* src = state_[parity_].Get();
    ID3D12Resource* dst = state_[1 - parity_].Get();
    Gpu::barrier(cl, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const uint32_t state_elements = state_total_;
    ext.set_texture(kParticleCompute, 0x00, src, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, state_elements);
    ext.set_texture(kParticleCompute, 0xC8, dst, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, state_elements);
    ext.set_texture(kParticleCompute, 0xD8, history_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, total_ * kHistoryFloat4s);
    GamePipeline::VolumeBatch update_batch, init_batch;
    std::vector<uint32_t> spawns(list.size());
    for (size_t k = 0; k < list.size(); ++k) {
        Emitter& e = emitters_[list[k]];
        System& s = *systems_[e.system];
        const float dt = dts[k];
        e.carry += s.def.rate * dt;
        spawns[k] = std::min<uint32_t>(uint32_t(e.carry), e.capacity);
        e.carry -= float(uint32_t(e.carry));
        const uint32_t used = uint32_t(std::min<uint64_t>(e.spawned, e.capacity));
        if (!used) continue;
        ext.set(kParticle, 0x14C, dt);
        ext.set(kParticle, 0x154, 0.0f);  // history index base
        ext.set(kParticleCompute, 0x110, float(used));
        ext.set(kParticleCompute, 0xF0, float4{float(e.state_base), 0, float(e.state_base), 0});
        ext.set(kParticleCompute, 0x134, float(e.offset * kHistoryFloat4s));
        const auto local_view = collision_views[list[k]];
        guard(s, [&] {
            if (local_view) {
                const ScopeBufferFn local_scopes = [&](ShaderStage stage, uint32_t reg) {
                    return reg == 12 && stage == ShaderStage::Compute ? local_view : scopes(stage, reg);
                };
                s.update.dispatch_batch(gpu, cl, ext, local_scopes, (used + kGroup - 1) / kGroup, update_batch);
            } else s.update.dispatch_batch(gpu, cl, ext, scopes, (used + kGroup - 1) / kGroup, update_batch);
        });
    }
    uav_barrier(cl, dst);
    for (size_t k = 0; k < list.size(); ++k) {
        Emitter& e = emitters_[list[k]];
        System& s = *systems_[e.system];
        uint32_t left = spawns[k];
        while (left) {
            const uint32_t run = std::min(left, e.capacity - e.head);
            ext.set(kParticle, 0x14C, dts[k]);
            ext.set(kParticle, 0x150, float(e.spawned));  // particle id base
            ext.set(kParticle, 0x15C, float(e.seed));
            ext.set(kParticle, 0x160, 0.0f);  // transform index into particle_transforms
            ext.set(kParticleCompute, 0x110, float(run));
            ext.set(kParticleCompute, 0xF0, float4{0, 0, float(e.state_base + e.head * s.def.state_float4s), 0});
            guard(s, [&] { s.init.dispatch_batch(gpu, cl, ext, scopes, (run + kGroup - 1) / kGroup, init_batch); });
            e.head = (e.head + run) % e.capacity;
            e.spawned += run;
            left -= run;
        }
    }
    Gpu::barrier(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(cl, dst);
    uav_barrier(cl, history_.Get());
    parity_ = 1 - parity_;
}

void ParticleScene::render(Gpu& gpu, ID3D12GraphicsCommandList* cl, GBuffer& gb, D3D12_CPU_DESCRIPTOR_HANDLE hdr,
                           const ViewMatrices& vm, const ExternStore& inputs, float dt) {
    if (emitters_.empty()) return;
    ExternStore& ext = *ext_;
    // Copy only the blocks particle shaders read (10 KB vs copying the full 1 MB ExternStore).
    for (uint8_t block : {ext::Frame, ext::View, ext::Deferred, ext::Atmosphere, ext::Transparent, ext::GlobalLighting,
                          ext::ShadowMask, uint8_t(80), uint8_t(81), uint8_t(82)})
        ext.copy_block_from(block, inputs);
    ext.channels = inputs.channels;
    ext.channel_ids = inputs.channel_ids;
    ext.world_channels = inputs.world_channels;
    if (inputs.has_frame_constants()) ext.set_frame_constants(inputs.frame_constants());
    dt = std::clamp(std::isfinite(dt) ? dt : 0.0f, 0.0f, 0.1f);

    // Engine values of the particle blocks that no file holds (initialized once per load).
    if (!ext_initialized_) {
        ext_initialized_ = true;
        ext.zero_when_missing(kParticle, "particle block values with no producer found (emitter parameters): zero");
        ext.zero_when_missing(kParticleCompute, "particle compute values with no producer found: zero");
        ext.zero_when_missing(kGeneric, "generic block values with no producer found: zero");
        for (uint32_t o = 0; o < 0x100; o += 16)
            ext.estimate(kGeneric, o, float4{1, 1, 1, 1},
                         "per-instance effect parameters (tint / alpha) the pattern sequencer writes; not decoded: 1");
        ext.set(kParticle, 0x144, 1.0f);
        ext.note_estimate("extern 27 +0x144 = 1", "particle draw fade (cb0[186] of the draw VS), producer not found");
        if (gated_)
            ext.note_estimate(std::format("{} particle placements of exfil devices not run: exfil_activation_progress = 0", gated_),
                              "their component's sequence reads exfil_activation_progress; no activation state was supplied, so the device is inactive");
        ext.note_estimate("particle spawn rate = definition +0x134 per second, life = definition +0x128",
                          "the FX expression program of the definition (808081EF) is not decoded");
        ext.note_estimate("particle_transforms / particle_location_metadata: identity emitter frame",
                          "engine-filled scopes; the emitter's placement goes through rigid_model instead");
        ext.note_estimate(std::format("particles within {:.0f} m of the eye warm over one life, at most {} steps per frame", radius, kWarmStepsPerFrame),
                          "viewer choice: the engine's FX culling is not decoded");
        ext.note_estimate("particle transparent_advanced cb8: zero, [7].w = 1",
                          "engine-filled per-object scope; as for the light shafts");
    }

    // Shared cbuffers. Per-emitter: rigid_model (b1, the placement). Engine-filled scopes get
    // explicit values; the rest are the original scopes evaluated against this frame.
    auto upload = [&](std::span<const float4> v) {
        auto a = gpu.dynamic(std::max<size_t>(v.size(), 1) * sizeof(float4));
        std::memcpy(a.cpu, v.data(), v.size() * sizeof(float4));
        return a.gpu;
    };
    std::array<float4, 64> transforms{};
    transforms[32] = {0, 0, 0, 1};  // rotation of transform 0 (quaternion xyzw)
    const auto transforms_cb = upload(transforms);
    const std::array<float4, 32> location{};
    const auto location_cb = upload(location);
    std::array<float4, 37> advanced{};
    advanced[7][3] = 1;
    std::map<std::pair<ShaderStage, uint32_t>, D3D12_GPU_VIRTUAL_ADDRESS> cache;
    D3D12_GPU_VIRTUAL_ADDRESS object_cb = 0;
    auto scope_of = [&](const Scope& scope, ShaderStage stage) {
        const ScopeStage ss = stage == ShaderStage::Compute ? ScopeStage::Pixel : scope_stage(stage);
        return upload(evaluate_scope(scope, ss, ext));
    };
    ScopeBufferFn scopes = [&](ShaderStage stage, uint32_t reg) -> D3D12_GPU_VIRTUAL_ADDRESS {
        if (reg == 1) return object_cb;
        if (reg == 9) return transforms_cb;
        if (reg == 10) return location_cb;
        const auto key = std::pair{stage, reg};
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        D3D12_GPU_VIRTUAL_ADDRESS a = 0;
        const ShaderStage scope_stage_key = stage == ShaderStage::Compute ? ShaderStage::Pixel : stage;
        if (reg == 13) {
            const auto& f = inputs.frame_constants();
            a = upload(f);
        } else if (reg == 12) a = scope_of(*view_scope_, stage);
        else if (reg == 2) a = scope_of(*transparent_scope_, stage);
        else if (reg == 3) {
            if (inputs.has_scope_constants(scope_stage_key, 3)) a = upload(inputs.scope_constants(scope_stage_key, 3));
            else a = scope_of(*grid_scope_, stage);
        } else if (reg == 8) {
            if (inputs.has_scope_constants(scope_stage_key, 8)) a = upload(inputs.scope_constants(scope_stage_key, 8));
            else a = upload(advanced);
        }
        return cache[key] = a;
    };

    // Which emitters run this frame; frustum-cull emitters (generous 18 m radius) so off-screen
    // emitters beyond 24 m skip simulation, instancing, and draw calls.
    std::array<float4, 6> planes{};
    {
        auto row = [&](int r) {
            return float4{vm.vp_abs.c[0][r], vm.vp_abs.c[1][r], vm.vp_abs.c[2][r], vm.vp_abs.c[3][r]};
        };
        auto norm = [](float4 p) {
            const float l = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            return l > 1e-8f ? float4{p[0] / l, p[1] / l, p[2] / l, p[3] / l} : float4{};
        };
        const float4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        planes[0] = norm({r3[0] + r0[0], r3[1] + r0[1], r3[2] + r0[2], r3[3] + r0[3]});
        planes[1] = norm({r3[0] - r0[0], r3[1] - r0[1], r3[2] - r0[2], r3[3] - r0[3]});
        planes[2] = norm({r3[0] + r1[0], r3[1] + r1[1], r3[2] + r1[2], r3[3] + r1[3]});
        planes[3] = norm({r3[0] - r1[0], r3[1] - r1[1], r3[2] - r1[2], r3[3] - r1[3]});
        planes[4] = norm({r2[0], r2[1], r2[2], r2[3]});
        planes[5] = norm({r3[0] - r2[0], r3[1] - r2[1], r3[2] - r2[2], r3[3] - r2[3]});
    }
    auto in_frustum = [&](const V3& p, float r) {
        if (vm.ortho) return true;
        for (const auto& pl : planes)
            if (pl[0] * p.x + pl[1] * p.y + pl[2] * p.z + pl[3] < -r) return false;
        return true;
    };
    std::vector<uint32_t> live, warm, visible;
    for (uint32_t i = 0; i < emitters_.size(); ++i) {
        Emitter& e = emitters_[i];
        if (systems_[e.system]->failed) continue;
        const V3 d{e.position.x - vm.eye.x, e.position.y - vm.eye.y, e.position.z - vm.eye.z};
        const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        const bool in_range = d2 <= radius * radius;
        if (!in_range) {
            e.active = false;
            continue;
        }
        const bool vis = in_frustum(e.position, 18.0f);
        if (!vis && d2 > 24.0f * 24.0f) continue;
        if (!e.active) {
            e.active = true;
            e.head = 0;
            e.spawned = 0;
            e.carry = 0;
            e.warm_steps = kWarmSteps;
        }
        if (e.warm_steps) warm.push_back(i);
        live.push_back(i);
        if (vis) visible.push_back(i);
    }
    active_ = live.size();
    if (live.empty()) return;
    const auto system_of = [&](uint32_t i) { return emitters_[i].system; };
    std::ranges::sort(live, {}, system_of);
    std::ranges::sort(warm, {}, system_of);
    std::ranges::sort(visible, {}, system_of);
    // Collision CS projects its stored emitter-local positions directly with b12[0..3].
    // Draw VS has a separate rigid_model stage and continues to receive the normal view.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> collision_views(emitters_.size());
    std::vector<float4> collision_constants;
    for (uint32_t i : live) {
        const auto& e = emitters_[i];
        if (!systems_[e.system]->screen_collision) continue;
        if (collision_constants.empty()) {
            ext.note_estimate("particle collision View b12: emitter-local projection and camera position",
                              "the update shader projects the stored local position directly against map depth");
            collision_constants = evaluate_scope(*view_scope_, ScopeStage::Pixel, ext);
            if (collision_constants.size() < 8) break;
        }
        const auto& p = e.rows;
        M4 world;
        world.c[0] = {p[0], p[4], p[8], 0};
        world.c[1] = {p[1], p[5], p[9], 0};
        world.c[2] = {p[2], p[6], p[10], 0};
        world.c[3] = {p[3], p[7], p[11], 1};
        const auto local_to_clip = mul(vm.vp_abs, world);
        std::copy(std::begin(local_to_clip.c), std::end(local_to_clip.c), collision_constants.begin());
        const V3 eye_offset = vm.eye - e.position;
        for (size_t axis = 0; axis < 3; ++axis) {
            const V3 x{world.c[axis][0], world.c[axis][1], world.c[axis][2]};
            collision_constants[7][axis] = dot(eye_offset, x) / std::max(dot(x, x), 1e-8f);
        }
        collision_views[i] = upload(collision_constants);
    }
    // Warm only new emitters. Each pair returns to the original buffer parity.
    std::vector<float> dts;
    for (uint32_t pair = 0; dt > 0 && pair < kWarmStepsPerFrame / 2 && !warm.empty(); ++pair) {
        dts.clear();
        for (uint32_t i : warm) {
            Emitter& e = emitters_[i];
            e.warm_steps -= 2;
            dts.push_back(systems_[e.system]->def.lifetime / float(kWarmSteps));
        }
        step(gpu, cl, warm, dts, scopes, collision_views);
        step(gpu, cl, warm, dts, scopes, collision_views);
        std::erase_if(warm, [&](uint32_t i) { return emitters_[i].warm_steps == 0; });
    }
    if (dt > 0) step(gpu, cl, live, std::vector<float>(live.size(), dt), scopes, collision_views);

    if (visible.empty()) return;
    // Expand each visible emitter's particles into draw instances.
    ID3D12Resource* cur = state_[parity_].Get();
    Gpu::barrier(cl, cur, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ext.set_texture(kParticleCompute, 0x00, cur, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, state_total_);
    ext.set_texture(kParticleCompute, 0xC8, instances_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, instance_total_);
    GamePipeline::VolumeBatch instance_batch;
    for (uint32_t i : visible) {
        const Emitter& e = emitters_[i];
        const uint32_t used = uint32_t(std::min<uint64_t>(e.spawned, e.capacity));
        if (!used) continue;
        ext.set(kParticleCompute, 0x110, float(used));
        ext.set(kParticleCompute, 0xF0, float4{float(e.state_base), 0, float(e.instance_base), 0});
        System& s = *systems_[e.system];
        guard(s, [&] { s.instance.dispatch_batch(gpu, cl, ext, scopes, (used + kGroup - 1) / kGroup, instance_batch); });
    }
    Gpu::barrier(cl, cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Gpu::barrier(cl, instances_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    Gpu::barrier(cl, history_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Draw, farthest emitter first (each emitter's quads in slot order).
    std::vector<std::pair<float, uint32_t>> order;
    order.reserve(visible.size());
    for (uint32_t i : visible) {
        const Emitter& e = emitters_[i];
        const V3 d{e.position.x - vm.eye.x, e.position.y - vm.eye.y, e.position.z - vm.eye.z};
        order.push_back({-(d.x * d.x + d.y * d.y + d.z * d.z), i});
    }
    std::sort(order.begin(), order.end());
    const D3D12_VIEWPORT vp{0, 0, float(gb.width()), float(gb.height()), 0, 1};
    const D3D12_RECT sc{0, 0, LONG(gb.width()), LONG(gb.height())};
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);
    Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE targets[] = {hdr, gb.null_rtv, gb.null_rtv, gb.null_rtv, gb.rtv[3]};
    cl->OMSetRenderTargets(5, targets, FALSE, &gb.dsv_ro);
    ext.set_texture(kParticle, 0x08, history_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, total_ * kHistoryFloat4s);
    ext.set_texture(kParticle, 0x10, history_.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, total_ * kHistoryFloat4s);
    GamePipeline::VolumeBatch draw_batch;
    for (const auto& [depth, i] : order) {
        const Emitter& e = emitters_[i];
        const uint32_t used = uint32_t(std::min<uint64_t>(e.spawned, e.capacity));
        if (!used) continue;
        object_cb = object_constants_->GetGPUVirtualAddress() + uint64_t(i) * sizeof(ParticleObjectConstants);
        System& s = *systems_[e.system];
        ext.set(kParticle, 0x178, float(e.offset * kHistoryFloat4s));
        ext.set(kParticle, 0x17C, float(e.offset * kHistoryFloat4s));
        const D3D12_VERTEX_BUFFER_VIEW vbv{instances_->GetGPUVirtualAddress() + uint64_t(e.instance_base) * 16,
                                           used * s.instance_float4s * 16, s.instance_float4s * 16};
        guard(s, [&] { s.draw.draw_instances(gpu, cl, ext, scopes, vbv, used, draw_batch); });
    }
    Gpu::barrier(cl, gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Gpu::barrier(cl, instances_.Get(), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Gpu::barrier(cl, history_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

std::string ParticleScene::debug_dump(Gpu& gpu) {
    std::string out;
    if (!total_) return out;
    // One readback of the instance stream: per system, the emitters and the particles the draw VS
    // keeps (normalized age, instance row 2 .w, at most 0.999).
    auto rb = gpu.buffer(uint64_t(instance_total_) * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    auto* cl = gpu.immediate();
    Gpu::barrier(cl, instances_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(rb.Get(), 0, instances_.Get(), 0, uint64_t(instance_total_) * 16);
    Gpu::barrier(cl, instances_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu.flush();
    std::vector<float> hist(size_t(total_) * kHistoryFloat4s * 4);
    {
        auto hb = gpu.buffer(hist.size() * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto* c2 = gpu.immediate();
        Gpu::barrier(c2, history_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        c2->CopyBufferRegion(hb.Get(), 0, history_.Get(), 0, hist.size() * 4);
        Gpu::barrier(c2, history_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu.flush();
        void* hp = nullptr;
        hb->Map(0, nullptr, &hp);
        std::memcpy(hist.data(), hp, hist.size() * 4);
        hb->Unmap(0, nullptr);
    }
    const float* f = nullptr;
    rb->Map(0, nullptr, (void**)&f);
    struct Row { uint32_t emitters = 0, used = 0, alive = 0; };
    std::map<uint32_t, Row> rows;
    const char* want = std::getenv("NIM_FX_DUMP");
    for (const auto& e : emitters_) {
        if (!e.active || !e.spawned) continue;
        const uint32_t used = uint32_t(std::min<uint64_t>(e.spawned, e.capacity));
        Row& r = rows[e.system];
        ++r.emitters;
        r.used += used;
        for (uint32_t k = 0; k < used; ++k) {
            const float* p = f + (uint64_t(e.instance_base) + uint64_t(k) * systems_[e.system]->instance_float4s) * 4;
            if (p[2 * 4 + 3] <= 0.999f) ++r.alive;
            if (want && std::strlen(want) == 8 && systems_[e.system]->def.tag.str() == want && k < 4)
                for (uint32_t c = 0; c < systems_[e.system]->instance_float4s; ++c)
                    out += std::format("  {} p{} in{} ({:.4g} {:.4g} {:.4g} {:.4g})\n", want, k, c, p[c * 4], p[c * 4 + 1], p[c * 4 + 2], p[c * 4 + 3]);
            if (want && std::strlen(want) == 8 && systems_[e.system]->def.tag.str() == want && k < 4)
                for (uint32_t c = 0; c < kHistoryFloat4s; ++c) {
                    const float* h = hist.data() + (uint64_t(e.offset + k) * kHistoryFloat4s + c) * 4;
                    out += std::format("  {} p{} history{} ({:.4g} {:.4g} {:.4g} {:.4g})\n", want, k, c, h[0], h[1], h[2], h[3]);
                }
        }
    }
    rb->Unmap(0, nullptr);
    for (const auto& [i, r] : rows)
        out += std::format("fx system {}: {} emitters, {} particles, {} with instance row 2 .w <= 0.999{}\n", systems_[i]->def.tag.str(), r.emitters,
                           r.used, r.alive, systems_[i]->failed ? " [failed]" : "");
    return out;
}

}  // namespace nim
