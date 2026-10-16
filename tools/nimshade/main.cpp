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
    bool reference = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf_s(args[++i].c_str(), "%ux%u", &width, &height);
        else if (args[i] == "--warp") warp = true;
        else if (args[i] == "--game") game = true;
        else if (args[i] == "--reference") reference = true;
        else if (args[i] == "--fog" && i + 1 < args.size()) fog = std::stof(args[++i]);
        else if (args[i] == "--clip" && i + 1 < args.size()) clip_tag = TagHash(uint32_t(std::stoul(args[++i], nullptr, 16)));
        else if (args[i] == "--frame" && i + 1 < args.size()) clip_frame = std::stof(args[++i]);
        else if (args[i] == "--time" && i + 1 < args.size()) material_time = std::stof(args[++i]);
        else if (args[i] == "--benchmark" && i + 1 < args.size()) benchmark_frames = std::stoul(args[++i]);
        else if (args[i] == "--camera-scale" && i + 1 < args.size()) camera_scale = std::stof(args[++i]);
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
    if (const Entry* e = pm.entry(mesh_tag); e && e->reference == kClassDynamicModel) {
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
    ViewMatrices vm = make_view(frame_sphere(scene.center(), scene.radius() * camera_scale, width, height));

    auto* cl = gpu.immediate();
    scene.set_material_frame(material_time);
    if (!channel_values.empty()) scene.set_channels(channel_values);
    scene.render_gbuffer(gpu, cl, gb, vm);
    for (const auto& [hash, channel] : scene.channels()) {
        const char* name = object_channel_name(hash);
        std::println("  channel {:08X} {}{}{} = ({:.4g}, {:.4g}, {:.4g}, {:.4g})", hash, name ? name : "",
                     channel.vs ? " VS" : "", channel.ps ? " PS" : "", channel.value[0], channel.value[1], channel.value[2], channel.value[3]);
    }
    LightingParams lp;
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
        std::println("game shading: technique {}", gl.shading_technique().str());
        for (const auto& n : gl.notes()) std::println("  {}", n);
        gl.render(gpu, gpu.immediate(), gb, vm, lp, scene.has_transparent() ? GameLighting::TransparentFn(transparents) : nullptr);
        gpu.flush();
        std::println("estimated engine inputs (no game data for these):");
        for (const auto& [what, why] : scene.estimates()) std::println("  material  {}  [{}]", what, why);
        for (const auto& [what, why] : gl.externs().estimates()) std::println("  lighting  {}  [{}]", what, why);
        write_png(out_dir / "composite_game.png", width, height,
                  gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
        if (fog > 0) {
            LightingParams lp;
            lp.time = material_time;
            lp.optimize = !reference;
            lp.fog = true;
            lp.fog_density = fog;
            gl.render(gpu, gpu.immediate(), gb, vm, lp, transparents);
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
            scene.render_gbuffer(gpu, list, gb, vm);
            if (game) gl.render(gpu, list, gb, vm, lp, transparents);
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
