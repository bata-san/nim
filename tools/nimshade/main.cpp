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
//            [--clip CLIP_TAG [--frame F] [--equipment MODEL]]  (skinned dynamic models)
//            [--probe-index N --dump-probe-mask] (original CSG/GPU membership verification)
//            [--probe-distance-fade] (opt in to the unverified camera-distance fade)
//            [--activity TAG --activity-map N] (DEIMOS ActivityTab selection)
//            [--entity-range BEGIN:END] (diagnostic half-open indices in the region's model list)
//            [--verify-static-motion] (16 frozen TAA frames must have < 0.1 px motion)
//            [--no-froxel-noise] (compare the original uniform-density atmosphere permutation)
//            [--no-froxel-history] (compare per-frame density without reprojection)
//            [--dump-lighting] (light/depth/sky/froxel buffers and camera metadata)
#include "sampler.hpp"
#include "nim/content/animation.hpp"
#include "nim/content/object_channels.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/map.hpp"
#include "nim/content/weather.hpp"
#include "nim/render/scene.hpp"
#include "nim/render/particles.hpp"
#include "nim/render/ai_actors.hpp"
#include "nim/sim/ai.hpp"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <print>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

using namespace nim;
namespace fs = std::filesystem;

namespace {

// 5x7 glyphs for the AI overlay's labels in headless renders (the GUI draws them with ImGui).
const uint8_t* glyph(char c) {
    static const struct { char c; uint8_t rows[7]; } kFont[] = {
        {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}}, {'2', {14, 17, 1, 2, 4, 8, 31}},
        {'3', {30, 1, 1, 14, 1, 1, 30}}, {'4', {2, 6, 10, 18, 31, 2, 2}}, {'5', {31, 16, 30, 1, 1, 17, 14}},
        {'6', {6, 8, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}}, {'8', {14, 17, 17, 14, 17, 17, 14}},
        {'9', {14, 17, 17, 15, 1, 2, 12}}, {'a', {0, 0, 14, 1, 15, 17, 15}}, {'b', {16, 16, 30, 17, 17, 17, 30}},
        {'c', {0, 0, 14, 16, 16, 17, 14}}, {'d', {1, 1, 15, 17, 17, 17, 15}}, {'e', {0, 0, 14, 17, 31, 16, 14}},
        {'f', {6, 9, 8, 28, 8, 8, 8}}, {'g', {0, 15, 17, 17, 15, 1, 14}}, {'h', {16, 16, 22, 25, 17, 17, 17}},
        {'i', {4, 0, 12, 4, 4, 4, 14}}, {'j', {2, 0, 6, 2, 2, 18, 12}}, {'k', {16, 16, 18, 20, 24, 20, 18}},
        {'l', {12, 4, 4, 4, 4, 4, 14}}, {'m', {0, 0, 26, 21, 21, 17, 17}}, {'n', {0, 0, 22, 25, 17, 17, 17}},
        {'o', {0, 0, 14, 17, 17, 17, 14}}, {'p', {0, 0, 30, 17, 30, 16, 16}}, {'q', {0, 0, 15, 17, 15, 1, 1}},
        {'r', {0, 0, 22, 25, 16, 16, 16}}, {'s', {0, 0, 15, 16, 14, 1, 30}}, {'t', {8, 8, 28, 8, 8, 9, 6}},
        {'u', {0, 0, 17, 17, 17, 19, 13}}, {'v', {0, 0, 17, 17, 17, 10, 4}}, {'w', {0, 0, 17, 17, 21, 21, 10}},
        {'x', {0, 0, 17, 10, 4, 10, 17}}, {'y', {0, 0, 17, 17, 15, 1, 14}}, {'z', {0, 0, 31, 2, 4, 8, 31}},
        {'?', {14, 17, 1, 2, 4, 0, 4}}, {'!', {4, 4, 4, 4, 4, 0, 4}}, {'.', {0, 0, 0, 0, 0, 12, 12}},
        {'_', {0, 0, 0, 0, 0, 0, 31}}, {'-', {0, 0, 0, 31, 0, 0, 0}},
    };
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    for (const auto& g : kFont)
        if (g.c == c) return g.rows;
    return nullptr;
}

