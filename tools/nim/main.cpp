// nim — Marathon asset viewer (GUI).
//
// Dear ImGui (docking) over D3D12. Meshes are drawn with the game's own G-buffer shaders through
// nim_render; text, dialogue and raw packages are browsable side by side.
#include "nim/content/dialogue.hpp"
#include "nim/content/strings.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/scene.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace nim;
namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.2.0";

// ---- small utilities ------------------------------------------------------------------------------
std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
fs::path exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}
std::string lower_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
bool contains_ci(const std::string& hay, const std::string& needle_lower) {
    if (needle_lower.empty()) return true;
    return lower_ascii(hay).find(needle_lower) != std::string::npos;
}

std::optional<fs::path> pick_folder(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Marathon のインストールフォルダを選択");
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    fs::path p(path);
    CoTaskMemFree(path);
    return p;
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
    check_hr(frame->WritePixels(h, w * 4, UINT(rgba.size()), const_cast<uint8_t*>(rgba.data())), "WritePixels");
    check_hr(frame->Commit(), "frame Commit");
    check_hr(enc->Commit(), "encoder Commit");
}

// ---- data loaded in the background -----------------------------------------------------------------
struct MeshEntry { TagHash tag; uint32_t size; std::string package; };
struct EntryRow { TagHash tag; uint16_t type, subtype; uint32_t reference, size; };

struct Catalog {
    std::mutex mutex;
    std::string status = "準備中…";
    std::atomic<bool> meshes_ready{false}, strings_ready{false}, dialogue_ready{false}, done{false};
    std::vector<MeshEntry> meshes;
    StringTable strings;
    std::vector<DialogueLine> dialogue;
    std::string error;
};

void load_catalog(fs::path install, Catalog* cat) {
    try {
        PackageManager pm(install);
        {
            std::lock_guard lk(cat->mutex);
            cat->status = "静的メッシュを列挙中…";
        }
        std::vector<MeshEntry> meshes;
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            for (uint32_t i = 0; i < p.entries().size(); ++i) {
                const Entry& e = p.entries()[i];
                if (e.type == 8 && e.reference == kClassStaticMesh)
                    meshes.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.file_size, info.name});
            }
        }
        {
            std::lock_guard lk(cat->mutex);
            cat->meshes = std::move(meshes);
            cat->status = "テキストを読込中…";
        }
        cat->meshes_ready = true;
        StringTable st;
        st.load_all(pm);
        {
            std::lock_guard lk(cat->mutex);
            cat->strings = std::move(st);
            cat->status = "セリフを読込中…";
        }
        cat->strings_ready = true;
        auto dlg = load_all_dialogue(pm);
        {
            std::lock_guard lk(cat->mutex);
            cat->dialogue = std::move(dlg);
            cat->status = "";
        }
        cat->dialogue_ready = true;
    } catch (const std::exception& ex) {
        std::lock_guard lk(cat->mutex);
        cat->error = ex.what();
        cat->status = std::string("読込エラー: ") + ex.what();
    }
    cat->done = true;
}

// ---- window / swapchain -----------------------------------------------------------------------------
struct Window {
    HWND hwnd = nullptr;
    ComPtr<IDXGISwapChain3> swapchain;
    HANDLE waitable = nullptr;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12Resource> back[2];
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{};
    ComPtr<ID3D12CommandAllocator> alloc[Gpu::kFramesInFlight];
    uint64_t fence[Gpu::kFramesInFlight]{};
    ComPtr<ID3D12GraphicsCommandList> list;
    UINT pending_w = 0, pending_h = 0;
    bool tearing = false;
};
Window g_win;

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_win.pending_w = LOWORD(lp);
            g_win.pending_h = HIWORD(lp);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void create_back_buffers(Gpu& gpu) {
    const UINT inc = gpu.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 2; ++i) {
        check_hr(g_win.swapchain->GetBuffer(i, IID_PPV_ARGS(&g_win.back[i])), "GetBuffer");
        g_win.rtv[i] = {g_win.rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + size_t(i) * inc};
        gpu.device()->CreateRenderTargetView(g_win.back[i].Get(), nullptr, g_win.rtv[i]);
    }
}

