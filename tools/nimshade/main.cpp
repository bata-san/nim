// nimshade — headless shading test.
//
// Renders a static mesh with the game's own compiled shaders (DXIL VS + PS, untouched), material
// constants evaluated by nim's TFX VM, and the game's BC textures; then runs nim's deferred
// lighting over the G-buffer the game's PS wrote. Writes the G-buffer and the lit image as PNGs.
//
// With --game the lit image also goes through the game's own global_lighting_and_shading pass
// (composite_game.png).
//
//   nimshade [--install DIR] STATIC_TAG [OUT_DIR] [--size WxH] [--warp] [--game]
//            [--clip CLIP_TAG [--frame F]]   (skinned dynamic models: posed with an animation clip)
#include "nim/content/animation.hpp"
#include "nim/content/object_channels.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/map.hpp"
#include "nim/content/weather.hpp"
#include "nim/render/scene.hpp"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

using namespace nim;
namespace fs = std::filesystem;

namespace {

void write_png(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    ComPtr<IWICImagingFactory> f;
    check_hr(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)), "WIC factory");
    ComPtr<IWICStream> stream;
    check_hr(f->CreateStream(&stream), "CreateStream");
    check_hr(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> enc;
    check_hr(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc), "CreateEncoder");
    check_hr(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache), "encoder Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    check_hr(enc->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    check_hr(frame->Initialize(nullptr), "frame Initialize");
    check_hr(frame->SetSize(w, h), "SetSize");
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    check_hr(frame->SetPixelFormat(&fmt), "SetPixelFormat");
    // The PNG encoder may answer with the closest format it supports (32bppBGRA): swizzle to it.
    std::vector<uint8_t> px = rgba;
    if (fmt != GUID_WICPixelFormat32bppRGBA)
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    check_hr(frame->WritePixels(h, w * 4, UINT(px.size()), px.data()), "WritePixels");
    check_hr(frame->Commit(), "frame Commit");
    check_hr(enc->Commit(), "encoder Commit");
}

