// nim — Marathon asset viewer (GUI).
//
// Dear ImGui (docking) over D3D12, laid out after goriath: a row of document tabs, a HOME page of
// big buttons, and browsers whose right-hand panel is a live 3D view. Meshes are drawn with the
// game's own G-buffer shaders and, by default, shaded by the game's own deferred lighting pass;
// the UI itself is set in the game's fonts, read from the user's install at startup.
#include "nim/content/dialogue.hpp"
#include "nim/content/fonts.hpp"
#include "nim/content/strings.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/game_lighting.hpp"
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
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <map>
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

constexpr const char* kVersion = "0.3.0";
constexpr ImU32 kGreen = IM_COL32(201, 254, 0, 255);  // Marathon green
constexpr uint32_t kThumbSize = 256;

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
// UTF-8 for a private-use icon codepoint (Segoe Fluent Icons / MDL2 Assets).
std::string icon(uint32_t cp) {
    std::string s;
    s += char(0xE0 | (cp >> 12));
    s += char(0x80 | ((cp >> 6) & 0x3F));
    s += char(0x80 | (cp & 0x3F));
    return s;
}
namespace ico {
constexpr uint32_t Home = 0xE80F, Settings = 0xE713, Statics = 0xE7B8, Text = 0xE8D2, Dialogue = 0xE8BD,
                   Packages = 0xE8B7, Log = 0xE9D9, Tune = 0xE9E9, Frame = 0xE740, Save = 0xE74E, Light = 0xE706,
                   Search = 0xE721, Fonts = 0xE8E9, Link = 0xE71B, Eye = 0xE890;
}
std::string format_bytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(b);
    int u = 0;
    while (v >= 1024 && u < 4) v /= 1024, ++u;
    return std::format("{:.1f} {}", v, units[u]);
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

// ---- fonts ----------------------------------------------------------------------------------------
struct Fonts {
    ImFont* body = nullptr;     // PP Fraktion Mono + Noto Sans JP/KR/SC/TC + icons
    ImFont* bold = nullptr;
    ImFont* heading = nullptr;  // MarathonShapiro Wide 65
    ImFont* button = nullptr;   // KH Interference
    bool from_game = false;
};
Fonts g_fonts;
std::vector<std::vector<uint8_t>> g_font_data;  // the atlas reads these lazily (dynamic fonts)

std::vector<uint8_t> read_file(const fs::path& p) {
    std::vector<uint8_t> d;
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        d.resize(size_t(ftell(f)));
        fseek(f, 0, SEEK_SET);
        if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
        fclose(f);
    }
    return d;
}

void load_fonts(PackageManager* pm) {
    ImGuiIO& io = ImGui::GetIO();
    std::map<std::string, std::vector<uint8_t>> game;
    if (pm) {
        try {
            for (auto& f : load_game_fonts(*pm)) game.emplace(f.postscript, std::move(f.data));
        } catch (...) {
        }
    }
    const fs::path sys = "C:/Windows/Fonts";
    auto add = [&](std::vector<uint8_t> data, float size, ImFont* merge_into, float extra = 1.0f) -> ImFont* {
        if (data.empty()) return nullptr;
        g_font_data.push_back(std::move(data));
        auto& d = g_font_data.back();
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.MergeMode = merge_into != nullptr;
        cfg.DstFont = merge_into;
        cfg.ExtraSizeScale = extra;
        return io.Fonts->AddFontFromMemoryTTF(d.data(), int(d.size()), size, &cfg);
    };
    auto game_font = [&](const char* ps) -> std::vector<uint8_t> {
        auto it = game.find(ps);
        return it == game.end() ? std::vector<uint8_t>{} : it->second;
    };
    auto first_sys = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            if (auto d = read_file(sys / n); !d.empty()) return d;
        return std::vector<uint8_t>{};
    };
    auto icons = [&] { return first_sys({"SegoeIcons.ttf", "segmdl2.ttf"}); };
    auto cjk = [&](ImFont* f, const char* jp) {
        if (!f) return;
        if (auto d = game_font(jp); !d.empty()) {
            // Noto is drawn smaller than PP Fraktion Mono at the same pixel size.
            add(std::move(d), 0, f, 1.3f);
            add(game_font("NotoSansKR-Regular"), 0, f, 1.3f);
            add(game_font("NotoSansSC-Regular"), 0, f, 1.3f);
            add(game_font("NotoSansTC-Regular"), 0, f, 1.3f);
        } else {
            add(first_sys({"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}), 0, f);
            add(first_sys({"malgun.ttf"}), 0, f);
            add(first_sys({"msyh.ttc"}), 0, f);
        }
        add(icons(), 0, f);
        add(first_sys({"segoeui.ttf"}), 0, f);
    };

    g_fonts.from_game = game.count("PPFraktionMono-Regular") != 0;
    g_fonts.body = add(game_font("PPFraktionMono-Regular"), 17, nullptr);
    if (!g_fonts.body) g_fonts.body = add(first_sys({"segoeui.ttf"}), 17, nullptr);
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    cjk(g_fonts.body, "NotoSansJP-Regular");
    g_fonts.bold = add(game_font("PPFraktionMono-Bold"), 17, nullptr);
    if (!g_fonts.bold) g_fonts.bold = add(first_sys({"segoeuib.ttf"}), 17, nullptr);
    cjk(g_fonts.bold, "NotoSansJP-Bold");
    g_fonts.heading = add(game_font("MarathonShapiro-Wide65"), 42, nullptr);
    if (!g_fonts.heading) g_fonts.heading = add(first_sys({"segoeuib.ttf"}), 42, nullptr);
    cjk(g_fonts.heading, "NotoSansJP-Bold");
    g_fonts.button = add(game_font("KHInterference-Regular"), 24, nullptr);
    if (!g_fonts.button) g_fonts.button = add(first_sys({"segoeuisl.ttf", "segoeui.ttf"}), 24, nullptr);
    cjk(g_fonts.button, "NotoSansJP-Medium");
    if (!g_fonts.bold) g_fonts.bold = g_fonts.body;
    if (!g_fonts.heading) g_fonts.heading = g_fonts.bold;
    if (!g_fonts.button) g_fonts.button = g_fonts.body;
    io.FontDefault = g_fonts.body;
}

// goriath's look: near-black panels, white text, black tab bar, white "D buttons".
void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 0;
    s.FrameRounding = 4;
    s.PopupRounding = 4;
    s.TabRounding = 0;
    s.ScrollbarRounding = 4;
    s.GrabRounding = 4;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.WindowPadding = {12, 10};
    s.FramePadding = {8, 7};  // 32 px tab bar
    s.ItemSpacing = {14, 8};
    s.ItemInnerSpacing = {8, 6};
    s.ScrollbarSize = 12;
    s.DockingSeparatorSize = 2;
    s.TabBarOverlineSize = 2;
    s.TabBarBorderSize = 0;
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1) { return ImVec4{r / 255.0f, g / 255.0f, b / 255.0f, a}; };
    const ImVec4 panel = rgb(19, 19, 22), bar = rgb(4, 4, 4), sel = rgb(0, 92, 128);
    c[ImGuiCol_Text] = {1, 1, 1, 1};
    c[ImGuiCol_TextDisabled] = rgb(150, 150, 156);
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0.376f};  // black alpha 96
    c[ImGuiCol_PopupBg] = rgb(14, 14, 16, 0.98f);
    c[ImGuiCol_Border] = rgb(48, 48, 54);
    c[ImGuiCol_FrameBg] = rgb(36, 36, 42);
    c[ImGuiCol_FrameBgHovered] = rgb(52, 52, 60);
    c[ImGuiCol_FrameBgActive] = rgb(64, 64, 72);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = bar;
    c[ImGuiCol_MenuBarBg] = bar;
    c[ImGuiCol_Header] = sel;
    c[ImGuiCol_HeaderHovered] = rgb(0, 92, 128, 0.6f);
    c[ImGuiCol_HeaderActive] = rgb(0, 110, 150);
    c[ImGuiCol_Button] = rgb(40, 40, 46);
    c[ImGuiCol_ButtonHovered] = rgb(60, 60, 68);
    c[ImGuiCol_ButtonActive] = rgb(80, 80, 90);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrab] = ImGui::ColorConvertU32ToFloat4(kGreen);
    c[ImGuiCol_SliderGrabActive] = {1, 1, 1, 1};
    c[ImGuiCol_Tab] = bar;
    c[ImGuiCol_TabHovered] = rgb(38, 38, 44);
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = {1, 1, 1, 1};
    c[ImGuiCol_TabDimmed] = bar;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = {0, 0, 0, 0};
    c[ImGuiCol_DockingPreview] = rgb(201, 254, 0, 0.5f);
    c[ImGuiCol_DockingEmptyBg] = panel;
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = rgb(48, 48, 54);
    c[ImGuiCol_SeparatorActive] = ImGui::ColorConvertU32ToFloat4(kGreen);
    c[ImGuiCol_TableHeaderBg] = rgb(28, 28, 32);
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.025f};
    c[ImGuiCol_TextSelectedBg] = rgb(0, 92, 128, 0.7f);
    c[ImGuiCol_NavCursor] = {1, 1, 1, 1};
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// goriath's DButton: white rounded slab with black text (or black with a white stroke).
bool dbutton(const char* label, float min_w, bool black = false) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushFont(g_fonts.button, 24);
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 size{std::max(min_w, ts.x + 50 * sc), std::max(60 * sc, ts.y + 40 * sc)};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    auto* dl = ImGui::GetWindowDrawList();
    const float r = 8 * sc;
    dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, black ? IM_COL32_BLACK : IM_COL32_WHITE, r);
    if (black) dl->AddRect(p, {p.x + size.x, p.y + size.y}, IM_COL32_WHITE, r, 0, 2 * sc);
    dl->AddText({p.x + 25 * sc, p.y + (size.y - ts.y) * 0.5f}, black ? IM_COL32_WHITE : IM_COL32_BLACK, label);
    if (hovered) {
        dl->AddRectFilled(p, {p.x + size.x, p.y + size.y}, IM_COL32(0, 0, 0, 96), r);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PopFont();
    return pressed;
}

