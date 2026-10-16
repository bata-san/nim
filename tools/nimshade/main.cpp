// nimshade — headless shading test.
//
// Renders a static mesh with the game's own compiled shaders (DXIL VS + PS, untouched), material
// constants evaluated by nim's TFX VM, and the game's BC textures; then runs nim's deferred
// lighting over the G-buffer the game's PS wrote. Writes the G-buffer and the lit image as PNGs.
//
//   nimshade [--install DIR] STATIC_TAG [OUT_DIR] [--size WxH] [--warp]
#include "nim/core/package_manager.hpp"
#include "nim/render/scene.hpp"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
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
    check_hr(frame->WritePixels(h, w * 4, UINT(rgba.size()), const_cast<uint8_t*>(rgba.data())), "WritePixels");
    check_hr(frame->Commit(), "frame Commit");
    check_hr(enc->Commit(), "encoder Commit");
}

int run(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    fs::path install;
    uint32_t width = 1600, height = 1000;
    bool warp = false;
    std::vector<std::string> pos;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--install" && i + 1 < args.size()) install = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size()) sscanf_s(args[++i].c_str(), "%ux%u", &width, &height);
        else if (args[i] == "--warp") warp = true;
        else pos.push_back(args[i]);
    }
    if (pos.empty()) {
        std::println(stderr, "usage: nimshade [--install DIR] STATIC_TAG [OUT_DIR] [--size WxH] [--warp]");
        return 2;
    }
    if (install.empty()) {
        auto found = PackageManager::find_install();
        if (!found) { std::println(stderr, "Marathon install not found; pass --install DIR"); return 2; }
        install = *found;
    }
    TagHash mesh_tag(uint32_t(std::stoul(pos[0], nullptr, 16)));
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
    scene.load(pm, gpu, mesh_tag, layouts);
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
    ViewMatrices vm = make_view(frame_sphere(scene.center(), scene.radius(), width, height));

    auto* cl = gpu.immediate();
    scene.render_gbuffer(gpu, cl, gb, vm);
    LightingParams lp;
    lighting.render(gpu, cl, gb, vm, lp);
    gpu.flush();
    std::vector<uint8_t> lit = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
    write_png(out_dir / "composite.png", width, height, lit);

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
    if (!covered) throw std::runtime_error("nothing was rendered");
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