int run(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path install;
    uint32_t width = 1600, height = 1000;
    bool warp = false, game = false;
    float fog = 0;
    std::optional<TagHash> clip_tag;
    float clip_frame = 0;
    float material_time = 0;
    ChannelValues channel_values;
    uint32_t benchmark_frames = 0;
    float camera_scale = 1;
    float map_radius = 120;
    bool shadows = true;
    uint32_t map_texture_cap = 0;  // full resolution
    bool no_lights = false, no_cubemaps = false, no_entities = false;
    std::optional<V3> eye_arg, look_arg;
    float sun_arg = -1, sky_arg = -1;
    bool reference = false;
    bool heat_cascade = false;
    std::vector<std::pair<std::string, uint32_t>> weather_args;  // controller name part, state hash
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf_s(args[++i].c_str(), "%ux%u", &width, &height);
        else if (args[i] == "--warp") warp = true;
        else if (args[i] == "--no-shadows") shadows = false;
        else if (args[i] == "--no-lights") no_lights = true;
        else if (args[i] == "--no-cubemaps") no_cubemaps = true;
        else if (args[i] == "--no-entities") no_entities = true;
        else if ((args[i] == "--eye" || args[i] == "--look") && i + 1 < args.size()) {
            V3 v{};
            sscanf_s(args[i + 1].c_str(), "%f,%f,%f", &v.x, &v.y, &v.z);
            (args[i] == "--eye" ? eye_arg : look_arg) = v;
            ++i;
        }
        else if (args[i] == "--sun" && i + 1 < args.size()) sun_arg = std::stof(args[++i]);
        else if (args[i] == "--sky" && i + 1 < args.size()) sky_arg = std::stof(args[++i]);
        else if (args[i] == "--game") game = true;
        else if (args[i] == "--heat-cascade") heat_cascade = true;
        else if (args[i] == "--weather" && i + 1 < args.size()) {
            const auto value = args[++i];
            const auto eq = value.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--weather expects CONTROLLER=STATE_HASH");
            weather_args.push_back({value.substr(0, eq), uint32_t(std::stoul(value.substr(eq + 1), nullptr, 16))});
        }
        else if (args[i] == "--reference") reference = true;
        else if (args[i] == "--fog" && i + 1 < args.size()) fog = std::stof(args[++i]);
        else if (args[i] == "--clip" && i + 1 < args.size()) clip_tag = TagHash(uint32_t(std::stoul(args[++i], nullptr, 16)));
        else if (args[i] == "--frame" && i + 1 < args.size()) clip_frame = std::stof(args[++i]);
        else if (args[i] == "--time" && i + 1 < args.size()) material_time = std::stof(args[++i]);
        else if (args[i] == "--benchmark" && i + 1 < args.size()) benchmark_frames = std::stoul(args[++i]);
        else if (args[i] == "--camera-scale" && i + 1 < args.size()) camera_scale = std::stof(args[++i]);
        else if (args[i] == "--map-radius" && i + 1 < args.size()) map_radius = std::stof(args[++i]);
        else if (args[i] == "--map-textures" && i + 1 < args.size()) map_texture_cap = std::stoul(args[++i]);
        else if (args[i] == "--channel" && i + 1 < args.size()) {
            const auto value = args[++i];
            const auto eq = value.find('=');
            if (eq == std::string::npos) throw std::runtime_error("--channel expects HASH=VALUE");
            const uint32_t hash = uint32_t(std::stoul(value.substr(0, eq), nullptr, 16));
            const float v = std::stof(value.substr(eq + 1));
            channel_values[hash] = {v, v, v, v};
        }
        else pos.push_back(args[i]);
    }
    if (pos.empty()) {
        std::println(stderr, "usage: nimshade [--install DIR] TAG [OUT_DIR] [--size WxH] [--warp] [--game [--fog DENSITY_PER_KM]] [--time SECONDS] [--channel HASH=VALUE] [--benchmark FRAMES] [--reference] [--camera-scale N]");
        return 2;
    }
    if (install.empty()) {
        auto found = PackageManager::find_install();
        if (!found) { std::println(stderr, "Marathon install not found; pass --install DIR"); return 2; }
        install = *found;
    }
    TagHash mesh_tag(uint32_t(std::stoul(pos[0], nullptr, 16)));
    if (!std::isfinite(material_time) || material_time < 0) throw std::runtime_error("--time must be finite and nonnegative");
    if (!std::isfinite(camera_scale) || camera_scale <= 0) throw std::runtime_error("--camera-scale must be finite and positive");
    fs::path out_dir = pos.size() >= 2 ? fs::path(pos[1]) : fs::path("shade_" + mesh_tag.str());
    fs::create_directories(out_dir);

    auto t0 = std::chrono::steady_clock::now();
    PackageManager pm(install);
    pm.build_hash64_index();
    auto layouts = load_input_layouts(pm);

    Gpu gpu;
    gpu.init(warp);
    std::println("adapter: {}", gpu.adapter_name());

    StaticMeshScene scene;
    std::vector<MapLight> map_lights;
    std::vector<const MapLight*> region_lights;
    std::vector<MapCubemap> map_cubemaps;
    std::vector<const MapCubemap*> region_cubemaps;
    std::map<uint32_t, float> weather_channels;
    bool is_map = false;
    if (const Entry* e = pm.entry(mesh_tag); e && e->reference == kClassBubbleParent) {
        // A map: the static instances within --map-radius of where the geometry is.
        auto t0 = std::chrono::steady_clock::now();
        MapLayout map = load_map_layout(pm, mesh_tag);
        is_map = true;
        if (heat_cascade || !weather_args.empty()) {
            const MapWeather weather = load_map_weather(pm, mesh_tag);
            std::println("weather: {} controller(s)", weather.controllers.size());
            auto apply = [&](const WeatherController& c, const WeatherState& st) {
                std::println("  {} state {}", c.name, weather.name(st.name));
                for (const auto& v : weather_values(st)) weather_channels[v.first] = v.second;
            };
            for (const auto& c : weather.controllers) {
                if (heat_cascade && c.heat_cascade())
                    if (const WeatherState* st = c.strongest_state()) apply(c, *st);
                for (const auto& [part, state] : weather_args) {
                    if (c.name.find(part) == std::string::npos && c.tag.str().find(part) == std::string::npos) continue;
                    for (const auto& st : c.states)
                        if (st.name == state) apply(c, st);
                }
            }
            for (const auto& [slot, v] : weather_channels) std::println("  global channel {} = {:.3g}", slot, v);
        }
        const auto focus = map_focus(map);
        auto region = map_region(map, focus, map_radius);
        size_t in_region = 0;
        for (const auto& st : region) in_region += st.rows.size();
        std::println("map {}: {} statics / {} instances; {} statics / {} instances within {} m of ({:.1f}, {:.1f}, {:.1f})",
                     mesh_tag.str(), map.statics.size(), map.instances, region.size(), in_region, map_radius,
                     focus[0], focus[1], focus[2]);
        // The placed entities' dynamic models, prepared after the statics.
        const size_t static_count = region.size();
        std::unordered_map<uint32_t, DynamicModelMaterials> entity_materials;
        if (!no_entities) {
            const auto te = std::chrono::steady_clock::now();
            entity_materials = index_dynamic_model_materials(pm);
            const MapEntities ents = load_map_entities(pm, mesh_tag, entity_materials);
            auto er = map_region(ents.models, focus, map_radius);
            size_t n = 0;
            for (const auto& m : er) n += m.rows.size();
            std::println("entities: {} placements of {} models ({} entities, {} node tables); {} models / {} placements in range ({:.1f}s)",
                         ents.placements, ents.models.size(), ents.entities, ents.node_tables, er.size(), n,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - te).count());
            for (auto& m : er) region.push_back(std::move(m));
        }
        PrepareCache cache;
        cache.max_texture_dim = map_texture_cap;
        cache.gbuffer_only = true;
        cache.all_lods = true;
        scene.begin(gpu);
        std::map<std::string, size_t> failures;
        double prep_s = 0, add_s = 0;
        const auto terrain = map_terrain_region(map, focus, map_radius);
        if (!no_lights) {
            map_lights = load_map_lights(pm, map);
            region_lights = map_light_region(map_lights, focus, map_radius);
            std::println("lights: {} of {} within range", region_lights.size(), map_lights.size());
        }
        if (!no_cubemaps) {
            map_cubemaps = load_map_cubemaps(pm, map);
            region_cubemaps = map_cubemap_region(map_cubemaps, focus, map_radius);
            std::println("cubemap volumes: {} of {} within range", region_cubemaps.size(), map_cubemaps.size());
        }
        // Prepared on every core (one forked PackageManager per thread), then added in order.
        const size_t jobs = terrain.size() + region.size();
        std::vector<std::optional<PreparedMesh>> prepared(jobs);
        std::vector<std::string> errors(jobs);
        {
            auto ta = std::chrono::steady_clock::now();
            std::atomic<size_t> next{0};
            auto work = [&](PackageManager& wpm) {
                for (size_t j; (j = next.fetch_add(1)) < jobs;) {
                    try {
                        const size_t r = j - terrain.size();
                        if (j < terrain.size()) prepared[j] = prepare_terrain(wpm, *terrain[j], layouts, &cache);
                        else if (r < static_count) prepared[j] = prepare_static_mesh(wpm, region[r].mesh, nullptr, &cache);
                        else {
                            const auto mat = entity_materials.find(region[r].mesh.value);
                            prepared[j] = prepare_dynamic_model(wpm, region[r].mesh, mat == entity_materials.end() ? nullptr : &mat->second, -1,
                                                                nullptr, &cache);
                            if (prepared[j]->no_drawable) prepared[j].reset(), errors[j] = "entity model with no drawable parts";
                        }
                    } catch (const std::exception& ex) {
                        errors[j] = ex.what();
                    }
                }
            };
            std::vector<std::unique_ptr<PackageManager>> helpers;
            unsigned threads_n = std::max(std::thread::hardware_concurrency(), 1u);
            if (const char* e = std::getenv("NIM_THREADS")) threads_n = std::max(1, std::atoi(e));
            for (unsigned i = 1; i < threads_n; ++i) helpers.push_back(pm.fork());
            std::vector<std::thread> threads;
            for (auto& h : helpers) threads.emplace_back([&, hp = h.get()] { work(*hp); });
            work(pm);
            for (auto& t : threads) t.join();
            prep_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count();
        }
        size_t terrain_ok = 0;
        const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        auto tb = std::chrono::steady_clock::now();
        for (size_t j = 0; j < jobs; ++j) {
            const bool is_terrain = j < terrain.size();
            const std::string what = is_terrain ? terrain[j]->tag.str() : region[j - terrain.size()].mesh.str();
            try {
                if (!prepared[j]) throw std::runtime_error(errors[j]);
                if (is_terrain) scene.add_mesh(gpu, std::move(*prepared[j]), layouts, std::span(&identity, 1));
                else scene.add_mesh(gpu, std::move(*prepared[j]), layouts, region[j - terrain.size()].rows);
                prepared[j].reset();
                terrain_ok += is_terrain;
            } catch (const std::exception& ex) {
                if (++failures[std::string(ex.what()).substr(0, 100)] == 1) std::println("  {}{}: {}", is_terrain ? "terrain " : "", what, ex.what());
            }
        }
        add_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - tb).count();
        std::println("terrain: {} of {} patches in range ({} in the map)", terrain_ok, terrain.size(), map.terrains.size());
        scene.finish(gpu, mesh_tag, V3{focus[0], focus[1], focus[2]}, map_radius * 0.6f);
        std::println("loaded {} meshes / {} instances in {:.1f}s", scene.mesh_count(), scene.instance_count(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::println("  prepare {:.1f}s, add {:.1f}s", prep_s, add_s);
        for (const auto& [why, n] : failures) std::println("  skipped {}: {}", n, why);
    } else if (const Entry* e = pm.entry(mesh_tag); e && e->reference == kClassDynamicModel) {
        auto t0 = std::chrono::steady_clock::now();
        auto mats = index_dynamic_model_materials(pm);
        auto it = mats.find(mesh_tag.value);
        std::println("dynamic model: {} components indexed in {:.1f}s, materials {}", mats.size(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                     it == mats.end() ? "none" : it->second.component.str());
        scene.upload(gpu, prepare_dynamic_model(pm, mesh_tag, it == mats.end() ? nullptr : &it->second), layouts);
        for (const auto& n : scene.notes()) std::println("  note: {}", n);
        if (clip_tag) {
            auto rigs = index_model_skeletons(pm, mats);
            auto r = rigs.find(mesh_tag.value);
            if (r == rigs.end() || r->second.rig.is_none()) throw std::runtime_error("--clip: the model has no skeleton with a rig");
            Skeleton sk = load_skeleton(pm, r->second.skeleton);
            AnimationRig rig = load_animation_rig(pm, r->second.rig);
            AnimationClip clip = load_animation_clip(pm, *clip_tag);
            std::vector<BoneTransform> bones;
            pose_skeleton(sk, rig, clip, clip_frame, bones);
            for (size_t b = 0; b < bones.size(); ++b) bones[b] = bones[b] * sk.inverse_bind[b];
            scene.set_skin_pose(std::move(bones));
            std::println("posed with clip {} ({}, {} frames{}) at frame {}", clip_tag->str(), clip_codec_name(clip.codec), clip.frame_count,
                         is_additive(rig, clip) ? ", additive" : "", clip_frame);
        }
    } else {
        scene.load(pm, gpu, mesh_tag, layouts);
    }
    std::println("mesh {}: {} G-buffer group(s) at lod {}, radius {:.2f}", mesh_tag.str(), scene.groups().size(),
                 scene.lod(), scene.radius());
    for (const auto& g : scene.groups()) {
        std::println("  part {}: technique {} VS {} PS {}  cb0 {} regs, vm {} instr", g.part, g.technique.str(),
                     g.vs.str(), g.ps.str(), g.cbuffer_regs, g.vm_instructions);
        for (const auto& t : g.textures)
            std::println("    t{:<2} {} format {} {}x{} mips {}{}", t.slot, t.tag.str(), t.format, t.width, t.height,
                         t.mips, t.error.empty() ? "" : "  (" + t.error + ")");
        for (const auto& u : g.stubbed_ops) std::println("    stubbed TFX op: {}", u);
    }

    GBuffer gb;
    gb.resize(gpu, width, height);
    LightingPass lighting;
    lighting.init(gpu);
    std::println("bounds center ({:.3f},{:.3f},{:.3f}) radius {:.3f}", scene.center().x, scene.center().y, scene.center().z, scene.radius());
    View view = frame_sphere(scene.center(), scene.radius() * camera_scale, width, height);
    if (eye_arg) view.eye = *eye_arg;
    if (look_arg) view.target = *look_arg;
    ViewMatrices vm = make_view(view);

    auto* cl = gpu.immediate();
    scene.set_material_frame(material_time);
    if (!channel_values.empty()) scene.set_channels(channel_values);
    if (!weather_channels.empty()) scene.set_global_channels(weather_channels);
    RainOcclusion rain;
    if (is_map) {
        rain.resize(gpu, 4096);
        scene.render_rain_occlusion(gpu, cl, rain, scene.center(), scene.radius());
        scene.set_rain_occlusion(&rain);
    }
    SunShadow shadow;
    if (shadows) shadow.resize(gpu, 4096);
    auto shadow_pass = [&](ID3D12GraphicsCommandList* list, const LightingParams& p) {
        if (!shadows) return;
        shadow.fit_view(sun_direction(p), vm.eye, scene.center(), scene.center(), scene.radius());
        scene.render_shadow(gpu, list, shadow);
    };
    shadow_pass(cl, LightingParams{});
    scene.render_gbuffer(gpu, cl, gb, vm);
    for (const auto& [hash, channel] : scene.channels()) {
        const char* name = object_channel_name(hash);
        std::println("  channel {:08X} {}{}{} = ({:.4g}, {:.4g}, {:.4g}, {:.4g})", hash, name ? name : "",
                     channel.vs ? " VS" : "", channel.ps ? " PS" : "", channel.value[0], channel.value[1], channel.value[2], channel.value[3]);
    }
    LightingParams lp;
    if (sun_arg >= 0) lp.sun_intensity = sun_arg;
    if (sky_arg >= 0) lp.sky_intensity = sky_arg;
    lp.time = material_time;
    lp.optimize = !reference;
    gpu.flush();
    // Lit output is produced only by the original game pipelines below.
    game = true;

    // Transparent-only meshes need the game's shading pass to have something to blend over.
    if (scene.has_transparent()) {
        std::println("transparent parts: {} (blended after the game's shading pass)", scene.transparent_count());
        game = true;
    }
    auto transparents = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
        scene.render_transparent(gpu, c, gb, hdr, vm);
    };
    GameLighting gl;
    if (game) {
        gl.init(pm, gpu);
        if (!weather_channels.empty()) gl.set_global_channels(weather_channels);
        std::println("game shading: technique {}", gl.shading_technique().str());
        if (!region_lights.empty()) {
            auto tl = std::chrono::steady_clock::now();
            gl.set_lights(pm, gpu, region_lights);
            std::println("local lights: {} prepared in {:.1f}s", gl.light_count(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tl).count());
            for (const auto& n : gl.light_notes()) std::println("  {}", n);
        }
        if (!region_cubemaps.empty()) {
            auto tc = std::chrono::steady_clock::now();
            gl.set_cubemaps(pm, gpu, region_cubemaps);
            std::println("cubemap volumes: {} prepared in {:.1f}s", gl.cubemap_count(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count());
            for (const auto& n : gl.cubemap_notes()) std::println("  {}", n);
        }
        for (const auto& n : gl.notes()) std::println("  {}", n);
        gl.render(gpu, gpu.immediate(), gb, vm, lp, scene.has_transparent() ? GameLighting::TransparentFn(transparents) : nullptr, &shadow);
        gpu.flush();
        if (gl.cubemap_count()) {
            std::println("cubemap volumes drawn: {}", gl.cubemaps_drawn());
            for (const auto& n : gl.cubemap_notes()) std::println("  {}", n);
        }
        if (gl.light_count()) {
            std::println("local lights drawn: {}", gl.lights_drawn());
            for (int b = 0; b < 2; ++b) {
                auto px = gpu.read_texture(gl.light_buffer(b), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8);
                const uint16_t* h = reinterpret_cast<const uint16_t*>(px.data());
                size_t lit = 0;
                float mx = 0;
                for (size_t k = 0; k < px.size() / 2; k += 4) {
                    for (int c = 0; c < 3; ++c) {
                        const uint16_t v = h[k + size_t(c)];
                        const int ex = (v >> 10) & 31; const float m = float(v & 1023) / 1024.0f;
                        const float f = ex ? std::ldexp(1.0f + m, ex - 15) : std::ldexp(m, -14);
                        if (!(v & 0x8000)) mx = std::max(mx, f);
                    }
                    lit += (h[k] & 0x7FFF) != 0;
                }
                std::println("  light buffer {}: {:.1f}% non-zero, max {:.4g}", b, 100.0 * double(lit) / double(px.size() / 8), mx);
            }
            for (const auto& n : gl.light_notes()) std::println("  {}", n);
        }
        std::println("estimated engine inputs (no game data for these):");
        for (const auto& [what, why] : scene.estimates()) std::println("  material  {}  [{}]", what, why);
        for (const auto& [what, why] : gl.externs().estimates()) std::println("  lighting  {}  [{}]", what, why);
        write_png(out_dir / "composite_game.png", width, height,
                  gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
        if (shadow.valid) {
            auto d = gpu.read_texture(shadow.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 4);
            const float* f = reinterpret_cast<const float*>(d.data());
            size_t n = d.size() / 4, covered = 0;
            float lo = 1e9f, hi = -1e9f;
            for (size_t i = 0; i < n; ++i) { lo = std::min(lo, f[i]); hi = std::max(hi, f[i]); covered += f[i] > 0; }
            std::println("shadow map {}: depth {}..{}, covered {:.1f}%, half {:.1f} m, range {:.1f} m", shadow.size, lo, hi, 100.0 * covered / n, shadow.half, shadow.range);
            auto mask = gpu.read_texture(gl.shadow_mask(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1);
            std::vector<uint8_t> rgba(mask.size() * 4);
            for (size_t i = 0; i < mask.size(); ++i) rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = mask[i], rgba[i * 4 + 3] = 255;
            write_png(out_dir / "sun_shadow_mask.png", width, height, rgba);
        }
        if (fog > 0) {
            LightingParams lp;
            lp.time = material_time;
            lp.optimize = !reference;
            lp.fog = true;
            lp.fog_density = fog;
            gl.render(gpu, gpu.immediate(), gb, vm, lp, transparents, &shadow);
            gpu.flush();
            write_png(out_dir / "composite_game_fog.png", width, height,
                      gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
        }
        for (const auto& n : gl.notes()) std::println("  {}", n);
    }

    if (benchmark_frames) {
        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        check_hr(gpu.device()->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "timestamp heap");
        auto readback = gpu.buffer(16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        uint64_t frequency = 0;
        check_hr(gpu.queue()->GetTimestampFrequency(&frequency), "timestamp frequency");
        double cpu_ms = 0, gpu_ms = 0;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < benchmark_frames + 8; ++i) {
            gpu.begin_frame(i);
            const auto cpu_start = std::chrono::steady_clock::now();
            auto* list = gpu.immediate();
            lp.time = material_time + float(i) / 60;
            lp.delta_time = 1.0f / 60;
            scene.set_material_frame(lp.time, lp.delta_time);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            shadow_pass(list, lp);
            scene.render_gbuffer(gpu, list, gb, vm);
            if (game) gl.render(gpu, list, gb, vm, lp, transparents, &shadow);
            else lighting.render(gpu, list, gb, vm, lp);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            const double record_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu_start).count();
            gpu.flush();
            if (i < 8) continue;
            uint64_t* ticks = nullptr;
            const D3D12_RANGE read{0, 16};
            check_hr(readback->Map(0, &read, reinterpret_cast<void**>(&ticks)), "timestamp readback");
            gpu_ms += double(ticks[1] - ticks[0]) * 1000.0 / double(frequency);
            const D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
            cpu_ms += record_ms;
        }
        std::println("benchmark {} frames at {}x{}: CPU record {:.4f} ms/frame, GPU {:.4f} ms/frame (wall {:.2f}s, 8 warmup frames)",
                     benchmark_frames, width, height, cpu_ms / benchmark_frames, gpu_ms / benchmark_frames,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }

    // Debug views of the game's G-buffer through the same pass.
    const std::pair<ViewMode, const char*> views[] = {{ViewMode::Albedo, "gbuffer0_albedo.png"},
                                                      {ViewMode::Normal, "gbuffer1_normal.png"},
                                                      {ViewMode::Smoothness, "gbuffer1_smoothness.png"},
                                                      {ViewMode::Material, "gbuffer2.png"}};
    for (auto [mode, name] : views) {
        lp.mode = mode;
        lighting.render(gpu, gpu.immediate(), gb, vm, lp);
        gpu.flush();
        write_png(out_dir / name, width, height,
                  gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
    }

    // Coverage: depth > 0 where something was drawn (reversed-Z, cleared to 0).
    auto depth = gpu.read_texture(gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 4);
    size_t covered = 0;
    for (size_t i = 0; i < depth.size(); i += 4) {
        float d;
        std::memcpy(&d, depth.data() + i, 4);
        covered += d > 0;
    }
    auto t1 = std::chrono::steady_clock::now();
    std::println("rendered {} pixels covered ({:.1f}%) in {:.2f}s", covered, 100.0 * covered / (size_t(width) * height),
                 std::chrono::duration<double>(t1 - t0).count());
    if (!covered && !scene.has_transparent()) throw std::runtime_error("nothing was rendered");
    std::println("wrote {}", fs::absolute(out_dir).string());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        return run(argc, argv);
    } catch (const std::exception& ex) {
        std::println(stderr, "error: {}", ex.what());
        return 1;
    }
}