void heading(const char* text) {
    ImGui::PushFont(g_fonts.heading, 42);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// Shadowed text, for HUDs drawn over the 3D view.
void hud_text(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text) {
    dl->AddText({p.x + 1, p.y + 1}, IM_COL32(0, 0, 0, 200), text);
    dl->AddText(p, col, text);
}

void spinner(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    const float t = float(ImGui::GetTime());
    const int n = 24;
    const float a0 = t * 6.0f;
    dl->PathClear();
    for (int i = 0; i <= n; ++i) {
        float a = a0 + float(i) / n * 4.5f;
        dl->PathLineTo({c.x + std::cos(a) * r, c.y + std::sin(a) * r});
    }
    dl->PathStroke(col, 0, r * 0.25f);
}

// ---- data loaded in the background -----------------------------------------------------------------
struct MeshEntry { TagHash tag; uint32_t size; uint16_t pkg; };
struct PackageGroup { uint16_t pkg_id; std::string name; std::vector<uint32_t> meshes; };
struct EntryRow { TagHash tag; uint16_t type, subtype; uint32_t reference, size; };

struct Catalog {
    std::mutex mutex;
    std::string status = "準備中…";
    std::atomic<bool> meshes_ready{false}, strings_ready{false}, dialogue_ready{false}, done{false};
    std::vector<MeshEntry> meshes;
    std::vector<PackageGroup> packages;  // packages that hold static meshes
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
        std::vector<PackageGroup> groups;
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            PackageGroup g{info.pkg_id, info.name, {}};
            for (uint32_t i = 0; i < p.entries().size(); ++i) {
                const Entry& e = p.entries()[i];
                if (e.type == 8 && e.reference == kClassStaticMesh) {
                    g.meshes.push_back(uint32_t(meshes.size()));
                    meshes.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.file_size, info.pkg_id});
                }
            }
            if (!g.meshes.empty()) groups.push_back(std::move(g));
        }
        {
            std::lock_guard lk(cat->mutex);
            cat->meshes = std::move(meshes);
            cat->packages = std::move(groups);
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

// Reads and evaluates meshes off the UI thread (own PackageManager); the UI thread only uploads.
// One slot for the live view (latest request wins) and a queue for thumbnails.
class MeshWorker {
public:
    struct Result {
        TagHash tag;
        bool thumbnail = false;
        std::optional<PreparedMesh> mesh;
        std::string error;
        double ms = 0;
    };

    void start(const fs::path& install) {
        stop();
        quit_ = false;
        thread_ = std::thread([this, install] { run(install); });
    }
    void stop() {
        {
            std::lock_guard lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        live_.reset();
        thumbs_.clear();
        results_.clear();
    }
    void request_live(TagHash t) {
        {
            std::lock_guard lk(m_);
            live_ = t;
        }
        cv_.notify_all();
    }
    void request_thumbnails(std::vector<TagHash> tags) {
        {
            std::lock_guard lk(m_);
            thumbs_.assign(tags.begin(), tags.end());
        }
        cv_.notify_all();
    }
    std::optional<Result> take(bool thumbnail) {
        std::lock_guard lk(m_);
        for (auto it = results_.begin(); it != results_.end(); ++it)
            if (it->thumbnail == thumbnail) {
                Result r = std::move(*it);
                results_.erase(it);
                return r;
            }
        return std::nullopt;
    }
    bool busy_live() {
        std::lock_guard lk(m_);
        return live_.has_value() || working_live_;
    }
    ~MeshWorker() { stop(); }

private:
    void run(fs::path install) {
        std::unique_ptr<PackageManager> pm;
        try {
            pm = std::make_unique<PackageManager>(install);
            pm->build_hash64_index();
        } catch (...) {
            return;
        }
        for (;;) {
            TagHash tag;
            bool thumb = false;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return quit_ || live_ || !thumbs_.empty(); });
                if (quit_) return;
                if (live_) {
                    tag = *live_;
                    live_.reset();
                    working_live_ = true;
                } else {
                    tag = thumbs_.front();
                    thumbs_.pop_front();
                    thumb = true;
                }
                // Keep the hand-off small: the UI uploads one result per frame.
                if (thumb) {
                    size_t pending = 0;
                    for (auto& r : results_) pending += r.thumbnail;
                    if (pending >= 2) {
                        thumbs_.push_front(tag);
                        lk.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                        continue;
                    }
                }
            }
            Result r;
            r.tag = tag;
            r.thumbnail = thumb;
            auto t0 = std::chrono::steady_clock::now();
            try {
                r.mesh = prepare_static_mesh(*pm, tag);
            } catch (const std::exception& ex) {
                r.error = ex.what();
            }
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard lk(m_);
            if (!thumb) working_live_ = false;
            results_.push_back(std::move(r));
        }
    }

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, working_live_ = false;
    std::optional<TagHash> live_;
    std::deque<TagHash> thumbs_;
    std::deque<Result> results_;
};

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