void init_swapchain(Gpu& gpu) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_win.rtv_heap)), "rtv heap");
    BOOL tearing = FALSE;
    gpu.factory()->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    g_win.tearing = tearing == TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.BufferCount = 2;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (g_win.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    check_hr(gpu.factory()->CreateSwapChainForHwnd(gpu.queue(), g_win.hwnd, &sd, nullptr, nullptr, &sc1),
             "CreateSwapChainForHwnd");
    check_hr(sc1.As(&g_win.swapchain), "IDXGISwapChain3");
    gpu.factory()->MakeWindowAssociation(g_win.hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_win.swapchain->SetMaximumFrameLatency(2);
    g_win.waitable = g_win.swapchain->GetFrameLatencyWaitableObject();
    create_back_buffers(gpu);
    for (auto& a : g_win.alloc)
        check_hr(gpu.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
    check_hr(gpu.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_win.alloc[0].Get(), nullptr,
                                             IID_PPV_ARGS(&g_win.list)),
             "command list");
    g_win.list->Close();
}

void resize_swapchain(Gpu& gpu, UINT w, UINT h) {
    gpu.wait_idle();
    for (auto& b : g_win.back) b.Reset();
    DXGI_SWAP_CHAIN_DESC1 sd{};
    g_win.swapchain->GetDesc1(&sd);
    check_hr(g_win.swapchain->ResizeBuffers(2, w, h, sd.Format, sd.Flags), "ResizeBuffers");
    create_back_buffers(gpu);
}

// ---- the app ----------------------------------------------------------------------------------------
struct LogLine { std::string text; bool error; };

struct Orbit {
    V3 target;
    float yaw = -0.93f, pitch = 0.41f, distance = 10, radius = 1;
};

class App {
public:
    Gpu gpu;
    fs::path install;
    std::unique_ptr<PackageManager> pm;
    std::vector<std::vector<InputElement>> layouts;
    std::unique_ptr<Catalog> catalog;
    std::thread loader;

    StaticMeshScene scene;
    GBuffer gb;
    LightingPass lighting;
    LightingParams light;
    Orbit orbit;
    uint32_t view_w = 0, view_h = 0;    // viewport size requested by the UI last frame
    bool viewport_visible = false;
    std::optional<TagHash> pending_load;
    bool capture_requested = false;

    std::deque<LogLine> log;
    int language = 1;  // second column language (ja by default)

    // browser state
    char mesh_filter[64] = "";
    std::vector<uint32_t> mesh_view;
    std::string mesh_filter_applied = "\x01";
    char string_filter[128] = "";
    std::vector<uint32_t> string_view;
    std::string string_filter_applied = "\x01";
    int string_filter_lang = -1;
    char dialogue_filter[128] = "";
    std::vector<uint32_t> dialogue_view;
    std::string dialogue_filter_applied = "\x01";
    int dialogue_filter_lang = -1;
    int selected_package = -1;
    std::vector<EntryRow> package_rows;
    char entry_filter[32] = "";
    int selected_entry = -1;
    std::vector<uint8_t> entry_preview;
    std::string entry_preview_error;

    void info(std::string s) { push_log(std::move(s), false); }
    void error(std::string s) { push_log(std::move(s), true); }
    void push_log(std::string s, bool err) {
        log.push_back({std::move(s), err});
        while (log.size() > 500) log.pop_front();
    }

    bool open_install(const fs::path& dir) {
        try {
            auto p = std::make_unique<PackageManager>(dir);
            if (p->packages().empty()) throw std::runtime_error("packages フォルダが見つからないか空です");
            p->build_hash64_index();
            layouts = load_input_layouts(*p);
            pm = std::move(p);
            install = dir;
            info(std::format("インストールを開きました: {} ({} パッケージ)", to_utf8(dir.wstring()), pm->packages().size()));
            if (loader.joinable()) loader.join();
            catalog = std::make_unique<Catalog>();
            loader = std::thread(load_catalog, dir, catalog.get());
            return true;
        } catch (const std::exception& ex) {
            error(std::format("インストールを開けません: {}", ex.what()));
            return false;
        }
    }

    // Runs at the top of a frame, before any UI references GPU resources.
    void apply_pending(Gpu& g) {
        if (view_w && view_h && (view_w != gb.width() || view_h != gb.height())) {
            g.wait_idle();
            gb.resize(g, view_w, view_h);
        }
        if (pending_load && pm) {
            TagHash tag = *pending_load;
            pending_load.reset();
            g.wait_idle();
            auto t0 = std::chrono::steady_clock::now();
            try {
                scene.load(*pm, g, tag, layouts);
                orbit.target = scene.center();
                orbit.radius = scene.radius();
                orbit.distance = scene.radius() / std::sin(20.0f * 3.14159265f / 180.0f) * 0.9f;
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                size_t stubbed = 0;
                for (const auto& gi : scene.groups()) stubbed += gi.stubbed_ops.size();
                info(std::format("{} を読込 ({} グループ, {:.0f} ms){}", tag.str(), scene.groups().size(), ms,
                                 stubbed ? std::format(", 未実装 TFX op {} 種", stubbed) : ""));
            } catch (const std::exception& ex) {
                scene.release(g);
                error(std::format("{} を描画できません: {}", tag.str(), ex.what()));
            }
        }
    }

    View current_view() const {
        View v;
        v.width = std::max(1u, gb.width());
        v.height = std::max(1u, gb.height());
        v.target = orbit.target;
        v.fov_y = 40.0f * 3.14159265f / 180.0f;
        V3 dir{std::cos(orbit.yaw) * std::cos(orbit.pitch), std::sin(orbit.yaw) * std::cos(orbit.pitch), std::sin(orbit.pitch)};
        v.eye = orbit.target + dir * orbit.distance;
        v.near_z = std::max(0.001f, orbit.radius * 0.005f);
        return v;
    }

    void record(ID3D12GraphicsCommandList* cl) {
        if (!gb.lit) return;
        ViewMatrices vm = make_view(current_view());
        scene.render_gbuffer(gpu, cl, gb, vm);
        lighting.render(gpu, cl, gb, vm, light);
    }

    void after_frame() {
        if (!capture_requested) return;
        capture_requested = false;
        try {
            gpu.wait_idle();
            auto px = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
            fs::path dir = exe_dir() / "captures";
            fs::create_directories(dir);
            auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            fs::path file = dir / std::format("{}_{}_{}.png", scene.loaded() ? scene.tag().str() : "empty",
                                               view_mode_name(light.mode)[0] == 'L' ? "lit" : "debug", stamp);
            write_png(file, gb.width(), gb.height(), px);
            info("保存しました: " + to_utf8(file.wstring()));
        } catch (const std::exception& ex) {
            error(std::string("保存に失敗: ") + ex.what());
        }
    }

    // ---- UI ----
    void ui() {
        ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        static bool layout_built = false;
        if (!layout_built) {
            layout_built = true;
            ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
            if (!node || node->IsLeafNode()) build_layout(dock);
        }
        menu_bar();
        if (!pm) install_picker();
        browser_window();
        viewport_window();
        inspector_window();
        log_window();
    }

    void build_layout(ImGuiID dock) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
        ImGuiID left, rest, right, center, bottom;
        ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.30f, &left, &rest);
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.28f, &right, &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.18f, &bottom, &center);
        ImGui::DockBuilderDockWindow("ブラウザ", left);
        ImGui::DockBuilderDockWindow("ビューポート", center);
        ImGui::DockBuilderDockWindow("インスペクタ", right);
        ImGui::DockBuilderDockWindow("ログ", bottom);
        ImGui::DockBuilderFinish(dock);
    }

    void menu_bar() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("ファイル")) {
            if (ImGui::MenuItem("インストールフォルダを開く…"))
                if (auto p = pick_folder(g_win.hwnd)) open_install(*p);
            if (ImGui::MenuItem("ビューポートを PNG で保存", "Ctrl+S", false, gb.lit != nullptr)) capture_requested = true;
            ImGui::Separator();
            if (ImGui::MenuItem("終了")) PostQuitMessage(0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("表示")) {
            for (uint32_t m = 0; m < uint32_t(ViewMode::Count); ++m)
                if (ImGui::MenuItem(view_mode_name(ViewMode(m)), nullptr, light.mode == ViewMode(m))) light.mode = ViewMode(m);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("言語")) {
            for (int l = 1; l < int(kTextLanguageCount); ++l)
                if (ImGui::MenuItem(std::string(language_code(TextLanguage(l))).c_str(), nullptr, language == l)) language = l;
            ImGui::EndMenu();
        }
        if (catalog) {
            std::lock_guard lk(catalog->mutex);
            if (!catalog->status.empty()) ImGui::TextDisabled("   %s", catalog->status.c_str());
        }
        std::string right = std::format("nim {}  |  {}", kVersion, gpu.adapter_name());
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 16);
        ImGui::TextDisabled("%s", right.c_str());
        ImGui::EndMainMenuBar();
    }

    void install_picker() {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin("Marathon が見つかりません", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
        ImGui::TextUnformatted("Steam のインストールを自動で見つけられませんでした。");
        ImGui::TextUnformatted("Marathon のフォルダ (Marathon.exe と packages があるところ) を選んでください。");
        if (ImGui::Button("フォルダを選択…"))
            if (auto p = pick_folder(g_win.hwnd)) open_install(*p);
        ImGui::End();
    }

    // -- browser --
    void browser_window() {
        ImGui::Begin("ブラウザ");
        if (ImGui::BeginTabBar("browser_tabs")) {
            if (ImGui::BeginTabItem("メッシュ")) { meshes_tab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("テキスト")) { strings_tab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("セリフ")) { dialogue_tab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("パッケージ")) { packages_tab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    void meshes_tab() {
        if (!catalog || !catalog->meshes_ready) { ImGui::TextDisabled("読込中…"); return; }
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##meshfilter", "タグ / パッケージ名で絞り込み", mesh_filter, sizeof(mesh_filter));
        std::string f = lower_ascii(mesh_filter);
        const auto& meshes = catalog->meshes;
        if (f != mesh_filter_applied) {
            mesh_filter_applied = f;
            mesh_view.clear();
            for (uint32_t i = 0; i < meshes.size(); ++i)
                if (f.empty() || contains_ci(meshes[i].tag.str(), f) || contains_ci(meshes[i].package, f)) mesh_view.push_back(i);
        }
        ImGui::TextDisabled("%zu / %zu 個  (クリックで表示)", mesh_view.size(), meshes.size());
        if (ImGui::BeginTable("meshes", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                               ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("タグ", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("サイズ", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("パッケージ");
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(mesh_view.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const auto& m = meshes[mesh_view[r]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    bool sel = scene.loaded() && scene.tag() == m.tag;
                    std::string id = m.tag.str();
                    if (ImGui::Selectable(id.c_str(), sel, ImGuiSelectableFlags_SpanAllColumns) && !sel) pending_load = m.tag;
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", m.size);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(m.package.c_str());
                }
            ImGui::EndTable();
        }
    }

    void strings_tab() {
        if (!catalog || !catalog->strings_ready) { ImGui::TextDisabled("読込中…"); return; }
        const auto& all = catalog->strings.strings();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##strfilter", "検索 (英語・選択中の言語・ハッシュ)", string_filter, sizeof(string_filter));
        std::string f = lower_ascii(string_filter);
        if (f != string_filter_applied || string_filter_lang != language) {
            string_filter_applied = f;
            string_filter_lang = language;
            string_view.clear();
            for (uint32_t i = 0; i < all.size(); ++i) {
                const auto& s = all[i];
                if (f.empty() || contains_ci(s.text[0], f) || s.text[size_t(language)].find(string_filter) != std::string::npos ||
                    contains_ci(std::format("{:08x}", s.hash), f))
                    string_view.push_back(i);
            }
        }
        ImGui::TextDisabled("%zu / %zu 件  (右クリックでコピー)", string_view.size(), all.size());
        const std::string lang_col(language_code(TextLanguage(language)));
        if (ImGui::BeginTable("strings", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("ハッシュ", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("en");
            ImGui::TableSetupColumn(lang_col.c_str());
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(string_view.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const auto& s = all[string_view[r]];
                    ImGui::TableNextRow();
                    ImGui::PushID(r);
                    ImGui::TableNextColumn();
                    ImGui::Selectable(std::format("{:08X}", s.hash).c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
                    if (ImGui::BeginPopupContextItem("ctx")) {
                        if (ImGui::MenuItem("英語をコピー")) ImGui::SetClipboardText(s.text[0].c_str());
                        if (ImGui::MenuItem((lang_col + " をコピー").c_str())) ImGui::SetClipboardText(s.text[size_t(language)].c_str());
                        if (ImGui::MenuItem("ハッシュをコピー")) ImGui::SetClipboardText(std::format("{:08X}", s.hash).c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.text[0].c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(s.text[size_t(language)].c_str());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }

    const std::string& text(uint32_t hash, size_t lang) {
        static const std::string empty;
        if (!catalog || !catalog->strings_ready) return empty;
        const LocalizedString* s = catalog->strings.find(hash);
        return s ? s->text[lang] : empty;
    }

    void dialogue_tab() {
        if (!catalog || !catalog->dialogue_ready) { ImGui::TextDisabled("読込中…"); return; }
        const auto& lines = catalog->dialogue;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##dlgfilter", "話者・台詞で検索", dialogue_filter, sizeof(dialogue_filter));
        std::string f = lower_ascii(dialogue_filter);
        if (f != dialogue_filter_applied || dialogue_filter_lang != language) {
            dialogue_filter_applied = f;
            dialogue_filter_lang = language;
            dialogue_view.clear();
            for (uint32_t i = 0; i < lines.size(); ++i) {
                const auto& l = lines[i];
                if (f.empty() || contains_ci(text(l.speaker_hash, 0), f) || contains_ci(text(l.subtitle_hash, 0), f) ||
                    text(l.subtitle_hash, size_t(language)).find(dialogue_filter) != std::string::npos ||
                    text(l.speaker_hash, size_t(language)).find(dialogue_filter) != std::string::npos)
                    dialogue_view.push_back(i);
            }
        }
        ImGui::TextDisabled("%zu / %zu 行", dialogue_view.size(), lines.size());
        if (ImGui::BeginTable("dialogue", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                 ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("話者", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("台詞");
            ImGui::TableSetupColumn("音声イベント", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("WEM", ImGuiTableColumnFlags_WidthFixed, 36);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip;
            clip.Begin(int(dialogue_view.size()));
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    const auto& l = lines[dialogue_view[r]];
                    ImGui::TableNextRow();
                    ImGui::PushID(r);
                    ImGui::TableNextColumn();
                    const std::string& sp = text(l.speaker_hash, size_t(language));
                    ImGui::TextUnformatted((sp.empty() ? text(l.speaker_hash, 0) : sp).c_str());
                    ImGui::TableNextColumn();
                    const std::string& en = text(l.subtitle_hash, 0);
                    const std::string& tr = text(l.subtitle_hash, size_t(language));
                    ImGui::TextWrapped("%s", en.c_str());
                    if (!tr.empty() && tr != en) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.65f, 0.8f, 1.0f, 1.0f});
                        ImGui::TextWrapped("%s", tr.c_str());
                        ImGui::PopStyleColor();
                    }
                    if (ImGui::BeginPopupContextItem("ctx")) {
                        if (ImGui::MenuItem("英語をコピー")) ImGui::SetClipboardText(en.c_str());
                        if (ImGui::MenuItem("翻訳をコピー")) ImGui::SetClipboardText(tr.c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(l.audio_event.is_none() ? "-" : l.audio_event.str().c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", l.wem.size());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
    }

    void packages_tab() {
        if (!pm) return;
        const auto& pkgs = pm->packages();
        ImGui::BeginChild("pkglist", {0, ImGui::GetContentRegionAvail().y * 0.35f}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        ImGuiListClipper clip;
        clip.Begin(int(pkgs.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                std::string label = std::format("{:04X}  {}", pkgs[i].pkg_id, pkgs[i].name);
                if (ImGui::Selectable(label.c_str(), selected_package == i)) {
                    selected_package = i;
                    selected_entry = -1;
                    package_rows.clear();
                    try {
                        Package& p = pm->open(pkgs[i]);
                        for (uint32_t e = 0; e < p.entries().size(); ++e) {
                            const Entry& en = p.entries()[e];
                            package_rows.push_back({TagHash::make(pkgs[i].pkg_id, uint16_t(e)), en.type, en.subtype,
                                                    en.reference, en.file_size});
                        }
                    } catch (const std::exception& ex) {
                        error(std::format("パッケージを開けません: {}", ex.what()));
                    }
                }
            }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##entfilter", "種別 (例 8, 32/1) またはクラス", entry_filter, sizeof(entry_filter));
        std::string f = lower_ascii(entry_filter);
        ImGui::BeginChild("entries", {0, ImGui::GetContentRegionAvail().y * 0.6f}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        if (ImGui::BeginTable("entrytable", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("タグ", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("種別", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("クラス/参照", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("サイズ");
            ImGui::TableHeadersRow();
            std::vector<int> rows;
            for (int i = 0; i < int(package_rows.size()); ++i) {
                const auto& r = package_rows[i];
                std::string ty = std::format("{}/{}", r.type, r.subtype);
                if (f.empty() || ty == f || std::to_string(r.type) == f || contains_ci(std::format("{:08x}", r.reference), f))
                    rows.push_back(i);
            }
            ImGuiListClipper c2;
            c2.Begin(int(rows.size()));
            while (c2.Step())
                for (int k = c2.DisplayStart; k < c2.DisplayEnd; ++k) {
                    const auto& r = package_rows[rows[k]];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(r.tag.str().c_str(), selected_entry == rows[k],
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                        selected_entry = rows[k];
                        entry_preview.clear();
                        entry_preview_error.clear();
                        try {
                            entry_preview = pm->read(r.tag);
                        } catch (const std::exception& ex) {
                            entry_preview_error = ex.what();
                        }
                        if (ImGui::IsMouseDoubleClicked(0) && r.type == 8 && r.reference == kClassStaticMesh) pending_load = r.tag;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", r.type, r.subtype);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", r.reference);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u%s", r.size, r.reference == kClassStaticMesh && r.type == 8 ? "  (静的メッシュ)" : "");
                }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::BeginChild("hex", {0, 0}, ImGuiChildFlags_Borders);
        if (!entry_preview_error.empty()) ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", entry_preview_error.c_str());
        else if (!entry_preview.empty()) {
            ImGui::TextDisabled("%zu バイト", entry_preview.size());
            ImGuiListClipper c3;
            c3.Begin(int((entry_preview.size() + 15) / 16));
            while (c3.Step())
                for (int row = c3.DisplayStart; row < c3.DisplayEnd; ++row) {
                    std::string line = std::format("{:08X} ", row * 16);
                    std::string ascii;
                    for (int b = 0; b < 16; ++b) {
                        size_t i = size_t(row) * 16 + b;
                        if (i < entry_preview.size()) {
                            line += std::format(" {:02X}", entry_preview[i]);
                            ascii += entry_preview[i] >= 32 && entry_preview[i] < 127 ? char(entry_preview[i]) : '.';
                        } else line += "   ";
                    }
                    ImGui::TextUnformatted((line + "  " + ascii).c_str());
                }
        }
        ImGui::EndChild();
    }

    // -- viewport --
    void viewport_window() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        viewport_visible = ImGui::Begin("ビューポート");
        ImGui::PopStyleVar();
        if (viewport_visible) {
            // toolbar
            ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
            ImGui::SetNextItemWidth(190);
            if (ImGui::BeginCombo("##mode", view_mode_name(light.mode))) {
                for (uint32_t m = 0; m < uint32_t(ViewMode::Count); ++m)
                    if (ImGui::Selectable(view_mode_name(ViewMode(m)), light.mode == ViewMode(m))) light.mode = ViewMode(m);
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("露出", &light.exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SameLine();
            if (ImGui::Button("ライト…")) ImGui::OpenPopup("light");
            if (ImGui::BeginPopup("light")) {
                ImGui::SliderAngle("太陽の方位", &light.sun_azimuth, -180, 180);
                ImGui::SliderAngle("太陽の高度", &light.sun_elevation, -10, 90);
                ImGui::SliderFloat("太陽の強さ", &light.sun_intensity, 0, 10);
                ImGui::ColorEdit3("太陽の色", &light.sun_color.x);
                ImGui::SliderFloat("空の強さ", &light.sky_intensity, 0, 4);
                ImGui::ColorEdit3("空 (天頂)", &light.sky_top.x);
                ImGui::ColorEdit3("空 (地平)", &light.sky_horizon.x);
                ImGui::ColorEdit3("地面", &light.ground.x);
                ImGui::ColorEdit3("背景 (上)", &light.background_top.x);
                ImGui::ColorEdit3("背景 (下)", &light.background_bottom.x);
                if (ImGui::Button("既定値に戻す")) { ViewMode m = light.mode; light = LightingParams{}; light.mode = m; }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("フレーム (F)")) frame_scene();
            ImGui::SameLine();
            if (ImGui::Button("PNG 保存")) capture_requested = true;

            ImVec2 avail = ImGui::GetContentRegionAvail();
            view_w = uint32_t(std::max(16.0f, avail.x));
            view_h = uint32_t(std::max(16.0f, avail.y));
            if (gb.lit) {
                ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::Image(ImTextureID(gpu.srv().gpu(gb.srv_first + 5).ptr), {float(gb.width()), float(gb.height())});
                ImGui::SetCursorScreenPos(origin);
                ImGui::InvisibleButton("view", {float(gb.width()), float(gb.height())},
                                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                           ImGuiButtonFlags_MouseButtonMiddle);
                handle_camera(ImGui::IsItemHovered(), ImGui::IsItemActive());
                if (!scene.loaded()) {
                    const char* msg = pm ? "ブラウザのメッシュをクリックすると、ゲームのシェーダで描画します"
                                         : "Marathon のインストールを開いてください";
                    ImVec2 ts = ImGui::CalcTextSize(msg);
                    ImGui::GetWindowDrawList()->AddText({origin.x + (gb.width() - ts.x) * 0.5f, origin.y + gb.height() * 0.5f},
                                                        IM_COL32(200, 200, 200, 200), msg);
                } else {
                    std::string hud = std::format("{}  LOD {}  半径 {:.2f}", scene.tag().str(), scene.lod(), scene.radius());
                    ImGui::GetWindowDrawList()->AddText({origin.x + 8, origin.y + 6}, IM_COL32(230, 230, 230, 220), hud.c_str());
                }
            }
        }
        ImGui::End();
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_S) && gb.lit) capture_requested = true;
    }

    void frame_scene() {
        if (!scene.loaded()) return;
        orbit.target = scene.center();
        orbit.distance = scene.radius() / std::sin(20.0f * 3.14159265f / 180.0f) * 0.9f;
    }

    void handle_camera(bool hovered, bool active) {
        ImGuiIO& io = ImGui::GetIO();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
            orbit.yaw -= io.MouseDelta.x * 0.008f;
            orbit.pitch = std::clamp(orbit.pitch + io.MouseDelta.y * 0.008f, -1.55f, 1.55f);
        }
        if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0))) {
            ViewMatrices vm = make_view(current_view());
            float k = orbit.distance * 0.0015f;
            orbit.target = orbit.target - vm.right * (io.MouseDelta.x * k) + vm.up * (io.MouseDelta.y * k);
        }
        if (hovered && io.MouseWheel != 0) orbit.distance *= std::pow(0.88f, io.MouseWheel);
        orbit.distance = std::clamp(orbit.distance, orbit.radius * 0.05f, orbit.radius * 200.0f);
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_F)) frame_scene();
    }

    // -- inspector --
    void inspector_window() {
        ImGui::Begin("インスペクタ");
        if (!scene.loaded()) {
            ImGui::TextDisabled("メッシュ未選択");
            ImGui::End();
            return;
        }
        ImGui::Text("静的メッシュ %s", scene.tag().str().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("コピー")) ImGui::SetClipboardText(scene.tag().str().c_str());
        ImGui::Text("LOD %u   グループ %zu   半径 %.3f", scene.lod(), scene.groups().size(), scene.radius());
        ImGui::Separator();
        for (size_t i = 0; i < scene.groups().size(); ++i) {
            const auto& g = scene.groups()[i];
            ImGui::PushID(int(i));
            if (ImGui::CollapsingHeader(std::format("パート {}  ({} インデックス)", g.part, g.index_count).c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("テクニック  %s", g.technique.str().c_str());
                ImGui::Text("VS %s   PS %s", g.vs.str().c_str(), g.ps.str().c_str());
                ImGui::Text("cbuffer %zu レジスタ   TFX %zu 命令", g.cbuffer_regs, g.vm_instructions);
                for (const auto& s : g.stubbed_ops) ImGui::TextColored({1, 0.75f, 0.3f, 1}, "未実装 TFX op: %s", s.c_str());
                if (ImGui::BeginTable("tex", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 34);
                    ImGui::TableSetupColumn("テクスチャ", ImGuiTableColumnFlags_WidthFixed, 76);
                    ImGui::TableSetupColumn("形式", ImGuiTableColumnFlags_WidthFixed, 34);
                    ImGui::TableSetupColumn("サイズ");
                    ImGui::TableHeadersRow();
                    for (const auto& t : g.textures) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("t%u", t.slot);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(t.tag.str().c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%u", t.format);
                        ImGui::TableNextColumn();
                        if (t.error.empty()) ImGui::Text("%ux%u, mip %u", t.width, t.height, t.mips);
                        else ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", t.error.c_str());
                    }
                    for (const auto& e : g.extern_textures) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("t%u", e.slot);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("extern %u", e.extern_id);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("-");
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("エンジン側 (未対応, 代替を使用)");
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::PopID();
        }
        ImGui::End();
    }

    void log_window() {
        ImGui::Begin("ログ");
        for (const auto& l : log) {
            if (l.error) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", l.text.c_str());
            else ImGui::TextUnformatted(l.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::End();
    }
};

void load_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    const fs::path fonts = "C:/Windows/Fonts";
    const char* bases[] = {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc", "segoeui.ttf"};
    const char* merges[] = {"segoeui.ttf", "malgun.ttf", "msyh.ttc", "msjh.ttc", "seguisym.ttf"};
    bool have_base = false;
    for (const char* b : bases) {
        if (!fs::exists(fonts / b)) continue;
        ImFontConfig cfg;
        io.Fonts->AddFontFromFileTTF((fonts / b).string().c_str(), 16.0f * scale, &cfg);
        have_base = true;
        break;
    }
    if (!have_base) io.Fonts->AddFontDefault();
    for (const char* m : merges) {
        if (!fs::exists(fonts / m)) continue;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF((fonts / m).string().c_str(), 16.0f * scale, &cfg);
    }
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4;
    s.FrameRounding = 3;
    s.TabRounding = 3;
    s.ScrollbarRounding = 3;
    s.GrabRounding = 3;
    s.WindowBorderSize = 1;
    ImVec4* c = s.Colors;
    const ImVec4 accent{0.29f, 0.85f, 0.78f, 1.0f};  // Marathon-ish cyan
    c[ImGuiCol_WindowBg] = {0.085f, 0.09f, 0.10f, 1};
    c[ImGuiCol_ChildBg] = {0.07f, 0.075f, 0.085f, 1};
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = {0.06f, 0.065f, 0.075f, 1};
    c[ImGuiCol_FrameBg] = {0.14f, 0.15f, 0.17f, 1};
    c[ImGuiCol_FrameBgHovered] = {0.19f, 0.21f, 0.24f, 1};
    c[ImGuiCol_Header] = {0.29f, 0.85f, 0.78f, 0.22f};
    c[ImGuiCol_HeaderHovered] = {0.29f, 0.85f, 0.78f, 0.35f};
    c[ImGuiCol_HeaderActive] = {0.29f, 0.85f, 0.78f, 0.5f};
    c[ImGuiCol_Button] = {0.17f, 0.19f, 0.22f, 1};
    c[ImGuiCol_ButtonHovered] = {0.29f, 0.85f, 0.78f, 0.45f};
    c[ImGuiCol_ButtonActive] = {0.29f, 0.85f, 0.78f, 0.7f};
    c[ImGuiCol_Tab] = {0.12f, 0.13f, 0.15f, 1};
    c[ImGuiCol_TabHovered] = {0.29f, 0.85f, 0.78f, 0.45f};
    c[ImGuiCol_TabSelected] = {0.18f, 0.32f, 0.31f, 1};
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_DockingPreview] = {0.29f, 0.85f, 0.78f, 0.5f};
    c[ImGuiCol_TextSelectedBg] = {0.29f, 0.85f, 0.78f, 0.35f};
}

int run(const std::wstring& cmdline) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"nim", nullptr};
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_win.hwnd = CreateWindowW(wc.lpszClassName, L"nim — Marathon アセットビューワー", WS_OVERLAPPEDWINDOW, 80, 60,
                               int(1600 * scale), int(960 * scale), nullptr, nullptr, wc.hInstance, nullptr);

    App app;
    try {
        app.gpu.init(false);
        init_swapchain(app.gpu);
        app.lighting.init(app.gpu);
    } catch (const std::exception& ex) {
        MessageBoxW(g_win.hwnd, to_wide(std::string("D3D12 の初期化に失敗しました:\n") + ex.what()).c_str(), L"nim", MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_win.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_win.hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string ini = (exe_dir() / "nim_layout.ini").string();
    io.IniFilename = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    apply_style();
    ImGui::GetStyle().ScaleAllSizes(scale);
    load_fonts(scale);
    ImGui_ImplWin32_Init(g_win.hwnd);
    static Gpu* s_gpu = &app.gpu;
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = app.gpu.device();
    ii.CommandQueue = app.gpu.queue();
    ii.NumFramesInFlight = Gpu::kFramesInFlight;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = app.gpu.srv().heap();
    ii.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
        uint32_t i = s_gpu->srv().alloc(1);
        *cpu = s_gpu->srv().cpu(i);
        *gpu = s_gpu->srv().gpu(i);
    };
    ii.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
        D3D12_CPU_DESCRIPTOR_HANDLE base = s_gpu->srv().cpu(0);
        UINT inc = s_gpu->device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        s_gpu->srv().free(uint32_t((cpu.ptr - base.ptr) / inc), 1);
    };
    ImGui_ImplDX12_Init(&ii);

    app.info(std::format("nim {} — GPU: {}", kVersion, app.gpu.adapter_name()));
    // Command line: [INSTALL_DIR] [STATIC_MESH_TAG] in any order.
    std::optional<fs::path> install_arg;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
        for (int i = 0; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            if (fs::is_directory(a)) install_arg = fs::path(a);
            else if (a.size() == 8 && a.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos)
                app.pending_load = TagHash(uint32_t(std::stoul(a, nullptr, 16)));
        }
        LocalFree(argv);
    }
    if (install_arg) app.open_install(*install_arg);
    else if (auto found = PackageManager::find_install()) app.open_install(*found);
    else app.error("Marathon の Steam インストールが見つかりません。ファイル → インストールフォルダを開く から選んでください。");

    uint64_t frame = 0;
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(g_win.hwnd)) { Sleep(16); continue; }
        if (g_win.pending_w && g_win.pending_h) {
            resize_swapchain(app.gpu, g_win.pending_w, g_win.pending_h);
            g_win.pending_w = g_win.pending_h = 0;
        }

        const uint32_t fi = uint32_t(frame % Gpu::kFramesInFlight);
        WaitForSingleObjectEx(g_win.waitable, 1000, TRUE);
        app.gpu.wait(g_win.fence[fi]);
        app.gpu.begin_frame(fi);
        app.apply_pending(app.gpu);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app.ui();
        ImGui::Render();

        auto* cl = g_win.list.Get();
        check_hr(g_win.alloc[fi]->Reset(), "allocator Reset");
        check_hr(cl->Reset(g_win.alloc[fi].Get(), nullptr), "list Reset");
        try {
            if (app.viewport_visible) app.record(cl);
        } catch (const std::exception& ex) {
            app.error(std::string("描画エラー: ") + ex.what());
        }
        const UINT bb = g_win.swapchain->GetCurrentBackBufferIndex();
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0.05f, 0.05f, 0.06f, 1};
        cl->ClearRenderTargetView(g_win.rtv[bb], clear, 0, nullptr);
        cl->OMSetRenderTargets(1, &g_win.rtv[bb], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {app.gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        check_hr(cl->Close(), "Close");
        ID3D12CommandList* lists[] = {cl};
        app.gpu.queue()->ExecuteCommandLists(1, lists);
        g_win.swapchain->Present(1, 0);
        g_win.fence[fi] = app.gpu.signal();
        ++frame;
        app.after_frame();
    }

    app.gpu.wait_idle();
    if (app.loader.joinable()) app.loader.join();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    app.scene.release(app.gpu);
    app.gb.release(app.gpu);
    DestroyWindow(g_win.hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdline, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int r = run(std::wstring(L"nim ") + cmdline);
    CoUninitialize();
    return r;
}