// Draws the AI overlay (lines, rectangles, labels) over an RGBA8 image, alpha-blended.
void draw_overlay(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, const AiOverlay& o) {
    auto plot = [&](int x, int y, uint32_t c) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
        uint8_t* px = rgba.data() + (size_t(y) * w + size_t(x)) * 4;
        const float a = float(c >> 24) / 255.0f;
        for (int k = 0; k < 3; ++k) px[k] = uint8_t(px[k] * (1 - a) + float((c >> (8 * k)) & 255) * a + 0.5f);
    };
    for (const auto& r : o.rects) {
        const int x0 = int(std::floor(r.min[0])), y0 = int(std::floor(r.min[1])), x1 = int(std::ceil(r.max[0])), y1 = int(std::ceil(r.max[1]));
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if (r.filled || y == y0 || y == y1 - 1 || x == x0 || x == x1 - 1) plot(x, y, r.color);
    }
    for (const auto& l : o.lines) {
        const float dx = l.b[0] - l.a[0], dy = l.b[1] - l.a[1];
        const int steps = int(std::min(4000.0f, std::max(std::abs(dx), std::abs(dy)))) + 1;
        const int half = std::max(0, int(l.width / 2));
        for (int i = 0; i <= steps; ++i) {
            const int x = int(l.a[0] + dx * float(i) / float(steps)), y = int(l.a[1] + dy * float(i) / float(steps));
            for (int oy = -half; oy <= half; ++oy)
                for (int ox = -half; ox <= half; ++ox) plot(x + ox, y + oy, l.color);
        }
    }
    for (const auto& t : o.texts) {
        const int scale = std::max(1, int(t.scale * 2 + 0.5f));
        int x = int(t.at[0]);
        for (const char c : t.text) {
            if (const uint8_t* g = glyph(c))
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (g[row] & (16 >> col))
                            for (int sy = 0; sy < scale; ++sy)
                                for (int sx = 0; sx < scale; ++sx) plot(x + col * scale + sx, int(t.at[1]) + row * scale + sy, t.color);
            x += 6 * scale;
        }
    }
}

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
    float time_of_day = -1;
    float day_night_state = -1;
    bool no_light_shadows = false;
    size_t frame_light_shadows = 0;  // > 0: draw the light maps as the GUI does, this many per whole frame
    bool verify_frame_inputs = false;
    bool no_atmosphere = false, no_environment = false, no_sky_ambient = false, no_overhead_occlusion = false, no_interior_fog = false, print_channels = false;
    std::optional<TagHash> clip_tag;
    TagHash equipment_model;
    bool list_clips = false;
    bool single_model = false;
    float clip_frame = 0;
    float material_time = 0;
    ChannelValues channel_values;
    uint32_t benchmark_frames = 0;
    float camera_scale = 1;
    float map_radius = 120;
    bool shadows = true;
    uint32_t map_texture_cap = 0;  // full resolution
    bool no_lights = false, no_cubemaps = false, no_entities = false, no_sky_objects = false, no_fx = false;
    float fx_radius = -1;
    std::optional<size_t> probe_index;
    bool dump_probe_mask = false;
    bool dump_lighting = false;
    bool no_froxels = false;
    bool no_froxel_noise = false;
    bool no_map_decals = false;
    bool no_froxel_history = false;
    bool verify_static_motion = false;
    bool probe_distance_fade = false;
    std::optional<std::pair<size_t, size_t>> entity_range;
    std::vector<std::pair<size_t, size_t>> sky_ranges;
    TagHash selected_activity;
    uint32_t selected_activity_map = 0;
    bool no_ssao = false, no_volumetrics = false, no_ssr = false;
    bool no_distortion = false;
    bool no_post = false, no_auto_exposure = false, no_bloom = false, no_aa = false, no_taa = false;
    float exposure_arg = 1;
    bool assume_lights_on = true;
    std::optional<V3> eye_arg, look_arg;
    float sun_arg = -1, sky_arg = -1;
    bool reference = false;
    bool heat_cascade = false;
    std::vector<std::pair<std::string, uint32_t>> weather_args;  // controller name part, state hash
    bool ai = false, ai_visible = false, ai_debug = false, ai_frame = false;
    float ai_time = 20, ai_range = 60, ai_frame_distance = 6, ai_frame_height = 1.7f;
    size_t ai_per_character = 4;
    uint32_t ai_strip = 0;     // --ai-strip N: N frames 0.1 s apart of a walking agent (strip_K.png)
    uint32_t ai_ship_strip = 0;  // --ai-dropship-strip N: the nearest dropship called, N frames 0.5 s apart (ship_K.png)
    std::optional<size_t> ai_ship;
    bool ai_ground_audit = false;  // --ai-ground-audit: every drawable agent posed in turn, its sink and body overlap
    std::optional<size_t> ai_subject;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf_s(args[++i].c_str(), "%ux%u", &width, &height);
        else if (args[i] == "--warp") warp = true;
        else if (args[i] == "--no-shadows") shadows = false;
        else if (args[i] == "--no-lights") no_lights = true;
        else if (args[i] == "--no-light-shadows") no_light_shadows = true;
        else if (args[i] == "--frame-light-shadows" && i + 1 < args.size()) frame_light_shadows = size_t(std::stoul(args[++i]));
        else if (args[i] == "--no-cubemaps") no_cubemaps = true;
        else if (args[i] == "--probe-index" && i + 1 < args.size()) probe_index = std::stoull(args[++i]);
        else if (args[i] == "--dump-probe-mask") dump_probe_mask = true;
        else if (args[i] == "--dump-lighting") dump_lighting = true;
        else if (args[i] == "--no-froxels") no_froxels = true;
        else if (args[i] == "--no-froxel-noise") no_froxel_noise = true;
        else if (args[i] == "--no-map-decals") no_map_decals = true;
        else if (args[i] == "--no-froxel-history") no_froxel_history = true;
        else if (args[i] == "--verify-static-motion") verify_static_motion = true;
        else if (args[i] == "--probe-distance-fade") probe_distance_fade = true;
        else if (args[i] == "--entity-range" && i + 1 < args.size()) {
            const auto value = args[++i];
            const auto split = value.find(':');
            if (split == std::string::npos) throw std::runtime_error("--entity-range expects BEGIN:END (exclusive)");
            entity_range = {std::stoull(value.substr(0, split)), std::stoull(value.substr(split + 1))};
        }
        else if (args[i] == "--sky-range" && i + 1 < args.size()) {
            std::string value = args[++i];
            for (size_t start = 0; start <= value.size();) {
                const size_t end = std::min(value.find(',', start), value.size());
                const auto part = value.substr(start, end - start);
                const auto split = part.find(':');
                if (split == std::string::npos) throw std::runtime_error("--sky-range expects BEGIN:END[,BEGIN:END...] (exclusive)");
                sky_ranges.emplace_back(std::stoull(part.substr(0, split)), std::stoull(part.substr(split + 1)));
                start = end + 1;
            }
        }
        else if (args[i] == "--activity" && i + 1 < args.size()) selected_activity = TagHash(uint32_t(std::stoul(args[++i], nullptr, 16)));
        else if (args[i] == "--activity-map" && i + 1 < args.size()) selected_activity_map = uint32_t(std::stoul(args[++i]));
        else if (args[i] == "--no-entities") no_entities = true;
        else if (args[i] == "--ai") ai = true;
        else if (args[i] == "--ai-visible") ai_visible = true;
        else if (args[i] == "--ai-debug") ai_debug = true;
        else if (args[i] == "--ai-frame") ai_frame = true;
        else if (args[i] == "--ai-frame-at" && i + 1 < args.size()) {
            ai_frame = true;
            if (std::sscanf(args[++i].c_str(), "%f,%f", &ai_frame_distance, &ai_frame_height) != 2) throw std::runtime_error("--ai-frame-at DISTANCE,HEIGHT");
        }
        else if (args[i] == "--ai-time" && i + 1 < args.size()) ai_time = std::stof(args[++i]);
        else if (args[i] == "--ai-range" && i + 1 < args.size()) ai_range = std::stof(args[++i]);
        else if (args[i] == "--ai-actors" && i + 1 < args.size()) ai_per_character = std::stoull(args[++i]);
        else if (args[i] == "--ai-strip" && i + 1 < args.size()) ai_strip = uint32_t(std::stoul(args[++i])), ai_frame = true;
        else if (args[i] == "--ai-dropship-strip" && i + 1 < args.size()) ai_ship_strip = uint32_t(std::stoul(args[++i]));
        else if (args[i] == "--ai-ground-audit") ai_ground_audit = true;
        else if (args[i] == "--no-fx") no_fx = true;
        else if (args[i] == "--fx-radius" && i + 1 < args.size()) fx_radius = std::stof(args[++i]);
        else if (args[i] == "--verify-frame-inputs") verify_frame_inputs = true;
        else if (args[i] == "--no-sky-objects") no_sky_objects = true;
        else if (args[i] == "--no-ssao") no_ssao = true;
        else if (args[i] == "--no-volumetrics") no_volumetrics = true;
        else if (args[i] == "--no-ssr") no_ssr = true;
        else if (args[i] == "--no-distortion") no_distortion = true;
        else if (args[i] == "--no-post") no_post = true;
        else if (args[i] == "--no-auto-exposure") no_auto_exposure = true;
        else if (args[i] == "--no-bloom") no_bloom = true;
        else if (args[i] == "--no-aa") no_aa = true;
        else if (args[i] == "--no-taa") no_taa = true;
        else if (args[i] == "--exposure" && i + 1 < args.size()) exposure_arg = std::stof(args[++i]);
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
        else if (args[i] == "--time-of-day" && i + 1 < args.size()) time_of_day = std::stof(args[++i]);
        else if (args[i] == "--day-night-state" && i + 1 < args.size()) day_night_state = std::stof(args[++i]);
        else if (args[i] == "--no-atmosphere") no_atmosphere = true;
        else if (args[i] == "--no-environment") no_environment = true;
        else if (args[i] == "--no-sky-ambient") no_sky_ambient = true;
        else if (args[i] == "--no-overhead-occlusion") no_overhead_occlusion = true;
        else if (args[i] == "--no-interior-fog") no_interior_fog = true;
        else if (args[i] == "--no-assumed-light-state") assume_lights_on = false;
        else if (args[i] == "--print-channels") print_channels = true;
        else if (args[i] == "--clip" && i + 1 < args.size()) clip_tag = TagHash(uint32_t(std::stoul(args[++i], nullptr, 16)));
        else if (args[i] == "--equipment" && i + 1 < args.size()) equipment_model = TagHash(uint32_t(std::stoul(args[++i], nullptr, 16)));
        else if (args[i] == "--single-model") single_model = true;
        else if (args[i] == "--list-clips") list_clips = true;
        else if (args[i] == "--frame" && i + 1 < args.size()) clip_frame = std::stof(args[++i]);
        else if (args[i] == "--time" && i + 1 < args.size()) material_time = std::stof(args[++i]);
        else if (args[i] == "--benchmark" && i + 1 < args.size()) benchmark_frames = std::stoul(args[++i]);
        else if (args[i] == "--camera-scale" && i + 1 < args.size()) camera_scale = std::stof(args[++i]);
        else if (args[i] == "--map-radius" && i + 1 < args.size()) {
            const auto value = args[++i];
            map_radius = value == "unlimited" ? 1e9f : std::stof(value);
            if (!std::isfinite(map_radius) || map_radius < 0) throw std::runtime_error("--map-radius expects a nonnegative distance or unlimited");
            if (map_radius == 0) map_radius = 1e9f;
        }
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
        std::println(stderr, "usage: nimshade [--install DIR] TAG [OUT_DIR] [--size WxH] [--warp] [--time-of-day DAYS] [--no-atmosphere] [--no-environment] [--no-sky-ambient] [--no-overhead-occlusion] [--no-interior-fog] [--no-assumed-light-state] [--print-channels] [--no-aa] [--no-taa] [--no-sky-objects] [--verify-frame-inputs] [--time SECONDS] [--channel HASH=VALUE] [--benchmark FRAMES] [--reference] [--camera-scale N] [--no-fx] [--no-distortion] [--fx-radius M] [--activity ACT] [--ai] [--ai-time S] [--ai-range M] [--ai-actors N] [--ai-visible] [--ai-debug] [--ai-frame] [--ai-frame-at DIST,HEIGHT] [--ai-strip N] [--ai-dropship-strip N] [--ai-ground-audit]");
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
    MapAtmosphere atmosphere;
    std::vector<const MapCubemap*> region_cubemaps;
    std::map<uint32_t, float> weather_channels;
    bool is_map = false;
    std::vector<MapParticle> particle_placements;
    // --ai (3.70): the activity's estimated AI simulation, its agents drawn as actors.
    std::optional<MapGameplay> ai_gameplay;
    std::unique_ptr<AiSimulation> ai_sim;
    AiActors ai_actors;
    if (const Entry* e = pm.entry(mesh_tag); e && e->reference == kClassBubbleParent) {
        // A map: the static instances within --map-radius of where the geometry is.
        auto t0 = std::chrono::steady_clock::now();
        std::vector<TagHash> activity_tables;
        if (!selected_activity.is_none()) {
            const auto entries = list_activity_maps(pm);
            const auto choice = std::find_if(entries.begin(), entries.end(), [&](const MapEntry& m) {
                return m.activity == selected_activity && m.activity_map == selected_activity_map && m.bubble == mesh_tag;
            });
            if (choice == entries.end()) throw std::runtime_error("activity/map selection does not reference this bubble");
            activity_tables = load_activity_node_tables(pm, choice->activity, choice->bubble_name, choice->activity_map);
            std::println("activity_view {} map[{}]: {} selected phase/ambient node tables", selected_activity.str(), selected_activity_map, activity_tables.size());
        } else std::println("map_view: bubble node tables only");
        MapLayout map = load_map_layout(pm, mesh_tag, activity_tables);
        is_map = true;
        // Without --eye / --look, the camera stands at the activity's first player start (estimate).
        const auto starts = find_map_starts(pm, map);
        for (const auto& s : starts)
            std::println("player start {:08X} at ({:.1f}, {:.1f}, {:.1f}) facing ({:.2f}, {:.2f}, {:.2f})", s.name, s.position[0], s.position[1],
                         s.position[2], s.forward()[0], s.forward()[1], s.forward()[2]);
        if (!starts.empty() && !eye_arg && !look_arg) {
            const auto& s = starts.front();
            const auto f = s.forward();
            eye_arg = V3{s.position[0], s.position[1], s.position[2] + kStartEyeHeight};
            look_arg = V3{eye_arg->x + f[0] * 10, eye_arg->y + f[1] * 10, eye_arg->z + f[2] * 10};
        }
        if (heat_cascade || !weather_args.empty()) {
            const MapWeather weather = load_map_weather(pm, mesh_tag);
            std::println("weather: {} controller(s)", weather.controllers.size());
            auto apply = [&](const WeatherController& c, const WeatherState& st) {
                std::println("  {} state {}", c.name, weather.name(st.name));
                for (const auto& v : weather_values(st)) weather_channels[v.first] = v.second;
            };
            for (const auto& c : weather.controllers) {
                for (const auto& [part, state] : weather_args) {
                    if (c.name.find(part) == std::string::npos && c.tag.str().find(part) == std::string::npos) continue;
                    for (const auto& st : c.states)
                        if (st.name == state) apply(c, st);
                }
            }
            if (heat_cascade) {
                if (!weather.heat_cascade_controller) throw std::runtime_error("original Heat Cascade controller not found");
                const auto& c = *weather.heat_cascade_controller;
                if (const WeatherState* st = c.strongest_state()) apply(c, *st);
                if (weather.heat_cascade_shared) std::println("  estimate: shared Outpost Heat Cascade state on this map");
            }
            for (const auto& [slot, v] : weather_channels) std::println("  global channel {} = {:.3g}", slot, v);
        }
        const auto focus = look_arg ? std::array<float, 3>{look_arg->x, look_arg->y, look_arg->z} :
                           eye_arg ? std::array<float, 3>{eye_arg->x, eye_arg->y, eye_arg->z} : map_focus(map);
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
            const MapEntities ents = load_map_entities(pm, map, entity_materials);
            auto er = map_region(ents.models, focus, map_radius);
            size_t n = 0;
            for (const auto& m : er) n += m.rows.size();
            std::println("entities: {} placements of {} models ({} entities, {} node tables); {} models / {} placements in range ({:.1f}s)",
                         ents.placements, ents.models.size(), ents.entities, ents.node_tables, er.size(), n,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - te).count());
            for (size_t i = 0; i < er.size(); ++i) {
                if (entity_range && (i < entity_range->first || i >= entity_range->second)) continue;
                if (entity_range) std::println("entity model [{}]: {} ({} placements)", i, er[i].mesh.str(), er[i].rows.size());
                region.push_back(std::move(er[i]));
            }
        }
        // The particle systems (FX) the map's entities place.
        if (!no_fx) {
            const auto tf = std::chrono::steady_clock::now();
            pm.build_hash64_index();
            particle_placements = load_map_particles(pm, map);
            std::println("fx: {} particle system placements ({:.1f}s)", particle_placements.size(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tf).count());
        }
        // The sky objects (distant scenery kilometres out): all of them, whatever the radius.
        float geometry_extent = 1;
        for (const auto& m : region) for (const auto& r : m.rows)
            geometry_extent = std::max(geometry_extent, std::hypot(r[3] - focus[0], r[7] - focus[1], r[11] - focus[2]));
        if (!no_sky_objects) {
            auto sky = load_map_sky_objects(pm, map);
            size_t n = 0;
            for (const auto& m : sky) n += m.rows.size();
            std::println("sky objects: {} placements of {} models", n, sky.size());
            for (size_t i = 0; i < sky.size(); ++i) {
                if (!sky_ranges.empty() && std::none_of(sky_ranges.begin(), sky_ranges.end(),
                                                        [&](const auto& r) { return i >= r.first && i < r.second; }))
                    continue;
                if (!sky_ranges.empty()) std::println("sky model [{}]: {} ({} placements, order {})", i, sky[i].mesh.str(), sky[i].rows.size(), sky[i].sky_order);
                region.push_back(std::move(sky[i]));
            }
        }
        PrepareCache cache;
        cache.max_texture_dim = map_texture_cap;
        cache.keep_static_lods_for_map = true;
        scene.begin(gpu);
        std::map<std::string, size_t> failures;
        double prep_s = 0, add_s = 0;
        const auto terrain = map_terrain_region(map, focus, map_radius);
        if (!no_lights) {
            map_lights = load_map_lights(pm, map);
            region_lights = map_light_region(map_lights, focus, map_radius);
            std::println("lights: {} of {} within range", region_lights.size(), map_lights.size());
        }
        if (!no_atmosphere) {
            atmosphere = load_map_atmosphere(pm, map);
            std::println("atmosphere: {}, sun path {} entries, map time of day {:.3f}", atmosphere.valid() ? atmosphere.lookups[1].str() : "none",
                         atmosphere.paths[0].size(), atmosphere.default_time);
        }
        if (!no_cubemaps) {
            map_cubemaps = load_map_cubemaps(pm, map);
            if (probe_index) {
                if (*probe_index >= map_cubemaps.size()) throw std::runtime_error("probe index out of range");
                region_cubemaps.push_back(&map_cubemaps[*probe_index]);
            } else for (const auto& probe : map_cubemaps) region_cubemaps.push_back(&probe);
            std::println("cubemap volumes: {} of {} authored volumes selected (view culling at draw time)", region_cubemaps.size(), map_cubemaps.size());
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
        size_t terrain_ok = 0, entity_models_loaded = 0;
        const std::array<float, 12> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        auto tb = std::chrono::steady_clock::now();
        std::unique_ptr<Sampler> load_sampler;
        if (const char* e = std::getenv("NIM_SAMPLE"); e && e[0] == '2') {
            Sampler::init_symbols();
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            load_sampler = std::make_unique<Sampler>();
            load_sampler->start(self);
        }
        for (size_t j = 0; j < jobs; ++j) {
            const bool is_terrain = j < terrain.size();
            const std::string what = is_terrain ? terrain[j]->tag.str() : region[j - terrain.size()].mesh.str();
            try {
                // Diagnostics: NIM_MESH_SKIP=TAG,... leaves those models out; NIM_MESH_LIST prints the entity models.
                if (const char* skip = std::getenv("NIM_MESH_SKIP"); skip && !is_terrain && std::strstr(skip, what.c_str())) continue;
                if (const char* list = std::getenv("NIM_MESH_LIST"); list && !is_terrain && (list[0] == 'a' || j - terrain.size() >= static_count))
                    std::println("mesh {} at ({:.1f} {:.1f} {:.1f})", what, region[j - terrain.size()].rows.empty() ? 0.0f : region[j - terrain.size()].rows[0][3],
                                 region[j - terrain.size()].rows.empty() ? 0.0f : region[j - terrain.size()].rows[0][7],
                                 region[j - terrain.size()].rows.empty() ? 0.0f : region[j - terrain.size()].rows[0][11]);
                if (!prepared[j]) throw std::runtime_error(errors[j]);
                if (is_terrain) scene.add_mesh(gpu, std::move(*prepared[j]), layouts, std::span(&identity, 1));
                else {
                    const auto& model = region[j - terrain.size()];
                    scene.add_mesh(gpu, std::move(*prepared[j]), layouts, model.rows, model.sky_order);
                    if (j - terrain.size() >= static_count && model.sky_order.empty()) ++entity_models_loaded;
                }
                prepared[j].reset();
                terrain_ok += is_terrain;
            } catch (const std::exception& ex) {
                if (++failures[std::string(ex.what()).substr(0, 100)] == 1 || std::getenv("NIM_LIST_SKIPPED"))
                    std::println("  {}{}: {}", is_terrain ? "terrain " : "", what, ex.what());
            }
        }
        if (!no_map_decals) {
            const auto decals = load_map_decals(pm, map);
            size_t ready = 0;
            for (const auto& d : decals) {
                try {
                    scene.add_mesh(gpu, prepare_map_decal(pm, d, layouts, &cache), layouts, std::span(&d.rows, 1));
                    ++ready;
                } catch (const std::exception& ex) {
                    ++failures[ex.what()];
                }
            }
            std::println("map decals: {} of {} prepared", ready, decals.size());
        }
        add_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - tb).count();
        if (load_sampler) load_sampler->stop_and_print();
        std::println("terrain: {} of {} patches in range ({} in the map)", terrain_ok, terrain.size(), map.terrains.size());
        std::println("placed entity models added: {}", entity_models_loaded);
        scene.finish(gpu, mesh_tag, V3{focus[0], focus[1], focus[2]}, std::min(map_radius, geometry_extent) * 0.6f);
        std::println("loaded {} meshes / {} instances in {:.1f}s", scene.mesh_count(), scene.instance_count(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::println("  prepare {:.1f}s, add {:.1f}s", prep_s, add_s);
        for (const auto& [why, n] : failures) std::println("  skipped {}: {}", n, why);
        if (ai) {
            if (selected_activity.is_none()) throw std::runtime_error("--ai needs --activity");
            const auto ta = std::chrono::steady_clock::now();
            const ActorSources sources = index_actor_sources(pm);
            ai_gameplay = load_map_gameplay(pm, selected_activity);
            std::vector<MotionSet> motions;
            std::vector<ActorCharacter> characters;
            std::map<uint32_t, size_t> first;
            for (const auto& ch : ai_gameplay->characters) {
                if (const auto it = first.find(ch.entity.value); it != first.end()) {
                    motions.push_back(motions[it->second]);
                    characters.push_back(characters[it->second]);
                    continue;
                }
                first[ch.entity.value] = motions.size();
                motions.push_back(build_motion_set(pm, ch.entity, 30.0f));
                characters.push_back(prepare_actor_character(pm, sources, motions.back()));
                const auto& c = characters.back();
                std::println("ai character {} {}: model {} clips {} {}", ch.entity.str(), ch.name_estimate, c.model.is_none() ? "-" : c.model.str(),
                             c.clips.size(), c.error);
            }
            AiParams params;
            params.player_invisible = !ai_visible;
            ai_sim = std::make_unique<AiSimulation>(*ai_gameplay, params, 1, motions);
            const V3 eye = eye_arg ? *eye_arg : V3{focus[0], focus[1], focus[2]};
            const std::array<float, 3> player{eye.x, eye.y, eye.z};
            {
                // Open floor of the firing areas the nearby squads use (3.70).
                const auto tw = std::chrono::steady_clock::now();
                std::set<size_t> wanted;
                for (const auto& a : ai_sim->agents())
                    if (ai_ground_audit || std::hypot(a.position[0] - eye.x, a.position[1] - eye.y) < ai_range + 30)
                        if (const auto ob = ai_gameplay->squads[a.squad].objective)
                            for (const auto fa : ai_gameplay->objectives[*ob].firing_areas) wanted.insert(fa);
                SunShadow walk_view;
                size_t open = 0, cells = 0;
                for (const auto fa : wanted) {
                    const auto spawns = squad_spawn_points(*ai_gameplay, fa);
                    auto grid = build_walk_grid(gpu, scene, ai_gameplay->firing_areas[fa], walk_view, 0.25f, spawns);
                    open += grid.open_count(), cells += grid.open.size();
                    ai_sim->set_walk_grid(fa, std::move(grid));
                }
                // Ground around the spawn and drop points the grids miss (3.72).
                add_ground_patches(gpu, scene, *ai_sim, walk_view);
                std::println("ai walk grids: {} firing areas, {:.0f} m2 open, {} ground patches in {:.1f}s", wanted.size(), double(open) * 0.0625,
                             ai_sim->ground_patches(), std::chrono::duration<double>(std::chrono::steady_clock::now() - tw).count());
            }
            std::vector<bool> characters_drawable;
            for (const auto& c : characters) characters_drawable.push_back(c.mesh != nullptr);
            ai_actors.build(gpu, scene, layouts, std::move(characters), *ai_gameplay, *ai_sim, ai_per_character);
            ai_actors.apply_radii(*ai_sim);
            std::vector<std::array<float, 3>> ai_start;
            for (const auto& a : ai_sim->agents()) ai_start.push_back(a.position);
            const auto ts = std::chrono::steady_clock::now();
            for (float t = 0; t < ai_time; t += 0.1f) ai_sim->step(0.1f, player);
            const auto& ps = ai_sim->path_stats();
            std::println("ai steps: {:.0f}s simulated in {:.2f}s; goals: {} straight, {} by path search, {} without a path, {} deferred", ai_time,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count(), ps.straight, ps.found, ps.failed, ps.deferred);
            // The UESC dropship (3.71): the model of the dropship squads' spawner nodes.
            for (const auto& sq : ai_gameplay->squads)
                if (const auto p = std::ranges::find(sq.positions, 0x80ACA5E7u, [](const MapGameplayPoint& q) { return q.model.value; });
                    p != sq.positions.end()) {
                    auto ship = prepare_actor_dropship(pm, sources, p->model);
                    std::println("ai dropship {}: {} arrivals, {} departures{}{}", p->model.str(), ship.arrivals.size(), ship.departures.size(),
                                 ship.error.empty() ? "" : ", ", ship.error);
                    ai_actors.build_dropships(gpu, scene, layouts, std::move(ship), 3);
                    break;
                }
            std::println("ai dropships: {} squads arrive by ship", ai_sim->dropships().size());
            if (ai_ship_strip) {
                std::vector<std::pair<float, size_t>> order;
                for (size_t i = 0; i < ai_sim->dropships().size(); ++i)
                    if (ai_sim->dropship_state(i).phase == AiDropshipPhase::Waiting && ai_sim->dropships()[i].members.size() >= 3) {
                        const auto& d = ai_sim->dropships()[i];
                        order.push_back({std::hypot(d.position[0] - eye.x, d.position[1] - eye.y), i});
                    }
                if (order.empty()) throw std::runtime_error("--ai-dropship-strip: no waiting dropship");
                ai_ship = std::ranges::min(order).second;
                const auto& d = ai_sim->dropships()[*ai_ship];
                // Behind the ship's rear bay and to its side, looking at the exit.
                const float c = std::cos(d.yaw), s = std::sin(d.yaw);
                auto at = [&](float x, float y, float z) { return V3{d.position[0] + c * x - s * y, d.position[1] + s * x + c * y, d.position[2] + z}; };
                eye_arg = at(-24, 12, 5);
                look_arg = at(-5, 0, 2);
                ai_sim->call_dropship(*ai_ship);
                std::println("ai dropship strip: ship {} at ({:.1f} {:.1f} {:.1f}) yaw {:.2f}, {} members", *ai_ship, d.position[0], d.position[1],
                             d.position[2], d.yaw, d.members.size());
            }
            // Run the clips' crossfades to rest over the last half second.
            for (int k = 0; k < 5; ++k) {
                ai_sim->step(0.1f, player);
                ai_actors.update(scene, *ai_sim, player, 0.1f, ai_range);
            }
            std::println("ai: {} agents, {} drones after {:.0f}s; {} actor slots, {} shown within {:.0f} m ({:.1f}s)", ai_sim->agents().size(),
                         ai_sim->drones().size(), ai_sim->time(), ai_actors.slots(), ai_actors.shown(), ai_range,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - ta).count());
            {
                std::vector<std::pair<float, size_t>> order;
                for (size_t k = 0; k < ai_sim->agents().size(); ++k) {
                    const auto& a = ai_sim->agents()[k];
                    order.push_back({std::hypot(a.position[0] - eye.x, a.position[1] - eye.y, a.position[2] - eye.z), k});
                }
                std::ranges::sort(order);
                for (size_t k = 0; k < std::min<size_t>(order.size(), 8); ++k) {
                    const auto& a = ai_sim->agents()[order[k].second];
                    std::println("  ai agent {} at ({:.1f} {:.1f} {:.1f}) {:.1f} m, yaw {:.2f}, {}", ai_gameplay->characters[a.character].name_estimate,
                                 a.position[0], a.position[1], a.position[2], order[k].first, a.yaw, a.motion.locomotion ? "moving" : "idle");
                }
            }
            for (const auto& n : ai_actors.notes()) std::println("  ai note: {}", n);
            if (ai_ground_audit) {
                // Each drawable agent nearest the camera in turn (the camera at its head).
                ai_actors.set_ground_checks(true);
                std::vector<AiActors::GroundCheck> all;
                for (size_t k = 0; k < ai_sim->agents().size(); ++k) {
                    const auto& a = ai_sim->agents()[k];
                    if (!a.active || a.character >= characters_drawable.size() || !characters_drawable[a.character]) continue;
                    ai_actors.update(scene, *ai_sim, {a.position[0], a.position[1], a.position[2] + 1.0f}, 0.0f, ai_range);
                    for (const auto& g : ai_actors.ground_checks())
                        if (g.agent == k) all.push_back(g);
                }
                ai_actors.set_ground_checks(false);
                size_t off_grid = 0, sink10 = 0, sink30 = 0, inside10 = 0;
                for (const auto& g : all) off_grid += !g.on_grid, sink10 += g.sink > 0.1f, sink30 += g.sink > 0.3f, inside10 += g.inside > 0.1f;
                std::println("ai ground audit: {} agents, {} off the walk grids, sink > 0.1 m {}, > 0.3 m {}, body > 10% under a surface {}", all.size(),
                             off_grid, sink10, sink30, inside10);
                {
                    size_t moved = 0, active = 0;
                    for (size_t k = 0; k < ai_sim->agents().size(); ++k) {
                        const auto& a = ai_sim->agents()[k];
                        if (!a.active) continue;
                        ++active;
                        moved += std::hypot(a.position[0] - ai_start[k][0], a.position[1] - ai_start[k][1]) > 1.5f;
                    }
                    size_t overlapped = 0;
                    for (size_t k = 0; k < ai_sim->agents().size(); ++k)
                        for (size_t j = k + 1; j < ai_sim->agents().size(); ++j) {
                            const auto &a = ai_sim->agents()[k], &b = ai_sim->agents()[j];
                            overlapped += a.active && b.active && std::abs(a.position[2] - b.position[2]) < 1 &&
                                          std::hypot(a.position[0] - b.position[0], a.position[1] - b.position[1]) < 0.5f * (ai_sim->radius(a) + ai_sim->radius(b));
                        }
                    std::println("  moved more than 1.5 m: {} of {} active; {} pairs closer than half their radii", moved, active, overlapped);
                    std::map<std::string, float> radii;
                    for (const auto& a : ai_sim->agents()) radii[ai_gameplay->characters[a.character].name_estimate] = ai_sim->radius(a);
                    for (const auto& [name, r] : radii) std::println("  radius {} {:.2f} m", name, r);
                }
                std::ranges::sort(all, [](const auto& x, const auto& y) { return std::max(x.sink, x.inside) > std::max(y.sink, y.inside); });
                for (size_t k = 0; k < std::min<size_t>(all.size(), 25); ++k) {
                    const auto& g = all[k];
                    const auto& a = ai_sim->agents()[g.agent];
                    std::println("  agent {} {} at ({:.1f} {:.1f} {:.1f}) floor {:.2f} sink {:.2f} inside {:.2f}{}{}", g.agent,
                                 ai_gameplay->characters[a.character].name_estimate, a.position[0], a.position[1], a.position[2], g.floor, g.sink,
                                 g.inside, g.on_grid ? "" : " off-grid", a.motion.locomotion ? " moving" : "");
                }
            }
            // --ai-frame: look at the drawable agent nearest the given view from open floor in
            // front of it (a point inside one of its squad's firing areas), 4 m away, at eye height.
            if (ai_frame) {
                // From open floor with a clear straight line at body height to the agent (walk grids).
                std::vector<std::pair<float, size_t>> order;
                for (size_t k = 0; k < ai_sim->agents().size(); ++k) {
                    const auto& a = ai_sim->agents()[k];
                    if (a.active && a.character < characters_drawable.size() && characters_drawable[a.character]) {
                        const float d = std::hypot(a.position[0] - eye.x, a.position[1] - eye.y);
                        const bool walking = std::hypot(a.velocity[0], a.velocity[1]) > 0.5f &&
                                             std::hypot(a.goal[0] - a.position[0], a.goal[1] - a.position[1]) > 4.0f;
                        if (ai_strip && !walking) continue;
                        order.push_back({d, k});
                    }
                }
                std::ranges::sort(order);
                std::optional<std::pair<V3, V3>> best;
                for (const auto& [unused, k] : order) {
                    const auto& a = ai_sim->agents()[k];
                    const auto ob = ai_gameplay->squads[a.squad].objective;
                    if (!ob) continue;
                    for (const auto fa : ai_gameplay->objectives[*ob].firing_areas) {
                        const WalkGrid* grid = ai_sim->walk_grid(fa);
                        if (!grid || !grid->walkable(a.position[0], a.position[1])) continue;
                        for (int s = 0; s < 16 && !best; ++s) {
                            const float ang = a.yaw + float(s) * 0.3927f;
                            const std::array<float, 3> q{a.position[0] + ai_frame_distance * std::cos(ang), a.position[1] + ai_frame_distance * std::sin(ang), a.position[2]};
                            if (!grid->walkable(q[0], q[1]) || !grid->clear(q, a.position)) continue;
                            if (std::ranges::any_of(ai_sim->agents(), [&](const AiAgent& o) { return std::hypot(o.position[0] - q[0], o.position[1] - q[1]) < 1.5f; }))
                                continue;
                            best = {V3{q[0], q[1], q[2] + ai_frame_height}, V3{a.position[0], a.position[1], a.position[2] + 1.0f}};
                        }
                        if (best) break;
                    }
                    if (best) break;
                }
                if (!best) throw std::runtime_error("--ai-frame: no drawable agent with open floor around it");
                eye_arg = best->first;
                look_arg = best->second;
                for (const auto& [unused, k] : order)
                    if (std::abs(ai_sim->agents()[k].position[0] - best->second.x) < 1e-3f && std::abs(ai_sim->agents()[k].position[1] - best->second.y) < 1e-3f)
                        ai_subject = k;
                ai_actors.update(scene, *ai_sim, {eye_arg->x, eye_arg->y, eye_arg->z}, 0.0f, ai_range);
                std::println("ai frame: eye ({:.1f} {:.1f} {:.1f}) look ({:.1f} {:.1f} {:.1f})", eye_arg->x, eye_arg->y, eye_arg->z, look_arg->x,
                             look_arg->y, look_arg->z);
            }
            for (const auto& e : ai_estimates(params)) std::println("  ai estimate {} = {:.2f}: {}", e.name, e.value, e.note);
        }
    } else if (const Entry* e = pm.entry(mesh_tag); e && e->reference == kClassDynamicModel) {
        auto t0 = std::chrono::steady_clock::now();
        auto mats = index_dynamic_model_materials(pm);
        ModelAssemblyIndex assemblies;
        ModelMotionIndex motions;
        ModelRigIndex rigs;
        if (!single_model || clip_tag || list_clips) rigs = index_model_skeletons(pm, mats, single_model ? nullptr : &assemblies, list_clips || clip_tag ? &motions : nullptr);
        auto it = mats.find(mesh_tag.value);
        std::println("dynamic model: {} components indexed in {:.1f}s, materials {}", mats.size(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                     it == mats.end() ? "none" : it->second.component.str());
        auto mesh = prepare_entity_preview(pm, mesh_tag, mats, assemblies, -1, &channel_values);
        // FBX texture bakes (tools/fbx): dump each opaque part's original VS/PS, then draw it through
        // <dir>/<part>.gs.dxil, a geometry shader that moves every triangle into the atlas.
        if (const char* dir = std::getenv("NIM_BAKE_GS_DIR")) {
            std::erase_if(mesh.groups, [](const auto& g) { return g.stage != 0; });
            for (auto& g : mesh.groups) {
                const fs::path base(dir);
                std::ofstream(base / std::format("{}.vs.dxil", g.info.part), std::ios::binary)
                    .write(reinterpret_cast<const char*>(g.vs.data()), std::streamsize(g.vs.size()));
                std::ofstream(base / std::format("{}.ps.dxil", g.info.part), std::ios::binary)
                    .write(reinterpret_cast<const char*>(g.ps.data()), std::streamsize(g.ps.size()));
                std::println("bake part {} topology {} first index {} count {}", g.info.part, g.part.primitive_type,
                             g.part.index_start, g.part.index_count);
                std::ifstream f(base / std::format("{}.gs.dxil", g.info.part), std::ios::binary);
                if (f) g.bake_gs.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
                else if (!std::getenv("NIM_BAKE_DUMP_ONLY"))
                    throw std::runtime_error(std::format("missing bake geometry shader for part {}", g.info.part));
            }
            if (std::getenv("NIM_BAKE_DUMP_ONLY")) return 0;
        }
        for (const auto& part : mesh.attachments) std::println("assembled part {}", part.tag.str());
        std::vector<ModelSkinPose> poses;
        if (list_clips) {
            const auto all = index_animation_clips(pm);
            if (const auto anim = prepare_preview_animation(pm, mesh, rigs, all, {}, {}, &motions)) {
                std::println("clip source: {}", anim->authored_clips ? "entity animation bank" : "compatible rig");
                for (const auto& c : anim->clips)
                    std::println("clip {} ({} frames){}{}", c.tag.str(), c.frames, c.shell_specific ? " [shell]" : "", c.shared_bones ? " [shared]" : "");
            } else std::println("no compatible clips");
        }
        if (clip_tag) {
            AnimationClip clip = load_animation_clip(pm, *clip_tag);
            const ClipInfo info{clip.tag, uint16_t(clip.frame_count), uint16_t(clip.bone_count), clip.bone_groups, clip.name_hash};
            const auto all = index_animation_clips(pm);
            auto anim = prepare_preview_animation(pm, mesh, rigs, std::span(&info, 1), {}, all, &motions);
            if (!anim || anim->clips.empty())
                throw std::runtime_error(std::format("clip {} is incompatible with the preview's skinned models", clip.tag.str()));
            restrict_preview_clip(clip, anim->clips.front());
            std::println("animation skeleton {} rig {}", anim->skeleton.component.str(), anim->rig.component.str());
            if (anim->clips.front().shared_bones) std::println("retargeted {} shared body bones", anim->clips.front().shared_bones);
            prepare_preview_equipment(pm, mesh, *anim, rigs, all, mats);
            for (const auto& option : anim->equipment) std::println("equipment candidate {}", option.model.str());
            poses = anim->poses(clip, clip_frame, equipment_model);
            if (!equipment_model.is_none() && std::ranges::none_of(poses, [](const auto& pose) { return bool(pose.placement); }))
                throw std::runtime_error(std::format("equipment {} is incompatible with this preview clip", equipment_model.str()));
            for (const auto& pose : poses)
                if (pose.placement) std::println("animation equipment {}", pose.model.str());
            std::println("posed model {} and {} linked parts with clip {} ({}, {} frames{}) at frame {}{}",
                         anim->model.str(), poses.size() - 1, clip.tag.str(), clip_codec_name(clip.codec), clip.frame_count,
                         is_additive(anim->rig, clip) ? ", additive" : "", clip_frame,
                         anim->inferred_binding ? " (estimated rig binding)" : "");
        }
        scene.upload(gpu, std::move(mesh), layouts);
        scene.load_animation_equipment(gpu, layouts, poses);
        if (!poses.empty()) scene.set_skin_poses(std::move(poses));
        for (const auto& n : scene.notes()) std::println("  note: {}", n);
    } else {
        scene.load(pm, gpu, mesh_tag, layouts);
    }
    std::println("mesh {}: {} material group(s) at lod {}, radius {:.2f}", mesh_tag.str(), scene.groups().size(),
                 scene.lod(), scene.radius());
    if (!is_map) for (const auto& g : scene.groups()) {
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
    // Match the interactive map camera: unlimited range must not move the near
    // plane tens of metres away or clip the geometry around an explicit eye.
    if (is_map) view.near_z = 0.05f;
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
    LightingParams lp;
    if (sun_arg >= 0) lp.sun_intensity = sun_arg;
    if (sky_arg >= 0) lp.sky_intensity = sky_arg;
    lp.time = material_time;
    lp.optimize = !reference;
    lp.ssao = !no_ssao;
    lp.volumetrics = !no_volumetrics;
    lp.ssr = !no_ssr;
    lp.atmosphere = !no_atmosphere;
    lp.environment = !no_environment;
    lp.sky_ambient = !no_sky_ambient;
    lp.overhead_occlusion = !no_overhead_occlusion;
    lp.interior_fog = !no_interior_fog;
    lp.assume_lights_on = assume_lights_on;
    lp.froxel_fog = !no_froxels;
    lp.froxel_noise = !no_froxel_noise;
    lp.froxel_temporal = !no_froxel_history;
    lp.probe_distance_fade = probe_distance_fade;
    lp.time_of_day = time_of_day;
    lp.day_night_state = day_night_state;
    lp.post = !no_post;
    lp.auto_exposure = !no_auto_exposure;
    lp.bloom = !no_bloom;
    lp.antialias = !no_aa;
    lp.temporal_aa = !no_taa;
    lp.exposure = exposure_arg;
    auto shadow_pass = [&](ID3D12GraphicsCommandList* list, const LightingParams& p) {
        if (!shadows) return;
        shadow.fit_view(sun_direction(p, atmosphere.valid() ? &atmosphere : nullptr), vm.eye, view.target, scene.center(), scene.radius(), is_map);
        scene.render_sun_shadow(gpu, list, shadow, is_map);
    };
    shadow_pass(cl, lp);
    scene.render_gbuffer(gpu, cl, gb, vm);
    for (const auto& [hash, channel] : scene.channels()) {
        const char* name = object_channel_name(hash);
        std::println("  channel {:08X} {}{}{} = ({:.4g}, {:.4g}, {:.4g}, {:.4g})", hash, name ? name : "",
                     channel.vs ? " VS" : "", channel.ps ? " PS" : "", channel.value[0], channel.value[1], channel.value[2], channel.value[3]);
    }
    gpu.flush();
    // Lit output is produced only by the original game pipelines below.
    game = true;

    // Transparent-only meshes need the game's shading pass to have something to blend over.
    if (scene.has_transparent()) {
        std::println("transparent parts: {} (blended after the game's shading pass)", scene.transparent_count());
        game = true;
    }
    GameLighting gl;
    ParticleScene particles;
    if (fx_radius >= 0) particles.radius = fx_radius;
    float fx_delta = 1.0f / 60;
    if (!particle_placements.empty()) {
        const auto tp = std::chrono::steady_clock::now();
        particles.load(pm, gpu, particle_placements);
        gpu.flush();
        for (const auto& n : particles.notes()) std::println("  {}", n);
        std::println("fx pipelines ready ({:.1f}s)", std::chrono::duration<double>(std::chrono::steady_clock::now() - tp).count());
        if (particles.loaded()) game = true;
    }
    auto prepare_game_frame = [&]() {
        gl.prepare_frame(gpu, gb, lp);
        scene.set_exposure(gl.exposure());
        scene.set_global_channel_values(gl.externs().channels);
        scene.set_global_channels(gl.world_channels());
        scene.set_material_time_of_day(gl.time_of_day(lp));
    };
    auto transparents = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
        const auto& inputs = gl.externs();
        if (verify_frame_inputs) {
            const auto depth = inputs.texture(ext::Deferred, 0x98);
            if (!depth.resource || depth.format != DXGI_FORMAT_R32G32_FLOAT ||
                depth.resource->GetDesc().Width != gb.width() || depth.resource->GetDesc().Height != gb.height() ||
                inputs.get_v(ext::Deferred, 0x00) != float4{0, 1, 0, 0})
                throw std::runtime_error("Transparent pass requires current linear depth with a matching decode");
        }
        if (scene.has_transparent()) scene.render_transparent(gpu, c, gb, hdr, vm, inputs);
        if (particles.loaded()) particles.render(gpu, c, gb, hdr, vm, inputs, fx_delta);
    };
    auto distortions = [&](ID3D12GraphicsCommandList* c, uint32_t w, uint32_t h) {
        scene.render_distortion(gpu, c, w, h, vm, gl.externs());
    };
    auto render_game = [&](ID3D12GraphicsCommandList* commands) {
        const bool fx = particles.loaded();
        gl.render(gpu, commands, gb, vm, lp,
                  scene.has_transparent() || fx ? GameLighting::TransparentFn(transparents) : nullptr,
                  &shadow, scene.has_visible_light_grid() || (fx && particles.has_emitters_near(vm.eye)),
                  !no_distortion && scene.has_visible_distortion() ? GameLighting::DistortionFn(distortions) : nullptr);
    };
    if (game) {
        gl.init(pm, gpu);
        if (!is_map && !no_environment) lp.entity_preview = gl.set_entity_preview(pm, gpu);
        if (rain.valid) gl.set_overhead_occlusion(&rain);
        if (!weather_channels.empty()) gl.set_global_channels(weather_channels);
        std::println("game shading: technique {}", gl.shading_technique().str());
        if (atmosphere.valid()) {
            gl.set_atmosphere(pm, gpu, &atmosphere);
            const V3 s = gl.sun_toward(lp);
            std::println("atmosphere {}: sun ({:.3f}, {:.3f}, {:.3f})", gl.has_atmosphere() ? "loaded" : "not loaded", s.x, s.y, s.z);
            for (const auto& n : gl.atmosphere_notes()) std::println("  {}", n);
        }
        if (!region_lights.empty()) {
            auto tl = std::chrono::steady_clock::now();
            gl.set_lights(pm, gpu, region_lights);
            std::println("local lights: {} prepared in {:.1f}s", gl.light_count(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tl).count());
            for (const auto& n : gl.light_notes()) std::println("  {}", n);
        }
        if (!region_cubemaps.empty()) {
            auto tc = std::chrono::steady_clock::now();
            gl.set_cubemaps(pm, gpu, region_cubemaps, dump_probe_mask);
            std::println("cubemap volumes: {} prepared in {:.1f}s", gl.cubemap_count(),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count());
            for (const auto& n : gl.cubemap_notes()) std::println("  {}", n);
        }
        for (const auto& n : gl.notes()) std::println("  {}", n);
        // The materials read the same global channels as the lighting (the map environment).
        scene.set_global_channel_values(gl.update_global_channels(lp));
        scene.set_global_channels(gl.world_channels());
        scene.set_material_time_of_day(gl.time_of_day(lp));
        if (gl.environment_channels()) {
            std::println("map environment: {} global channels at time of day {:.3f}", gl.environment_channels(),
                         gl.time_of_day(lp));
        }
        if (print_channels) {
            const auto& ch = gl.externs().channels;
            const auto& ids = gl.externs().channel_ids;
            for (size_t i = 0; i < ch.size(); ++i)
                std::println("  channel {:02X} {:08X} = ({:.4g}, {:.4g}, {:.4g}, {:.4g})", i, i < ids.size() ? ids[i] : 0u,
                             ch[i][0], ch[i][1], ch[i][2], ch[i][3]);
        }
        std::println("GPU memory before lighting: {:.1f} MiB used / {:.1f} MiB budget",
                     double(gpu.vram_usage()) / (1 << 20), double(gpu.vram_budget()) / (1 << 20));
        if (no_light_shadows) gl.set_light_shadows(false);
        if (frame_light_shadows && !no_light_shadows) {
            // The GUI's frame: sun shadow, a few light maps, G-buffer and lighting in one frame's
            // upload space, repeated until every light on screen has its map.
            size_t frames = 0, drawn = 0;
            for (;; ++frames) {
                gpu.begin_frame(uint32_t(frames));
                prepare_game_frame();
                shadow_pass(gpu.immediate(), lp);
                const auto maps = gl.light_shadows_to_render(gpu, gpu.immediate(), vm, scene.item_count(), frame_light_shadows);
                for (SunShadow* s : maps) scene.render_shadow(gpu, gpu.immediate(), *s);
                scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
                render_game(gpu.immediate());
                gpu.flush();
                drawn += maps.size();
                if (maps.empty()) break;
            }
            std::println("light shadow maps: {} drawn over {} frames of {} per frame", drawn, frames, frame_light_shadows);
            gpu.begin_frame(0);
        } else {
            int light_maps_reported = 0;
            const auto maps = gl.light_shadows_to_render(gpu, gpu.immediate(), vm, scene.item_count(), SIZE_MAX);
            for (SunShadow* s : maps) {
                scene.render_shadow(gpu, gpu.immediate(), *s);
                gpu.flush();
                gpu.begin_frame(0);  // flushed: the transient space is free for the next map
                if (s->valid && light_maps_reported++ < 4) {
                    const auto d = gpu.read_texture(s->depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 4);
                    const float* f = reinterpret_cast<const float*>(d.data());
                    size_t n = d.size() / 4, covered = 0;
                    float lo = 1e9f, hi = -1e9f;
                    for (size_t i = 0; i < n; ++i) { lo = std::min(lo, f[i]); hi = std::max(hi, f[i]); covered += f[i] > 0; }
                    std::println("  light shadow map at ({:.1f}, {:.1f}, {:.1f}): depth {}..{}, covered {:.1f}%", s->vm.eye.x, s->vm.eye.y, s->vm.eye.z, lo, hi,
                                 100.0 * double(covered) / double(n));
                } else if (!s->valid) {
                    std::println("  light shadow map not drawn (no opaque scene items)");
                }
            }
            if (!maps.empty()) {
                std::println("light shadow maps: {} drawn", maps.size());
            }
        }
        prepare_game_frame();
        scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
        render_game(gpu.immediate());
        gpu.flush();
        // Screen-space reflections trace the previous frame's image: one more frame gives them one.
        if (lp.ssr && !gl.ssr_drawn()) {
            gpu.begin_frame(1);
            prepare_game_frame();
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
        }
        std::println("screen-space reflections: {}", gl.ssr_drawn() ? "traced" : "not drawn");
        // Auto exposure is read back from the previous frame: render again until it has settled
        // (the first measurement is taken as is), with the materials at the same exposure.
        for (int k = 0; k < 3 && lp.post && lp.auto_exposure && gl.post_ready(); ++k) {
            gpu.begin_frame(uint32_t(k + 1));  // the previous frame was flushed: its transient space is free
            prepare_game_frame();
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
        }
        // TAA: a still camera converges over the jitter sequence (16 frames: two passes of 8).
        float stationary_motion_max = 0;
        for (int k = 0; k < 16 && gl.temporal_aa_on(lp); ++k) {
            gpu.begin_frame(uint32_t(k));  // the previous frame was flushed: its transient space is free
            prepare_game_frame();
            vm = gl.temporal_view(view, lp);
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
            if (verify_static_motion) {
                const auto motion = gpu.read_texture(gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 8);
                auto half = [](uint16_t v) {
                    const int ex = (v >> 10) & 31;
                    const float m = float(v & 1023) / 1024;
                    const float f = ex ? std::ldexp(1 + m, ex - 15) : std::ldexp(m, -14);
                    return v & 0x8000 ? -f : f;
                };
                const auto* h = reinterpret_cast<const uint16_t*>(motion.data());
                float worst = 0;
                for (size_t n = 0; n < motion.size() / 2; n += 4) {
                    const float x = half(h[n]) * float(width), y = half(h[n + 1]) * float(height);
                    if (!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("non-finite stationary motion");
                    worst = std::max(worst, std::sqrt(x * x + y * y));
                }
                std::println("stationary motion frame {}: max {} pixels", k, worst);
                stationary_motion_max = std::max(stationary_motion_max, worst);
            }
        }
        if (verify_static_motion && (!gl.temporal_aa_on(lp) || stationary_motion_max > 0.1f))
            throw std::runtime_error(std::format("stationary TAA motion failed: {} pixels", stationary_motion_max));
        std::println("distortion draws: {}", scene.distortion_draws());
        std::println("world transparent draws: {}", scene.world_transparent_draws());
        if (is_map) std::println("sky transparent draws: {}", scene.sky_transparent_draws());
        if (scene.has_visible_light_grid())
            std::println("transparent light grid: {}", gl.light_grid_drawn() ? "ready" : "unavailable");
        for (size_t k = 0; k < scene.notes().size() && k < 20; ++k) std::println("  scene: {}", scene.notes()[k]);
        std::println("frame use: {} descriptors, {:.1f} MB upload", gpu.transient_used(), double(gpu.dynamic_used()) / (1 << 20));
        std::println("view tables: {} unique, {} reused", gpu.transient_table_count(), gpu.transient_table_hits());
        if (lp.post && gl.post_ready())
            std::println("post: exposure {:.3f} (scene luminance {:.4f}), bloom {}", gl.exposure(), gl.scene_luminance(), lp.bloom ? "on" : "off");
        if (lp.ssao && gl.ambient_occlusion()) {
            auto px = gpu.read_texture(gl.ambient_occlusion(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8);
            const uint16_t* hv = reinterpret_cast<const uint16_t*>(px.data());
            auto half = [](uint16_t v) { const int ex = (v >> 10) & 31; const float m = float(v & 1023) / 1024.0f; return ex ? std::ldexp(1.0f + m, ex - 15) : std::ldexp(m, -14); };
            double sum = 0; float lo = 1e9f; size_t n = px.size() / 8, dark = 0;
            for (size_t k = 0; k < n; ++k) { const float a = half(hv[k * 4]); sum += a; lo = std::min(lo, a); dark += a < 0.9f; }
            std::println("ssao: mean {:.3f}, min {:.3f}, {:.1f}% below 0.9", sum / double(n), lo, 100.0 * double(dark) / double(n));
        }
        if (gl.cubemap_count()) {
            std::println("cubemap volumes drawn: {}", gl.cubemaps_drawn());
            for (const auto& n : gl.cubemap_notes()) std::println("  {}", n);
        }
        if (gl.light_count() || gl.cubemap_count()) {
            std::println("local lights drawn: {}, light shafts {} of {}", gl.lights_drawn(), gl.volumetrics_drawn(), gl.volumetric_lights());
            std::println("shadowed lights drawn: {}, shadowed light shafts: {}", gl.light_shadows_used(), gl.volumetric_shadows_used());
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
        for (const auto& [what, why] : particles.estimates()) std::println("  fx  {}  [{}]", what, why);
        if (particles.loaded()) {
            std::println("fx: {} of {} emitters simulated", particles.active_count(), particles.emitter_count());
            for (const auto& n : particles.notes()) std::println("  {}", n);
            if (std::getenv("NIM_FX_DUMP")) std::print("{}", particles.debug_dump(gpu));
        }
        auto composite = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
        write_png(out_dir / "composite_game.png", width, height, composite);
        if (ai_sim) {
            AiOverlayOptions options;
            options.debug = ai_debug;
            options.range = ai_range;
            const auto overlay = build_ai_overlay(*ai_sim, *ai_gameplay, vm, float(width), float(height), options);
            draw_overlay(composite, width, height, overlay);
            write_png(out_dir / "composite_ai.png", width, height, composite);
            std::println("ai overlay: {} lines, {} rects, {} labels", overlay.lines.size(), overlay.rects.size(), overlay.texts.size());
        }
        // --ai-dropship-strip: the called ship from behind its bay, one frame per 0.5 s.
        for (uint32_t k = 0; ai_sim && ai_ship && k < ai_ship_strip; ++k) {
            const std::array<float, 3> player{view.eye.x, view.eye.y, view.eye.z};
            for (int sub = 0; sub < 5; ++sub) {
                ai_sim->step(0.1f, player);
                ai_actors.update(scene, *ai_sim, player, 0.1f, ai_range);
            }
            vm = make_view(view);
            gpu.begin_frame(k % 2);
            prepare_game_frame();
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
            write_png(out_dir / std::format("ship_{:02}.png", k), width, height,
                      gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
            const auto state = ai_sim->dropship_state(*ai_ship);
            size_t out = 0;
            for (const auto m : ai_sim->dropships()[*ai_ship].members) out += ai_sim->agents()[m].active;
            std::println("ship {}: phase {} {:.2f}s, {} of {} members out, {} ships drawn, {} actors", k, int(state.phase), state.seconds, out,
                         ai_sim->dropships()[*ai_ship].members.size(), ai_actors.ships_shown(), ai_actors.shown());
        }
        // --ai-strip: the camera follows the agent from its side, one frame per 0.1 s.
        for (uint32_t k = 0; ai_sim && ai_subject && k < ai_strip; ++k) {
            const std::array<float, 3> player{view.eye.x, view.eye.y, view.eye.z};
            ai_sim->step(0.1f, player);
            const auto& a = ai_sim->agents()[*ai_subject];
            const float side = a.yaw + 1.5708f;
            view.eye = {a.position[0] + ai_frame_distance * std::cos(side), a.position[1] + ai_frame_distance * std::sin(side), a.position[2] + ai_frame_height};
            view.target = {a.position[0], a.position[1], a.position[2] + 1.0f};
            ai_actors.update(scene, *ai_sim, player, 0.1f, ai_range);
            vm = make_view(view);
            gpu.begin_frame(k % 2);
            prepare_game_frame();
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
            write_png(out_dir / std::format("strip_{:02}.png", k), width, height,
                      gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4));
            std::println("strip {}: clip {} t {:.2f} rate {:.2f} speed {:.2f} yaw {:.2f}", k,
                         a.motion.locomotion ? ai_sim->motions()[a.character].locomotion[*a.motion.locomotion].clip.str() : "idle", a.motion_time,
                         a.motion.rate, std::hypot(a.velocity[0], a.velocity[1]), a.yaw);
        }
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
        for (const auto& n : gl.notes()) std::println("  {}", n);
    }

    if (benchmark_frames) {
        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
        check_hr(gpu.device()->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "timestamp heap");
        auto readback = gpu.buffer(32, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        uint64_t frequency = 0;
        check_hr(gpu.queue()->GetTimestampFrequency(&frequency), "timestamp frequency");
        std::array<double, 3> cpu{}, gpu_pass{};
        std::vector<double> budget_samples;
        budget_samples.reserve(benchmark_frames);
        std::unique_ptr<Sampler> sampler;
        if (const char* e = std::getenv("NIM_SAMPLE"); e && e[0] == '1') {
            Sampler::init_symbols();
            HANDLE self = nullptr;
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
            sampler = std::make_unique<Sampler>();
            sampler->start(self);
        }
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < benchmark_frames + 8; ++i) {
            gpu.begin_frame(i);
            const auto cpu_start = std::chrono::steady_clock::now();
            auto* list = gpu.immediate();
            lp.time = material_time + float(i) / 60;
            lp.delta_time = 1.0f / 60;
            scene.set_material_frame(lp.time, lp.delta_time);
            if (game) prepare_game_frame();
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            shadow_pass(list, lp);
            const auto after_shadow = std::chrono::steady_clock::now();
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            scene.render_gbuffer(gpu, list, gb, vm);
            const auto after_geometry = std::chrono::steady_clock::now();
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            if (game) render_game(list);
            else lighting.render(gpu, list, gb, vm, lp);
            list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
            list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, readback.Get(), 0);
            const auto end_record = std::chrono::steady_clock::now();
            gpu.flush();
            if (i < 8) continue;
            uint64_t* ticks = nullptr;
            const D3D12_RANGE read{0, 32};
            check_hr(readback->Map(0, &read, reinterpret_cast<void**>(&ticks)), "timestamp readback");
            const std::array times{cpu_start, after_shadow, after_geometry, end_record};
            double cpu_total = 0, gpu_total = 0;
            for (size_t pass = 0; pass < 3; ++pass) {
                const double record = std::chrono::duration<double, std::milli>(times[pass + 1] - times[pass]).count();
                const double render = double(ticks[pass + 1] - ticks[pass]) * 1000.0 / double(frequency);
                cpu[pass] += record;
                gpu_pass[pass] += render;
                cpu_total += record;
                gpu_total += render;
            }
            budget_samples.push_back(std::max(cpu_total, gpu_total));
            const D3D12_RANGE written{0, 0};
            readback->Unmap(0, &written);
        }
        if (sampler) sampler->stop_and_print();
        const double cpu_ms = cpu[0] + cpu[1] + cpu[2], gpu_ms = gpu_pass[0] + gpu_pass[1] + gpu_pass[2];
        std::println("benchmark {} frames at {}x{}: CPU record {:.4f} ms/frame, GPU {:.4f} ms/frame (wall {:.2f}s, 8 warmup frames)",
                     benchmark_frames, width, height, cpu_ms / benchmark_frames, gpu_ms / benchmark_frames,
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        std::sort(budget_samples.begin(), budget_samples.end());
        constexpr std::array names{"sun shadows", "geometry", "lighting and post"};
        for (size_t i = 0; i < names.size(); ++i)
            std::println("  {}: CPU {:.2f} ms, GPU {:.2f} ms", names[i], cpu[i] / benchmark_frames, gpu_pass[i] / benchmark_frames);
        std::println("  p95 frame budget max(CPU record, GPU): {:.2f} ms", budget_samples[(budget_samples.size() - 1) * 95 / 100]);
        const auto within_budget = std::ranges::upper_bound(budget_samples, 1000.0 / 60.0) - budget_samples.begin();
        std::println("  p99 {:.2f} ms, worst {:.2f} ms, {} / {} frames within 16.67 ms",
                     budget_samples[(budget_samples.size() - 1) * 99 / 100], budget_samples.back(), within_budget, budget_samples.size());
        if (is_map) {
            const auto [statics, lod0, entities] = scene.visibility_counts();
            std::println("  visible: {} static placements ({} at LOD0), {} entity placements", statics, lod0, entities);
        }
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
    if (dump_lighting) {
        auto save = [&](const char* name, const std::vector<uint8_t>& bytes) {
            std::ofstream f(out_dir / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            if (!f) throw std::runtime_error("lighting dump failed");
        };
        save("lighting-depth.bin", depth);
        save("lighting-motion.bin", gpu.read_texture(gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 8));
        save("lighting-diffuse.bin", gpu.read_texture(gl.light_buffer(0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        save("lighting-specular.bin", gpu.read_texture(gl.light_buffer(1), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        save("lighting-ibl.bin", gpu.read_texture(gl.specular_ibl(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        if (gl.externs().texture(0x5D, 0x00).resource == gl.material_occlusion())
            save("lighting-material-ao.bin", gpu.read_texture(gl.material_occlusion(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1));
        if (gl.ssr_drawn()) {
            save("lighting-ssr-radiance.bin", gpu.read_texture(gl.ssr_radiance(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 8));
            save("lighting-ssr-distance.bin", gpu.read_texture(gl.ssr_hit_distance(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, 4));
        }
        for (int k = 0; k < 2 && gl.volumetric_layer(k); ++k)
            save(k ? "lighting-shafts-near.bin" : "lighting-shafts-far.bin", gpu.read_texture(gl.volumetric_layer(k), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        std::ofstream f(out_dir / "lighting.json");
        f << std::format("{{\"width\":{},\"height\":{},\"vp\":[", width, height);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) f << std::format("{}{:.9g}", c || r ? "," : "", vm.vp_abs.c[c][size_t(r)]);
        const auto fog_mapping = gl.externs().get_v(ext::Atmosphere, 0x130);
        f << std::format("],\"view_depth_near\":{},\"froxel_mapping\":[{},{},{},{}],\"probes\":[",
            vm.vp_rel.c[3][2], fog_mapping[0], fog_mapping[1], fog_mapping[2], fog_mapping[3]);
        for (size_t i = 0; i < region_cubemaps.size(); ++i) {
            const auto& p = *region_cubemaps[i];
            f << std::format("{}{{\"center\":[{},{},{}],\"extents\":[{},{},{}],\"rotation\":[{},{},{},{}]}}",
                i ? "," : "", p.center[0], p.center[1], p.center[2], p.extents[0], p.extents[1], p.extents[2],
                p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]);
        }
        f << "],\"atmosphere\":[";
        bool first_atmosphere = true;
        for (auto [offset, name] : {std::pair{0xE0u, "lighting-sky-far.bin"},
                                   std::pair{0xF0u, "lighting-sky-near.bin"},
                                   std::pair{0x108u, "lighting-froxels.bin"}}) {
            auto* resource = gl.externs().texture(ext::Atmosphere, offset).resource;
            if (!resource || !lp.atmosphere || (offset == 0x108 && !lp.froxel_fog)) continue;
            const auto desc = resource->GetDesc();
            const auto state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON);
            save(name, gpu.read_texture(resource, state, 8));
            f << std::format("{}{{\"file\":\"{}\",\"width\":{},\"height\":{},\"depth\":{},\"format\":{}}}",
                first_atmosphere ? "" : ",", name, desc.Width, desc.Height, desc.DepthOrArraySize, int(desc.Format));
            first_atmosphere = false;
        }
        f << "],\"environment\":[";
        if (auto* hemi = gl.externs().texture(ext::Deferred, 0xF8).resource) {
            const auto desc = hemi->GetDesc();
            bool first = true;
            for (uint32_t mip : {0u, 6u, 8u}) {
                if (mip >= desc.MipLevels) continue;
                const auto name = std::format("lighting-hemisphere-mip{}.bin", mip);
                save(name.c_str(), gpu.read_texture(hemi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8, mip));
                f << std::format("{}{{\"file\":\"{}\",\"width\":{},\"height\":{},\"depth\":1,\"format\":{},\"mip\":{}}}",
                    first ? "" : ",", name, std::max<uint64_t>(1, desc.Width >> mip), std::max(1u, desc.Height >> mip), int(desc.Format), mip);
                first = false;
            }
        }
        f << "],\"global_channels\":[";
        const auto& globals = gl.externs().channels;
        for (size_t i = 0; i < globals.size(); ++i)
            f << std::format("{}[{},{},{},{}]", i ? "," : "", globals[i][0], globals[i][1], globals[i][2], globals[i][3]);
        f << "]}";
        if (!f) throw std::runtime_error("lighting metadata failed");
    }
    if (dump_probe_mask) {
        if (!probe_index || region_cubemaps.size() != 1) throw std::runtime_error("--dump-probe-mask requires one --probe-index");
        const auto& m = *region_cubemaps[0];
        auto save = [&](const char* name, const auto& bytes) {
            std::ofstream f(out_dir / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
            if (!f) throw std::runtime_error("probe verification dump failed");
        };
        save("probe-depth.bin", depth);
        save("probe-stencil.bin", gpu.read_texture(gb.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, 1, 1));
        save("probe-light-diffuse.bin", gpu.read_texture(gl.light_buffer(0), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        save("probe-light-specular.bin", gpu.read_texture(gl.light_buffer(1), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8));
        save("probe-shapes.bin", m.shape_records.is_none() ? std::vector<uint8_t>{} : pm.read(m.shape_records));
        std::ofstream f(out_dir / "probe-mask.json");
        f << std::format("{{\"width\":{},\"height\":{},\"index\":{},\"vp\":[", width, height, *probe_index);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) f << std::format("{}{:.9g}", c || r ? "," : "", vm.vp_abs.c[c][size_t(r)]);
        f << "],\"local_to_world\":[";
        for (size_t i = 0; i < 16; ++i) f << std::format("{}{:.9g}", i ? "," : "", m.local_to_world[i]);
        f << "]}";
        if (!f) throw std::runtime_error("probe verification metadata write failed");
    }
    size_t covered = 0;
    for (size_t i = 0; i < depth.size(); i += 4) {
        float d;
        std::memcpy(&d, depth.data() + i, 4);
        covered += d > 0;
    }
    auto t1 = std::chrono::steady_clock::now();
    std::println("rendered {} pixels covered ({:.1f}%) in {:.2f}s", covered, 100.0 * covered / (size_t(width) * height),
                 std::chrono::duration<double>(t1 - t0).count());
    if (!covered && !scene.has_transparent() && !scene.distortion_draws() && !(gl.has_atmosphere() && lp.atmosphere)) throw std::runtime_error("nothing was rendered");
    std::println("wrote {}", fs::absolute(out_dir).string());
    if (verify_frame_inputs) {
        lp.mode = ViewMode::Lit;
        if (!gl.has_atmosphere() || !lp.atmosphere) throw std::runtime_error("--verify-frame-inputs requires a map atmosphere");
        const auto source = load_texture(pm, atmosphere.density_lut);
        const bool white = (source.dxgi_format == DXGI_FORMAT_R8G8B8A8_UNORM || source.dxgi_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
            std::all_of(source.data.begin(), source.data.end(), [](uint8_t b) { return b == 255; });
        auto half = [](uint16_t v) {
            const int exponent = (v >> 10) & 31;
            const float mantissa = float(v & 1023) / 1024;
            const float value = exponent == 31 ? INFINITY : exponent ? std::ldexp(1 + mantissa, exponent - 15) : std::ldexp(mantissa, -14);
            return (v & 0x8000) ? -value : value;
        };
        if (lp.post) {
            if (!gl.post_ready()) throw std::runtime_error("Original SDR post-processing did not load");
            auto* lut = gl.grading_lut();
            if (!lut) throw std::runtime_error("Native color grading LUT was not generated");
            const auto desc = lut->GetDesc();
            constexpr uint32_t side = 32;
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
                desc.Width != side || desc.Height != side || desc.DepthOrArraySize != side)
                throw std::runtime_error("Unexpected native color grading LUT layout");
            const auto data = gpu.read_texture(lut, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
            if (data.size() != side * side * side * 4) throw std::runtime_error("Incomplete native color grading LUT");
            float max_rgb_error = 0, max_luma_error = 0;
            for (uint32_t z = 0; z < side; ++z) for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
                const size_t i = ((size_t(z) * side + y) * side + x) * 4;
                const std::array<float, 3> rgb{float(x) / (side - 1), float(y) / (side - 1), float(z) / (side - 1)};
                for (size_t c = 0; c < rgb.size(); ++c)
                    max_rgb_error = std::max(max_rgb_error, std::abs(data[i + c] / 255.0f - rgb[c]));
                const float luma = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
                max_luma_error = std::max(max_luma_error, std::abs(data[i + 3] / 255.0f - luma));
            }
            // Allow one UNORM8 step after the half-float strip and the compute conversion.
            if (max_rgb_error > 1.0f / 255 || max_luma_error > 1.0f / 255)
                throw std::runtime_error(std::format("Native color grading LUT mismatch: RGB {}, luma {}", max_rgb_error, max_luma_error));
            std::println("verified color grading LUT: 32x32x32, RGB max error {}, luma max error {}", max_rgb_error, max_luma_error);
        }
        auto verify_lut = [&](const ViewMatrices& camera) {
            if (gl.cubemap_count() != gl.cubemaps_requested()) throw std::runtime_error("Original cubemap preparation omitted volumes");
            if (gl.cubemaps_failed()) throw std::runtime_error("Original cubemap volume draw failed");
            const auto& ext = gl.externs();
            if (lp.sky_ambient || gl.cubemaps_drawn()) {
                const auto* packed_ao = gl.material_occlusion();
                if (!packed_ao || ext.texture(0x5D, 0x00).resource != packed_ao || packed_ao == gl.ambient_occlusion())
                    throw std::runtime_error("Cubemaps are not reading the current material AO");
                const auto source = gpu.read_texture(gb.rt[2].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 4);
                const auto material = gpu.read_texture(gl.material_occlusion(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 1);
                if (material.size() * 4 != source.size()) throw std::runtime_error("Stale material AO dimensions");
                int max_delta = 0;
                for (size_t i = 0; i < material.size(); ++i)
                    max_delta = std::max(max_delta, std::abs(int(material[i]) - std::min(255, 2 * int(source[i * 4 + 1]))));
                if (max_delta > 1) throw std::runtime_error(std::format("Material AO disagrees with G-buffer max error {}", max_delta));
                std::println("verified material AO: {} pixels match G-buffer decode (max UNORM8 delta {})", material.size(), max_delta);
            }
            if (lp.sky_ambient && gl.has_atmosphere()) {
                auto* hemisphere = ext.texture(ext::Deferred, 0xF8).resource;
                if (!hemisphere) throw std::runtime_error("Original sky environment was not produced");
                const auto desc = hemisphere->GetDesc();
                for (uint32_t mip : {0u, uint32_t(desc.MipLevels - 1)}) {
                    const auto data = gpu.read_texture(hemisphere, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8, mip);
                    for (size_t o = 0; o < data.size(); o += 2) {
                        uint16_t value;
                        std::memcpy(&value, data.data() + o, 2);
                        if (!std::isfinite(half(value))) throw std::runtime_error("Non-finite original sky environment mip");
                    }
                }
                std::println("verified sky environment: {}x{}, {} mips, finite base and diffuse tail", desc.Width, desc.Height, desc.MipLevels);
            }
            if (lp.froxel_fog) {
                auto* volume = ext.texture(ext::Atmosphere, 0x108).resource;
                if (!volume || volume->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D || volume->GetDesc().DepthOrArraySize <= 1)
                    throw std::runtime_error("Original atmosphere froxel volume was not produced");
                const auto vd = volume->GetDesc();
                const auto pixels = gpu.read_texture(volume,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 8);
                const size_t plane = size_t(vd.Width) * vd.Height;
                if (pixels.size() != plane * vd.DepthOrArraySize * 8) throw std::runtime_error("Incomplete froxel volume readback");
                const auto* data = reinterpret_cast<const uint16_t*>(pixels.data());
                float previous_min = 1;
                const bool no_interior = ext.get_v(81, 0x10) == float4{};
                for (uint32_t z = 0; z < vd.DepthOrArraySize; ++z) {
                    float slice_min = 1;
                    for (size_t xy = 0; xy < plane; ++xy) {
                        const size_t n = (size_t(z) * plane + xy) * 4;
                        for (size_t c = 0; c < 4; ++c) {
                            const float v = half(data[n+c]);
                            if (!std::isfinite(v) || v < 0 || (c < 2 && v > 1.002f))
                                throw std::runtime_error("Invalid froxel transmittance / in-scatter");
                            if (no_interior && c == 1 && std::abs(v - 1) > 0.002f)
                                throw std::runtime_error("Indoor fog was integrated without an interior height map");
                        }
                        const float transmittance = half(data[n]);
                        if (z && transmittance > half(data[n-plane*4]) + 0.002f)
                            throw std::runtime_error("Froxel transmittance increases with distance");
                        slice_min = std::min(slice_min, transmittance);
                    }
                    previous_min = slice_min;
                }
                if (ext.texture(ext::Transparent, 0x78).resource != volume ||
                    ext.get_v(ext::Transparent, 0xD0) != ext.get_v(ext::Atmosphere, 0x130))
                    throw std::runtime_error("Opaque and transparent passes have different froxel volumes or depth coordinates");
                std::println("verified froxels: {}x{}x{}, all slices finite, transmittance monotonic (far minimum {})",
                             vd.Width, vd.Height, vd.DepthOrArraySize, previous_min);
            }
            auto* texture = ext.texture(ext::Transparent, 0x20).resource;
            if (!texture) throw std::runtime_error("Missing transparent atmosphere LUT");
            const auto desc = texture->GetDesc();
            const auto data = gpu.read_texture(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 8);
            const auto* pixels = reinterpret_cast<const uint16_t*>(data.data());
            float error = 0;
            for (uint32_t y = 0; y < desc.Height; ++y) for (uint32_t x = 0; x < desc.Width; ++x) {
                const float u = float(x) / float(desc.Width - 1), elevation = 2 * float(y) / float(desc.Height - 1) - 1;
                const float distance = u * u * 50000, z = distance * elevation + camera.eye.z;
                auto density = [&](uint32_t amount, uint32_t falloff, uint32_t height) {
                    return 1.442695f * ext.get_f(ext::Atmosphere, amount) *
                        std::exp2(-0.001f * 1.442695f * ext.get_f(ext::Atmosphere, falloff) * std::max(z - ext.get_f(ext::Atmosphere, height), 0.0f));
                };
                const float optical_depth = std::max(distance - ext.get_v(ext::Atmosphere, 0x130)[1], 0.0f) * 0.001f *
                    (density(0x174, 0x188, 0x18C) + density(0x1A4, 0x1A8, 0x1AC));
                for (uint32_t c = 0; c < 4; ++c) {
                    const float actual = half(pixels[(size_t(y) * desc.Width + x) * 4 + c]);
                    if (!std::isfinite(actual) || actual < 0 || actual > 1.001f || (!x && std::abs(actual - 1) > 0.001f))
                        throw std::runtime_error("Atmosphere LUT contains invalid transmittance / zero-distance endpoint");
                    if (white) {
                        const float scale = c == 3 ? 1 : ext.get_v(ext::Atmosphere, 0x190)[c] * ext.get_f(ext::Atmosphere, 0x1A0);
                        error = std::max(error, std::abs(actual - std::exp2(-optical_depth * scale)));
                    }
                }
            }
            if (error > 0.003f) throw std::runtime_error(std::format("Atmosphere LUT / CPU equation mismatch: {}", error));
            std::println("verified atmosphere LUT: {}x{}, finite transmittance, zero-distance = 1, CPU max error {}{}",
                desc.Width, desc.Height, error, white ? "" : " (nonuniform source: analytic comparison skipped)");
            if (scene.distortion_draws()) {
                auto* reduced = gl.distortion_view_depth();
                if (!reduced) throw std::runtime_error("Native distortion view depth was not produced");
                const auto reduced_desc = reduced->GetDesc();
                if (reduced_desc.Format != DXGI_FORMAT_R16G16_FLOAT || reduced_desc.Width != (gb.width() + 1) / 2 ||
                    reduced_desc.Height != (gb.height() + 1) / 2)
                    throw std::runtime_error("Stale or incompatible distortion view depth");
                const auto reduced_pixels = gpu.read_texture(reduced, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
                for (size_t i = 0; i < reduced_pixels.size(); i += 4) {
                    uint16_t pair[2];
                    std::memcpy(pair, &reduced_pixels[i], sizeof(pair));
                    const float closest = half(pair[0]), farthest = half(pair[1]);
                    if (!std::isfinite(closest) || !std::isfinite(farthest) || closest <= 0 || farthest < closest)
                        throw std::runtime_error("Invalid native min/max depth for distortion");
                }
                std::println("verified distortion view depth: {}x{}, finite ordered min/max", reduced_desc.Width, reduced_desc.Height);
            }
            if (gl.volumetrics_drawn()) {
                const auto depths = gpu.read_texture(gl.volumetric_depth(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 16);
                const auto* values = reinterpret_cast<const float*>(depths.data());
                for (size_t i = 0; i < depths.size() / sizeof(float); i += 4) {
                    const float far_depth = values[i], near_depth = values[i + 1], mean = values[i + 2], coverage = values[i + 3];
                    if (!std::isfinite(far_depth) || !std::isfinite(near_depth) || !std::isfinite(mean) || !std::isfinite(coverage) ||
                        near_depth <= 0 || mean < near_depth * 0.99999f || mean > far_depth * 1.00001f || coverage < 0 || coverage > 1)
                        throw std::runtime_error("Invalid depth range for volumetric lights");
                }
                std::println("verified volumetric depth: {} finite min/max ranges", depths.size() / 16);
            }
        };
        verify_lut(vm);
        // Both a larger target and the original size: the first transparent draw after resize
        // must receive the new depth / sky resources without needing a warm-up frame.
        for (int pass = 0; pass < 2; ++pass) {
            gpu.begin_frame(uint32_t(pass));
            const uint32_t w = pass ? width : width + 17, h = pass ? height : height + 11;
            gb.resize(gpu, w, h);
            View moved = view;
            moved.width = w; moved.height = h;
            if (!pass) moved.target = moved.eye + normalize(V3{1, 0, 1});  // rotate toward mostly empty sky
            prepare_game_frame();
            vm = gl.temporal_view(moved, lp);
            scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
            render_game(gpu.immediate());
            gpu.flush();
            if (lp.froxel_fog && gl.externs().get_v(0x65, 0x20)[3] != 0)
                throw std::runtime_error("Froxel history was reused after resize");
            verify_lut(vm);
            const auto depth = gl.externs().texture(ext::Deferred, 0x98);
            if (depth.resource != gb.depth.Get()) throw std::runtime_error("Stale depth binding after resize");
            for (uint32_t offset : {0u, 0x10u}) {
                const auto sky = gl.externs().texture(ext::Transparent, offset).resource->GetDesc();
                if (sky.Width != w || sky.Height != h) throw std::runtime_error("Stale transparent sky dimensions after resize");
            }
            const auto motion = gpu.read_texture(gb.rt[3].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, 8);
            const auto* values = reinterpret_cast<const uint16_t*>(motion.data());
            size_t moving = 0;
            for (size_t i = 0; i < motion.size() / 2; i += 4) {
                const float x = half(values[i]), y = half(values[i + 1]);
                if (!std::isfinite(x) || !std::isfinite(y)) throw std::runtime_error("Non-finite motion vectors after camera rotation");
                moving += std::abs(x) + std::abs(y) > 1e-5f;
            }
            if (!pass && !moving && gl.temporal_aa_on(lp)) throw std::runtime_error("Camera rotation did not produce motion vectors");
            std::println("verified resized frame {}x{}: current depth / sky, finite motion ({} moving pixels)", w, h, moving);
        }
        if (lp.froxel_fog && lp.froxel_temporal) {
            constexpr auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            constexpr uint8_t froxel = 0x65;
            auto frame_density = [&](const View& camera, bool history, bool expect_history) {
                gpu.begin_frame(0);
                lp.froxel_temporal = history;
                prepare_game_frame();
                vm = gl.temporal_view(camera, lp);
                scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
                render_game(gpu.immediate());
                gpu.flush();
                const auto& ext = gl.externs();
                const auto* prior = ext.texture(froxel, 0x90).resource;
                auto* current = ext.texture(froxel, 0x98).resource;
                if (!current || prior == current || (ext.get_v(froxel, 0x20)[3] > 0) != expect_history)
                    throw std::runtime_error("Froxel temporal bindings do not match the expected history state");
                return gpu.read_texture(current, read, 8);
            };
            View moved = view;
            const V3 shift{0.35f, 0.2f, 0.3f};
            moved.eye = moved.eye + shift;
            moved.target = moved.target + shift;
            const auto independent = frame_density(moved, false, false);
            frame_density(view, true, false);
            const auto temporal = frame_density(moved, true, true);
            verify_lut(vm);
            if (temporal.size() != independent.size() || temporal.size() % 8)
                throw std::runtime_error("Froxel history changed the density volume size");
            size_t different = 0;
            for (size_t i = 0; i < temporal.size(); i += 8)
                different += !std::equal(temporal.begin() + i, temporal.begin() + i + 8, independent.begin() + i);
            if (!different) throw std::runtime_error("Froxel reprojection did not affect the density volume");
            // A large teleport at the same orientation must drop the previous density.
            moved.eye = moved.eye + V3{40, 0, 0};
            moved.target = moved.target + V3{40, 0, 0};
            frame_density(moved, true, false);
            std::println("verified froxel history: {} changed voxels after camera motion, reset after teleport and resize", different);
            vm = gl.temporal_view(view, lp);
        }
        lp.froxel_temporal = false;  // exposure tests isolate post-processing from temporal density
        if (gl.temporal_aa_on(lp)) {
            const bool ssr = lp.ssr;
            lp.auto_exposure = false;
            lp.ssr = false;
            lp.antialias = false;
            // Hold the same jittered camera while toggling the resolve, so any pixel
            // difference comes from history and not a changed subpixel sample.
            auto render_exposure = [&](bool taa, float exposure) {
                gpu.begin_frame(0);
                lp.temporal_aa = taa;
                lp.exposure = exposure;
                prepare_game_frame();
                scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
                render_game(gpu.immediate());
                gpu.flush();
            };
            render_exposure(false, 2.0f);
            const auto reference = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
            render_exposure(true, 1.0f);  // seed the history at a lower exposure
            render_exposure(true, 2.0f);  // one stop rejects that history
            const auto actual = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
            int max_delta = 0;
            for (size_t i = 0; i < reference.size(); ++i)
                max_delta = std::max(max_delta, std::abs(int(reference[i]) - int(actual[i])));
            if (max_delta > 1) throw std::runtime_error(std::format("TAA retained old exposure: max channel difference {}", max_delta));
            std::println("verified TAA history: exact one-stop change matches current tonemap (max 8-bit delta {})", max_delta);
            render_exposure(true, 2.0f * std::sqrt(2.0f));
            const float remaining_weight = gl.externs().get_f(0x6A, 0x04);
            if (std::abs(remaining_weight - 0.46f) > 0.0001f)
                throw std::runtime_error("Half-stop exposure change did not reduce TAA history weight");
            std::println("verified TAA history weight after half a stop: {}", remaining_weight);
            lp.ssr = ssr;
        }
        if (lp.ssr && gl.ssr_source() && !vm.ortho) {
            lp.auto_exposure = false;
            lp.temporal_aa = false;
            auto frame_at_exposure = [&](float exposure) {
                gpu.begin_frame(0);
                lp.exposure = exposure;
                prepare_game_frame();
                vm = gl.temporal_view(view, lp);
                scene.render_gbuffer(gpu, gpu.immediate(), gb, vm);
                render_game(gpu.immediate());
                gpu.flush();
            };
            auto read_source = [&](float exposure) {
                gl.drop_ssr_history();
                frame_at_exposure(1);
                if (gl.ssr_drawn()) throw std::runtime_error("SSR used invalidated history");
                frame_at_exposure(exposure);
                if (!gl.ssr_drawn()) throw std::runtime_error("SSR reflection source missing after warmup");
                constexpr auto read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                return gpu.read_texture(gl.ssr_source(), read, 8);
            };
            const auto base = read_source(1), brighter = read_source(2);
            const auto* a = reinterpret_cast<const uint16_t*>(base.data());
            const auto* b = reinterpret_cast<const uint16_t*>(brighter.data());
            float max_error = 0;
            size_t lit = 0;
            for (size_t i = 0; i < base.size() / 2; ++i) {
                if (i % 4 == 3) continue;
                const float expected = half(a[i]) * 2, actual = half(b[i]);
                if (!std::isfinite(expected) || !std::isfinite(actual))
                    throw std::runtime_error("Non-finite SSR source after exposure step");
                max_error = std::max(max_error, std::abs(actual - expected) / std::max(1.0f, std::abs(expected)));
                lit += expected > 0.01f;
            }
            if (!lit || max_error > 0.005f)
                throw std::runtime_error(std::format("SSR history exposure correction mismatch: {} lit channels, error {}", lit, max_error));
            std::println("verified SSR history: 1-to-2 exposure step, {} lit channels, max normalized error {}", lit, max_error);
        }
    }

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