// Orbit camera; holding the right mouse button flies (WASD / QE, Shift x5, Ctrl /5, wheel = speed).
struct Camera {
    V3 target;
    float yaw = -0.93f, pitch = 0.41f, distance = 10, radius = 1;
    float fly_speed = 1;
};

enum class Engine : int { Game, Nim };

enum Tab : int { TabHome, TabSettings, TabStatics, TabText, TabDialogue, TabPackages, TabLog, TabCount };
struct TabInfo { uint32_t icon; const char* title; const char* id; bool closable; };
constexpr TabInfo kTabs[TabCount] = {
    {ico::Home, "HOME", "home", false},         {ico::Settings, "", "settings", false},
    {ico::Statics, "STATICS", "statics", true}, {ico::Text, "TEXT", "text", true},
    {ico::Dialogue, "DIALOGUE", "dialogue", true}, {ico::Packages, "PACKAGES", "packages", true},
    {ico::Log, "LOG", "log", true},
};

struct Thumb {
    ComPtr<ID3D12Resource> tex;
    uint32_t srv = UINT32_MAX;
    uint64_t last_used = 0;
    bool failed = false;
    std::string error;
};

class App {
public:
    Gpu gpu;
    fs::path install;
    std::unique_ptr<PackageManager> pm;
    std::vector<std::vector<InputElement>> layouts;
    std::unique_ptr<Catalog> catalog;
    std::thread loader;
    MeshWorker worker;

    // live view
    StaticMeshScene scene;
    GBuffer gb;
    LightingPass lighting;
    GameLighting game_lighting;
    bool game_ready = false;
    Engine engine = Engine::Game;
    LightingParams light;
    Camera cam;
    uint32_t view_w = 0, view_h = 0;
    bool viewport_visible = false;
    std::optional<TagHash> loading;
    bool capture_requested = false;
    bool show_fps = true;

    // thumbnails
    StaticMeshScene thumb_scene;
    GBuffer thumb_gb;
    GameLighting thumb_lighting;
    bool thumb_game_ready = false;
    bool thumbnails_enabled = true;
    std::unordered_map<uint32_t, Thumb> thumbs;
    uint64_t frame_no = 0;

    std::deque<LogLine> log;
    int language = 1;  // second column language (ja by default)
    bool tab_open[TabCount] = {true, true, true, true, true, true, false};
    int focus_tab = -1, focus_frames = 0;

    // statics browser
    char static_filter[64] = "";
    int sort_mode = 0;  // 0 id, 1 name, 2 count
    int current_pkg = -1;
    std::vector<int> pkg_order;
    int pkg_order_sort = -1;
    float right_w = 0;
    float details_h = 0;

    // text / dialogue / packages
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
            worker.start(dir);
            init_game_lighting();
            return true;
        } catch (const std::exception& ex) {
            error(std::format("インストールを開けません: {}", ex.what()));
            return false;
        }
    }

    void init_game_lighting() {
        gpu.wait_idle();
        try {
            game_lighting.release(gpu);
            game_lighting.init(*pm, gpu);
            game_ready = true;
            info(std::format("ゲームのライティング: global_lighting_and_shading ({})",
                             game_lighting.shading_technique().str()));
            for (const auto& n : game_lighting.notes()) info("  " + n);
        } catch (const std::exception& ex) {
            game_ready = false;
            engine = Engine::Nim;
            error(std::format("ゲームのライティングを使えません (nim のライティングで表示): {}", ex.what()));
        }
        try {
            thumb_lighting.release(gpu);
            thumb_lighting.init(*pm, gpu);
            thumb_game_ready = true;
        } catch (...) {
            thumb_game_ready = false;
        }
    }

    void request_load(TagHash tag) {
        if (loading && *loading == tag) return;
        loading = tag;
        worker.request_live(tag);
    }

    // Runs at the top of a frame, before any UI references GPU resources.
    void apply_pending(Gpu& g) {
        if (view_w && view_h && (view_w != gb.width() || view_h != gb.height())) {
            g.wait_idle();
            gb.resize(g, view_w, view_h);
        }
        if (auto r = worker.take(false)) {
            if (loading && *loading == r->tag) loading.reset();
            g.wait_idle();
            if (r->mesh) {
                try {
                    auto t0 = std::chrono::steady_clock::now();
                    scene.upload(g, std::move(*r->mesh), layouts);
                    frame_scene();
                    double up = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    size_t stubbed = 0;
                    for (const auto& gi : scene.groups()) stubbed += gi.stubbed_ops.size();
                    info(std::format("{} を読込 ({} グループ, 準備 {:.0f} ms + GPU {:.0f} ms){}", r->tag.str(),
                                     scene.groups().size(), r->ms, up,
                                     stubbed ? std::format(", 未実装 TFX op {} 種", stubbed) : ""));
                } catch (const std::exception& ex) {
                    scene.release(g);
                    error(std::format("{} を描画できません: {}", r->tag.str(), ex.what()));
                }
            } else {
                error(std::format("{} を読めません: {}", r->tag.str(), r->error));
            }
        }
        make_thumbnail(g);
    }

    void make_thumbnail(Gpu& g) {
        auto r = worker.take(true);
        if (!r) return;
        Thumb& t = thumbs[r->tag.value];
        t.last_used = frame_no;
        if (!r->mesh) {
            t.failed = true;
            t.error = r->error;
            return;
        }
        try {
            g.wait_idle();
            thumb_scene.upload(g, std::move(*r->mesh), layouts);
            thumb_gb.resize(g, kThumbSize, kThumbSize);
            ViewMatrices vm = make_view(frame_sphere(thumb_scene.center(), thumb_scene.radius(), kThumbSize, kThumbSize));
            auto* cl = g.immediate();
            thumb_scene.render_gbuffer(g, cl, thumb_gb, vm);
            LightingParams lp = light;
            lp.mode = ViewMode::Lit;
            if (thumb_game_ready) thumb_lighting.render(g, cl, thumb_gb, vm, lp);
            else lighting.render(g, cl, thumb_gb, vm, lp);
            if (!t.tex) {
                t.tex = g.texture2d(kThumbSize, kThumbSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
                t.srv = g.srv().alloc(1);
                g.device()->CreateShaderResourceView(t.tex.Get(), nullptr, g.srv().cpu(t.srv));
            } else {
                Gpu::barrier(cl, t.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            }
            Gpu::barrier(cl, thumb_gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyResource(t.tex.Get(), thumb_gb.lit.Get());
            Gpu::barrier(cl, thumb_gb.lit.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Gpu::barrier(cl, t.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            g.flush();
            thumb_scene.release(g);
        } catch (const std::exception& ex) {
            t.failed = true;
            t.error = ex.what();
            thumb_scene.release(g);
        }
        evict_thumbnails(g);
    }

    void evict_thumbnails(Gpu& g) {
        constexpr size_t kMax = 600;
        if (thumbs.size() <= kMax) return;
        std::vector<std::pair<uint64_t, uint32_t>> order;
        for (auto& [k, t] : thumbs) order.push_back({t.last_used, k});
        std::sort(order.begin(), order.end());
        g.wait_idle();
        for (size_t i = 0; i < order.size() - kMax; ++i) {
            auto it = thumbs.find(order[i].second);
            if (it->second.srv != UINT32_MAX) g.srv().free(it->second.srv, 1);
            thumbs.erase(it);
        }
    }

    View current_view() const {
        View v;
        v.width = std::max(1u, gb.width());
        v.height = std::max(1u, gb.height());
        v.target = cam.target;
        v.fov_y = 40.0f * 3.14159265f / 180.0f;
        V3 dir{std::cos(cam.yaw) * std::cos(cam.pitch), std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch)};
        v.eye = cam.target + dir * cam.distance;
        v.near_z = std::max(0.001f, cam.radius * 0.005f);
        return v;
    }

    void record(ID3D12GraphicsCommandList* cl) {
        if (!gb.lit) return;
        ViewMatrices vm = make_view(current_view());
        scene.render_gbuffer(gpu, cl, gb, vm);
        if (engine == Engine::Game && game_ready && light.mode == ViewMode::Lit)
            game_lighting.render(gpu, cl, gb, vm, light);
        else
            lighting.render(gpu, cl, gb, vm, light);
    }

    void after_frame() {
        ++frame_no;
        if (!capture_requested) return;
        capture_requested = false;
        try {
            gpu.wait_idle();
            auto px = gpu.read_texture(gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, 4);
            fs::path dir = exe_dir() / "captures";
            fs::create_directories(dir);
            auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const char* kind = light.mode != ViewMode::Lit ? "debug" : engine == Engine::Game ? "game" : "nim";
            fs::path file = dir / std::format("{}_{}_{}.png", scene.loaded() ? scene.tag().str() : "empty", kind, stamp);
            write_png(file, gb.width(), gb.height(), px);
            info("保存しました: " + to_utf8(file.wstring()));
        } catch (const std::exception& ex) {
            error(std::string("保存に失敗: ") + ex.what());
        }
    }

    // ---- UI ----
    std::string tab_title(int t) const {
        const auto& ti = kTabs[t];
        return std::format("{}{}{}###{}", icon(ti.icon), *ti.title ? "  " : "", ti.title, ti.id);
    }
    void open_tab(int t) {
        tab_open[t] = true;
        focus_tab = t;
    }

    void ui() {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        status_bar(vp);
        const ImGuiID dock = ImGui::GetID("nim_dock");
        if (!ImGui::DockBuilderGetNode(dock) || ImGui::DockBuilderGetNode(dock)->IsLeafNode() &&
                                                    ImGui::DockBuilderGetNode(dock)->Windows.Size == 0) {
            ImGui::DockBuilderRemoveNode(dock);
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, vp->WorkSize);
            for (int t = 0; t < TabCount; ++t) ImGui::DockBuilderDockWindow(tab_title(t).c_str(), dock);
            ImGui::DockBuilderFinish(dock);
        }
        ImGui::DockSpaceOverViewport(dock, vp, ImGuiDockNodeFlags_NoWindowMenuButton | ImGuiDockNodeFlags_NoCloseButton);
        viewport_visible = false;
        for (int t = 0; t < TabCount; ++t) {
            if (!tab_open[t]) continue;
            if (focus_tab == t) {
                ImGui::SetNextWindowFocus();
                if (++focus_frames > 2) focus_tab = -1, focus_frames = 0;  // docking settles over a few frames
            }
            ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
            const bool pad0 = t == TabStatics;
            if (pad0) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            bool visible = ImGui::Begin(tab_title(t).c_str(), kTabs[t].closable ? &tab_open[t] : nullptr,
                                        ImGuiWindowFlags_NoCollapse);
            if (pad0) ImGui::PopStyleVar();
            if (visible) {
                switch (t) {
                case TabHome: home_tab(); break;
                case TabSettings: settings_tab(); break;
                case TabStatics: statics_tab(); break;
                case TabText: strings_tab(); break;
                case TabDialogue: dialogue_tab(); break;
                case TabPackages: packages_tab(); break;
                case TabLog: log_tab(); break;
                }
            }
            ImGui::End();
        }
        if (!pm) install_picker();
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_S) && gb.lit && scene.loaded())
            capture_requested = true;
    }

    void status_bar(ImGuiViewport* vp) {
        const float h = ImGui::GetFrameHeight();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{4 / 255.0f, 4 / 255.0f, 4 / 255.0f, 1});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 4});
        if (ImGui::BeginViewportSideBar("##status", vp, ImGuiDir_Down, h,
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoDecoration)) {
            std::string left;
            if (catalog) {
                std::lock_guard lk(catalog->mutex);
                left = catalog->status;
            }
            if (left.empty() && !log.empty()) left = log.back().text;
            ImGui::AlignTextToFramePadding();
            if (!log.empty() && log.back().error && left == log.back().text)
                ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", left.c_str());
            else
                ImGui::TextDisabled("%s", left.c_str());
            if (ImGui::IsItemClicked()) open_tab(TabLog);
            std::string right = std::format("nim {}  ·  {}  ·  {}", kVersion, gpu.adapter_name(),
                                            g_fonts.from_game ? "game fonts" : "system fonts");
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(right.c_str()).x - 14);
            ImGui::TextDisabled("%s", right.c_str());
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void install_picker() {
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::Begin("Marathon が見つかりません", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
        ImGui::TextUnformatted("Steam のインストールを自動で見つけられませんでした。");
        ImGui::TextUnformatted("Marathon のフォルダ (Marathon.exe と packages があるところ) を選んでください。");
        if (dbutton("SELECT FOLDER", 240))
            if (auto p = pick_folder(g_win.hwnd)) open_install(*p);
        ImGui::End();
    }

    // -- HOME --
    void home_tab() {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + 115 * sc, ImGui::GetCursorPosY() + 64 * sc});
        ImGui::BeginGroup();
        const float col_w = std::min(520 * sc, (ImGui::GetContentRegionAvail().x - 230 * sc) * 0.5f);
        const float bw = std::max(240 * sc, col_w - 40 * sc);
        if (ImGui::BeginTable("home", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("3d", ImGuiTableColumnFlags_WidthFixed, col_w);
            ImGui::TableSetupColumn("2d", ImGuiTableColumnFlags_WidthFixed, col_w);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            heading("3D");
            ImGui::Dummy({0, 4 * sc});
            if (dbutton((icon(ico::Statics) + "  STATICS").c_str(), bw)) open_tab(TabStatics);
            ImGui::SetItemTooltip("静的メッシュをライブ 3D サムネイルで一覧し、ゲームのシェーダで表示");
            ImGui::TableNextColumn();
            heading("2D");
            ImGui::Dummy({0, 4 * sc});
            if (dbutton((icon(ico::Text) + "  TEXT").c_str(), bw)) open_tab(TabText);
            ImGui::SetItemTooltip("ローカライズ文字列 (全言語)");
            if (dbutton((icon(ico::Dialogue) + "  DIALOGUE").c_str(), bw)) open_tab(TabDialogue);
            ImGui::SetItemTooltip("話者・台詞・音声イベント");
            if (dbutton((icon(ico::Packages) + "  PACKAGES").c_str(), bw)) open_tab(TabPackages);
            ImGui::SetItemTooltip("パッケージとエントリの生データ");
            ImGui::EndTable();
        }
        ImGui::EndGroup();

        // footer
        const float fh = ImGui::GetFrameHeightWithSpacing() * 2;
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + 115 * sc, ImGui::GetWindowHeight() - fh - 24 * sc});
        ImGui::TextDisabled("nim %s  —  Marathon asset viewer", kVersion);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 115 * sc);
        ImGui::TextDisabled("%s", pm ? to_utf8(install.wstring()).c_str() : "インストール未選択");
        ImGui::SameLine();
        if (ImGui::SmallButton((icon(ico::Link) + " GitHub").c_str()))
            ShellExecuteW(nullptr, L"open", L"https://github.com/bata-san/nim", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // -- settings --
    void settings_tab() {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetCursorPos({ImGui::GetCursorPosX() + 115 * sc, ImGui::GetCursorPosY() + 48 * sc});
        ImGui::BeginGroup();
        heading("SETTINGS");
        ImGui::PushItemWidth(320 * sc);
        ImGui::SeparatorText("インストール");
        ImGui::TextUnformatted(pm ? to_utf8(install.wstring()).c_str() : "(未選択)");
        if (ImGui::Button("フォルダを選択…"))
            if (auto p = pick_folder(g_win.hwnd)) open_install(*p);
        ImGui::SeparatorText("表示");
        int e = int(engine);
        ImGui::TextUnformatted("ライティング");
        ImGui::RadioButton("ゲームのシェーダ (global_lighting_and_shading)", &e, int(Engine::Game));
        ImGui::RadioButton("nim (GGX)", &e, int(Engine::Nim));
        if (e == int(Engine::Game) && !game_ready) e = int(Engine::Nim);
        engine = Engine(e);
        ImGui::Checkbox("3D サムネイルを生成", &thumbnails_enabled);
        ImGui::Checkbox("FPS / VRAM を表示", &show_fps);
        if (ImGui::BeginCombo("第 2 言語", std::string(language_code(TextLanguage(language))).c_str())) {
            for (int l = 1; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(std::string(language_code(TextLanguage(l))).c_str(), language == l)) language = l;
            ImGui::EndCombo();
        }
        ImGui::SeparatorText("情報");
        ImGui::Text("nim %s", kVersion);
        ImGui::Text("GPU: %s", gpu.adapter_name().c_str());
        ImGui::Text("VRAM: %s / %s", format_bytes(gpu.vram_usage()).c_str(), format_bytes(gpu.vram_budget()).c_str());
        ImGui::Text("フォント: %s", g_fonts.from_game ? "ゲームのフォント (インストールから読込)" : "システムフォント");
        ImGui::Text("サムネイル: %zu 枚", thumbs.size());
        ImGui::PopItemWidth();
        ImGui::EndGroup();
    }

    // -- STATICS --
    void sort_packages() {
        const auto& pk = catalog->packages;
        pkg_order.resize(pk.size());
        for (size_t i = 0; i < pk.size(); ++i) pkg_order[i] = int(i);
        std::sort(pkg_order.begin(), pkg_order.end(), [&](int a, int b) {
            if (sort_mode == 1) return pk[a].name < pk[b].name;
            if (sort_mode == 2) return pk[a].meshes.size() > pk[b].meshes.size();
            return pk[a].pkg_id < pk[b].pkg_id;
        });
        pkg_order_sort = sort_mode;
    }

    void statics_tab() {
        if (!catalog || !catalog->meshes_ready) {
            ImGui::SetCursorPos({20, 20});
            ImGui::TextDisabled("静的メッシュを列挙中…");
            return;
        }
        const auto& pk = catalog->packages;
        const auto& meshes = catalog->meshes;
        if (pkg_order_sort != sort_mode || pkg_order.size() != pk.size()) sort_packages();
        if (current_pkg < 0 && !pk.empty()) current_pkg = pkg_order[0];

        const float sc = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float left_w = 300 * sc;
        if (right_w <= 0) right_w = std::clamp(avail.x * 0.4f, 420 * sc, 960 * sc);
        right_w = std::clamp(right_w, 320 * sc, std::max(320 * sc, avail.x - left_w - 300 * sc));

        // filter: a full tag selects its package
        std::optional<TagHash> want;
        {
            std::string f = static_filter;
            if (f.size() == 8 && f.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
                want = TagHash(uint32_t(std::stoul(f, nullptr, 16)));
        }

        // left: packages
        ImGui::BeginChild("pkgs", {left_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos({10 * sc, 10 * sc});
        ImGui::SetNextItemWidth(left_w - 20 * sc);
        ImGui::InputTextWithHint("##sfilter", (icon(ico::Search) + "  タグ / パッケージ名").c_str(), static_filter,
                                 sizeof(static_filter));
        ImGui::SetCursorPosX(10 * sc);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("並び順");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(left_w - 20 * sc - ImGui::GetCursorPosX() + 10 * sc);
        const char* sorts[] = {"ID", "名前", "数"};
        ImGui::Combo("##sort", &sort_mode, sorts, 3);
        ImGui::BeginChild("pkglist", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        const std::string f = lower_ascii(static_filter);
        for (int idx : pkg_order) {
            const auto& p = pk[idx];
            if (want) {
                if (p.pkg_id != want->pkg_id()) continue;
            } else if (!f.empty() && !contains_ci(p.name, f) && !contains_ci(std::format("{:04x}", p.pkg_id), f)) {
                continue;
            }
            ImGui::PushID(idx);
            const bool sel = idx == current_pkg;
            ImVec2 pos = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##p", sel, 0, {0, ImGui::GetTextLineHeight()})) current_pkg = idx;
            auto* dl = ImGui::GetWindowDrawList();
            std::string a = std::format("{:04x} -", p.pkg_id);
            dl->AddText({pos.x + 6 * sc, pos.y}, IM_COL32(150, 150, 156, 255), a.c_str());
            float x = pos.x + 6 * sc + ImGui::CalcTextSize(a.c_str()).x + 6 * sc;
            dl->AddText({x, pos.y}, IM_COL32_WHITE, p.name.c_str());
            x += ImGui::CalcTextSize(p.name.c_str()).x + 6 * sc;
            dl->AddText({x, pos.y}, IM_COL32(150, 150, 156, 255), std::format("({})", p.meshes.size()).c_str());
            ImGui::PopID();
        }
        if (want) {
            for (size_t i = 0; i < pk.size(); ++i)
                if (pk[i].pkg_id == want->pkg_id() && current_pkg != int(i)) current_pkg = int(i);
        }
        ImGui::EndChild();
        ImGui::EndChild();

        // center: cards
        ImGui::SameLine(0, 0);
        const float center_w = avail.x - left_w - right_w - 6 * sc;
        ImGui::BeginChild("cards", {center_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (current_pkg >= 0 && current_pkg < int(pk.size())) {
            const auto& p = pk[current_pkg];
            const float card = 200 * sc, gap = 10 * sc, footer = 30 * sc;
            const int cols = std::max(1, int((center_w - gap) / (card + gap)));
            const int rows = int((p.meshes.size() + cols - 1) / cols);
            std::vector<TagHash> visible_missing;
            ImGui::SetCursorPos({gap, gap});
            ImGuiListClipper clip;
            clip.Begin(rows, card + footer + gap);
            while (clip.Step())
                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                    ImGui::SetCursorPosX(gap);
                    for (int c = 0; c < cols; ++c) {
                        const size_t i = size_t(r) * cols + c;
                        if (i >= p.meshes.size()) break;
                        const MeshEntry& m = meshes[p.meshes[i]];
                        if (want && m.tag != *want) {
                            if (c + 1 < cols) ImGui::SameLine(0, gap);
                            continue;
                        }
                        if (card_ui(m, card, footer)) request_load(m.tag);
                        auto it = thumbs.find(m.tag.value);
                        if (it == thumbs.end()) visible_missing.push_back(m.tag);
                        else it->second.last_used = frame_no;
                        if (c + 1 < cols) ImGui::SameLine(0, gap);
                    }
                    ImGui::Dummy({0, gap - ImGui::GetStyle().ItemSpacing.y});
                }
            if (thumbnails_enabled) worker.request_thumbnails(std::move(visible_missing));
            else worker.request_thumbnails({});
        }
        ImGui::EndChild();

        // splitter
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton("##split", {6 * sc, avail.y});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) right_w -= ImGui::GetIO().MouseDelta.x;
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemActive() ? kGreen : IM_COL32(4, 4, 4, 255));

        // right: live view + details
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("scene", {right_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (details_h <= 0) details_h = 220 * sc;
        const float view_h_px = std::max(120 * sc, ImGui::GetContentRegionAvail().y - details_h - 6 * sc);
        viewport_ui({ImGui::GetContentRegionAvail().x, view_h_px});
        ImGui::InvisibleButton("##hsplit", {ImGui::GetContentRegionAvail().x, 6 * sc});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) details_h = std::max(40 * sc, details_h - ImGui::GetIO().MouseDelta.y);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemActive() ? kGreen : IM_COL32(4, 4, 4, 255));
        ImGui::BeginChild("details", {0, 0}, ImGuiChildFlags_None);
        details_ui();
        ImGui::EndChild();
        ImGui::EndChild();
    }

    // One browser card: thumbnail (or spinner) and a black footer with the tag. Returns clicked.
    bool card_ui(const MeshEntry& m, float card, float footer) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::PushID(int(m.tag.value));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("card", {card, card + footer});
        const bool hovered = ImGui::IsItemHovered();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 q{p.x + card, p.y + card};
        dl->AddRectFilled(p, q, IM_COL32(12, 12, 14, 255));
        auto it = thumbs.find(m.tag.value);
        if (it != thumbs.end() && it->second.srv != UINT32_MAX) {
            dl->AddImage(ImTextureID(gpu.srv().gpu(it->second.srv).ptr), p, q);
        } else if (it != thumbs.end() && it->second.failed) {
            const char* t = "×";
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText({p.x + (card - ts.x) * 0.5f, p.y + (card - ts.y) * 0.5f}, IM_COL32(200, 80, 80, 255), t);
            if (hovered) ImGui::SetTooltip("%s", it->second.error.c_str());
        } else if (thumbnails_enabled) {
            spinner(dl, {p.x + card * 0.5f, p.y + card * 0.5f}, 14 * sc, IM_COL32(255, 255, 255, 90));
        }
        dl->AddRectFilled({p.x, q.y}, {q.x, q.y + footer}, IM_COL32_BLACK);
        std::string label = m.tag.str();
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText({p.x + 8 * sc, q.y + (footer - ts.y) * 0.5f}, IM_COL32_WHITE, label.c_str());
        const bool selected = scene.loaded() && scene.tag() == m.tag;
        const bool is_loading = loading && *loading == m.tag;
        if (selected || is_loading) dl->AddRect(p, {q.x, q.y + footer}, kGreen, 0, 0, 2 * sc);
        else if (hovered) dl->AddRect(p, {q.x, q.y + footer}, IM_COL32_WHITE, 0, 0, 1.5f * sc);
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        return clicked;
    }

    // The live 3D view with goriath's overlay toolbar and FPS counter.
    void viewport_ui(ImVec2 size) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        viewport_visible = true;
        view_w = uint32_t(std::max(16.0f, size.x));
        view_h = uint32_t(std::max(16.0f, size.y));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        if (gb.lit && gb.width() == view_w && gb.height() == view_h)
            ImGui::Image(ImTextureID(gpu.srv().gpu(gb.srv_first + 5).ptr), size);
        else
            ImGui::Dummy(size);
        const ImVec2 end = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(origin);
        ImGui::InvisibleButton("view", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                   ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        handle_camera(hovered, active);

        // toolbar overlay
        const float bar_h = 32 * sc;
        const bool bar_hover = ImGui::IsMouseHoveringRect(origin, {origin.x + size.x, origin.y + bar_h});
        dl->AddRectFilled(origin, {origin.x + size.x, origin.y + bar_h}, IM_COL32(0, 0, 0, bar_hover ? 160 : 64));
        ImGui::SetCursorScreenPos({origin.x + 6 * sc, origin.y + (bar_h - ImGui::GetFrameHeight()) * 0.5f});
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 120));
        if (ImGui::Button(icon(ico::Tune).c_str())) ImGui::OpenPopup("tune");
        ImGui::SetItemTooltip("ライト・露出");
        if (ImGui::BeginPopup("tune")) {
            ImGui::SliderFloat("露出", &light.exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
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
            if (ImGui::Button("既定値に戻す")) {
                ViewMode m = light.mode;
                light = LightingParams{};
                light.mode = m;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 4 * sc);
        ImGui::SetNextItemWidth(170 * sc);
        if (ImGui::BeginCombo("##mode", view_mode_name(light.mode))) {
            for (uint32_t m = 0; m < uint32_t(ViewMode::Count); ++m)
                if (ImGui::Selectable(view_mode_name(ViewMode(m)), light.mode == ViewMode(m))) light.mode = ViewMode(m);
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4 * sc);
        ImGui::SetNextItemWidth(220 * sc);
        const char* engines[] = {"GAME SHADING", "NIM GGX"};
        if (ImGui::BeginCombo("##engine", (icon(ico::Light) + "  " + engines[int(engine)]).c_str())) {
            if (ImGui::Selectable("GAME SHADING", engine == Engine::Game) && game_ready) engine = Engine::Game;
            ImGui::SetItemTooltip("ゲーム自身の global_lighting_and_shading で陰影付け");
            if (ImGui::Selectable("NIM GGX", engine == Engine::Nim)) engine = Engine::Nim;
            ImGui::SetItemTooltip("nim の簡易 GGX ライティング");
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4 * sc);
        if (ImGui::Button(icon(ico::Frame).c_str())) frame_scene();
        ImGui::SetItemTooltip("全体を表示 (F)");
        ImGui::SameLine(0, 4 * sc);
        if (ImGui::Button(icon(ico::Save).c_str()) && scene.loaded()) capture_requested = true;
        ImGui::SetItemTooltip("PNG で保存 (Ctrl+S)");
        ImGui::PopStyleColor(2);

        // FPS / VRAM, top right (goriath: green monospace with a shadow)
        if (show_fps) {
            std::string fps = std::format("{:.0f} FPS", ImGui::GetIO().Framerate);
            std::string vram = std::format("VRAM {}", format_bytes(gpu.vram_usage()));
            ImVec2 fs1 = ImGui::CalcTextSize(fps.c_str()), fs2 = ImGui::CalcTextSize(vram.c_str());
            hud_text(dl, {origin.x + size.x - fs1.x - 10 * sc, origin.y + bar_h + 6 * sc}, kGreen, fps.c_str());
            hud_text(dl, {origin.x + size.x - fs2.x - 10 * sc, origin.y + bar_h + 6 * sc + fs1.y}, IM_COL32(200, 200, 200, 255),
                     vram.c_str());
        }
        // bottom-left HUD / messages
        if (scene.loaded()) {
            std::string hud = std::format("{}   LOD {}   r {:.2f}   {} groups{}", scene.tag().str(), scene.lod(),
                                          scene.radius(), scene.groups().size(),
                                          light.mode == ViewMode::Lit
                                              ? (engine == Engine::Game ? "   GAME SHADING" : "   NIM GGX")
                                              : "");
            hud_text(dl, {origin.x + 10 * sc, origin.y + size.y - ImGui::GetTextLineHeight() - 8 * sc},
                     IM_COL32(235, 235, 235, 230), hud.c_str());
        } else if (!loading) {
            const char* msg = pm ? "カードをクリックすると、ゲームのシェーダで描画します" : "Marathon のインストールを開いてください";
            ImVec2 ts = ImGui::CalcTextSize(msg);
            hud_text(dl, {origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f}, IM_COL32(200, 200, 200, 200), msg);
        }
        if (loading) {
            spinner(dl, {origin.x + size.x * 0.5f, origin.y + size.y * 0.5f}, 20 * sc, kGreen);
            std::string t = std::format("{} を読込中…", loading->str());
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            hud_text(dl, {origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f + 32 * sc}, IM_COL32_WHITE, t.c_str());
        }
        ImGui::SetCursorScreenPos(end);
    }

    void frame_scene() {
        if (!scene.loaded()) return;
        cam.target = scene.center();
        cam.radius = scene.radius();
        cam.distance = scene.radius() / std::sin(20.0f * 3.14159265f / 180.0f) * 0.9f;
        cam.fly_speed = std::max(0.05f, scene.radius() * 0.8f);
    }

    void handle_camera(bool hovered, bool active) {
        ImGuiIO& io = ImGui::GetIO();
        const V3 dir{std::cos(cam.yaw) * std::cos(cam.pitch), std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch)};
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
            cam.yaw -= io.MouseDelta.x * 0.008f;
            cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.008f, -1.55f, 1.55f);
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
            ViewMatrices vm = make_view(current_view());
            float k = cam.distance * 0.0015f;
            cam.target = cam.target - vm.right * (io.MouseDelta.x * k) + vm.up * (io.MouseDelta.y * k);
        }
        if (active && ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            // fly: rotate around the eye, move with WASD / QE
            const V3 eye = cam.target + dir * cam.distance;
            cam.yaw -= io.MouseDelta.x * 0.006f;
            cam.pitch = std::clamp(cam.pitch + io.MouseDelta.y * 0.006f, -1.55f, 1.55f);
            const V3 nd{std::cos(cam.yaw) * std::cos(cam.pitch), std::sin(cam.yaw) * std::cos(cam.pitch), std::sin(cam.pitch)};
            cam.target = eye - nd * cam.distance;
            ViewMatrices vm = make_view(current_view());
            float speed = cam.fly_speed * io.DeltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) speed *= 5;
            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)) speed /= 5;
            V3 move{};
            if (ImGui::IsKeyDown(ImGuiKey_W)) move = move + vm.fwd;
            if (ImGui::IsKeyDown(ImGuiKey_S)) move = move - vm.fwd;
            if (ImGui::IsKeyDown(ImGuiKey_D)) move = move + vm.right;
            if (ImGui::IsKeyDown(ImGuiKey_A)) move = move - vm.right;
            if (ImGui::IsKeyDown(ImGuiKey_E)) move = move + V3{0, 0, 1};
            if (ImGui::IsKeyDown(ImGuiKey_Q)) move = move - V3{0, 0, 1};
            cam.target = cam.target + move * speed;
            if (io.MouseWheel != 0) cam.fly_speed *= std::pow(1.25f, io.MouseWheel);
        } else if (hovered && io.MouseWheel != 0) {
            cam.distance *= std::pow(0.88f, io.MouseWheel);
        }
        cam.distance = std::clamp(cam.distance, cam.radius * 0.02f, cam.radius * 400.0f);
        if (hovered && ImGui::IsKeyPressed(ImGuiKey_F)) frame_scene();
    }

    void details_ui() {
        if (!scene.loaded()) {
            ImGui::TextDisabled("メッシュ未選択");
            return;
        }
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::Text("STATIC %s", scene.tag().str().c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("コピー")) ImGui::SetClipboardText(scene.tag().str().c_str());
        ImGui::TextDisabled("LOD %u   グループ %zu   半径 %.3f", scene.lod(), scene.groups().size(), scene.radius());
        if (engine == Engine::Game && game_ready)
            ImGui::TextDisabled("シェーディング: %s (ゲームの global_lighting_and_shading)",
                                game_lighting.shading_technique().str().c_str());
        for (size_t i = 0; i < scene.groups().size(); ++i) {
            const auto& g = scene.groups()[i];
            ImGui::PushID(int(i));
            if (ImGui::CollapsingHeader(std::format("パート {}  ({} インデックス)", g.part, g.index_count).c_str())) {
                ImGui::Text("テクニック  %s", g.technique.str().c_str());
                ImGui::Text("VS %s   PS %s", g.vs.str().c_str(), g.ps.str().c_str());
                ImGui::Text("cbuffer %zu レジスタ   TFX %zu 命令", g.cbuffer_regs, g.vm_instructions);
                for (const auto& s : g.stubbed_ops) ImGui::TextColored({1, 0.75f, 0.3f, 1}, "未実装 TFX op: %s", s.c_str());
                if (ImGui::BeginTable("tex", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 40);
                    ImGui::TableSetupColumn("テクスチャ", ImGuiTableColumnFlags_WidthFixed, 90);
                    ImGui::TableSetupColumn("形式");
                    for (const auto& t : g.textures) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("t%u", t.slot);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(t.tag.str().c_str());
                        ImGui::TableNextColumn();
                        if (t.error.empty()) ImGui::Text("%u  %ux%u  mip %u", t.format, t.width, t.height, t.mips);
                        else ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", t.error.c_str());
                    }
                    for (const auto& e : g.extern_textures) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Text("t%u", e.slot);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("extern %u", e.extern_id);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("エンジン側 (+0x%X)", e.offset * 8);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::PopID();
        }
    }

    // -- TEXT --
    void strings_tab() {
        if (!catalog || !catalog->strings_ready) { ImGui::TextDisabled("読込中…"); return; }
        const auto& all = catalog->strings.strings();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##strfilter", (icon(ico::Search) + "  検索 (英語・選択中の言語・ハッシュ)").c_str(),
                                 string_filter, sizeof(string_filter));
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
            ImGui::TableSetupColumn("HASH", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("EN");
            ImGui::TableSetupColumn(lower_ascii(lang_col).c_str());
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

    // -- DIALOGUE --
    void dialogue_tab() {
        if (!catalog || !catalog->dialogue_ready) { ImGui::TextDisabled("読込中…"); return; }
        const auto& lines = catalog->dialogue;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##dlgfilter", (icon(ico::Search) + "  話者・台詞で検索").c_str(), dialogue_filter,
                                 sizeof(dialogue_filter));
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
            ImGui::TableSetupColumn("SPEAKER", ImGuiTableColumnFlags_WidthFixed, 130);
            ImGui::TableSetupColumn("LINE");
            ImGui::TableSetupColumn("AUDIO EVENT", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("WEM", ImGuiTableColumnFlags_WidthFixed, 44);
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
                    ImGui::PushFont(g_fonts.bold, 0);
                    ImGui::TextUnformatted((sp.empty() ? text(l.speaker_hash, 0) : sp).c_str());
                    ImGui::PopFont();
                    ImGui::TableNextColumn();
                    const std::string& en = text(l.subtitle_hash, 0);
                    const std::string& tr = text(l.subtitle_hash, size_t(language));
                    ImGui::TextWrapped("%s", en.c_str());
                    if (!tr.empty() && tr != en) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kGreen));
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

    // -- PACKAGES --
    void packages_tab() {
        if (!pm) return;
        const auto& pkgs = pm->packages();
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::BeginChild("pkglist", {360 * sc, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
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
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##entfilter", "種別 (例 8, 32/1) またはクラス", entry_filter, sizeof(entry_filter));
        std::string f = lower_ascii(entry_filter);
        ImGui::BeginChild("entries", {0, ImGui::GetContentRegionAvail().y * 0.55f}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        if (ImGui::BeginTable("entrytable", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("TAG", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("CLASS", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("SIZE");
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
                        if (ImGui::IsMouseDoubleClicked(0) && r.type == 8 && r.reference == kClassStaticMesh) {
                            request_load(r.tag);
                            open_tab(TabStatics);
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", r.type, r.subtype);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", r.reference);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u%s", r.size, r.reference == kClassStaticMesh && r.type == 8 ? "  (static mesh)" : "");
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
        ImGui::EndGroup();
    }

    void log_tab() {
        for (const auto& l : log) {
            if (l.error) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", l.text.c_str());
            else ImGui::TextUnformatted(l.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }

    void shutdown() {
        worker.stop();
        gpu.wait_idle();
        for (auto& [k, t] : thumbs)
            if (t.srv != UINT32_MAX) gpu.srv().free(t.srv, 1);
        thumbs.clear();
        game_lighting.release(gpu);
        thumb_lighting.release(gpu);
        scene.release(gpu);
        thumb_scene.release(gpu);
        gb.release(gpu);
        thumb_gb.release(gpu);
    }
};

int run(const std::wstring& cmdline) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"nim", nullptr};
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_win.hwnd = CreateWindowW(wc.lpszClassName, L"nim — Marathon asset viewer", WS_OVERLAPPEDWINDOW, 80, 60,
                               int(1680 * scale), int(1000 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    {  // dark title bar
        BOOL dark = TRUE;
        using DwmSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
            if (auto f = reinterpret_cast<DwmSet>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) f(g_win.hwnd, 20, &dark, sizeof(dark));
    }

    auto app = std::make_unique<App>();
    try {
        app->gpu.init(false);
        init_swapchain(app->gpu);
        app->lighting.init(app->gpu);
    } catch (const std::exception& ex) {
        MessageBoxW(g_win.hwnd, to_wide(std::string("D3D12 の初期化に失敗しました:\n") + ex.what()).c_str(), L"nim", MB_ICONERROR);
        return 1;
    }

    // Command line: [INSTALL_DIR] [STATIC_MESH_TAG] in any order.
    std::optional<fs::path> install_arg;
    std::optional<TagHash> tag_arg;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            if (fs::is_directory(a)) install_arg = fs::path(a);
            else if (a.size() == 8 && a.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos)
                tag_arg = TagHash(uint32_t(std::stoul(a, nullptr, 16)));
        }
        LocalFree(argv);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static std::string ini = (exe_dir() / "nim_layout.ini").string();
    io.IniFilename = ini.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = false;
    apply_style(scale);

    // Open the install before building fonts: the UI is set in the game's own fonts.
    ShowWindow(g_win.hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_win.hwnd);
    if (install_arg) app->open_install(*install_arg);
    else if (auto found = PackageManager::find_install()) app->open_install(*found);
    else app->error("Marathon の Steam インストールが見つかりません。SETTINGS からフォルダを選んでください。");
    load_fonts(app->pm.get());

    ImGui_ImplWin32_Init(g_win.hwnd);
    static Gpu* s_gpu = &app->gpu;
    ImGui_ImplDX12_InitInfo ii;
    ii.Device = app->gpu.device();
    ii.CommandQueue = app->gpu.queue();
    ii.NumFramesInFlight = Gpu::kFramesInFlight;
    ii.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    ii.DSVFormat = DXGI_FORMAT_UNKNOWN;
    ii.SrvDescriptorHeap = app->gpu.srv().heap();
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

    app->info(std::format("nim {} — GPU: {}", kVersion, app->gpu.adapter_name()));
    if (tag_arg && app->pm) {
        app->request_load(*tag_arg);
        app->focus_tab = TabStatics;
    }

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
            resize_swapchain(app->gpu, g_win.pending_w, g_win.pending_h);
            g_win.pending_w = g_win.pending_h = 0;
        }

        const uint32_t fi = uint32_t(frame % Gpu::kFramesInFlight);
        WaitForSingleObjectEx(g_win.waitable, 1000, TRUE);
        app->gpu.wait(g_win.fence[fi]);
        app->gpu.begin_frame(fi);
        app->apply_pending(app->gpu);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app->ui();
        ImGui::Render();

        auto* cl = g_win.list.Get();
        check_hr(g_win.alloc[fi]->Reset(), "allocator Reset");
        check_hr(cl->Reset(g_win.alloc[fi].Get(), nullptr), "list Reset");
        try {
            if (app->viewport_visible) app->record(cl);
        } catch (const std::exception& ex) {
            app->error(std::string("描画エラー: ") + ex.what());
        }
        const UINT bb = g_win.swapchain->GetCurrentBackBufferIndex();
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0.016f, 0.016f, 0.016f, 1};
        cl->ClearRenderTargetView(g_win.rtv[bb], clear, 0, nullptr);
        cl->OMSetRenderTargets(1, &g_win.rtv[bb], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {app->gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        check_hr(cl->Close(), "Close");
        ID3D12CommandList* lists[] = {cl};
        app->gpu.queue()->ExecuteCommandLists(1, lists);
        g_win.swapchain->Present(1, 0);
        g_win.fence[fi] = app->gpu.signal();
        ++frame;
        app->after_frame();
    }

    app->gpu.wait_idle();
    if (app->loader.joinable()) app->loader.join();
    app->shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
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
