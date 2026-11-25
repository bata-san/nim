// nim — Marathon asset viewer (GUI).
//
// Dear ImGui (docking) over D3D12, laid out after goriath: a row of document tabs, a HOME page of
// big buttons, and browsers whose right-hand panel is a live 3D view. Meshes are drawn with the
// game's own G-buffer shaders and, by default, shaded by the game's own deferred lighting pass;
// the UI itself is set in the game's fonts, read from the user's install at startup.
#include "media.hpp"
#include "frame_generation.hpp"
#include "frame_profile.hpp"
#include "nim/core/profile.hpp"
#include "ui_text.hpp"
#include "nim/content/animation.hpp"
#include "nim/content/audio.hpp"
#include "nim/content/map_audio.hpp"
#include "nim/content/movies.hpp"
#include "nim/content/dialogue.hpp"
#include "nim/core/parallel.hpp"
#include "nim/content/codex.hpp"
#include "nim/content/fonts.hpp"
#include "nim/content/map.hpp"
#include "nim/content/strings.hpp"
#include "nim/content/weather.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/object_channels.hpp"
#include "nim/render/scene.hpp"
#include "nim/render/particles.hpp"
#include "nim/render/ai_actors.hpp"
#include "nim/render/event_playback.hpp"
#include "nim/sim/ai.hpp"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace nim;
namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = NIM_VERSION;
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
std::string upper_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return s;
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
// Display names of the string-table language slots (TextLanguage order). Content, shown in the
// selectors; the names themselves follow the interface language.
const char* text_language_name(size_t l) { return ui::content_language_name(l); }
// Localized audio packages end in a language suffix (w64_sr_audio_015a_jpn). The index is the
// TextLanguage slot the suffix stands for (-1 when unknown).
int audio_language_slot(const std::string& suffix) {
    static const std::pair<const char*, int> k[] = {{"en", 0},   {"jpn", 1},  {"ja", 1},   {"de", 2},  {"fr", 3},
                                                    {"es", 4},   {"esmx", 5}, {"mx", 5},   {"it", 6},  {"ko", 7},
                                                    {"kor", 7},  {"cht", 8},  {"zht", 8},  {"chs", 9}, {"zhs", 9},
                                                    {"ptbr", 10}, {"pt", 10}, {"pl", 11},  {"ru", 12}};
    for (const auto& [s, l] : k)
        if (suffix == s) return l;
    return -1;
}
std::string audio_language_name(const std::string& suffix) {
    const int slot = audio_language_slot(suffix);
    return slot < 0 ? suffix : text_language_name(size_t(slot));
}
// The longest prefix of `s` that is at most `bytes` long and ends on a UTF-8 character boundary.
std::string utf8_prefix(const std::string& s, size_t bytes) {
    if (s.size() <= bytes) return s;
    size_t n = bytes;
    while (n > 0 && (uint8_t(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}
std::string safe_file_name(std::string s) {
    static constexpr char kBadChars[] = {'<', '>', ':', '"', '/', '\\', '|', '?', '*', 0};
    for (auto& c : s)
        if (uint8_t(c) < 0x20 || std::strchr(kBadChars, c)) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    s = utf8_prefix(s, 120);
    return s.empty() ? std::string("_") : s;
}
bool write_wav_file(const fs::path& path, const PcmAudio& pcm) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t data = uint32_t(pcm.samples.size() * 2);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4);
    u32(36 + data);
    f.write("WAVEfmt ", 8);
    u32(16), u16(1), u16(pcm.channels), u32(pcm.sample_rate), u32(pcm.sample_rate * pcm.channels * 2u);
    u16(uint16_t(pcm.channels * 2)), u16(16);
    f.write("data", 4);
    u32(data);
    f.write(reinterpret_cast<const char*>(pcm.samples.data()), std::streamsize(data));
    return bool(f);
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
                   Search = 0xE721, Entities = 0xE99A, Fonts = 0xE8E9, Link = 0xE71B, Eye = 0xE890, Maps = 0xE774,
                   Movies = 0xE714, Play = 0xE768, Pause = 0xE769, Stop = 0xE71A, Volume = 0xE767, Folder = 0xE8B7, Codex = 0xE8F1, People = 0xE716;
}
std::string format_bytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(b);
    int u = 0;
    while (v >= 1024 && u < 4) v /= 1024, ++u;
    return std::format("{:.1f} {}", v, units[u]);
}

// Preferences kept in nim_settings.ini next to the exe: "language=ja" and "sidebar=1". Missing or
// unknown values fall back to Japanese and an open sidebar.
struct Prefs {
    ui::Lang lang = ui::Lang::Ja;
    bool sidebar_open = true;
    bool auto_update = false;
};
Prefs g_prefs;

void load_prefs() {
    std::ifstream f(exe_dir() / "nim_settings.ini");
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("language=", 0) == 0) ui::parse_lang(line.substr(9), g_prefs.lang);
        else if (line.rfind("sidebar=", 0) == 0) g_prefs.sidebar_open = line.substr(8) != "0";
        else if (line == "auto_update=1") g_prefs.auto_update = true;
    }
}
void save_prefs() {
    std::ofstream f(exe_dir() / "nim_settings.ini", std::ios::trunc);
    if (f) f << "language=" << ui::code(g_prefs.lang) << "\nsidebar=" << (g_prefs.sidebar_open ? 1 : 0)
             << "\nauto_update=" << (g_prefs.auto_update ? 1 : 0) << "\n";
}

HANDLE g_update_process = nullptr;

bool start_update(bool automatic) {
    if (g_update_process) {
        if (WaitForSingleObject(g_update_process, 0) == WAIT_TIMEOUT) return true;
        CloseHandle(g_update_process);
        g_update_process = nullptr;
    }
    const auto dir = exe_dir();
    const auto script = dir / "nim-update.ps1";
    if (!fs::is_regular_file(script)) return false;
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    const auto shell = fs::path(system) / L"WindowsPowerShell/v1.0/powershell.exe";
    std::wstring command = std::format(L"\"{}\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"{}\" -Version {} -ViewerPid {}{}",
                                       shell.wstring(), script.wstring(), to_wide(kVersion), GetCurrentProcessId(),
                                       automatic ? L" -Automatic" : L"");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(shell.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, dir.c_str(), &startup, &process)) return false;
    CloseHandle(process.hThread);
    g_update_process = process.hProcess;
    return true;
}

std::optional<fs::path> pick_folder(HWND owner) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(to_wide(ui::tr(ui::S::PickInstallFolder)).c_str());
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
    // The PNG encoder may answer with the closest format it supports (32bppBGRA): swizzle to it.
    std::vector<uint8_t> px = rgba;
    if (fmt != GUID_WICPixelFormat32bppRGBA)
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    check_hr(frame->WritePixels(h, w * 4, UINT(px.size()), px.data()), "WritePixels");
    check_hr(frame->Commit(), "frame Commit");
    check_hr(enc->Commit(), "encoder Commit");
}

// ---- fonts ----------------------------------------------------------------------------------------
struct Fonts {
    ImFont* body = nullptr;     // Noto Sans JP + KR/SC/TC + icons
    ImFont* bold = nullptr;
    ImFont* heading = nullptr;  // titles (the bold face)
    ImFont* button = nullptr;   // labels and buttons (medium)
    ImFont* mono = nullptr;     // PP Fraktion Mono Regular: terminal body, tags and numbers
    ImFont* mono_med = nullptr; // PP Fraktion Mono Medium: terminal headers and rules
    ImFont* wide = nullptr;     // MarathonShapiro Wide 65: CyberAcme ticker bars
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
    auto add = [&](std::vector<uint8_t> data, float size, ImFont* merge_into, float extra = 1.0f, float dy = 0) -> ImFont* {
        if (data.empty()) return nullptr;
        g_font_data.push_back(std::move(data));
        auto& d = g_font_data.back();
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.MergeMode = merge_into != nullptr;
        cfg.DstFont = merge_into;
        cfg.ExtraSizeScale = extra;
        cfg.GlyphOffset = {0, dy};
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
            // Hangul the Noto set lacks (merged: only missing glyphs are taken from Malgun Gothic).
            add(first_sys({"malgun.ttf"}), 0, f, 1.0f);
        } else {
            add(first_sys({"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}), 0, f);
            add(first_sys({"malgun.ttf"}), 0, f);
            add(first_sys({"msyh.ttc"}), 0, f);
        }
        add(icons(), 0, f);
        add(first_sys({"segoeui.ttf"}), 0, f);
    };

    // shadcn-like: one sans family (the game's Noto Sans JP, which also covers Latin) in three weights,
    // the game's mono face for tags and numbers.
    auto sans = [&](const char* jp, float size, std::initializer_list<const char*> sys_fallback) -> ImFont* {
        ImFont* f = add(game_font(jp), size, nullptr);
        const bool game_face = f != nullptr;
        if (!f) f = add(first_sys(sys_fallback), size, nullptr);
        if (!f) return nullptr;
        if (game_face) {
            add(game_font("NotoSansKR-Regular"), 0, f);
            add(game_font("NotoSansSC-Regular"), 0, f);
            add(game_font("NotoSansTC-Regular"), 0, f);
            add(first_sys({"malgun.ttf"}), 0, f);
        } else {
            add(first_sys({"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"}), 0, f);
            add(first_sys({"malgun.ttf"}), 0, f);
            add(first_sys({"msyh.ttc"}), 0, f);
        }
        add(icons(), 0, f, 1.0f, 3.0f);  // Segoe icons sit high next to Noto's tall line
        add(first_sys({"segoeui.ttf"}), 0, f);
        return f;
    };
    g_fonts.from_game = game.count("NotoSansJP-Regular") != 0;
    g_fonts.body = sans("NotoSansJP-Regular", 17, {"segoeui.ttf"});
    if (!g_fonts.body) g_fonts.body = io.Fonts->AddFontDefault();
    g_fonts.button = sans("NotoSansJP-Medium", 17, {"seguisb.ttf", "segoeui.ttf"});
    g_fonts.bold = sans("NotoSansJP-Bold", 17, {"segoeuib.ttf"});
    g_fonts.heading = g_fonts.bold;
    g_fonts.mono = add(game_font("PPFraktionMono-Regular"), 15, nullptr);
    if (!g_fonts.mono) g_fonts.mono = add(first_sys({"consola.ttf"}), 15, nullptr);
    cjk(g_fonts.mono, "NotoSansJP-Regular");
    g_fonts.mono_med = add(game_font("PPFraktionMono-Medium"), 15, nullptr);
    if (g_fonts.mono_med) cjk(g_fonts.mono_med, "NotoSansJP-Medium");
    g_fonts.wide = add(game_font("MarathonShapiro-Wide65"), 15, nullptr);
    if (!g_fonts.wide) g_fonts.wide = add(game_font("Shapiro-65LightHeavyWide"), 15, nullptr);
    if (!g_fonts.bold) g_fonts.bold = g_fonts.body;
    if (!g_fonts.heading) g_fonts.heading = g_fonts.bold;
    if (!g_fonts.button) g_fonts.button = g_fonts.body;
    if (!g_fonts.mono) g_fonts.mono = g_fonts.body;
    if (!g_fonts.mono_med) g_fonts.mono_med = g_fonts.mono;
    if (!g_fonts.wide) g_fonts.wide = g_fonts.bold;
    io.FontDefault = g_fonts.body;
}

// The interface theme, after shadcn/ui's dark zinc palette: near-black background, hairline borders,
// 6-8 px corners, white primary buttons, grey "accent" fills for hover and selection.
namespace theme {
constexpr ImU32 kBg = IM_COL32(9, 9, 11, 255);           // background
constexpr ImU32 kRail = IM_COL32(17, 17, 19, 255);       // sidebar, title bar and status line
constexpr ImU32 kSurface = IM_COL32(14, 14, 16, 255);    // cards
constexpr ImU32 kSurfaceHi = IM_COL32(39, 39, 42, 255);  // secondary / accent
constexpr ImU32 kHover = IM_COL32(32, 32, 35, 255);
constexpr ImU32 kActive = IM_COL32(39, 39, 42, 255);
constexpr ImU32 kBorder = IM_COL32(39, 39, 42, 255);
constexpr ImU32 kRing = IM_COL32(113, 113, 122, 255);
constexpr ImU32 kText = IM_COL32(250, 250, 250, 255);
constexpr ImU32 kMuted = IM_COL32(161, 161, 170, 255);
constexpr ImU32 kError = IM_COL32(248, 113, 113, 255);
constexpr ImU32 kAccentHi = IM_COL32(250, 250, 250, 255);
constexpr ImU32 kPrimaryText = IM_COL32(24, 24, 27, 255);
} // namespace theme

ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

void apply_style(float scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 8;
    s.FrameRounding = 6;
    s.PopupRounding = 8;
    s.TabRounding = 6;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 6;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 1;
    s.TabBorderSize = 0;
    s.WindowPadding = {14, 12};
    s.FramePadding = {10, 6};
    s.ItemSpacing = {8, 8};
    s.ItemInnerSpacing = {8, 6};
    s.CellPadding = {8, 5};
    s.IndentSpacing = 16;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 10;
    s.SelectableTextAlign = {0, 0.5f};
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = {0, 4};
    s.DockingSeparatorSize = 1;
    s.TabBarOverlineSize = 0;
    s.TabBarBorderSize = 1;
    ImVec4* c = s.Colors;
    auto rgb = [](int r, int g, int b, float a = 1) { return ImVec4{r / 255.0f, g / 255.0f, b / 255.0f, a}; };
    c[ImGuiCol_Text] = col4(theme::kText);
    c[ImGuiCol_TextDisabled] = col4(theme::kMuted);
    c[ImGuiCol_WindowBg] = col4(theme::kBg);
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = rgb(12, 12, 14, 0.99f);
    c[ImGuiCol_Border] = col4(theme::kBorder);
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = rgb(9, 9, 11);
    c[ImGuiCol_FrameBgHovered] = rgb(24, 24, 27);
    c[ImGuiCol_FrameBgActive] = rgb(32, 32, 35);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col4(theme::kRail);
    c[ImGuiCol_MenuBarBg] = col4(theme::kRail);
    c[ImGuiCol_Header] = col4(theme::kActive);
    c[ImGuiCol_HeaderHovered] = rgb(39, 39, 42, 0.6f);
    c[ImGuiCol_HeaderActive] = rgb(52, 52, 56);
    c[ImGuiCol_Button] = col4(theme::kSurfaceHi);
    c[ImGuiCol_ButtonHovered] = rgb(52, 52, 56);
    c[ImGuiCol_ButtonActive] = rgb(63, 63, 70);
    c[ImGuiCol_CheckMark] = col4(theme::kText);
    c[ImGuiCol_SliderGrab] = rgb(212, 212, 216);
    c[ImGuiCol_SliderGrabActive] = col4(theme::kText);
    c[ImGuiCol_Tab] = {0, 0, 0, 0};
    c[ImGuiCol_TabHovered] = rgb(39, 39, 42, 0.6f);
    c[ImGuiCol_TabSelected] = col4(theme::kActive);
    c[ImGuiCol_TabSelectedOverline] = {0, 0, 0, 0};
    c[ImGuiCol_TabDimmed] = {0, 0, 0, 0};
    c[ImGuiCol_TabDimmedSelected] = col4(theme::kActive);
    c[ImGuiCol_TabDimmedSelectedOverline] = {0, 0, 0, 0};
    c[ImGuiCol_DockingPreview] = rgb(250, 250, 250, 0.3f);
    c[ImGuiCol_DockingEmptyBg] = col4(theme::kBg);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = col4(theme::kBorder);
    c[ImGuiCol_SeparatorActive] = col4(theme::kRing);
    c[ImGuiCol_ResizeGrip] = {0, 0, 0, 0};
    c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = rgb(250, 250, 250, 0.2f);
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = rgb(39, 39, 42);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(63, 63, 70);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(82, 82, 91);
    c[ImGuiCol_TableHeaderBg] = rgb(17, 17, 19);
    c[ImGuiCol_TableBorderStrong] = col4(theme::kBorder);
    c[ImGuiCol_TableBorderLight] = rgb(28, 28, 31);
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.02f};
    c[ImGuiCol_TextSelectedBg] = rgb(250, 250, 250, 0.18f);
    c[ImGuiCol_NavCursor] = col4(theme::kRing);
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotLines] = rgb(212, 212, 216);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// Text drawn at `pos` and cut with "..." at `max_x` (list rows, labels in narrow columns).
void text_fit(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float max_x) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    if (pos.x + ts.x <= max_x) {
        dl->AddText(pos, col, text);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, pos, {max_x, pos.y + ts.y}, max_x, text, nullptr, &ts);
    ImGui::PopStyleColor();
}

// Combo boxes draw a thin chevron instead of ImGui's filled triangle in its own grey square.
bool begin_combo(const char* label, const char* preview, ImGuiComboFlags flags = 0) {
    auto* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    const bool open = ImGui::BeginCombo(label, preview, flags | ImGuiComboFlags_NoArrowButton);
    const std::string g = icon(0xE70D);  // ChevronDown
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.7f / ImGui::GetStyle().FontScaleDpi);
    const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
    const float x = p.x + w - gs.x - 10 * ImGui::GetStyle().FontScaleDpi;
    dl->AddText({x, p.y + (h - gs.y) * 0.5f}, theme::kMuted, g.c_str());
    ImGui::PopFont();
    return open;
}
bool combo(const char* label, int* current, const char* const items[], int count) {
    bool changed = false;
    if (begin_combo(label, *current >= 0 && *current < count ? items[*current] : "")) {
        for (int i = 0; i < count; ++i)
            if (ImGui::Selectable(items[i], i == *current)) *current = i, changed = true;
        ImGui::EndCombo();
    }
    return changed;
}

// A line of text that ends in "..." instead of running past the available width; the full text is
// the tooltip when it was cut.
void text_line_fit(const char* text, ImU32 col = theme::kText) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::Dummy({std::min(w, ts.x), ts.y});
    text_fit(ImGui::GetWindowDrawList(), p, col, text, p.x + w);
    if (ts.x > w && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// A button: primary (white, dark text) or `secondary` (grey fill, hairline border). `tall` is the
// larger size used on the start page.
bool dbutton(const char* label, float min_w, bool secondary = false, bool tall = false) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushFont(g_fonts.button, 0);
    const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    const float pad_x = (tall ? 18 : 14) * sc;
    const ImVec2 size{std::max(min_w, ts.x + 2 * pad_x), (tall ? 40 : 32) * sc};
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    auto* dl = ImGui::GetWindowDrawList();
    const float r = 6 * sc;
    const ImVec2 q{p.x + size.x, p.y + size.y};
    ImU32 fill, text;
    if (secondary) {
        fill = held ? IM_COL32(63, 63, 70, 255) : hovered ? IM_COL32(52, 52, 56, 255) : theme::kSurfaceHi;
        text = theme::kText;
    } else {
        fill = held ? IM_COL32(212, 212, 216, 255) : hovered ? IM_COL32(228, 228, 231, 255) : theme::kText;
        text = theme::kPrimaryText;
    }
    if (disabled) {
        fill = (fill & 0x00FFFFFF) | (IM_COL32_A_MASK & IM_COL32(0, 0, 0, 110));
        text = (text & 0x00FFFFFF) | (IM_COL32_A_MASK & IM_COL32(0, 0, 0, 140));
    }
    dl->AddRectFilled(p, q, fill, r);
    if (secondary) dl->AddRect(p, q, theme::kBorder, r);
    const float tx = std::max(p.x + pad_x, p.x + (size.x - ts.x) * 0.5f);
    const std::string shown(label, ImGui::FindRenderedTextEnd(label));
    text_fit(dl, {tx, p.y + (size.y - ts.y) * 0.5f}, text, shown.c_str(), q.x - pad_x * 0.5f);
    if (hovered && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopFont();
    return pressed;
}

// A title in the bold face at a given pixel size.
void heading(const char* text, float size = 24) {
    ImGui::PushFont(g_fonts.heading, size);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

// Text centred on a line within `w` pixels (single line).
void centered_text(const char* text, float w, ImU32 col = theme::kText) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - ts.x) * 0.5f));
    ImGui::TextColored(col4(col), "%s", text);
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

// A centred block for "nothing here yet": glyph, title, and a short hint that says what to do.
void empty_state(uint32_t glyph, const char* title, const char* hint) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float wrap = std::min(avail.x, 440 * sc);
    const ImVec2 hs = ImGui::CalcTextSize(hint, nullptr, false, wrap);
    const float block = ImGui::GetTextLineHeight() * 2 + 14 * sc + hs.y;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - block) * 0.38f));
    ImGui::PushFont(g_fonts.bold, 0);
    centered_text(icon(glyph).c_str(), avail.x, theme::kMuted);
    centered_text(title, avail.x);
    ImGui::PopFont();
    ImGui::Dummy({0, 6 * sc});
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail.x - wrap) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::PushStyleColor(ImGuiCol_Text, col4(theme::kMuted));
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

// A centred spinner and a line of text while something is being read.
void loading_state(const char* text) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (avail.y - 60 * sc) * 0.4f));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    spinner(ImGui::GetWindowDrawList(), {p.x + avail.x * 0.5f, p.y + 12 * sc}, 11 * sc, theme::kMuted);
    ImGui::Dummy({0, 34 * sc});
    centered_text(text, avail.x, theme::kMuted);
}

// ---- data loaded in the background -----------------------------------------------------------------
struct MeshEntry { TagHash tag; uint32_t size; uint16_t pkg; };
struct PackageGroup { uint16_t pkg_id; std::string name; std::vector<uint32_t> meshes; };
struct EntryRow { TagHash tag; uint16_t type, subtype; uint32_t reference, size; };

struct Catalog {
    std::mutex mutex;
    std::string status = ui::tr(ui::S::Preparing);
    std::atomic<bool> meshes_ready{false}, strings_ready{false}, dialogue_ready{false}, codex_ready{false}, movies_ready{false}, done{false};
    std::vector<MeshEntry> meshes;
    std::vector<PackageGroup> packages;  // packages that hold static meshes
    std::vector<MeshEntry> dynamics;     // dynamic models (entity meshes)
    std::vector<PackageGroup> dyn_packages;
    StringTable strings;
    std::vector<DialogueLine> dialogue;
    std::vector<MovieEntry> movies;
    Codex codex;
    std::string codex_error;  // the codex failed alone; the rest of the catalog is fine
    std::string error;
};

void load_catalog(fs::path install, Catalog* cat) {
    try {
        PackageManager pm(install);
        {
            std::lock_guard lk(cat->mutex);
            cat->status = ui::tr(ui::S::StatusListingMeshes);
        }
        std::vector<MeshEntry> meshes, dynamics;
        std::vector<PackageGroup> groups, dyn_groups;
        for (const auto& info : pm.packages()) {
            Package& p = pm.open(info);
            PackageGroup g{info.pkg_id, info.name, {}}, dg{info.pkg_id, info.name, {}};
            for (uint32_t i = 0; i < p.entries().size(); ++i) {
                const Entry& e = p.entries()[i];
                if (e.type != 8) continue;
                if (e.reference == kClassStaticMesh) {
                    g.meshes.push_back(uint32_t(meshes.size()));
                    meshes.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.file_size, info.pkg_id});
                } else if (e.reference == kClassDynamicModel) {
                    dg.meshes.push_back(uint32_t(dynamics.size()));
                    dynamics.push_back({TagHash::make(info.pkg_id, uint16_t(i)), e.file_size, info.pkg_id});
                }
            }
            if (!g.meshes.empty()) groups.push_back(std::move(g));
            if (!dg.meshes.empty()) dyn_groups.push_back(std::move(dg));
        }
        {
            std::lock_guard lk(cat->mutex);
            cat->meshes = std::move(meshes);
            cat->packages = std::move(groups);
            cat->dynamics = std::move(dynamics);
            cat->dyn_packages = std::move(dyn_groups);
            cat->status = ui::tr(ui::S::StatusLoadingText);
        }
        cat->meshes_ready = true;
        StringTable st;
        st.load_all(pm);
        {
            std::lock_guard lk(cat->mutex);
            cat->strings = std::move(st);
            cat->status = ui::tr(ui::S::StatusLoadingDialogue);
        }
        cat->strings_ready = true;
        auto dlg = load_all_dialogue(pm);
        {
            std::lock_guard lk(cat->mutex);
            cat->dialogue = std::move(dlg);
            cat->status = ui::tr(ui::S::StatusListingMovies);
        }
        cat->dialogue_ready = true;
        {
            std::lock_guard lk(cat->mutex);
            cat->status = ui::tr(ui::S::StatusLoadingCodex);
        }
        try {
            Codex cx = load_codex(pm, cat->dialogue);
            std::lock_guard lk(cat->mutex);
            cat->codex = std::move(cx);
            cat->codex_ready = true;
        } catch (const std::exception& ex) {
            std::lock_guard lk(cat->mutex);
            cat->codex_error = ex.what();
        }
        {
            std::lock_guard lk(cat->mutex);
            cat->status = ui::tr(ui::S::StatusListingMovies);
        }
        auto movies = list_movies(pm);
        {
            std::lock_guard lk(cat->mutex);
            cat->movies = std::move(movies);
            cat->status = "";
        }
        cat->movies_ready = true;
    } catch (const std::exception& ex) {
        std::lock_guard lk(cat->mutex);
        cat->error = ex.what();
        cat->status = ui::trf(ui::S::StatusLoadError, ex.what());
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
        bool no_drawable = false;
        ComPtr<ID3D12Resource> thumbnail_texture;
        std::optional<PreparedMesh> mesh;
        std::string error;
        double ms = 0;
        // A preview may hold more than one model. Each clip belongs to one model's rig.
        std::optional<PreviewAnimation> animation;
        std::optional<PreviewAnimation> part_animation;
    };

    void start(const fs::path& install, ID3D12Device* device, const std::vector<std::vector<InputElement>>& layouts) {
        stop();
        quit_ = false;
        thread_ = std::thread([this, install, device = ComPtr<ID3D12Device>(device), layouts] { run(install, device.Get(), layouts); });
    }
    void stop() {
        {
            std::lock_guard lk(m_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        live_.reset();
        working_thumbnail_ = {};
        thumbs_.clear();
        results_.clear();
        thumbnail_cache_ = PrepareCache{512};
        materials_.reset();
        rigs_.reset();
        clips_.reset();
        assemblies_.clear();
        motions_.clear();
    }
    void request_live(TagHash t, int permutation = -1, ChannelValues channels = {}) {
        {
            std::lock_guard lk(m_);
            live_ = t;
            live_permutation_ = permutation;
            live_channels_ = std::move(channels);
        }
        cv_.notify_all();
    }
    void request_thumbnails(std::vector<TagHash> tags) {
        {
            std::lock_guard lk(m_);
            thumbs_.clear();
            for (TagHash tag : tags) {
                if (working_thumbnail_ == tag || std::ranges::any_of(results_, [tag](const Result& r) { return r.thumbnail && r.tag == tag; })) continue;
                thumbs_.push_back(tag);
            }
        }
        cv_.notify_all();
    }
    std::optional<Result> take(bool thumbnail) {
        std::lock_guard lk(m_);
        for (auto it = results_.begin(); it != results_.end(); ++it)
            if (it->thumbnail == thumbnail) {
                Result r = std::move(*it);
                results_.erase(it);
                cv_.notify_one();
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
    struct ThumbnailRenderer {
        Gpu gpu;
        StaticMeshScene scene;
        GBuffer gb;
        GameLighting lighting;
        bool preview;

        ThumbnailRenderer(ID3D12Device* device, PackageManager& pm) {
            gpu.init_preview(device);
            lighting.init(pm, gpu);
            preview = false;
            try { preview = lighting.set_entity_preview(pm, gpu); }
            catch (const std::exception&) {}
            gb.resize(gpu, kThumbSize, kThumbSize);
        }

        ComPtr<ID3D12Resource> render(PreparedMesh&& mesh, const std::vector<std::vector<InputElement>>& layouts) {
            gpu.begin_frame(0);
            scene.upload(gpu, std::move(mesh), layouts);
            const auto view = make_view(frame_sphere(scene.center(), scene.radius(), kThumbSize, kThumbSize));
            auto* cl = gpu.immediate();
            LightingParams params;
            params.atmosphere = false;
            params.entity_preview = preview;
            params.auto_exposure = params.temporal_aa = false;
            lighting.prepare_frame(gpu, gb, params);
            scene.set_exposure(lighting.exposure());
            scene.render_gbuffer(gpu, cl, gb, view);
            GameLighting::TransparentFn transparent;
            if (scene.has_transparent()) transparent = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
                scene.render_transparent(gpu, c, gb, hdr, view, lighting);
            };
            GameLighting::DistortionFn distortion;
            if (scene.has_visible_distortion()) distortion = [&](auto* c, uint32_t w, uint32_t h) {
                scene.render_distortion(gpu, c, w, h, view, lighting.externs());
            };
            lighting.render(gpu, cl, gb, view, params, transparent, nullptr, scene.has_visible_light_grid(), distortion);
            auto texture = gpu.texture2d(kThumbSize, kThumbSize, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
            Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyResource(texture.Get(), gb.lit.Get());
            Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            Gpu::barrier(cl, texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            gpu.flush();
            return texture;
        }
    };

    void run(fs::path install, ID3D12Device* device, const std::vector<std::vector<InputElement>>& layouts) {
        WorkerPool::use_current_thread_only();
        std::optional<ThumbnailRenderer> thumbnail_renderer;
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
            int permutation = -1;
            ChannelValues channels;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return quit_ || live_ || (!thumbs_.empty() && std::ranges::none_of(results_, &Result::thumbnail)); });
                if (quit_) return;
                if (live_) {
                    tag = *live_;
                    permutation = live_permutation_;
                    channels = std::move(live_channels_);
                    live_channels_.clear();
                    live_.reset();
                    working_live_ = true;
                } else {
                    tag = thumbs_.front();
                    thumbs_.pop_front();
                    working_thumbnail_ = tag;
                    thumb = true;
                }
            }
            Result r;
            r.tag = tag;
            r.thumbnail = thumb;
            const bool background = thumb && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            auto t0 = std::chrono::steady_clock::now();
            try {
                const Entry* e = pm->entry(tag);
                if (e && e->reference == kClassDynamicModel) {
                    // The entity components hold the materials; index them once.
                    if (!materials_) materials_ = index_dynamic_model_materials(*pm);
                    if (!rigs_) rigs_ = index_model_skeletons(*pm, *materials_, &assemblies_, &motions_);
                    r.mesh = prepare_entity_preview(*pm, tag, *materials_, assemblies_, permutation,
                                                    channels.empty() ? nullptr : &channels, thumb ? &thumbnail_cache_ : nullptr);
                    const auto skinned = [](const PreparedMesh& m) {
                        return std::ranges::any_of(m.buffers, [](const auto& b) { return !b.skin.bones.empty(); });
                    };
                    if (!thumb && !r.mesh->no_drawable &&
                        (rigs_->contains(tag.value) || !r.mesh->attachments.empty() || skinned(*r.mesh))) {
                        try {
                            if (!clips_) clips_ = index_animation_clips(*pm);
                            r.animation = prepare_preview_animation(*pm, *r.mesh, *rigs_, *clips_, {}, {}, &motions_);
                            if (r.animation && r.animation->model != tag) {
                                auto local = prepare_preview_animation(*pm, *r.mesh, *rigs_, *clips_, tag, {}, &motions_);
                                if (local && !local->clips.empty()) r.part_animation = std::move(local);
                            }
                            if (r.animation) prepare_preview_equipment(*pm, *r.mesh, *r.animation, *rigs_, *clips_, *materials_);
                        } catch (const std::exception& ex) {
                            r.mesh->notes.push_back(std::format("animation: {}", ex.what()));
                        }
                    }
                } else {
                    r.mesh = prepare_static_mesh(*pm, tag, channels.empty() ? nullptr : &channels, thumb ? &thumbnail_cache_ : nullptr);
                }
            } catch (const std::exception& ex) {
                r.error = ex.what();
            }
            if (thumb) {
                thumbnail_cache_.textures.clear();
                if (r.mesh) {
                    r.no_drawable = r.mesh->no_drawable;
                    try {
                        if (r.no_drawable) r.error = r.mesh->notes.empty() ? "no drawable geometry" : r.mesh->notes.front();
                        else {
                            if (!thumbnail_renderer) thumbnail_renderer.emplace(device, *pm);
                            r.thumbnail_texture = thumbnail_renderer->render(std::move(*r.mesh), layouts);
                        }
                    } catch (const std::exception& ex) {
                        r.error = ex.what();
                        thumbnail_renderer.reset();
                    }
                    r.mesh.reset();
                }
            }
            if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
            r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard lk(m_);
            if (!thumb) working_live_ = false;
            else working_thumbnail_ = {};
            results_.push_back(std::move(r));
        }
    }

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, working_live_ = false;
    TagHash working_thumbnail_;
    std::optional<TagHash> live_;
    int live_permutation_ = -1;
    ChannelValues live_channels_;
    std::optional<std::unordered_map<uint32_t, DynamicModelMaterials>> materials_;
    std::optional<ModelRigIndex> rigs_;
    ModelAssemblyIndex assemblies_;
    ModelMotionIndex motions_;
    std::optional<std::vector<ClipInfo>> clips_;
    PrepareCache thumbnail_cache_{512};
    std::deque<TagHash> thumbs_;
    std::deque<Result> results_;
};

// Maps: lists the bubbles once, then streams the static meshes of a region (prepared on this
// thread, nearest first) to the UI, which adds a time-boxed batch per frame. A newer request
// abandons the current one.
class MapWorker {
public:
    struct Piece {
        uint64_t generation = 0;
        PreparedMesh mesh;
        std::vector<std::array<float, 12>> rows;
        std::vector<float> sky_order;
    };
    struct Progress {
        uint64_t generation = 0;
        size_t total = 0, prepared = 0, failed = 0;
        bool done = false;
        std::array<float, 3> center{};
        float extent = 0;  // farthest loaded placement from the centre (m)
        float typical = 0;  // 80th percentile placement distance (framing; ignores far-off outliers)
        std::optional<MapStart> start;  // the first player start (estimate), the initial camera
        std::shared_ptr<const std::vector<MapLight>> lights;  // the region's local lights, once read
        std::shared_ptr<const std::vector<MapCubemap>> cubemaps;  // the region's reflection / ambient volumes
        std::shared_ptr<const std::vector<MapParticle>> particles;  // the region's particle systems (FX)
        std::shared_ptr<const MapWeather> weather;  // the map's world channel controllers
        std::shared_ptr<const MapAtmosphere> atmosphere;  // the map's sky lookups and sun path
        std::shared_ptr<const MapAudio> audio;  // the map's ambient sound emitters
        std::shared_ptr<const std::vector<MapEventPreview>> events;
        std::shared_ptr<const std::vector<PreparedEventTrack>> event_tracks;
        std::map<std::string, size_t> failures;  // reason -> count
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
        pieces_.clear();
        request_.reset();
        listed_ = false;
        list_requested_ = false;
        maps_.clear();
    }
    void request_list() {
        {
            std::lock_guard lk(m_);
            if (list_requested_) return;
            list_requested_ = true;
        }
        cv_.notify_all();
    }
    bool listed() {
        std::lock_guard lk(m_);
        return listed_;
    }
    const std::vector<MapEntry>& maps() const { return maps_; }  // valid once listed()
    std::string list_error() {
        std::lock_guard lk(m_);
        return list_error_;
    }
    // center: nullopt = where the map's geometry is (map_focus).
    uint64_t request(const MapEntry& selection, std::optional<std::array<float, 3>> center, float radius, uint32_t texture_cap, uint64_t preview) {
        std::lock_guard lk(m_);
        request_ = Request{++generation_, selection.bubble, center, radius, texture_cap, selection.activity, selection.bubble_name, selection.activity_map, preview};
        pieces_.clear();
        progress_ = Progress{generation_};
        cv_.notify_all();
        return generation_;
    }
    void cancel() {
        std::lock_guard lk(m_);
        ++generation_;
        request_.reset();
        pieces_.clear();
        cv_.notify_all();
    }
    std::optional<Piece> take(uint64_t generation) {
        std::lock_guard lk(m_);
        while (!pieces_.empty() && pieces_.front().generation != generation) pieces_.pop_front();
        if (pieces_.empty()) return std::nullopt;
        Piece p = std::move(pieces_.front());
        const bool was_full = pieces_.size() == kMaxQueuedPieces;
        pieces_.pop_front();
        if (was_full) cv_.notify_all();
        return p;
    }
    Progress progress() {
        std::lock_guard lk(m_);
        Progress result = progress_;
        result.done = result.done && pieces_.empty();
        return result;
    }
    ~MapWorker() { stop(); }

private:
    struct Request {
        uint64_t generation;
        TagHash bubble;
        std::optional<std::array<float, 3>> center;
        float radius;
        uint32_t texture_cap;
        TagHash activity;
        uint32_t bubble_name = 0, activity_map = 0;
        uint64_t preview = 0;
    };
    void run(fs::path install) {
        std::unique_ptr<PackageManager> pm;
        try {
            pm = std::make_unique<PackageManager>(install);
            pm->build_hash64_index();
        } catch (const std::exception& ex) {
            std::lock_guard lk(m_);
            list_error_ = ex.what();
            listed_ = true;
            return;
        }
        std::optional<MapLayout> layout;
        TagHash layout_activity;
        uint32_t layout_activity_map = 0;
        uint64_t layout_preview = 0;
        std::shared_ptr<const std::vector<MapEventPreview>> events;
        std::shared_ptr<const std::vector<PreparedEventTrack>> event_tracks;
        std::optional<PrepareCache> cache;
        std::vector<std::vector<InputElement>> layouts;
        try {
            layouts = load_input_layouts(*pm);
        } catch (const std::exception& ex) {
            std::lock_guard lk(m_);
            list_error_ = ex.what();
            listed_ = true;
            return;
        }
        std::vector<std::unique_ptr<PackageManager>> helpers;
        std::shared_ptr<const MapWeather> weather;
        std::shared_ptr<const MapAtmosphere> atmosphere;
        std::shared_ptr<const MapAudio> audio;
        std::optional<std::unordered_map<uint32_t, DynamicModelMaterials>> entity_materials;
        std::optional<MapEntities> entities;
        TagHash entities_bubble;
        std::vector<MapStart> starts;
        for (;;) {
            Request req;
            bool list = false;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return quit_ || request_ || (list_requested_ && !listed_); });
                if (quit_) return;
                if (list_requested_ && !listed_) list = true;
                else {
                    req = *request_;
                    request_.reset();
                }
            }
            if (list) {
                std::vector<MapEntry> maps;
                std::string err;
                try {
                    maps = list_maps(*pm);
                    auto activities = list_activity_maps(*pm);
                    maps.insert(maps.end(), std::make_move_iterator(activities.begin()), std::make_move_iterator(activities.end()));
                } catch (const std::exception& ex) {
                    err = ex.what();
                }
                std::lock_guard lk(m_);
                maps_ = std::move(maps);
                list_error_ = err;
                listed_ = true;
                continue;
            }
            auto stale = [&] {
                std::lock_guard lk(m_);
                return quit_ || generation_ != req.generation;
            };
            try {
                ProfileScope profile_layout("map.layout");
                const bool selection_changed = !layout || layout->bubble != req.bubble || layout_activity != req.activity || layout_activity_map != req.activity_map || layout_preview != req.preview;
                if (selection_changed) {
                    const auto tables = req.activity.is_none() ? std::vector<TagHash>{} : load_activity_node_tables(*pm, req.activity, req.bubble_name, req.activity_map);
                    layout = load_map_layout(*pm, req.bubble, tables);
                    try {  // DCON FX and the lockdown ships, left out by user decision (3.111)
                        pm->build_hash64_index();
                        hide_scripted_nodes(*pm, *layout, req.activity, req.preview);
                    } catch (const std::exception&) {
                    }
                    layout_activity = req.activity;
                    layout_activity_map = req.activity_map;
                    layout_preview = req.preview;
                    events = std::make_shared<const std::vector<MapEventPreview>>(std::move(layout->event_previews));
                    event_tracks.reset();
                    if (req.preview) {
                        auto tracks = prepare_event_tracks(*pm, *layout);
                        event_tracks = std::make_shared<const std::vector<PreparedEventTrack>>(std::move(tracks));
                    }
                    entities.reset();
                    starts = find_map_starts(*pm, *layout);
                }
                profile_layout.stop();
                ProfileScope profile_entities("map.entities");
                if (!cache || cache->max_texture_dim != req.texture_cap) {
                    cache.emplace();
                    cache->max_texture_dim = req.texture_cap;
                    cache->keep_static_lods_for_map = true;
                }
                const auto center = req.center ? *req.center : !starts.empty() ? starts.front().position : map_focus(*layout);
                // Lights, probes, FX, weather, sky and audio need only the layout: read them on a
                // thread of their own while this one gathers the meshes, so the UI can set the
                // lighting up before the first mesh arrives.
                std::shared_ptr<PackageManager> env_pm = pm->fork();
                auto env_task = std::async(std::launch::async, [&, env_pm, center] {
                    ProfileScope profile_env("map.environment");
                    auto lights = std::make_shared<std::vector<MapLight>>();
                    try {
                        auto all = load_map_lights(*env_pm, *layout);
                        try {  // the lights the map's FX carry (3.111), shown with the FX
                            env_pm->build_hash64_index();
                            for (auto& l : load_map_fx_lights(*env_pm, *layout)) all.push_back(std::move(l));
                        } catch (const std::exception&) {
                        }
                        for (const MapLight* l : map_light_region(all, center, req.radius)) lights->push_back(*l);
                    } catch (const std::exception&) {
                        // a map whose node tables cannot be read is drawn without local lights
                    }
                    auto cubemaps = std::make_shared<std::vector<MapCubemap>>();
                    // Surface probe failures through the worker's existing error path; silently
                    // continuing with zero probes makes a data failure look like missing lighting.
                    *cubemaps = load_map_cubemaps(*env_pm, *layout);
                    auto particles = std::make_shared<std::vector<MapParticle>>();
                    try {
                        env_pm->build_hash64_index();
                        for (auto& p : load_map_particles(*env_pm, *layout))
                            if (std::hypot(p.rows[3] - center[0], p.rows[7] - center[1], p.rows[11] - center[2]) <= req.radius)
                                particles->push_back(p);
                    } catch (const std::exception&) {
                        // drawn without FX
                    }
                    if (selection_changed || !weather || weather_bubble_ != req.bubble) {
                        try {
                            weather = std::make_shared<const MapWeather>(load_map_weather(*env_pm, req.bubble));
                        } catch (const std::exception&) {
                            weather = std::make_shared<const MapWeather>();  // no weather controls
                        }
                        try {
                            atmosphere = std::make_shared<const MapAtmosphere>(load_map_atmosphere(*env_pm, *layout));
                        } catch (const std::exception&) {
                            atmosphere = std::make_shared<const MapAtmosphere>();  // drawn without a sky
                        }
                        try {
                            audio = std::make_shared<const MapAudio>(load_map_audio(*env_pm, *layout));
                        } catch (const std::exception&) {
                            audio = std::make_shared<const MapAudio>();
                        }
                        weather_bubble_ = req.bubble;
                    }
                    {
                        std::lock_guard lk(m_);
                        if (generation_ == req.generation) {
                            progress_.weather = weather;
                            progress_.atmosphere = atmosphere;
                            progress_.audio = audio;
                            progress_.lights = std::move(lights);
                            progress_.cubemaps = std::move(cubemaps);
                            progress_.particles = std::move(particles);
                        }
                    }
                });
                auto region = map_region(*layout, center, req.radius);
                const size_t static_count = region.size();
                const auto terrain = map_terrain_region(*layout, center, req.radius);
                if (!entities || entities_bubble != req.bubble) {
                    if (!entity_materials) entity_materials = index_dynamic_model_materials(*pm);
                    entities = load_map_entities(*pm, *layout, *entity_materials);
                    entities_bubble = req.bubble;
                }
                auto entity_region = map_region(entities->models, center, req.radius);
                for (auto& model : entity_region) region.push_back(std::move(model));
                float extent = 1;
                std::vector<float> distances;
                std::vector<std::pair<float, size_t>> world_jobs;
                world_jobs.reserve(region.size());
                for (size_t j = 0; j < region.size(); ++j) {
                    const auto& st = region[j];
                    float nearest = std::numeric_limits<float>::infinity();
                    for (const auto& r : st.rows) {
                        distances.push_back(std::hypot(r[3] - center[0], r[7] - center[1], r[11] - center[2]));
                        extent = std::max(extent, distances.back());
                        nearest = std::min(nearest, distances.back());
                    }
                    world_jobs.emplace_back(nearest, j + terrain.size());
                }
                float typical = extent;
                if (!distances.empty()) {
                    const auto k = distances.begin() + std::ptrdiff_t(distances.size() * 8 / 10);
                    std::nth_element(distances.begin(), k, distances.end());
                    typical = std::max(1.0f, *k);
                }
                // Authored sky collections are independent of the world streaming radius.
                // Append after framing statistics so kilometre-distant scenery cannot move
                // the user's initial camera.
                profile_entities.stop();
                ProfileScope profile_sky("map.sky_decals");
                const size_t first_sky = region.size();
                for (auto& model : load_map_sky_objects(*pm, *layout)) region.push_back(std::move(model));
                const auto decals = load_map_decals(*pm, *layout);
                const auto decal_volumes = load_map_decal_volumes(*pm, *layout, center, req.radius);
                const size_t decal_start = terrain.size() + region.size();
                profile_sky.stop();
                std::sort(world_jobs.begin(), world_jobs.end());
                std::vector<size_t> jobs;
                jobs.reserve(decal_start + decals.size() + decal_volumes.size());
                // The skyline must be visible while the thousands of world meshes are loading.
                for (size_t i = first_sky; i < region.size(); ++i) jobs.push_back(terrain.size() + i);
                for (size_t i = 0; i < terrain.size(); ++i) jobs.push_back(i);
                for (const auto& [distance, index] : world_jobs) jobs.push_back(index);
                for (size_t i = 0; i < decals.size() + decal_volumes.size(); ++i) jobs.push_back(decal_start + i);
                {
                    std::lock_guard lk(m_);
                    if (generation_ != req.generation) continue;
                    progress_.total = jobs.size();
                    progress_.events = events;
                    progress_.event_tracks = event_tracks;
                    progress_.center = center;
                    progress_.extent = extent;
                    progress_.typical = typical;
                    if (!starts.empty() && !req.center) progress_.start = starts.front();
                }
                // Prepare sky, terrain and then nearby world assets in parallel.
                // Keep one logical CPU available for the UI and upload thread.
                if (helpers.empty()) {
                    const unsigned n = std::max(std::thread::hardware_concurrency(), 2u) - 2;
                    for (unsigned i = 0; i < n; ++i) helpers.push_back(pm->fork());
                }
                std::atomic<size_t> next{0};
                auto work = [&](PackageManager& wpm) {
                    for (;;) {
                        const size_t slot = next.fetch_add(1);
                        if (slot >= jobs.size() || stale()) return;
                        const size_t j = jobs[slot];
                        Piece piece{req.generation};
                        std::string failure;
                        try {
                            if (j >= decal_start + decals.size()) {
                                piece.mesh = prepare_map_decal_volumes(wpm, decal_volumes[j - decal_start - decals.size()], &*cache);
                                piece.rows = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
                            } else if (j >= decal_start) {
                                const auto& decal = decals[j - decal_start];
                                piece.mesh = prepare_map_decal(wpm, decal, layouts, &*cache);
                                piece.rows = {decal.rows};
                            } else if (j < terrain.size()) {
                                piece.mesh = prepare_terrain(wpm, *terrain[j], layouts, &*cache);
                                piece.rows = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}};
                            } else if (j - terrain.size() < static_count) {
                                auto& st = region[j - terrain.size()];
                                piece.mesh = prepare_static_mesh(wpm, st.mesh, nullptr, &*cache);
                                piece.rows = std::move(st.rows);
                            } else {
                                auto& model = region[j - terrain.size()];
                                const auto mat = entity_materials->find(model.mesh.value);
                                piece.mesh = prepare_dynamic_model(wpm, model.mesh,
                                    mat == entity_materials->end() ? nullptr : &mat->second, -1, nullptr,
                                    &*cache, model.regions.get());
                                if (piece.mesh.no_drawable) throw std::runtime_error("entity model with no drawable parts");
                                piece.mesh.patrol_start = model.patrol_start;
                                piece.rows = std::move(model.rows);
                                piece.sky_order = std::move(model.sky_order);
                            }
                        } catch (const std::exception& ex) {
                            failure = (j >= decal_start ? "decal: " : j < terrain.size() ? "terrain: " :
                                       j - terrain.size() >= static_count ? "entity: " : "")
                                    + std::string(ex.what()).substr(0, 110);
                        }
                        std::unique_lock lk(m_);
                        // Back-pressure: the UI adds a few meshes per frame.
                        cv_.wait(lk, [&] { return quit_ || generation_ != req.generation || pieces_.size() < kMaxQueuedPieces; });
                        if (quit_ || generation_ != req.generation) return;
                        ++progress_.prepared;
                        if (!failure.empty()) {
                            ++progress_.failed;
                            ++progress_.failures[failure];
                        } else pieces_.push_back(std::move(piece));
                    }
                };
                std::vector<std::jthread> threads;
                for (auto& h : helpers) threads.emplace_back([&, hp = h.get()] { work(*hp); });
                // The environment (above) was read alongside; its errors surface here as before.
                env_task.get();
                ProfileScope profile_tail("map.prepare_tail");
                work(*pm);
                for (auto& t : threads) t.join();
                profile_tail.stop();
                // All prepares have finished. Queued pieces keep their own shared references;
                // uploaded textures live on the GPU. Retaining every CPU mip in this cache
                // needlessly keeps several GB resident after a whole-map load.
                {
                    std::unique_lock texture_lock(*cache->texture_mutex);
                    cache->textures.clear();
                }
            } catch (const std::exception& ex) {
                std::lock_guard lk(m_);
                if (generation_ == req.generation) {
                    ++progress_.failed;
                    ++progress_.failures[std::string(ex.what()).substr(0, 120)];
                }
            }
            std::lock_guard lk(m_);
            if (generation_ == req.generation) progress_.done = true;
        }
    }

    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    bool quit_ = false, list_requested_ = false, listed_ = false;
    std::vector<MapEntry> maps_;
    std::string list_error_;
    uint64_t generation_ = 0;
    std::optional<Request> request_;
    static constexpr size_t kMaxQueuedPieces = 64;
    std::deque<Piece> pieces_;
    TagHash weather_bubble_;
    Progress progress_;
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
    // The title bar is drawn by the UI: the top `caption_h` pixels drag the window except over
    // `caption_hot` (the search box and the window buttons, refreshed every frame).
    int caption_h = 0;
    std::vector<RECT> caption_hot;
};
Window g_win;
nimview::FrameProfile g_prof;  // NIM_BENCH (3.111)
using Prof = nimview::FrameProfile;
FrameGeneration g_frame_generation;
bool g_frame_generation_requested = false, g_frame_generation_proxy = false;

LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        // The whole window is client area (the UI draws its own title bar). Maximized, Windows places
        // the frame off screen, so take it back off the client rectangle.
        if (wp == TRUE) {
            if (IsZoomed(hwnd)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const UINT dpi = GetDpiForWindow(hwnd);
                const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy;
                p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (!IsZoomed(hwnd)) {
            const int b = MulDiv(6, int(GetDpiForWindow(hwnd)), 96);
            const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, d = pt.y >= rc.bottom - b;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (d && l) return HTBOTTOMLEFT;
            if (d && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (d) return HTBOTTOM;
        }
        if (pt.y < g_win.caption_h) {
            for (const RECT& h : g_win.caption_hot)
                if (PtInRect(&h, pt)) return HTCLIENT;
            return HTCAPTION;
        }
        return HTCLIENT;
    }
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

void create_swapchain(Gpu& gpu, bool use_frame_generation) {
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
    ComPtr<IDXGIFactory6> factory = gpu.factory();
    g_frame_generation_proxy = use_frame_generation && g_frame_generation.upgrade_factory(factory);
    HRESULT created = factory->CreateSwapChainForHwnd(gpu.queue(), g_win.hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(created) && g_frame_generation_proxy) {
        g_frame_generation_proxy = false;
        created = gpu.factory()->CreateSwapChainForHwnd(gpu.queue(), g_win.hwnd, &sd, nullptr, nullptr, &sc1);
    }
    check_hr(created, "CreateSwapChainForHwnd");
    if (!g_frame_generation_proxy) g_frame_generation_requested = false;
    check_hr(sc1.As(&g_win.swapchain), "IDXGISwapChain3");
    gpu.factory()->MakeWindowAssociation(g_win.hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_win.swapchain->SetMaximumFrameLatency(2);
    g_win.waitable = g_win.swapchain->GetFrameLatencyWaitableObject();
    create_back_buffers(gpu);
}

void init_swapchain(Gpu& gpu) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    check_hr(gpu.device()->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_win.rtv_heap)), "rtv heap");
    create_swapchain(gpu, false);
    for (auto& a : g_win.alloc)
        check_hr(gpu.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)), "allocator");
    check_hr(gpu.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_win.alloc[0].Get(), nullptr,
                                             IID_PPV_ARGS(&g_win.list)),
             "command list");
    g_win.list->SetName(L"nim viewport and window present");
    g_win.list->Close();
}

void release_swapchain(Gpu& gpu) {
    g_frame_generation.set_active(false);
    if (!gpu.removed()) gpu.wait_idle();
    for (auto& b : g_win.back) b.Reset();
    if (g_win.waitable) CloseHandle(g_win.waitable);
    g_win.waitable = nullptr;
    g_win.swapchain.Reset();
}

void release_window_gpu(Gpu& gpu) {
    release_swapchain(gpu);
    g_win.list.Reset();
    g_win.rtv_heap.Reset();
    for (auto& alloc : g_win.alloc) alloc.Reset();
    std::fill(std::begin(g_win.fence), std::end(g_win.fence), 0);
}

void resize_swapchain(Gpu& gpu, UINT w, UINT h) {
    g_frame_generation.set_active(false);
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

enum class Engine : int { Game };

// The pages of the navigation rail. Every page is always reachable from the rail.
enum Tab : int { TabHome, TabFind, TabSettings, TabStatics, TabEntities, TabMaps, TabText, TabDialogue, TabMovies, TabCodex, TabPackages, TabLog, TabCount };
struct TabInfo { uint32_t icon; ui::S name; ui::S desc; };
constexpr TabInfo kTabs[TabCount] = {
    {ico::Home, ui::S::NavHome, ui::S::DescHome},
    {ico::Search, ui::S::NavFind, ui::S::DescFind},
    {ico::Settings, ui::S::NavSettings, ui::S::DescSettings},
    {ico::Statics, ui::S::NavStatics, ui::S::DescStatics},
    {ico::Entities, ui::S::NavEntities, ui::S::DescEntities},
    {ico::Maps, ui::S::NavMaps, ui::S::DescMaps},
    {ico::Text, ui::S::NavText, ui::S::DescText},
    {ico::Dialogue, ui::S::NavDialogue, ui::S::DescDialogue},
    {ico::Movies, ui::S::NavMovies, ui::S::DescMovies},
    {ico::Codex, ui::S::NavCodex, ui::S::DescCodex},
    {ico::Packages, ui::S::NavPackages, ui::S::DescPackages},
    {ico::Log, ui::S::NavLog, ui::S::DescLog},
};

struct Thumb {
    ComPtr<ID3D12Resource> tex;
    uint32_t srv = UINT32_MAX;
    uint64_t last_used = 0;
    bool failed = false;
    bool no_drawable = false;
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
    MapWorker map_worker;

    // The loaded map region (the live scene holds its meshes).
    struct MapView {
        // 0: the map alone, 1: the map as a game mode (activity) uses it. Default 1: the game always
        // plays a map through an activity, whose placements carry most props (terminals, screens).
        int selection_mode = 1;
        char filter[64] = "", event_filter[64] = "";
        int selected = -1;     // index into map_worker.maps()
        static constexpr float kUnlimited = 5000;
        float radius = kUnlimited;  // default to the user's whole-world workflow
        bool lights_set = false, particles_set = false, audio_set = false;
        int texture_cap = 3;   // kMapTextureCaps index (full resolution)
        uint64_t generation = 0;
        bool streaming = false, framed = false;
        size_t processed = 0;
        std::chrono::steady_clock::time_point load_started;
        double load_seconds = 0, first_piece_seconds = 0, last_piece_seconds = 0;
        TagHash bubble;
        std::string name;
        TagHash activity;
        uint32_t activity_map = 0;
        std::array<float, 3> center{};
        float typical = 0;              // Progress::typical
        std::optional<MapStart> start;  // Progress::start
        uint64_t preview = 0;
        std::shared_ptr<const std::vector<MapEventPreview>> events;
    };
    MapView map;
    bool map_scene = false;  // the live scene is a map region
    std::optional<TagHash> pending_map;  // from the command line, loaded once the maps are listed
    std::optional<std::pair<TagHash, uint32_t>> pending_activity;
    std::optional<Camera> pending_camera;
    static constexpr uint32_t kMapTextureCaps[] = {256, 512, 1024, 0};

    // live view
    StaticMeshScene scene;
    GBuffer gb;
    LightingPass lighting;
    GameLighting game_lighting;
    ParticleScene particles;  // the map's FX
    EventPlayback event_player;
    bool event_playing = false;
    std::unique_ptr<media::EntityAudioPlayer> event_audio;
    std::vector<media::EntityAudioPlayer::Cue> event_cues;
    bool fx_enabled = true;
    bool game_ready = false;
    Engine engine = Engine::Game;
    std::string render_error;
    LightingParams light{.vignette = true, .depth_of_field = true, .emissive_gain = 1.35f, .output_gamma = 0.85f};
    Camera cam;
    uint32_t view_w = 0, view_h = 0;
    bool viewport_visible = false;
    bool frame_generation_rect_valid = false;
    uint32_t frame_generation_x = 0, frame_generation_y = 0;
    std::optional<TagHash> loading;
    std::optional<TagHash> no_drawable_tag;
    std::string no_drawable_reason;
    bool capture_requested = false;
    bool show_fps = true;
    TagHash scene_component;
    char channel_filter[96]{};
    ChannelValues channel_overrides;  // object channels the user set for the live mesh
    uint32_t scene_permutation = 0, scene_permutation_count = 0;
    bool keep_camera = false;
    bool shadows = true;   // nim's sun shadow map (viewer estimate)
    SunShadow sun_shadow;
    float material_time = 0, material_speed = 1;
    bool material_playing = true;

    // Skeletal animation of the live model (clips are sampled at 30 fps).
    struct Animation {
        TagHash model;
        PreviewAnimation binding;
        std::optional<PreviewAnimation> alternate;
        bool part_only = false;
        int clip = -1;  // index into clips
        TagHash equipment_model;
        AnimationClip loaded;
        std::string error;
        bool playing = true, dirty = false;
        float frame = 0, speed = 1;
        char filter[96]{};
    };
    std::optional<Animation> anim;
    static constexpr float kClipFps = 30;

    // thumbnails
    std::deque<std::pair<uint64_t, Thumb>> retired_thumbnails;
    bool thumbnails_enabled = true;
    std::unordered_map<uint32_t, Thumb> thumbs;
    uint64_t frame_no = 0;

    std::deque<LogLine> log;
    int language = 1;  // second column language (ja by default)
    Tab tab = TabMaps;  // the page shown in the content area

    // statics browser
    struct Browser {
        char filter[64] = "";
        int sort_mode = 0;  // 0 id, 1 name, 2 count
        int current_pkg = -1;
        std::vector<int> order;
        int order_sort = -1;
    };
    Browser browsers[2];  // statics, entities
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
    struct DialogueGroup {
        uint64_t key = 0;
        std::string name;
        std::vector<uint32_t> lines;
    };
    int dialogue_text_lang = -1;  // -1: the second-language setting
    bool dialogue_show_english = true;
    int dialogue_group_mode = 0;  // 0 speaker, 1 conversation table
    char dialogue_group_filter[64] = "";
    int dialogue_group = -1;      // index into dialogue_groups, -1 = all lines
    std::vector<DialogueGroup> dialogue_groups;
    int dialogue_groups_mode = -1, dialogue_groups_lang = -1;
    int dialogue_selected = -1;   // index into catalog->dialogue
    std::vector<std::pair<uint16_t, std::string>> audio_langs;  // installed localized audio packages (id, name)
    std::vector<std::string> audio_suffixes;                    // their package suffixes (names follow the UI language)
    int audio_lang = -1;          // -1 follows this screen's text language

    // audio playback (dialogue, map ambient) and WAV export
    std::unique_ptr<media::AudioEngine> audio_engine_;
    std::unique_ptr<media::ClipPlayer> clip;
    std::unique_ptr<media::AmbientAudioPlayer> ambient_audio;
    bool ambient_audio_enabled = true;
    float ambient_audio_volume = 0.75f;
    // Sounds of the AI actors and dropships (3.90): cues gathered by update_ai, played each frame.
    std::unique_ptr<media::EntityAudioPlayer> entity_audio;
    bool entity_audio_enabled = true;
    float entity_audio_volume = 0.75f;
    std::vector<media::EntityAudioPlayer::Cue> entity_cues;
    std::vector<uint64_t> entity_stopped;
    std::vector<std::pair<uint64_t, std::array<float, 3>>> entity_emitters;
    struct AudioRequest {
        uint64_t id = 0;
        std::future<std::shared_ptr<const PcmAudio>> result;
    };
    std::optional<AudioRequest> audio_request;
    std::string audio_error;
    std::thread export_thread;
    std::atomic<bool> export_running{false}, export_cancel{false};
    std::atomic<size_t> export_done{0}, export_total{0}, export_failed{0}, export_missing{0};
    std::mutex export_mutex;
    std::string export_message;

    // movies
    static constexpr int kSubtitlesOff = -1;
    struct MovieState {
        char filter[64] = "";
        int selected = -1;   // index into catalog->movies
        int channel = 0;     // USM audio channel
        int sub_lang = -2;   // -2: the second-language setting, kSubtitlesOff, or a text language
        std::unique_ptr<media::MoviePlayer> player;
        std::unique_ptr<media::MoviePlayer::Frame> pending;
        ComPtr<ID3D12Resource> tex;
        std::array<ComPtr<ID3D12Resource>, Gpu::kFramesInFlight> upload;
        std::array<size_t, Gpu::kFramesInFlight> upload_size{};
        uint32_t srv = UINT32_MAX, w = 0, h = 0;
        bool tex_readable = false, has_frame = false, seeking = false;
        float seek_value = 0;
    } movie;
    struct CodexUi {
        char filter[128] = "";
        int lang = -1;  // -1: the second-language setting
        bool audio_only = false;
        bool crt_tint = true;  // phosphor green CRT filter on codex reader artwork
        int collection = -1, entry = -1;
        bool open_reader = false;
        std::vector<std::pair<int, int>> view;  // (collection, record) in grid order
        // Illustrations, uploaded on first view and kept (183 distinct textures at most).
        struct Art {
            ComPtr<ID3D12Resource> tex;
            uint32_t srv = UINT32_MAX;
            int w = 0, h = 0;
            std::string error;
        };
        std::unordered_map<uint32_t, Art> art;
        std::unique_ptr<PackageManager> art_pm;
    } codex;
    int selected_package = -1;
    std::vector<EntryRow> package_rows;
    char entry_filter[32] = "";
    // The prompt line and the find page.
    char prompt[256] = "";
    bool focus_prompt = false;
    std::string find_query;
    struct FindGroup {
        Tab tab;
        size_t count = 0;
        std::vector<std::string> samples;
    };
    std::vector<FindGroup> find_results;
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
            if (p->packages().empty()) throw std::runtime_error(ui::tr(ui::S::ErrPackagesMissing));
            p->build_hash64_index();
            layouts = load_input_layouts(*p);
            pm = std::move(p);
            install = dir;
            info(ui::trf(ui::S::LogInstallOpened, to_utf8(dir.wstring()), pm->packages().size()));
            if (loader.joinable()) loader.join();
            catalog = std::make_unique<Catalog>();
            loader = std::thread(load_catalog, dir, catalog.get());
            worker.start(dir, gpu.device(), layouts);
            map_worker.start(dir);
            map = MapView{};
            map_scene = false;
            if (ambient_audio) ambient_audio->clear();
            game_lighting.clear_lights(gpu);
            game_lighting.clear_cubemaps(gpu);
            game_lighting.clear_atmosphere(), map_atmosphere.reset();
            entity_preview_active = false;
            init_game_lighting();
            return true;
        } catch (const std::exception& ex) {
            error(ui::trf(ui::S::ErrOpenInstall, ex.what()));
            return false;
        }
    }

    void init_game_lighting() {
        gpu.wait_idle();
        try {
            game_lighting.release(gpu);
            game_lighting.init(*pm, gpu);
            game_ready = true;
            info(ui::trf(ui::S::LogLighting, 
                             game_lighting.shading_technique().str()));
            for (const auto& n : game_lighting.notes()) info("  " + n);
        } catch (const std::exception& ex) {
            game_ready = false;
            error(ui::trf(ui::S::ErrLighting, ex.what()));
        }

    }

    bool set_entity_preview(GameLighting& renderer, bool& active) {
        if (active) return true;
        try {
            active = renderer.set_entity_preview(*pm, gpu);
        } catch (const std::exception& ex) {
            error(ui::trf(ui::S::ErrEntityLight, ex.what()));
        }
        return active;
    }

    // Object channels: the per-object state values the materials read (frozen, in a heat cascade,
    // damage, dissolve ...). Editing one re-evaluates the materials' TFX with the new value.
    // Engine inputs with no game data behind them (no map loaded, run-time state): what nim used
    // instead and why. Collapsed by default; the header count is the summary.
    void estimates_panel() {
        std::map<std::string, std::string> all = scene.estimates();
        if (engine == Engine::Game && game_ready)
            for (const auto& [what, why] : game_lighting.externs().estimates()) all.emplace(what, why);
        if (all.empty()) return;
        ImGui::Spacing();
        if (!ImGui::CollapsingHeader((ui::trf(ui::S::EstimatesHeader, all.size()) + "###estimates").c_str())) return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(ui::tr(ui::S::EstimatesHint));
        std::map<std::string, std::vector<const std::string*>> by_reason;
        for (const auto& [what, why] : all) by_reason[why].push_back(&what);
        for (const auto& [why, items] : by_reason) {
            ImGui::Spacing();
            ImGui::TextUnformatted(why.c_str());
            for (const auto* w : items) ImGui::TextDisabled("  %s", w->c_str());
        }
        ImGui::PopTextWrapPos();
    }

    void channels_panel() {
        const auto& used = scene.channels();
        if (used.empty()) return;
        ImGui::Spacing();
        if (!ImGui::CollapsingHeader((ui::trf(ui::S::ChannelsHeader, used.size()) + "###channels").c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen))
            return;
        const float em = ImGui::GetFontSize();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##channel_filter", ui::tr(ui::S::ChannelFilter), channel_filter, sizeof(channel_filter));
        const auto filter = lower_ascii(channel_filter);
        bool changed = false;
        ImGui::BeginDisabled(channel_overrides.empty());
        if (ImGui::Button(ui::tr(ui::S::ResetAll))) {
            channel_overrides.clear();
            changed = true;
        }
        ImGui::EndDisabled();
        bool found = false;
        for (const auto& [hash, use] : used) {
            const auto id = std::format("{:08X}", hash);
            const char* name = object_channel_name(hash);
            if (!filter.empty() && !contains_ci(id, filter) && (!name || !contains_ci(name, filter))) continue;
            found = true;
            ImGui::PushID(int(hash));
            auto override = channel_overrides.find(hash);
            const bool overridden = override != channel_overrides.end();
            const auto label = name ? std::format("{}  ({})###value", name, id) : id + "###value";
            if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.8f, 0.3f, 1});
            const bool edit = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (overridden) ImGui::PopStyleColor();
            if (edit) {
                ImGui::TextDisabled("%s%s", use.vs ? "VS" : "", use.ps ? (use.vs ? " + PS" : "PS") : "");
                float4 v = overridden ? override->second : use.value;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::DragFloat4("##v", v.data(), 0.01f, -10.0f, 10.0f, "%.3f")) {
                    channel_overrides[hash] = v;
                    changed = true;
                }
                const bool zero = ImGui::Button("0", {em * 2.5f, 0});
                ImGui::SameLine();
                const bool one = ImGui::Button("1", {em * 2.5f, 0});
                ImGui::SameLine();
                ImGui::BeginDisabled(!channel_overrides.contains(hash));
                if (ImGui::Button(ui::tr(ui::S::ResetDefault))) {
                    channel_overrides.erase(hash);
                    changed = true;
                }
                ImGui::EndDisabled();
                if (zero || one) {
                    const float value = one ? 1.0f : 0.0f;
                    channel_overrides[hash] = {value, value, value, value};
                    changed = true;
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (!found) ImGui::TextDisabled("%s", ui::tr(ui::S::NoMatches));
        if (changed) scene.set_channels(channel_overrides);
    }

    void request_load(TagHash tag) {
        if (loading && *loading == tag) return;
        stop_map();
        channel_overrides.clear();
        channel_filter[0] = '\0';
        loading = tag;
        no_drawable_tag.reset();
        no_drawable_reason.clear();
        worker.request_live(tag);
    }

    // Runs at the top of a frame, before any UI references GPU resources.
    void apply_pending(Gpu& g) {
        if (view_w && view_h) {
            const DlssMode active_dlss = (game_ready && light.mode == ViewMode::Lit && light.post && light.temporal_aa && Dlss::built())
                ? (light.dlss_mode != DlssMode::Off ? light.dlss_mode : (light.dlaa ? DlssMode::DLAA : DlssMode::Off))
                : DlssMode::Off;
            const auto [rw, rh] = Dlss::render_size(view_w, view_h, active_dlss);
            if (rw != gb.width() || rh != gb.height() || view_w != gb.output_width() || view_h != gb.output_height()) {
                g.wait_idle();
                gb.resize(g, rw, rh, view_w, view_h);
            }
        }
        if (auto r = worker.take(false)) {
            const bool expected = loading && *loading == r->tag;
            if (expected) loading.reset();
            if (map_scene && !expected) return;  // requested before a map replaced the live scene
            g.wait_idle();
            const bool no_drawable = r->mesh && r->mesh->no_drawable;
            if (no_drawable) {
                scene.release(g);
                no_drawable_tag = r->tag;
                no_drawable_reason = r->mesh->notes.empty() ? "no drawable geometry" : r->mesh->notes.front();
                info(std::format("{}: {}", r->tag.str(), no_drawable_reason));
            }
            if (r->mesh && !no_drawable) {
                try {
                    auto t0 = std::chrono::steady_clock::now();
                    scene_component = r->mesh->component;
                    scene_permutation = r->mesh->permutation;
                    scene_permutation_count = r->mesh->permutation_count;
                    const TagHash prev = map_scene ? TagHash{} : scene.tag();
                    stop_map();
                    map_scene = false;
                    game_lighting.clear_lights(g);
                    if (!entity_preview_active) {
                        game_lighting.clear_cubemaps(g);
                        game_lighting.clear_atmosphere();
                        entity_preview_active = false;
                    }
                    map_atmosphere.reset();
                    map_weather.reset();
                    weather_states.clear();
                    apply_weather();
                    scene.upload(g, std::move(*r->mesh), layouts);
                    scene.set_channels(channel_overrides);
                    if (game_ready) set_entity_preview(game_lighting, entity_preview_active);
                    if (!r->animation) {
                        anim.reset();
                    } else if (!anim || anim->model != r->tag) {
                        anim = Animation{r->tag, std::move(*r->animation), std::move(r->part_animation)};
                    } else {
                        anim->dirty = true;  // same model reloaded (variant / channels): pose it again
                    }
                    keep_camera = prev == scene.tag();
                    if (!keep_camera) {
                        frame_scene();
                        material_time = 0;
                    }
                    double up = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    size_t stubbed = 0;
                    for (const auto& gi : scene.groups()) stubbed += gi.stubbed_ops.size();
                    info(ui::trf(ui::S::LogModelLoaded, r->tag.str(),
                                     scene.groups().size(), r->ms, up,
                                     stubbed ? ui::trf(ui::S::LogStubbed, stubbed) : ""));
                } catch (const std::exception& ex) {
                    scene.release(g);
                    error(ui::trf(ui::S::ErrDraw, r->tag.str(), ex.what()));
                }
            } else if (!no_drawable) {
                error(ui::trf(ui::S::ErrRead, r->tag.str(), r->error));
            }
        }
        publish_thumbnail(g);
        try {
            const auto ps = g_prof.scope(Prof::Stream);
            stream_map(g);
        } catch (const std::exception& ex) {
            stop_map();
            error(ui::trf(ui::S::ErrMapStopped, ex.what()));
        }
        try {
            update_ai(g);
        } catch (const std::exception& ex) {
            ai.failed = true;
            error(ui::trf(ui::S::ErrAi, ex.what()));
        }
    }

    // ---- maps ----
    void stop_map() {
        if (!map.streaming) return;
        map_worker.cancel();
        map.streaming = false;
    }
    void load_map(int index, std::optional<std::array<float, 3>> center, uint64_t preview = 0) {
        const auto& maps = map_worker.maps();
        if (index < 0 || index >= int(maps.size())) return;
        const auto& m = maps[size_t(index)];
        map.load_started = std::chrono::steady_clock::now();
        map.processed = 0;
        map.load_seconds = 0;
        const bool whole = map.radius >= MapView::kUnlimited;
        if (index != map.selected) {
            map.events.reset();
            map.event_filter[0] = 0;
        }
        map.preview = preview;
        map.generation = map_worker.request(m, center, whole ? 1e9f : map.radius, kMapTextureCaps[map.texture_cap], preview);
        loading.reset();  // a live mesh still being prepared is discarded when it arrives
        map_add_failures.clear();
        gpu.wait_idle();
        reset_ai();
        event_player = {};
        event_playing = false;
        if (event_audio) event_audio->clear();
        scene.begin(gpu);
        anim.reset();
        channel_overrides.clear();
        const bool same = map_scene && map.bubble == m.bubble;
        map.framed = center.has_value();  // reloading around the camera keeps the view
        map.bubble = m.bubble;
        map.activity = m.activity;
        map.activity_map = m.activity_map;
        map.name = m.name.empty() ? m.package : m.name;
        map.selected = index;
        map.streaming = true;
        map_scene = true;
        game_lighting.clear_lights(gpu);
        game_lighting.clear_cubemaps(gpu);
        game_lighting.clear_atmosphere(), map_atmosphere.reset();
        entity_preview_active = false;
        if (!same) map_weather.reset(), weather_states.clear();
        rain.valid = false;
        apply_weather();
        map.lights_set = false;
        map.particles_set = false;
        map.audio_set = false;
        if (ambient_audio) ambient_audio->clear();
        particles.release(gpu);
        info(whole ? ui::trf(ui::S::LogMapLoadAll, m.package, m.bubble.str())
                   : ui::trf(ui::S::LogMapLoadRadius, m.package, m.bubble.str(), map.radius));
    }
    void stream_map(Gpu& g) {
        if (!map.streaming) return;
        const auto t0 = std::chrono::steady_clock::now();
        bool added = false;
        ProfileScope profile_stream("map.ui_stream");
        constexpr auto upload_budget = std::chrono::milliseconds(32);
        // Batch more uploads while the viewport is black. Bound each batch so the loading UI
        // can still respond and recorded copies can release their staging memory regularly.
        while (std::chrono::steady_clock::now() - t0 < upload_budget) {
            auto piece = map_worker.take(map.generation);
            if (!piece) break;
            added = true;
            if (!map.processed)
                map.first_piece_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - map.load_started).count();
            ++map.processed;
            ProfileScope profile_add("map.ui_add");
            try {
                scene.add_mesh(g, std::move(piece->mesh), layouts, piece->rows, piece->sky_order);
            } catch (const std::exception& ex) {
                // add_mesh rolled the mesh back; counted with the worker's failures below.
                ++map_add_failures[std::string(ex.what()).substr(0, 120)];
            }
        }
        const auto pr = map_worker.progress();
        if (pr.generation != map.generation) return;
        map.center = pr.center;
        map.typical = pr.typical;
        map.start = pr.start;
        if (pr.events) map.events = pr.events;
        if (pr.weather && map_weather != pr.weather) {
            map_weather = pr.weather;
            weather_states.assign(map_weather->controllers.size(), -1);
            apply_weather();
        }
        if (pr.atmosphere && map_atmosphere != pr.atmosphere) {
            map_atmosphere = pr.atmosphere;
            ProfileScope profile_atm("map.ui_atmosphere");
            game_lighting.set_atmosphere(*pm, g, map_atmosphere.get());
            for (const auto& n : game_lighting.atmosphere_notes()) ++map_add_failures[n.substr(0, 120)];
        }
        if (pr.lights && !map.lights_set) {
            map.lights_set = true;
            ProfileScope profile_lights("map.ui_lights_cubemaps");
            map_lights = pr.lights;  // kept alive while the lighting refers to it
            apply_map_lights(g);
            if (pr.cubemaps) {
                std::vector<const MapCubemap*> cps;
                for (const auto& c : *pr.cubemaps) cps.push_back(&c);
                map_cubemaps = pr.cubemaps;
                try {
                    game_lighting.set_cubemaps(*pm, g, cps);
                } catch (const std::exception& ex) {
                    ++map_add_failures[std::string("cubemaps: ") + std::string(ex.what()).substr(0, 100)];
                }
                for (const auto& n : game_lighting.cubemap_notes()) ++map_add_failures[n.substr(0, 120)];
            }
        }
        if (pr.particles && !map.particles_set) {
            map.particles_set = true;
            ProfileScope profile_fx("map.ui_particles");
            try {
                particles.load(*pm, g, *pr.particles);
            } catch (const std::exception& ex) {
                ++map_add_failures[std::string("fx: ") + std::string(ex.what()).substr(0, 100)];
            }
            for (const auto& n : particles.notes()) ++map_add_failures[n.substr(0, 120)];
        }
        if (pr.audio && !map.audio_set) {
            map.audio_set = true;
            if (auto* eng = audio_engine()) {
                if (!ambient_audio) ambient_audio = std::make_unique<media::AmbientAudioPlayer>(*eng);
                ambient_audio->set_map(*pm, pr.audio);
            }
        }
        if (pr.done) {
            ProfileScope profile_finish("map.ui_finish");
            if (map.processed) {
                if (pr.event_tracks && !pr.event_tracks->empty())
                    event_player.build(g, scene, layouts, pr.event_tracks);
                scene.finish(g, map.bubble, V3{pr.center[0], pr.center[1], pr.center[2]},
                             std::min(map.radius, pr.extent) * 0.6f);
                if (!map.framed && scene.loaded()) {
                    frame_scene();
                    if (map.start)
                        info(ui::trf(ui::S::LogCameraStart, map.start->position[0],
                                     map.start->position[1], map.start->position[2], kStartEyeHeight));
                    map.framed = true;
                }
            }
            map.streaming = false;
            map.load_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - map.load_started).count();
            info(ui::trf(ui::S::LogMapDone, map.name, scene.mesh_count(), scene.instance_count(),
                             pr.failed ? ui::trf(ui::S::LogSkipped, pr.failed) : ""));
        } else if (added) {
            map.last_piece_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - map.load_started).count();
            g.submit();  // queue this batch's uploads without waiting for the GPU
        }
    }
    // ---- AI (docs/PROGRESS.md 3.69 / 3.70) ----
    // The activity's estimated AI simulation, its agents drawn as the characters' own models and
    // nim's indicators over them. The gameplay, motion banks and character meshes are read on a
    // thread of their own (own PackageManager); the GPU part runs at the top of a frame.
    struct AiPrepared {
        TagHash activity;
        std::shared_ptr<MapGameplay> gameplay;
        std::vector<MotionSet> motions;
        std::vector<ActorCharacter> characters;
        ActorDropship ship;  // the dropship squads' ship (3.71); no model = none on this map
        std::vector<ActorCharacter> drones;  // one per scan drone entity (3.94)
        std::string error;
        double seconds = 0;
    };
    struct AiView {
        bool enabled = false, awareness = true, markers = true, debug = false, visible = false;
        float range = 60;
        TagHash activity;  // the activity the state below belongs to
        std::future<AiPrepared> pending;
        std::shared_ptr<MapGameplay> gameplay;
        std::unique_ptr<AiSimulation> sim;
        AiActors actors;
        bool failed = false;
    } ai;
    std::vector<std::future<AiPrepared>> ai_discarded;  // preparations of maps left since

    static AiPrepared prepare_ai(fs::path dir, TagHash activity) {
        AiPrepared out;
        out.activity = activity;
        const auto t0 = std::chrono::steady_clock::now();
        try {
            PackageManager pm(dir);
            pm.build_hash64_index();
            const ActorSources sources = index_actor_sources(pm);
            out.gameplay = std::make_shared<MapGameplay>(load_map_gameplay(pm, activity));
            // Motion sets per entity; characters per entity and spawn tags (3.92 / 3.93).
            std::map<uint32_t, size_t> motion_of;
            std::map<std::pair<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>, size_t> first;
            for (const auto& ch : out.gameplay->characters) {
                const auto [m, fresh_motion] = motion_of.try_emplace(ch.entity.value, out.motions.size());
                out.motions.push_back(fresh_motion ? build_motion_set(pm, ch.entity, kClipFps) : MotionSet(out.motions[m->second]));
                if (const auto it = first.find({ch.entity.value, ch.tags}); it != first.end()) {
                    out.characters.push_back(out.characters[it->second]);
                    continue;
                }
                first[{ch.entity.value, ch.tags}] = out.characters.size();
                out.characters.push_back(prepare_actor_character(pm, sources, out.motions.back(), ch.tags));
            }
            // The UESC dropship (3.71): the model of the dropship squads' spawner nodes; its sounds
            // (3.90) from every such node's entity.
            if (const auto ships = dropship_entities(*out.gameplay); !ships.empty())
                out.ship = prepare_actor_dropship(pm, sources, TagHash(0x80ACA5E7u), ships);
            // Scan drones (3.94): each drone entity once.
            for (const auto e : drone_entities(*out.gameplay)) out.drones.push_back(prepare_actor_drone(pm, sources, e));
        } catch (const std::exception& ex) {
            out.error = ex.what();
        }
        out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return out;
    }
    void reset_ai() {
        if (ai.pending.valid()) ai_discarded.push_back(std::move(ai.pending));
        ai.actors.clear();  // the scene's actors go with the scene (begin / release)
        if (entity_audio) entity_audio->clear();
        entity_cues.clear(), entity_stopped.clear(), entity_emitters.clear();
        ai.sim.reset();
        ai.gameplay.reset();
        ai.activity = TagHash();
        ai.failed = false;
    }
    // Top of a frame: start / finish the preparation, then step the simulation and pose the actors.
    void update_ai(Gpu& g) {
        std::erase_if(ai_discarded, [](auto& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
        const bool ready_map = map_scene && !map.activity.is_none() && !map.streaming && scene.loaded();
        if (!ready_map || ai.activity != map.activity) {
            if (ai.sim || ai.pending.valid() || !ai.activity.is_none()) reset_ai();
            if (!ready_map) return;
        }
        if (!ai.enabled && !ai.sim) return;
        if (!ai.sim && !ai.pending.valid() && !ai.failed) {
            ai.activity = map.activity;
            ai.pending = std::async(std::launch::async, prepare_ai, install, map.activity);
            info(ui::trf(ui::S::LogAiPreparing, map.activity.str()));
        }
        if (ai.pending.valid() && ai.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            AiPrepared p = ai.pending.get();
            if (!p.error.empty() || !p.gameplay) {
                ai.failed = true;
                error(ui::trf(ui::S::ErrAi, p.error));
                return;
            }
            try {
                const auto t0 = std::chrono::steady_clock::now();
                ai.gameplay = p.gameplay;
                AiParams params;
                params.player_invisible = !ai.visible;
                ai.sim = std::make_unique<AiSimulation>(*ai.gameplay, params, 1, std::move(p.motions));
                // Open floor of every firing area the squads use (drawn top-down, before actors exist).
                std::set<size_t> areas;
                for (const auto& sq : ai.gameplay->squads)
                    if (sq.objective)
                        for (const auto fa : ai.gameplay->objectives[*sq.objective].firing_areas) areas.insert(fa);
                g.wait_idle();
                SunShadow walk_view;
                for (const auto fa : areas)
                    ai.sim->set_walk_grid(fa, build_walk_grid(g, scene, ai.gameplay->firing_areas[fa], walk_view, 0.25f,
                                                              squad_spawn_points(*ai.gameplay, fa)));
                add_ground_patches(g, scene, *ai.sim, walk_view);  // spawn and drop points the grids miss (3.72)
                ai.actors.build(g, scene, layouts, std::move(p.characters), *ai.gameplay, *ai.sim, 4);
                ai.actors.apply_radii(*ai.sim);
                if (!p.ship.model.is_none()) ai.actors.build_dropships(g, scene, layouts, std::move(p.ship), 3);
                ai.actors.build_drones(g, scene, layouts, std::move(p.drones), *ai.gameplay, *ai.sim, 3);
                if (auto* eng = audio_engine()) {
                    if (!entity_audio) entity_audio = std::make_unique<media::EntityAudioPlayer>(*eng);
                    entity_audio->set_package(*pm);
                }
                for (const auto& n : ai.actors.notes()) ++map_add_failures["ai: " + n.substr(0, 110)];
                info(ui::trf(ui::S::LogAiReady, ai.sim->agents().size(), ai.sim->drones().size(), ai.actors.slots(), p.seconds,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
            } catch (const std::exception& ex) {
                ai.failed = true;
                ai.actors.clear();
                ai.sim.reset();
                error(ui::trf(ui::S::ErrAi, ex.what()));
                return;
            }
        }
        if (!ai.sim) return;
        const ViewMatrices vm = make_view(current_view());
        const std::array<float, 3> eye{vm.eye.x, vm.eye.y, vm.eye.z};
        if (!ai.enabled) {
            {
                const auto ps = g_prof.scope(Prof::AiActors);
                ai.actors.update(scene, *ai.sim, eye, 0, -1);  // hidden, simulation paused
            }
            take_actor_sounds();
            return;
        }
        const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
        ai.sim->set_player_invisible(!ai.visible);
        {
            const auto ps = g_prof.scope(Prof::AiSim);
            ai.sim->step(dt, eye);
        }
        {
            const auto ps = g_prof.scope(Prof::AiActors);
            ai.actors.update(scene, *ai.sim, eye, dt, ai.range);
        }
        take_actor_sounds();
    }
    void take_actor_sounds() {
        for (const auto& s : ai.actors.sounds()) entity_cues.push_back({s.event, s.emitter});
        entity_stopped.insert(entity_stopped.end(), ai.actors.stopped_emitters().begin(), ai.actors.stopped_emitters().end());
        entity_emitters = ai.actors.emitters();
    }
    void draw_ai_overlay(ImDrawList* dl, ImVec2 origin, ImVec2 size, float sc) {
        if (!ai.enabled || !ai.sim || !ai.gameplay || !map_scene) return;
        AiOverlayOptions options;
        options.awareness = ai.awareness;
        options.markers = ai.markers;
        options.debug = ai.debug;
        options.range = ai.range;
        const ViewMatrices vm = make_view(current_view());
        const AiOverlay o = build_ai_overlay(*ai.sim, *ai.gameplay, vm, size.x, size.y, options);
        dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (const auto& r : o.rects) {
            const ImVec2 a{origin.x + r.min[0], origin.y + r.min[1]}, b{origin.x + r.max[0], origin.y + r.max[1]};
            if (r.filled) dl->AddRectFilled(a, b, r.color);
            else dl->AddRect(a, b, r.color);
        }
        for (const auto& l : o.lines)
            dl->AddLine({origin.x + l.a[0], origin.y + l.a[1]}, {origin.x + l.b[0], origin.y + l.b[1]}, l.color, std::max(1.0f, l.width) * sc);
        for (const auto& t : o.texts) {
            const ImVec2 at{origin.x + t.at[0], origin.y + t.at[1]};
            const float font = ImGui::GetFontSize() * std::max(0.6f, t.scale);
            dl->AddText(nullptr, font, {at.x + 1, at.y + 1}, IM_COL32(0, 0, 0, (t.color >> 24) * 3 / 4), t.text.c_str());
            dl->AddText(nullptr, font, at, t.color, t.text.c_str());
        }
        dl->PopClipRect();
        const std::string status = ui::trf(ui::S::AiHud, ai.sim->agents().size(), ai.actors.shown(), ai.sim->time());
        hud_text(dl, {origin.x + 10 * sc, origin.y + size.y - 2 * ImGui::GetTextLineHeight() - 12 * sc}, IM_COL32(255, 210, 120, 230), status.c_str());
    }

    std::map<std::string, size_t> map_add_failures;
    std::shared_ptr<const std::vector<MapLight>> map_lights;
    bool map_lights_fx = true;  // the FX lights (MapLight::fx) are in the lighting
    // The map's local lights, with the FX's own lights only while FX are shown (3.111).
    void apply_map_lights(Gpu& g) {
        if (!map_lights) return;
        std::vector<const MapLight*> ptrs;
        for (const auto& l : *map_lights)
            if ((!l.fx || fx_enabled) && (!l.event || event_playing)) ptrs.push_back(&l);
        map_lights_fx = fx_enabled;
        try {
            game_lighting.set_lights(*pm, g, ptrs);
        } catch (const std::exception& ex) {
            ++map_add_failures[std::string("local lights: ") + std::string(ex.what()).substr(0, 100)];
        }
    }

    void set_event_playing(bool play) {
        const bool lights_changed = event_playing != play && map_lights &&
            std::ranges::any_of(*map_lights, &MapLight::event);
        event_playing = play;
        particles.set_event_playing(play);
        event_cues.clear();
        if (event_audio) event_audio->clear();
        if (play) {
            event_player.play();
            if (!event_audio && event_player.size())
                if (auto* eng = audio_engine()) {
                    event_audio = std::make_unique<media::EntityAudioPlayer>(*eng);
                    event_audio->set_package(*pm);
                }
        } else {
            event_player.stop(scene);
        }
        if (lights_changed) apply_map_lights(gpu);
    }
    std::shared_ptr<const std::vector<MapCubemap>> map_cubemaps;
    std::shared_ptr<const MapAtmosphere> map_atmosphere;
    bool entity_preview_active = false;
    // Weather: the map's world channel controllers and the state chosen for each (-1 = none,
    // the render-globals defaults).
    std::shared_ptr<const MapWeather> map_weather;
    std::vector<int> weather_states;
    bool heat_cascade_enabled = false; // keep the user's selection when changing maps/views
    RainOcclusion rain;

    void apply_weather() {
        std::map<uint32_t, float> channels;
        if (map_scene && map_weather)
            for (size_t i = 0; i < weather_states.size() && i < map_weather->controllers.size(); ++i) {
                const auto& c = map_weather->controllers[i];
                if (weather_states[i] >= 0 && size_t(weather_states[i]) < c.states.size())
                    for (const auto& [slot, v] : weather_values(c.states[size_t(weather_states[i])])) channels[slot] = v;
            }
        if (map_scene && map_weather && heat_cascade_enabled && map_weather->heat_cascade_controller)
            if (const auto* on = map_weather->heat_cascade_controller->strongest_state())
                for (const auto& [slot, v] : weather_values(*on)) channels[slot] = v;
        scene.set_global_channels(channels);
        game_lighting.set_global_channels(channels);
    }
    std::string weather_state_label(const WeatherState& st) const {
        std::string s = map_weather->name(st.name);
        for (const auto& v : st.values) {
            if (v.slot < 0) continue;
            s += std::format("  {}={:.3g}", map_weather->name(v.channel), 0.5f * (v.min + v.max));
        }
        return s;
    }
    void weather_ui() {
        if (!map_weather) return;
        ImGui::Spacing();
        const auto& cs = map_weather->controllers;
        ImGui::BeginDisabled(!map_weather->heat_cascade_controller);
        if (ImGui::Checkbox(ui::tr(ui::S::HeatCascade), &heat_cascade_enabled)) apply_weather();
        ImGui::EndDisabled();
        if (map_weather->heat_cascade_controller)
            ImGui::SetItemTooltip(ui::tr(ui::S::HeatCascadeTip),
                map_weather->heat_cascade_controller->name.c_str(),
                map_weather->heat_cascade_shared ? ui::tr(ui::S::HeatCascadeShared) : ui::tr(ui::S::HeatCascadeOwn));
        else ImGui::SetItemTooltip(ui::tr(ui::S::HeatCascadeMissing));
        if (ImGui::CollapsingHeader((ui::trf(ui::S::WeatherHeader, cs.size()) + "###mapweather").c_str())) {
            const float w = ImGui::GetContentRegionAvail().x * 0.6f;
            for (size_t i = 0; i < cs.size(); ++i) {
                const auto& c = cs[i];
                if (c.heat_cascade()) continue; // controlled by the persistent toggle above
                ImGui::PushID(int(i));
                const int cur = weather_states[i];
                const std::string preview = cur < 0 ? ui::tr(ui::S::Default) : weather_state_label(c.states[size_t(cur)]);
                ImGui::SetNextItemWidth(w);
                if (begin_combo("##state", preview.c_str())) {
                    if (ImGui::Selectable(ui::tr(ui::S::Default), cur < 0)) weather_states[i] = -1, apply_weather();
                    for (size_t k = 0; k < c.states.size(); ++k)
                        if (ImGui::Selectable(std::format("{}##{}", weather_state_label(c.states[k]), k).c_str(), cur == int(k)))
                            weather_states[i] = int(k), apply_weather();
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", c.name.c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled(ui::tr(ui::S::WeatherEstimate));
        }
    }

    void maps_tab() {
        if (!pm) return;
        map_worker.request_list();
        const float sc = ImGui::GetStyle().FontScaleDpi;
        if (!map_worker.listed()) {
            ImGui::SetCursorPos({20 * sc, 20 * sc});
            spinner(ImGui::GetWindowDrawList(), {ImGui::GetCursorScreenPos().x + 8 * sc, ImGui::GetCursorScreenPos().y + 8 * sc},
                    7 * sc, kGreen);
            ImGui::SetCursorPos({44 * sc, 20 * sc});
            ImGui::TextDisabled(ui::tr(ui::S::MapsListing));
            return;
        }
        if (const auto err = map_worker.list_error(); !err.empty()) {
            ImGui::SetCursorPos({20 * sc, 20 * sc});
            ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", ui::trf(ui::S::ErrMapList, err).c_str());
            return;
        }
        const auto& maps = map_worker.maps();
        if (pending_map) {
            // The map as its first game mode uses it (the list's kind follows), else the map alone.
            int pick = -1;
            if (pending_activity)
                for (size_t i = 0; i < maps.size() && pick < 0; ++i)
                    if (maps[i].bubble == *pending_map &&
                        maps[i].activity == pending_activity->first && maps[i].activity_map == pending_activity->second)
                        pick = int(i);
            for (size_t i = 0; i < maps.size() && pick < 0; ++i)
                if (maps[i].bubble == *pending_map && !maps[i].activity.is_none()) pick = int(i);
            for (size_t i = 0; i < maps.size() && pick < 0; ++i)
                if (maps[i].bubble == *pending_map) pick = int(i);
            if (pick >= 0) {
                map.selection_mode = maps[size_t(pick)].activity.is_none() ? 0 : 1;
                std::optional<std::array<float, 3>> center;
                if (pending_camera) {
                    cam = *pending_camera;
                    center = std::array{cam.target.x, cam.target.y, cam.target.z};
                }
                load_map(pick, center);
            }
            pending_map.reset();
            pending_activity.reset();
            pending_camera.reset();
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float left_w = 300 * sc, pad = 10 * sc;
        const auto grey = theme::kMuted;

        // left: maps, then the region controls
        ImGui::BeginChild("maps", {left_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos({pad, pad});
        ImGui::SetNextItemWidth(left_w - 2 * pad);
        {
            const char* kinds[] = {ui::tr(ui::S::MapKindMap), ui::tr(ui::S::MapKindMode)};
            if (combo("##map_view_kind", &map.selection_mode, kinds, 2)) map.selected = -1;
        }
        ImGui::SetItemTooltip(ui::tr(ui::S::MapKindTip));
        ImGui::SetNextItemWidth(left_w - 2 * pad);
        ImGui::InputTextWithHint("##mfilter", (icon(ico::Search) + ui::tr(ui::S::MapFilterHint)).c_str(), map.filter, sizeof(map.filter));
        const float footer = ImGui::GetFrameHeightWithSpacing() * 3 + ImGui::GetTextLineHeightWithSpacing() * 3 + pad * 2;
        ImGui::SetCursorPosX(0);
        ImGui::BeginChild("maplist", {0, -footer}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        const std::string f = lower_ascii(map.filter);
        for (size_t i = 0; i < maps.size(); ++i) {
            const auto& m = maps[i];
            if (int(!m.activity.is_none()) != map.selection_mode) continue;
            if (!f.empty() && !contains_ci(m.package, f) && !contains_ci(m.name, f) && !contains_ci(m.bubble.str(), f) && !contains_ci(m.activity.str(), f)) continue;
            ImGui::PushID(int(i));
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const bool live = map_scene && map.selected == int(i);
            if (ImGui::Selectable("##m", int(i) == map.selected, 0, {0, ImGui::GetTextLineHeight() * 2 + 4 * sc})) {
                map.selected = int(i);
                load_map(int(i), std::nullopt);
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            auto* dl = ImGui::GetWindowDrawList();
            const float lh = ImGui::GetTextLineHeight();
            if (live) dl->AddRectFilled({pos.x, pos.y + 2 * sc}, {pos.x + 2 * sc, pos.y + lh * 2 + 2 * sc}, kGreen);
            const float row_max = pos.x + ImGui::GetContentRegionAvail().x - pad * 0.5f;
            text_fit(dl, {pos.x + pad, pos.y + 2 * sc}, theme::kText, (m.name.empty() ? m.package : m.name).c_str(), row_max);
            text_fit(dl, {pos.x + pad, pos.y + 2 * sc + lh}, grey,
                     (m.activity.is_none() ? ui::trf(ui::S::MapRowStats, m.bubble.str(), m.statics, m.instances) :
                      ui::trf(ui::S::MapActivityRow, m.activity.str(), m.activity_map, m.bubble.str())).c_str(), row_max);
            ImGui::PopID();
        }
        if (maps.empty()) {
            ImGui::SetCursorPosX(pad);
            ImGui::TextDisabled(ui::tr(ui::S::MapsEmpty));
        }
        ImGui::EndChild();

        // region controls
        ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + pad});
        ImGui::BeginGroup();
        const float w = left_w - 2 * pad;
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##radius", &map.radius, 50, MapView::kUnlimited,
                           map.radius >= MapView::kUnlimited ? ui::tr(ui::S::RadiusUnlimited) : ui::tr(ui::S::RadiusFmt), ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip(ui::tr(ui::S::RadiusTip));
        ImGui::SetNextItemWidth(w);
        const char* caps[] = {ui::tr(ui::S::TexCap256), ui::tr(ui::S::TexCap512), ui::tr(ui::S::TexCap1024), ui::tr(ui::S::TexCapFull)};
        combo("##texcap", &map.texture_cap, caps, 4);
        ImGui::SetItemTooltip(ui::tr(ui::S::TexCapTip));
        ImGui::BeginDisabled(map.selected < 0);
        if (dbutton(ui::tr(ui::S::LoadAtCamera), w)) {
            const bool here = map_scene && map.selected >= 0 && maps[size_t(map.selected)].bubble == map.bubble;
            load_map(map.selected, here ? std::optional(std::array<float, 3>{cam.target.x, cam.target.y, cam.target.z}) : std::nullopt, map.preview);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(ui::tr(ui::S::LoadAtCameraTip));
        if (map_scene) {
            const auto pr = map_worker.progress();
            if (map.streaming && pr.generation == map.generation)
                ImGui::TextDisabled(ui::tr(ui::S::MapLoading), std::min(pr.total, map.processed + pr.failed), pr.total);
            else
                ImGui::TextDisabled(ui::tr(ui::S::MapCounts), scene.mesh_count(), scene.instance_count());
            ImGui::TextDisabled(ui::tr(ui::S::MapCenter), map.center[0], map.center[1], map.center[2]);
            ImGui::TextDisabled(ui::tr(ui::S::MapProbes), game_lighting.cubemap_count(), game_lighting.cubemaps_drawn());
        }
        ImGui::EndGroup();
        ImGui::EndChild();

        // right: the live view, map details below
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("mapscene", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (details_h <= 0) details_h = 310 * sc;
        const float view_h_px = std::max(120 * sc, ImGui::GetContentRegionAvail().y - details_h - 6 * sc);
        viewport_ui({ImGui::GetContentRegionAvail().x, view_h_px});
        ImGui::InvisibleButton("##mhsplit", {ImGui::GetContentRegionAvail().x, 6 * sc});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) details_h = std::max(40 * sc, details_h - ImGui::GetIO().MouseDelta.y);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemActive() ? theme::kRing : theme::kBorder);
        ImGui::BeginChild("mapdetails", {0, 0}, ImGuiChildFlags_None);
        map_details_ui();
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void map_details_ui() {
        if (!map_scene) {
            ImGui::TextDisabled(ui::tr(ui::S::MapEmptyHint));
            ImGui::TextDisabled(ui::tr(ui::S::FlyHint));
            return;
        }
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::TextUnformatted(map.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", map.bubble.str().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(ui::tr(ui::S::Copy))) ImGui::SetClipboardText(map.bubble.str().c_str());
        if (map.activity.is_none()) ImGui::TextDisabled(ui::tr(ui::S::MapViewLine), map.bubble.str().c_str());
        else ImGui::TextDisabled(ui::tr(ui::S::ActivityViewLine), map.activity.str().c_str(), map.activity_map, map.bubble.str().c_str());
        if (map.events && !map.events->empty()) {
            const auto events = map.events;
            const auto selected = std::ranges::find(*events, map.preview, &MapEventPreview::id);
            const char* label = selected == events->end() ? ui::tr(ui::S::EventIdle) : selected->name.c_str();
            ImGui::TextUnformatted(ui::tr(ui::S::EventPalette));
            ImGui::SameLine();
            ImGui::BeginDisabled(map.streaming);
            ImGui::SetNextItemWidth(360 * ImGui::GetStyle().FontScaleDpi);
            if (ImGui::BeginCombo("##eventpalette", label)) {
                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##eventfilter", ui::tr(ui::S::EventFilter), map.event_filter, sizeof(map.event_filter));
                const auto filter = lower_ascii(map.event_filter);
                if (ImGui::Selectable(ui::tr(ui::S::EventIdle), map.preview == 0) && map.preview)
                    load_map(map.selected, std::array{cam.target.x, cam.target.y, cam.target.z});
                for (const auto& event : *events) {
                    const auto& p = event.position;
                    const auto row = std::format("{}  ({:.0f}, {:.0f}, {:.0f})##{:016X}", event.name, p[0], p[1], p[2], event.id);
                    if (!filter.empty() && !contains_ci(row, filter)) continue;
                    if (ImGui::Selectable(row.c_str(), map.preview == event.id) && map.preview != event.id) {
                        cam.target = {p[0], p[1], p[2]};
                        cam.distance = 25;
                        cam.pitch = 0.2f;
                        material_time = 0;
                        load_map(map.selected, p, event.id);
                    }
                }
                ImGui::EndCombo();
            }
            if (selected != events->end()) {
                ImGui::SameLine();
                const bool playable = event_player.size() || particles.event_count() ||
                    (map_lights && std::ranges::any_of(*map_lights, &MapLight::event));
                ImGui::BeginDisabled(!playable);
                if (ImGui::Button(ui::tr(event_playing ? ui::S::EventReplay : ui::S::EventPlay)))
                    set_event_playing(true);
                ImGui::EndDisabled();
                if (event_playing) {
                    ImGui::SameLine();
                    if (ImGui::Button(ui::tr(ui::S::EventStop))) set_event_playing(false);
                } else if (!playable && !map.streaming) {
                    ImGui::TextDisabled("%s", ui::tr(ui::S::EventStaticOnly));
                }
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", ui::tr(ui::S::EventPreviewHint));
        }
        weather_ui();
        ImGui::TextDisabled(ui::tr(ui::S::FlyHint));
        const auto pr = map_worker.progress();
        std::map<std::string, size_t> skipped = map_add_failures;
        if (pr.generation == map.generation)
            for (const auto& [why, n] : pr.failures) skipped[why] += n;
        size_t total = 0;
        for (const auto& [why, n] : skipped) total += n;
        if (total) {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader((ui::trf(ui::S::SkippedHeader, total) + "###mapskipped").c_str())) {
                ImGui::PushTextWrapPos(0);
                for (const auto& [why, n] : skipped) ImGui::TextDisabled("%zu  %s", n, why.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled(ui::tr(ui::S::MapUnsupported));
        ImGui::PopTextWrapPos();
        estimates_panel();
    }

    void publish_thumbnail(Gpu& g) {
        while (!retired_thumbnails.empty() && retired_thumbnails.front().first <= g.completed()) {
            const auto descriptor = retired_thumbnails.front().second.srv;
            g.srv().free(descriptor, 1);
            retired_thumbnails.pop_front();
        }
        auto result = worker.take(true);
        if (!result || thumbs.contains(result->tag.value)) return;
        Thumb card;
        card.last_used = frame_no;
        card.tex = std::move(result->thumbnail_texture);
        card.failed = !card.tex && !result->no_drawable;
        card.no_drawable = result->no_drawable;
        card.error = std::move(result->error);
        if (card.tex) {
            card.srv = g.srv().alloc(1);
            g.device()->CreateShaderResourceView(card.tex.Get(), nullptr, g.srv().cpu(card.srv));
        }
        thumbs.emplace(result->tag.value, std::move(card));
        constexpr size_t max_cards = 600;
        if (thumbs.size() > max_cards) {
            const auto oldest = std::ranges::min_element(thumbs, {}, [](const auto& entry) { return entry.second.last_used; });
            if (oldest->second.srv != UINT32_MAX) retired_thumbnails.emplace_back(g.pending_fence(), std::move(oldest->second));
            thumbs.erase(oldest);
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
        // Inside a map the camera walks among the geometry: a near plane at human scale.
        v.near_z = map_scene ? 0.05f : std::max(0.001f, cam.radius * 0.005f);
        return v;
    }

    void select_clip(int i) {
        if (!anim || i < 0 || i >= int(anim->binding.clips.size())) return;
        anim->clip = i;
        anim->equipment_model = {};
        anim->frame = 0;
        anim->error.clear();
        try {
            anim->loaded = load_animation_clip(*pm, anim->binding.clips[size_t(i)].tag);
            restrict_preview_clip(anim->loaded, anim->binding.clips[size_t(i)]);
        } catch (const std::exception& ex) {
            anim->loaded = AnimationClip{};
            anim->error = ex.what();
        }
        anim->dirty = true;
    }

    // Advances the clip and hands the skin pose (pose * inverse bind per bone) to the scene.
    void update_animation(float dt) {
        if (!anim || !scene.skinned()) return;
        Animation& a = *anim;
        if (a.clip < 0 || !a.loaded.has_tracks()) {
            if (a.dirty) scene.set_skin_poses({});
            a.dirty = false;
            return;
        }
        const float frames = float(a.loaded.frame_count);
        if (a.playing && frames > 1 && !a.dirty) {
            a.frame = std::fmod(a.frame + dt * kClipFps * a.speed, frames);
            if (a.frame < 0) a.frame += frames;
            a.dirty = true;
        }
        if (!a.dirty) return;
        a.dirty = false;
        auto poses = a.binding.poses(a.loaded, a.frame, a.equipment_model);
        try {
            scene.load_animation_equipment(gpu, layouts, poses);
        } catch (const std::exception& ex) {
            a.error = ex.what();
        }
        scene.set_skin_poses(std::move(poses));
    }

    void animation_panel() {
        if (!anim || !scene.skinned()) return;
        Animation& a = *anim;
        if (a.alternate && ImGui::Checkbox(ui::tr(ui::S::PartClips), &a.part_only)) {
            std::swap(a.binding, *a.alternate);
            a.clip = -1;
            a.filter[0] = '\0';
            a.error.clear();
            a.dirty = true;
        }
        const auto& binding = a.binding;
        const auto& clips = binding.clips;
        if (clips.empty()) {
            ImGui::TextDisabled("%s", ui::tr(ui::S::NoClips));
            return;
        }
        const float em = ImGui::GetFontSize();
        const std::string preview = a.clip < 0 ? std::string(ui::tr(ui::S::BindPose))
                                               : ui::trf(ui::S::ClipFrames, clips[size_t(a.clip)].tag.str(),
                                                             clips[size_t(a.clip)].frames);
        ImGui::SetNextItemWidth(-1);
        ImGui::SetNextWindowSizeConstraints({0, 0}, {FLT_MAX, em * 23});
        if (begin_combo("##clip", preview.c_str())) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            const bool enter = ImGui::InputTextWithHint("##clip_filter", ui::tr(ui::S::ClipFilter), a.filter,
                sizeof(a.filter), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            const auto filter = lower_ascii(a.filter);
            if (ImGui::Selectable(ui::tr(ui::S::BindPose), a.clip < 0)) {
                a.clip = -1;
                a.error.clear();
                a.dirty = true;
            }
            std::vector<int> visible;
            for (int i = 0; i < int(clips.size()); ++i) {
                const auto& c = clips[size_t(i)];
                if (filter.empty() || contains_ci(c.tag.str(), filter) ||
                    contains_ci(std::format("{:08X}", c.name_hash), filter))
                    visible.push_back(i);
            }
            if (visible.empty()) ImGui::TextDisabled("%s", ui::tr(ui::S::NoMatches));
            if (enter && visible.size() == 1) {
                select_clip(visible.front());
                ImGui::CloseCurrentPopup();
            }
            const float rows = float(std::clamp<size_t>(visible.size(), 1, 12));
            ImGui::BeginChild("clip_choices", {0, ImGui::GetTextLineHeightWithSpacing() * rows},
                              ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
            ImGuiListClipper list;
            list.Begin(int(visible.size()));
            while (list.Step())
                for (int row = list.DisplayStart; row < list.DisplayEnd; ++row) {
                    const int i = visible[size_t(row)];
                    const auto& c = clips[size_t(i)];
                    std::string tag = c.tag.str();
                    if (c.shell_specific) tag += std::string(" ") + ui::tr(ui::S::ShellClipLabel);
                    if (c.shared_bones) tag += std::string(" ") + ui::tr(ui::S::SharedClipLabel);
                    if (ImGui::Selectable(ui::trf(ui::S::ClipFramesItem, tag, c.frames, i).c_str(), a.clip == i,
                                          ImGuiSelectableFlags_NoAutoClosePopups)) {
                        select_clip(i);
                        ImGui::CloseCurrentPopup();
                    }
                }
            ImGui::EndChild();
            ImGui::EndCombo();
        }
        if (!a.error.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s", a.error.c_str());
            ImGui::PopTextWrapPos();
        }
        auto equipment = binding.equipment | std::views::filter([&](const auto& e) { return a.clip >= 0 && e.source_clip == a.loaded.tag; });
        if (!equipment.empty()) {
            const TagHash active = a.equipment_model.is_none() ? equipment.front().model : a.equipment_model;
            ImGui::SetNextItemWidth(-1);
            const auto label = std::format("{}  {}", ui::tr(ui::S::EquipmentCandidate), active.str());
            if (begin_combo("##equipment", label.c_str())) {
                for (const auto& option : equipment)
                    if (ImGui::Selectable(option.model.str().c_str(), active == option.model)) {
                        a.equipment_model = option.model;
                        a.dirty = true;
                    }
                ImGui::EndCombo();
            }
        }
        const bool selected = a.clip >= 0;
        const float last = float(std::max(1u, a.loaded.frame_count) - 1);
        ImGui::BeginDisabled(!selected);
        ImGui::SetNextItemWidth(-1);
        auto seek = [&](float target) {
            a.frame = std::clamp(target, 0.0f, last);
            a.playing = false;
            a.dirty = true;
        };
        if (ImGui::SliderFloat("##frame", &a.frame, 0, last, ui::tr(ui::S::FrameFmt), ImGuiSliderFlags_AlwaysClamp))
            seek(a.frame);
        ImGui::EndDisabled();

        const int clip_index = a.clip;
        if (ImGui::Button("<##prev_clip", {em * 2.1f, 0}))
            select_clip(clip_index > 0 ? clip_index - 1 : int(clips.size()) - 1);
        ImGui::SetItemTooltip("%s", ui::tr(ui::S::PrevClip));
        ImGui::SameLine();
        if (ImGui::Button(">##next_clip", {em * 2.1f, 0})) select_clip((clip_index + 1) % int(clips.size()));
        ImGui::SetItemTooltip("%s", ui::tr(ui::S::NextClip));
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected);
        if (ImGui::Button(a.playing ? ui::tr(ui::S::Pause) : ui::tr(ui::S::Play), {em * 5, 0})) a.playing = !a.playing;
        ImGui::SameLine();
        if (ImGui::Button("|<##rewind", {em * 2.1f, 0})) seek(0);
        ImGui::SetItemTooltip("%s", ui::tr(ui::S::Rewind));
        ImGui::SameLine();
        if (ImGui::Button("-1##frame", {em * 2.1f, 0})) seek(std::ceil(a.frame) - 1);
        ImGui::SameLine();
        if (ImGui::Button("+1##frame", {em * 2.1f, 0})) seek(std::floor(a.frame) + 1);
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##speed", &a.speed, 0.1f, 2.0f, ui::tr(ui::S::SpeedFmt), ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::TreeNode(ui::tr(ui::S::AnimationInfo))) {
            ImGui::TextDisabled("%s", ui::tr(binding.authored_clips ? ui::S::AuthoredMotionBank : ui::S::RigCompatibleClips));
            ImGui::TextDisabled(ui::tr(ui::S::SkeletonLine), binding.skeleton.component.str().c_str(), binding.skeleton.size(),
                                binding.rig.component.str().c_str());
            if (binding.inferred_binding) ImGui::TextDisabled("%s", ui::tr(ui::S::SkeletonEstimated));
            if (!binding.followers.empty()) ImGui::TextDisabled(ui::tr(ui::S::FollowerParts), binding.followers.size());
            if (binding.model != a.model) ImGui::TextDisabled(ui::tr(ui::S::SourcePart), binding.model.str().c_str());
            if (a.clip >= 0) {
                if (clips[size_t(a.clip)].shared_bones) ImGui::TextDisabled("%s", ui::tr(ui::S::SharedBodyClip));
                ImGui::TextDisabled("%s  %s%s", clip_codec_name(a.loaded.codec),
                    is_additive(binding.rig, a.loaded) ? ui::tr(ui::S::Additive) : "",
                    ui::trf(ui::S::Seconds, float(a.loaded.frame_count) / kClipFps).c_str());
            }
            ImGui::TreePop();
        }

    }

    void record(ID3D12GraphicsCommandList* cl) {
        if (!gb.lit) return;
        g_prof.gpu_mark(Prof::GStart);
        {
            const auto ps = g_prof.scope(Prof::Anim);
            update_animation(ImGui::GetIO().DeltaTime);
        }
        const float material_dt = material_playing ? ImGui::GetIO().DeltaTime * material_speed : 0;
        material_time += material_dt;
        scene.set_material_frame(material_time, material_dt);
        light.frame_delta_time = ImGui::GetIO().DeltaTime;
        light.time = material_time;
        light.delta_time = material_dt;
        if (map_scene && event_playing) event_player.step(scene, light.frame_delta_time);
        else if (event_playing) {
            event_playing = false;
            if (event_audio) event_audio->clear();
        }
        light.entity_preview = entity_preview_active && !map_scene;
        light.dof_edges_only = map_scene;  // flying retains an orbit radius unrelated to surface depth
        light.dof_focus = cam.distance;
        LightingParams frame_light = light;
        frame_light.auto_exposure = map_scene && light.auto_exposure;
        try {
            const bool game_view = game_ready && light.mode == ViewMode::Lit;
            auto prepare_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::Prepare);
            if (game_ready) {
                if (game_view) game_lighting.prepare_frame(gpu, gb, frame_light);
                else game_lighting.update_global_channels(light);
                scene.set_exposure(game_view ? game_lighting.exposure() : light.exposure);
                scene.set_global_channel_values(game_lighting.externs().channels);
                scene.set_global_channels(game_lighting.world_channels());
                scene.set_material_time_of_day(game_lighting.time_of_day(light));
            }
            const View camera = current_view();
            ViewMatrices vm = game_view ? game_lighting.temporal_view(camera, light) : make_view(camera);
            if (map_scene) scene.update_curtains(vm.eye, light.frame_delta_time);
            prepare_scope.reset();
            auto audio_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::Audio);
            if (ambient_audio) {
                if (map_scene)
                    ambient_audio->update({vm.eye.x, vm.eye.y, vm.eye.z}, {vm.right.x, vm.right.y, vm.right.z},
                                          light.frame_delta_time, ambient_audio_volume, ambient_audio_enabled);
                else
                    ambient_audio->update({}, {}, light.frame_delta_time, 0.0f, false);
            }
            if (entity_audio) {
                entity_audio->update(entity_cues, entity_stopped, entity_emitters, {vm.eye.x, vm.eye.y, vm.eye.z},
                                     {vm.right.x, vm.right.y, vm.right.z}, light.frame_delta_time, entity_audio_volume,
                                     entity_audio_enabled && map_scene);
                entity_cues.clear(), entity_stopped.clear();
            }
            if (event_audio && map_scene && event_playing) {
                event_cues.clear();
                for (const auto& cue : event_player.sounds()) event_cues.push_back({cue.event, cue.emitter});
                event_audio->update(event_cues, {}, event_player.emitters(), {vm.eye.x, vm.eye.y, vm.eye.z},
                                    {vm.right.x, vm.right.y, vm.right.z}, light.frame_delta_time,
                                    entity_audio_volume, entity_audio_enabled);
            }
            audio_scope.reset();
            SunShadow* shadow = nullptr;
            auto sun_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::SunShadow);
            if (shadows && light.mode == ViewMode::Lit && game_ready) {
                if (!sun_shadow.depth) sun_shadow.resize(gpu, 4096);
                sun_shadow.fit_view(game_lighting.sun_toward(light), vm.eye, cam.target, scene.center(), scene.radius(), map_scene);
                scene.render_sun_shadow(gpu, cl, sun_shadow, map_scene);
                shadow = &sun_shadow;
            }
            sun_scope.reset();
            g_prof.gpu_mark(Prof::GSunShadow);
            auto light_shadow_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::LightShadow);
            // Shadowing local lights: a couple of depth maps per frame for the lights on screen,
            // once the map is complete (a growing scene would discard them again).
            game_lighting.set_light_shadows(shadows);
            if (shadows && light.mode == ViewMode::Lit && game_ready && map_scene && !map.streaming && scene.loaded())
                for (SunShadow* s : game_lighting.light_shadows_to_render(gpu, cl, vm, scene.item_count(), 2)) scene.render_shadow(gpu, cl, *s);
            light_shadow_scope.reset();
            auto rain_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::Rain);
            if (map_scene && !map.streaming && scene.loaded() && (!rain.valid || rain.items != scene.item_count())) {
                if (!rain.mask) rain.resize(gpu, 4096);
                scene.render_rain_occlusion(gpu, cl, rain, scene.center(), scene.radius());
            }
            scene.set_rain_occlusion(map_scene && rain.valid ? &rain : nullptr);
            game_lighting.set_overhead_occlusion(map_scene && rain.valid ? &rain : nullptr);
            rain_scope.reset();
            g_prof.gpu_mark(Prof::GLightShadow);
            {
                const auto ps = g_prof.scope(Prof::GBuffer);
                scene.render_gbuffer(gpu, cl, gb, vm);
            }
            g_prof.gpu_mark(Prof::GGBuffer);
            auto lighting_scope = std::optional<Prof::Scope>(std::in_place, g_prof, Prof::Lighting);
            if (light.mode == ViewMode::Lit) {
                if (!game_ready) throw std::runtime_error("Original game lighting is unavailable; no substitute renderer");
                GameLighting::TransparentFn transparents;
                const bool fx = map_scene && fx_enabled && particles.loaded();
                if (scene.has_transparent() || fx) transparents = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
                    if (scene.has_transparent()) scene.render_transparent(gpu, c, gb, hdr, vm, game_lighting);
                    if (fx) particles.render(gpu, c, gb, hdr, vm, game_lighting.externs(), light.delta_time);
                };
                GameLighting::DistortionFn distortion;
                if (scene.has_visible_distortion()) distortion = [&](auto* c, uint32_t w, uint32_t h) {
                    scene.render_distortion(gpu, c, w, h, vm, game_lighting.externs());
                };
                game_lighting.render(gpu, cl, gb, vm, frame_light, transparents, shadow,
                                     scene.has_visible_light_grid() || (fx && particles.has_emitters_near(vm.eye)), distortion);
                if (g_frame_generation_proxy && !map.streaming && map_scene && scene.loaded() && frame_generation_rect_valid)
                    g_frame_generation.tag(cl, gb, camera, vm, frame_generation_x, frame_generation_y);
            } else lighting.render(gpu, cl, gb, vm, light);
            lighting_scope.reset();
            g_prof.gpu_mark(Prof::GLighting);
            render_error.clear();
        } catch (const std::exception& ex) {
            if (render_error != ex.what()) error(ui::trf(ui::S::ErrRenderStopped, ex.what()));
            render_error = ex.what();
            g_frame_generation.set_active(false);
            capture_requested = false;
            // This is an explicit error surface, never a replacement material or lit scene.
            Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            const float clear[4] = {0, 0, 0, 1};
            cl->ClearRenderTargetView(gb.lit_rtv, clear, 0, nullptr);
            Gpu::barrier(cl, gb.lit.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
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
            const char* kind = light.mode != ViewMode::Lit ? "debug" : "game";
            fs::path file = dir / std::format("{}_{}_{}.png", scene.loaded() ? scene.tag().str() : "empty", kind, stamp);
            write_png(file, gb.output_width(), gb.output_height(), px);
            info(ui::trf(ui::S::LogSaved, to_utf8(file.wstring())));
        } catch (const std::exception& ex) {
            error(ui::trf(ui::S::ErrSave, ex.what()));
        }
    }

    // ---- UI ----
    // Layout: a navigation rail on the left, a page header and the page body on the right, and a
    // status line along the bottom. Every page is one rail entry; the page shown is `tab`.
    void open_tab(int t) {
        if (tab != t && (tab == TabDialogue || tab == TabCodex)) stop_line_audio();
        if (tab != t && (tab == TabStatics || tab == TabEntities)) worker.request_thumbnails({});
        tab = Tab(t);
    }
    void set_ui_language(ui::Lang l) {
        ui::set_lang(l);
        g_prefs.lang = l;
        save_prefs();
        for (size_t i = 0; i < audio_langs.size() && i < audio_suffixes.size(); ++i)
            audio_langs[i].second = audio_language_name(audio_suffixes[i]);
    }

    void ui() {
        media_tick();
        viewport_visible = false;
        frame_generation_rect_valid = false;
        ImGuiViewport* vp = ImGui::GetMainViewport();
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const float side_w = (g_prefs.sidebar_open ? 220 : 52) * sc, title_h = 44 * sc, status_h = 28 * sc;
        const ImVec2 work = vp->WorkPos, wsize = vp->WorkSize;
        const ImGuiWindowFlags chrome = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNav;
        g_win.caption_h = int(title_h);
        g_win.caption_hot.clear();

        // sidebar (full height above the status line; its top is part of the title bar)
        ImGui::SetNextWindowPos(work);
        ImGui::SetNextWindowSize({side_w, wsize.y - status_h});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(theme::kRail));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("##side", nullptr, chrome | ImGuiWindowFlags_NoScrollbar)) sidebar(title_h);
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // title bar: page name and description, search, window buttons
        ImGui::SetNextWindowPos({work.x + side_w, work.y});
        ImGui::SetNextWindowSize({wsize.x - side_w, title_h});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(theme::kBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("##title", nullptr, chrome | ImGuiWindowFlags_NoScrollbar)) title_bar(title_h);
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::SetNextWindowPos({work.x + side_w, work.y + title_h});
        ImGui::SetNextWindowSize({wsize.x - side_w, wsize.y - title_h - status_h});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(theme::kBg));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("##content", nullptr, chrome | ImGuiWindowFlags_NoScrollbar)) content_frame(sc);
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        status_bar(work, wsize, status_h);
        if (!pm) install_picker();
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_L)))
            focus_prompt = true;
        if (ImGui::IsKeyDown(ImGuiMod_Ctrl) && ImGui::IsKeyPressed(ImGuiKey_B) && !ImGui::GetIO().WantTextInput) toggle_sidebar();
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_S) && gb.lit && scene.loaded() && !map.streaming)
            capture_requested = true;
    }

    void toggle_sidebar() {
        g_prefs.sidebar_open = !g_prefs.sidebar_open;
        save_prefs();
    }

    // A square icon button in the title bar. Returns true on click.
    bool title_icon(const char* id, uint32_t glyph, float x, float h, const char* tip) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const float s = 30 * sc;
        ImGui::SetCursorPos({x, (h - s) * 0.5f});
        const bool clicked = ImGui::InvisibleButton(id, {s, s});
        caption_hot();
        const bool hov = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin();
        auto* dl = ImGui::GetWindowDrawList();
        if (hov) dl->AddRectFilled(a, {a.x + s, a.y + s}, theme::kHover, 4 * sc);
        const std::string g = icon(glyph);
        const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
        dl->AddText({a.x + (s - gs.x) * 0.5f, a.y + (s - gs.y) * 0.5f}, hov ? theme::kText : theme::kMuted, g.c_str());
        if (hov && tip) ImGui::SetTooltip("%s", tip);
        return clicked;
    }

    // JA / EN / KO: the interface language, one click away. Returns the control's left edge.
    float lang_switch(float right_x, float h) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        auto* dl = ImGui::GetWindowDrawList();
        const char* labels[3] = {"JA", "EN", "KO"};
        const ui::Lang langs[3] = {ui::Lang::Ja, ui::Lang::En, ui::Lang::Ko};
        const float bw = 34 * sc, bh = 26 * sc, total = 3 * bw;
        const float x0 = right_x - total;
        const ImVec2 o = ImGui::GetWindowPos();
        const float y = (h - bh) * 0.5f;
        dl->AddRect({o.x + x0, o.y + y}, {o.x + x0 + total, o.y + y + bh}, theme::kBorder, 4 * sc);
        ImGui::PushFont(g_fonts.button, ImGui::GetFontSize() * 0.8f / sc);
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPos({x0 + i * bw, y});
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##lang", {bw, bh});
            caption_hot();
            const bool hov = ImGui::IsItemHovered();
            const bool sel = ui::lang() == langs[i];
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (sel) dl->AddRectFilled({a.x + 2 * sc, a.y + 2 * sc}, {b.x - 2 * sc, b.y - 2 * sc}, theme::kActive, 3 * sc);
            else if (hov) dl->AddRectFilled({a.x + 2 * sc, a.y + 2 * sc}, {b.x - 2 * sc, b.y - 2 * sc}, theme::kHover, 3 * sc);
            const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
            dl->AddText({a.x + (bw - ts.x) * 0.5f, a.y + (bh - ts.y) * 0.5f}, sel || hov ? theme::kText : theme::kMuted, labels[i]);
            if (hov) ImGui::SetTooltip("%s", ui::native_name(langs[i]));
            if (clicked && !sel) set_ui_language(langs[i]);
            ImGui::PopID();
        }
        ImGui::PopFont();
        return x0;
    }

    // Marks the last item as clickable inside the title bar (everything else there drags the window).
    void caption_hot() {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        g_win.caption_hot.push_back({LONG(a.x), LONG(a.y), LONG(b.x), LONG(b.y)});
    }

    void title_bar(float h) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 o = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddLine({o.x, o.y + h - 1}, {o.x + w, o.y + h - 1}, theme::kBorder);

        // window buttons on the right
        const float bw = 46 * sc;
        const float buttons_x = w - 3 * bw;
        struct Btn { uint32_t glyph; int id; };
        const Btn btns[3] = {{0xE921, 0}, {IsZoomed(g_win.hwnd) ? 0xE923u : 0xE922u, 1}, {0xE8BB, 2}};
        for (int i = 0; i < 3; ++i) {
            ImGui::SetCursorPos({buttons_x + i * bw, 0});
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##wb", {bw, h - 1});
            caption_hot();
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (hov) dl->AddRectFilled(a, b, i == 2 ? IM_COL32(196, 43, 28, 255) : theme::kHover);
            ImGui::PushFont(g_fonts.body, 11);
            const std::string g = icon(btns[i].glyph);
            const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
            dl->AddText({a.x + (bw - gs.x) * 0.5f, a.y + (h - gs.y) * 0.5f}, hov || IsWindowActive() ? theme::kText : theme::kMuted, g.c_str());
            ImGui::PopFont();
            ImGui::PopID();
            if (clicked) {
                if (i == 0) ShowWindow(g_win.hwnd, SW_MINIMIZE);
                else if (i == 1) ShowWindow(g_win.hwnd, IsZoomed(g_win.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                else PostMessageW(g_win.hwnd, WM_CLOSE, 0, 0);
            }
        }

        // language, then search
        const float lang_x = lang_switch(buttons_x - 12 * sc, h);
        const float search_w = std::min(320 * sc, std::max(140 * sc, lang_x * 0.4f));
        const float search_x = lang_x - search_w - 10 * sc;
        const float fh = ImGui::GetFrameHeight();
        ImGui::SetCursorPos({search_x, (h - fh) * 0.5f});
        search_box(search_w);

        // page name and its description
        const TabInfo& info = kTabs[tab];
        if (title_icon("##side", 0xE700, 8 * sc, h, ui::tr(g_prefs.sidebar_open ? ui::S::SidebarHide : ui::S::SidebarShow)))
            toggle_sidebar();
        const float x0 = 46 * sc, max_x = o.x + search_x - 16 * sc;
        ImGui::PushFont(g_fonts.button, 0);
        const float ty = o.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        const char* name = ui::tr(info.name);
        text_fit(dl, {o.x + x0, ty}, theme::kText, name, max_x);
        const float name_w = ImGui::CalcTextSize(name).x;
        ImGui::PopFont();
        (void)name_w;
    }

    static bool IsWindowActive() { return GetForegroundWindow() == g_win.hwnd; }

    // The page body. The 3D pages draw edge to edge; the rest sit inside the page margins.
    void content_frame(float sc) {
        const bool flush = tab == TabStatics || tab == TabEntities || tab == TabMaps;
        ImGui::SetCursorPos({0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, flush ? ImVec2{0, 0} : ImVec2{24 * sc, 20 * sc});
        ImGui::BeginChild("##page", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        switch (tab) {
        case TabHome: home_tab(); break;
        case TabFind: find_tab(); break;
        case TabSettings: settings_tab(); break;
        case TabStatics: statics_tab(false); break;
        case TabEntities: statics_tab(true); break;
        case TabMaps: maps_tab(); break;
        case TabText: strings_tab(); break;
        case TabDialogue: dialogue_tab(); break;
        case TabMovies: movies_tab(); break;
        case TabCodex: codex_tab(); break;
        case TabPackages: packages_tab(); break;
        case TabLog: log_tab(); break;
        default: break;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // Sidebar: the name, the pages as plain text in three groups, log and settings at the foot.
    void sidebar(float title_h) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 o = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddLine({o.x + w - 1, o.y}, {o.x + w - 1, o.y + h}, theme::kBorder);
        ImGui::PushFont(g_fonts.bold, 0);
        const char* brand = g_prefs.sidebar_open ? "nim" : "n";
        const float bx = g_prefs.sidebar_open ? 20 * sc : (w - ImGui::CalcTextSize(brand).x) * 0.5f;
        dl->AddText({o.x + bx, o.y + (title_h - ImGui::GetTextLineHeight()) * 0.5f}, theme::kText, brand);
        ImGui::PopFont();

        const float item = 30 * sc + 2 * sc;
        const float foot_h = 2 * item + 12 * sc;
        ImGui::SetCursorPos({0, title_h});
        ImGui::BeginChild("##nav", {w - 1, h - title_h - foot_h}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
        nav_item(TabMaps);
        nav_item(TabEntities);
        nav_item(TabStatics);
        nav_gap();
        nav_item(TabDialogue);
        nav_item(TabCodex);
        nav_item(TabMovies);
        nav_item(TabText);
        nav_gap();
        nav_item(TabPackages);
        ImGui::PopStyleVar();
        ImGui::EndChild();

        ImGui::SetCursorPos({0, h - foot_h + 4 * sc});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
        nav_item(TabLog);
        nav_item(TabSettings);
        ImGui::PopStyleVar();
    }

    // The search box: Enter finds the words in every page with a list and opens the results.
    void search_box(float width) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextItemWidth(width);
        if (focus_prompt) {
            ImGui::SetKeyboardFocusHere();
            focus_prompt = false;
        }
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col4(IM_COL32(17, 17, 19, 255)));
        const std::string hint = ui::tr(ui::S::PromptHint);
        const bool enter = ImGui::InputTextWithHint("##search", hint.c_str(), prompt, sizeof(prompt), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        caption_hot();
        if (enter) run_prompt();
    }

    void nav_item(Tab t) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const TabInfo& info = kTabs[t];
        const float h = 30 * sc, x = 8 * sc;
        const float w = ImGui::GetWindowWidth() - 2 * x;
        ImGui::SetCursorPosX(x);
        ImGui::PushID(int(t));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("nav", {w, h});
        const bool hovered = ImGui::IsItemHovered();
        if (clicked) open_tab(t);
        const bool sel = tab == t;
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 q{p.x + w, p.y + h};
        if (sel) dl->AddRectFilled(p, q, theme::kActive, 4 * sc);
        else if (hovered) dl->AddRectFilled(p, q, theme::kHover, 4 * sc);
        const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
        const ImU32 col = sel || hovered ? theme::kText : theme::kMuted;
        if (g_prefs.sidebar_open) {
            text_fit(dl, {p.x + 12 * sc, ty}, col, ui::tr(info.name), q.x - 8 * sc);
        } else {  // folded: the page's icon, its name as the tooltip
            const std::string g = icon(info.icon);
            const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
            dl->AddText({p.x + (w - gs.x) * 0.5f, ty}, col, g.c_str());
            if (hovered) ImGui::SetTooltip("%s", ui::tr(info.name));
        }
        ImGui::PopID();
    }

    void nav_gap() {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::Dummy({0, 10 * sc});
    }

    // Runs the search box: a page name opens that page, anything else is found everywhere.
    void run_prompt() {
        std::string q = prompt;
        while (!q.empty() && q.back() == ' ') q.pop_back();
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        if (q.empty()) return;
        for (int t = 0; t < TabCount; ++t)
            if (q == ui::tr(kTabs[t].name) && t != TabFind) {
                prompt[0] = 0;
                open_tab(t);
                return;
            }
        find(q);
        open_tab(TabFind);
    }

    // Finds `q` in every page that has a list: places, voices, codex, films and text.
    void find(const std::string& q) {
        find_query = q;
        find_results.clear();
        const std::string ql = lower_ascii(q);
        auto group = [&](Tab t) -> FindGroup& { return find_results.emplace_back(FindGroup{t}); };
        auto hit = [](FindGroup& g, std::string sample) {
            if (g.samples.size() < 6) g.samples.push_back(std::move(sample));
            ++g.count;
        };
        {
            auto& g = group(TabMaps);
            for (const auto& m : map_worker.maps())
                if (contains_ci(m.name, ql) || contains_ci(m.package, ql) || contains_ci(m.bubble.str(), ql))
                    hit(g, m.name.empty() ? m.package : m.name);
        }
        if (!catalog || !catalog->strings_ready) return;
        const auto& strings = catalog->strings;
        const size_t lang = size_t(language);
        auto text_of = [&](uint32_t h, TagHash container = {}) -> std::string {
            const auto* s = strings.find(h, container);
            if (!s) return {};
            return s->text[lang].empty() ? s->text[0] : s->text[lang];
        };
        auto matches = [&](uint32_t h, TagHash container = {}) {
            const auto* s = strings.find(h, container);
            return s && (contains_ci(s->text[0], ql) || s->text[lang].find(q) != std::string::npos);
        };
        if (catalog->dialogue_ready) {
            auto& g = group(TabDialogue);
            for (const auto& d : catalog->dialogue)
                if (matches(d.subtitle_hash, d.subtitle_container) || matches(d.speaker_hash))
                    hit(g, text_of(d.speaker_hash) + ": " + text_of(d.subtitle_hash, d.subtitle_container));
        }
        if (catalog->codex_ready) {
            auto& g = group(TabCodex);
            for (const auto& c : catalog->codex.collections)
                for (const auto& e : c.entries)
                    if (matches(e.title.hash, e.title.container) || matches(e.body.hash, e.body.container))
                        hit(g, text_of(c.title.hash, c.title.container) + " / " + text_of(e.title.hash, e.title.container));
        }
        if (catalog->movies_ready) {
            auto& g = group(TabMovies);
            for (const auto& m : catalog->movies)
                if (contains_ci(m.usm.str(), ql)) hit(g, m.usm.str());
        }
        {
            auto& g = group(TabText);
            for (const auto& s : strings.strings())
                if (contains_ci(s.text[0], ql) || s.text[lang].find(q) != std::string::npos)
                    hit(g, s.text[lang].empty() ? s.text[0] : s.text[lang]);
        }
    }

    // Opens a page with its own filter set to the query.
    void open_filtered(Tab t, const std::string& q) {
        auto put = [&](char* dst, size_t n) {
            const size_t k = std::min(n - 1, q.size());
            std::memcpy(dst, q.data(), k);
            dst[k] = 0;
        };
        switch (t) {
        case TabMaps: put(map.filter, sizeof(map.filter)); break;
        case TabDialogue: put(dialogue_filter, sizeof(dialogue_filter)); break;
        case TabCodex: put(codex.filter, sizeof(codex.filter)); break;
        case TabMovies: put(movie.filter, sizeof(movie.filter)); break;
        case TabText: put(string_filter, sizeof(string_filter)); break;
        default: break;
        }
        open_tab(t);
    }

    // -- FIND --
    void find_tab() {
        if (find_query.empty()) {
            ImGui::TextDisabled("%s", ui::tr(ui::S::FindEmpty));
            return;
        }
        heading(("\"" + find_query + "\"").c_str(), 26);
        ImGui::Spacing();
        bool any = false;
        for (const auto& g : find_results) {
            if (!g.count) continue;
            any = true;
            ImGui::PushID(int(g.tab));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "%s", ui::tr(kTabs[g.tab].name));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", ui::trf(ui::S::FindHits, g.count).c_str());
            ImGui::SameLine();
            if (dbutton(ui::tr(ui::S::FindOpen), 0, true)) open_filtered(g.tab, find_query);
            for (const auto& line : g.samples) {
                ImGui::TextDisabled("  ");
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(line.c_str());
            }
            if (g.count > g.samples.size()) ImGui::TextDisabled("  ...");
            ImGui::Spacing();
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("%s", ui::trf(ui::S::FindNone, find_query).c_str());
    }

    // The bottom line: the current status (the catalogue or the last message), the version and GPU.
    void status_bar(ImVec2 work, ImVec2 size, float h) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowPos({work.x, work.y + size.y - h});
        ImGui::SetNextWindowSize({size.x, h});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(theme::kRail));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        if (ImGui::Begin("##status", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav)) {
            const ImVec2 o = ImGui::GetWindowPos();
            const float w = ImGui::GetWindowWidth();
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddLine(o, {o.x + w, o.y}, theme::kBorder);
            std::string left;
            if (catalog) {
                std::lock_guard lk(catalog->mutex);
                left = catalog->status;
            }
            if (left.empty() && !log.empty()) left = log.back().text;
            if (left.empty()) left = ui::tr(ui::S::LogIdle);
            const bool last_error = !log.empty() && log.back().error && left == log.back().text;
            ImGui::PushFont(g_fonts.body, ImGui::GetFontSize() * 0.85f / sc);
            const float ty = o.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
            const std::string right = std::format("nim {}  \xC2\xB7  {}", kVersion, gpu.adapter_name());
            const float rw = ImGui::CalcTextSize(right.c_str()).x;
            dl->AddText({o.x + w - rw - 14 * sc, ty}, theme::kMuted, right.c_str());
            const float lx = o.x + 14 * sc;
            text_fit(dl, {lx, ty}, last_error ? theme::kError : theme::kMuted, left.c_str(), o.x + w - rw - 40 * sc);
            ImGui::PopFont();
            ImGui::SetCursorPos({0, 0});
            if (ImGui::InvisibleButton("##st", {std::max(1.0f, w - rw - 40 * sc), h})) open_tab(TabLog);
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("%s", left.c_str());
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void install_picker() {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24 * sc, 22 * sc});
        ImGui::Begin(ui::tr(ui::S::NoInstallTitle), nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking);
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::TextUnformatted(ui::tr(ui::S::NoInstallTitle));
        ImGui::PopFont();
        ImGui::Dummy({0, 6 * sc});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 460 * sc);
        ImGui::TextDisabled("%s", ui::tr(ui::S::NoInstallLine1));
        ImGui::TextDisabled("%s", ui::tr(ui::S::NoInstallLine2));
        ImGui::PopTextWrapPos();
        ImGui::Dummy({0, 10 * sc});
        if (dbutton(ui::tr(ui::S::SelectFolder), 240 * sc))
            if (auto p = pick_folder(g_win.hwnd)) open_install(*p);
        ImGui::End();
        ImGui::PopStyleVar();
    }

    // -- HOME --
    // Not in the sidebar (the app opens on the maps); kept for the install line.
    void home_tab() {
        ImGui::TextDisabled("%s", ui::tr(ui::S::HomeInstall));
        text_line_fit(pm ? to_utf8(install.wstring()).c_str() : ui::tr(ui::S::NoInstallSelected));
        if (dbutton(ui::tr(pm ? ui::S::HomeChange : ui::S::SelectFolder), 0, pm != nullptr))
            if (auto f = pick_folder(g_win.hwnd)) open_install(*f);
    }

    // -- SETTINGS --
    void settings_tab() {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const float col = std::min(ImGui::GetContentRegionAvail().x, 640 * sc);
        ImGui::PushItemWidth(std::min(360 * sc, col));
        ImGui::SeparatorText(ui::tr(ui::S::SettingsInstall));
        ImGui::TextUnformatted(pm ? to_utf8(install.wstring()).c_str() : ui::tr(ui::S::SettingsNoInstall));
        if (ImGui::Button(ui::tr(ui::S::SettingsSelectFolder)))
            if (auto p = pick_folder(g_win.hwnd)) open_install(*p);

        ImGui::SeparatorText(ui::tr(ui::S::SettingsLanguage));
        if (begin_combo(ui::tr(ui::S::SettingsUiLanguage), ui::native_name(ui::lang()))) {
            for (uint8_t i = 0; i < uint8_t(ui::Lang::Count); ++i) {
                const ui::Lang l = ui::Lang(i);
                if (ImGui::Selectable(ui::native_name(l), ui::lang() == l)) set_ui_language(l);
            }
            ImGui::EndCombo();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + col);
        ImGui::TextDisabled("%s", ui::tr(ui::S::SettingsLanguageNote));
        ImGui::PopTextWrapPos();
        if (begin_combo(ui::tr(ui::S::SettingsSecondLang), std::string(language_code(TextLanguage(language))).c_str())) {
            for (int l = 1; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(std::string(language_code(TextLanguage(l))).c_str(), language == l)) language = l;
            ImGui::EndCombo();
        }

        ImGui::SeparatorText(ui::tr(ui::S::SettingsDisplay));
        ImGui::TextUnformatted(ui::tr(ui::S::SettingsShaders));
        ImGui::Checkbox(ui::tr(ui::S::SettingsThumbnails), &thumbnails_enabled);
        ImGui::Checkbox(ui::tr(ui::S::SettingsShowFps), &show_fps);

        ImGui::SeparatorText(ui::tr(ui::S::SettingsInfo));
        ImGui::Text("nim %s", kVersion);
        if (ImGui::Checkbox(ui::tr(ui::S::SettingsAutoUpdate), &g_prefs.auto_update)) {
            save_prefs();
            if (g_prefs.auto_update && !start_update(true)) error(ui::tr(ui::S::UpdateUnavailable));
        }
        if (ImGui::Button(ui::tr(ui::S::CheckUpdates)) && !start_update(false)) error(ui::tr(ui::S::UpdateUnavailable));
        static std::string update_state;
        static std::chrono::steady_clock::time_point next_update_poll;
        if (const auto now = std::chrono::steady_clock::now(); now >= next_update_poll) {
            std::ifstream status(exe_dir() / "nim-update-status.txt");
            update_state.clear();
            std::getline(status, update_state);
            next_update_poll = now + std::chrono::milliseconds(500);
        }
        if (update_state == "auth") ImGui::TextWrapped("%s", ui::tr(ui::S::UpdateAuth));
        else if (update_state.starts_with("ready ")) {
            ImGui::TextWrapped("%s (v%s)", ui::tr(ui::S::UpdateReady), update_state.c_str() + 6);
            if (ImGui::Button(ui::tr(ui::S::UpdateRestart))) {
                std::ofstream marker(exe_dir() / ".nim-update-relaunch", std::ios::trunc);
                marker << "1\n";
                marker.close();
                PostQuitMessage(0);
            }
        }
        else if (update_state == "current") ImGui::TextDisabled("%s", ui::tr(ui::S::UpdateCurrent));
        else if (update_state.starts_with("error ") || update_state == "running")
            ImGui::TextDisabled("%s (%s)", ui::tr(ui::S::UpdateUnavailable), update_state.c_str());
        else if (update_state == "checking" || update_state.starts_with("downloading "))
            ImGui::TextDisabled("%s", ui::tr(ui::S::UpdateChecking));
        ImGui::Text("%s", ui::trf(ui::S::SettingsGpu, gpu.adapter_name()).c_str());
        ImGui::Text("%s", ui::trf(ui::S::SettingsVram, format_bytes(gpu.vram_usage()), format_bytes(gpu.vram_budget())).c_str());
        ImGui::Text("%s", ui::trf(ui::S::SettingsFonts, ui::tr(g_fonts.from_game ? ui::S::FontsGame : ui::S::FontsSystem)).c_str());
        ImGui::Text("%s", ui::trf(ui::S::SettingsThumbs, thumbs.size()).c_str());
        ImGui::PopItemWidth();
    }

    // -- STATICS --
    void sort_packages(Browser& b, const std::vector<PackageGroup>& pk) {
        b.order.resize(pk.size());
        for (size_t i = 0; i < pk.size(); ++i) b.order[i] = int(i);
        std::sort(b.order.begin(), b.order.end(), [&](int x, int y) {
            if (b.sort_mode == 1) return pk[x].name < pk[y].name;
            if (b.sort_mode == 2) return pk[x].meshes.size() > pk[y].meshes.size();
            return pk[x].pkg_id < pk[y].pkg_id;
        });
        b.order_sort = b.sort_mode;
    }

    // STATICS (static meshes) and ENTITIES (dynamic models) share the browser layout.
    void statics_tab(bool dynamic) {
        if (!catalog || !catalog->meshes_ready) {
            ImGui::SetCursorPos({20, 20});
            ImGui::TextDisabled(ui::tr(ui::S::MeshesListing));
            return;
        }
        Browser& B = browsers[dynamic ? 1 : 0];
        auto& static_filter = B.filter;
        auto& sort_mode = B.sort_mode;
        auto& current_pkg = B.current_pkg;
        auto& pkg_order = B.order;
        const auto& pk = dynamic ? catalog->dyn_packages : catalog->packages;
        const auto& meshes = dynamic ? catalog->dynamics : catalog->meshes;
        if (B.order_sort != sort_mode || pkg_order.size() != pk.size()) sort_packages(B, pk);
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
        ImGui::InputTextWithHint("##sfilter", (icon(ico::Search) + ui::tr(ui::S::StaticFilterHint)).c_str(), static_filter,
                                 sizeof(static_filter));
        ImGui::SetCursorPosX(10 * sc);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui::tr(ui::S::SortLabel));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(left_w - 20 * sc - ImGui::GetCursorPosX() + 10 * sc);
        const char* sorts[] = {"ID", ui::tr(ui::S::SortName), ui::tr(ui::S::SortCount)};
        combo("##sort", &sort_mode, sorts, 3);
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
                                                  ImGui::IsItemActive() ? theme::kRing : theme::kBorder);

        // right: live view + details
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("scene", {right_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (details_h <= 0) details_h = 310 * sc;
        const float view_h_px = std::max(120 * sc, ImGui::GetContentRegionAvail().y - details_h - 6 * sc);
        viewport_ui({ImGui::GetContentRegionAvail().x, view_h_px});
        ImGui::InvisibleButton("##hsplit", {ImGui::GetContentRegionAvail().x, 6 * sc});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) details_h = std::max(40 * sc, details_h - ImGui::GetIO().MouseDelta.y);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemActive() ? theme::kRing : theme::kBorder);
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
        } else if (it != thumbs.end() && it->second.no_drawable) {
            const char* t = ui::tr(ui::S::NoPath);
            ImVec2 ts = ImGui::CalcTextSize(t);
            dl->AddText({p.x + (card - ts.x) * 0.5f, p.y + (card - ts.y) * 0.5f}, IM_COL32(170, 170, 170, 255), t);
            if (hovered) ImGui::SetTooltip("%s", it->second.error.c_str());
        } else if (thumbnails_enabled) {
            spinner(dl, {p.x + card * 0.5f, p.y + card * 0.5f}, 14 * sc, IM_COL32(255, 255, 255, 90));
        }
        dl->AddRectFilled({p.x, q.y}, {q.x, q.y + footer}, IM_COL32_BLACK);
        std::string label = m.tag.str();
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText({p.x + 8 * sc, q.y + (footer - ts.y) * 0.5f}, IM_COL32_WHITE, label.c_str());
        const bool selected = (scene.loaded() && scene.tag() == m.tag) || (no_drawable_tag && *no_drawable_tag == m.tag);
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
        view_w = uint32_t(std::max(16.0f, size.x));
        view_h = uint32_t(std::max(16.0f, size.y));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        if (map_scene && map.streaming) {
            ImGui::Dummy(size);
            dl->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, IM_COL32_BLACK);
            const ImVec2 center{origin.x + size.x * 0.5f, origin.y + size.y * 0.5f};
            spinner(dl, {center.x, center.y - 14 * sc}, 14 * sc, kGreen);
            const auto progress = map_worker.progress();
            const size_t done = std::min(progress.total, map.processed + progress.failed);
            const std::string label = progress.total ? std::format("{}/{}", done, progress.total) : map.name;
            const auto width = ImGui::CalcTextSize(label.c_str()).x;
            hud_text(dl, {center.x - width * 0.5f, center.y + 13 * sc}, IM_COL32_WHITE, label.c_str());
            return;
        }
        viewport_visible = true;
        if (gb.lit) {
            ImGui::Image(ImTextureID(gpu.srv().gpu(gb.srv_first + 5).ptr), size);
            const auto display = ImGui::GetIO().DisplaySize;
            frame_generation_rect_valid = gb.output_width() == view_w && gb.output_height() == view_h &&
                origin.x >= 0 && origin.y >= 0 &&
                origin.x + float(view_w) <= display.x && origin.y + float(view_h) <= display.y;
            if (frame_generation_rect_valid) {
                frame_generation_x = uint32_t(origin.x);
                frame_generation_y = uint32_t(origin.y);
            }
        } else {
            ImGui::Dummy(size);
        }
        const ImVec2 end = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(origin);
        // The toolbar drawn on top must win hover and clicks over this catch-all.
        ImGui::SetNextItemAllowOverlap();
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
        ImGui::SetItemTooltip(ui::tr(ui::S::LightExposure));
        if (ImGui::BeginPopup("tune")) {
            ImGui::SliderFloat(ui::tr(ui::S::Exposure), &light.exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderAngle(ui::tr(ui::S::SunAzimuth), &light.sun_azimuth, -180, 180);
            ImGui::SliderAngle(ui::tr(ui::S::SunElevation), &light.sun_elevation, -10, 90);
            ImGui::SliderFloat(ui::tr(ui::S::SunIntensity), &light.sun_intensity, 0, 10);
            ImGui::Checkbox(ui::tr(ui::S::SunShadows), &shadows);
            ImGui::SetItemTooltip(ui::tr(ui::S::SunShadowsTip));
            ImGui::Checkbox("SSAO", &light.ssao);
            ImGui::SetItemTooltip(ui::tr(ui::S::SSAOTip));
            ImGui::Checkbox(ui::tr(ui::S::SSR), &light.ssr);
            ImGui::SetItemTooltip(ui::tr(ui::S::SSRTip));
            ImGui::Checkbox(ui::tr(ui::S::LightShafts), &light.volumetrics);
            ImGui::SetItemTooltip(ui::tr(ui::S::LightShaftsTip));
            if (light.volumetrics)
                ImGui::SliderFloat(ui::tr(ui::S::LightShaftStrength), &light.near_shaft_strength, 0.0f, 1.0f, "%.2f");
            if (map_scene && ImGui::Checkbox("FX (パーティクル)", &fx_enabled) && map.lights_set && map_lights_fx != fx_enabled) {
                gpu.wait_idle();  // the lights' pipelines and shadow maps are in use by frames in flight
                apply_map_lights(gpu);
            }
            ImGui::Checkbox(ui::tr(ui::S::PostProcess), &light.post);
            ImGui::SetItemTooltip(ui::tr(ui::S::PostProcessTip));
            ImGui::BeginDisabled(!light.post);
            if (map_scene) ImGui::Checkbox(ui::tr(ui::S::AutoExposure), &light.auto_exposure);
            ImGui::SetItemTooltip(ui::tr(ui::S::AutoExposureTip));
            ImGui::SameLine();
            ImGui::Checkbox(ui::tr(ui::S::Bloom), &light.bloom);
            ImGui::SetItemTooltip(ui::tr(ui::S::BloomTip));
            ImGui::SliderFloat(ui::tr(ui::S::BloomHighlight), &light.bloom_highlight, 0.0f, 6.0f, "%.1f");
            ImGui::SetItemTooltip(ui::tr(ui::S::BloomHighlightTip));
            ImGui::SliderFloat(ui::tr(ui::S::EmissiveGain), &light.emissive_gain, 0.0f, 3.0f, "%.2f");
            if (map_scene && fx_enabled)
                ImGui::SliderFloat(ui::tr(ui::S::ParticleStrength), &particles.draw_strength, 0.0f, 1.0f, "%.2f");
            if (g_frame_generation.built()) {
                if (ImGui::Checkbox("NVIDIA DLSS Frame Generation (2x)", &g_frame_generation_requested) &&
                    g_frame_generation_requested && !g_frame_generation.initialize_device(gpu.device()))
                    g_frame_generation_requested = false;
                if (!g_frame_generation.status().empty())
                    ImGui::TextDisabled("%s", g_frame_generation.status().c_str());
                if (g_frame_generation_proxy)
                    ImGui::TextDisabled("Presents per rendered frame: %u", g_frame_generation.presents_per_frame());
            }
            ImGui::Checkbox("TAA", &light.temporal_aa);
            ImGui::SetItemTooltip(ui::tr(ui::S::TAATip));
            ImGui::SameLine();
            ImGui::Checkbox("FXAA", &light.antialias);
            ImGui::Checkbox("被写界深度 (DoF, estimated)", &light.depth_of_field);
            if (Dlss::built()) {
                ImGui::BeginDisabled(!light.temporal_aa);
                static constexpr const char* kDlssLabels[] = {
                    "Off", "DLAA (Native)", "Quality (67%)", "Balanced (58%)", "Performance (50%)", "Ultra Performance (33%)"
                };
                int mode_idx = int(light.dlss_mode != DlssMode::Off ? light.dlss_mode : (light.dlaa ? DlssMode::DLAA : DlssMode::Off));
                if (ImGui::Combo("NVIDIA DLSS", &mode_idx, kDlssLabels, int(std::size(kDlssLabels)))) {
                    light.dlss_mode = DlssMode(mode_idx);
                    light.dlaa = (light.dlss_mode == DlssMode::DLAA);
                }
                ImGui::EndDisabled();
                if (light.temporal_aa && (light.dlss_mode != DlssMode::Off || light.dlaa) && !game_lighting.dlaa_status().empty())
                    ImGui::TextDisabled("%s", game_lighting.dlaa_status().c_str());
            }
            ImGui::SliderFloat(ui::tr(ui::S::ChromaticAberration), &light.chromatic_aberration, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat(ui::tr(ui::S::OutputGamma), &light.output_gamma, 0.5f, 2.0f, "%.2f");
            ImGui::SliderFloat(ui::tr(ui::S::OutputSharpen), &light.output_sharpen, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat(ui::tr(ui::S::OutputGrain), &light.output_grain, 0.0f, 0.1f, "%.3f");
            ImGui::SameLine();
            ImGui::Checkbox("周辺減光", &light.vignette);
            ImGui::SetItemTooltip("ゲームの v2_dof_cinematic パス。焦点はカメラの注視点 (焦点範囲・ぼけの大きさは推定)");
            ImGui::EndDisabled();
            if (map_scene && light.post && light.auto_exposure && game_ready)
                ImGui::TextDisabled("%s %.3f", ui::tr(ui::S::Exposure), game_lighting.exposure());
            ImGui::ColorEdit3(ui::tr(ui::S::SunColor), &light.sun_color.x);
            ImGui::SliderFloat(ui::tr(ui::S::SkyIntensity), &light.sky_intensity, 0, 4);
            ImGui::ColorEdit3(ui::tr(ui::S::SkyZenith), &light.sky_top.x);
            ImGui::ColorEdit3(ui::tr(ui::S::SkyHorizon), &light.sky_horizon.x);
            ImGui::ColorEdit3(ui::tr(ui::S::GroundColor), &light.ground.x);
            ImGui::ColorEdit3(ui::tr(ui::S::BackgroundTop), &light.background_top.x);
            ImGui::ColorEdit3(ui::tr(ui::S::BackgroundBottom), &light.background_bottom.x);
            ImGui::SeparatorText(ui::tr(ui::S::AtmosphereSection));
            ImGui::BeginDisabled(!game_lighting.has_atmosphere());
            ImGui::Checkbox(ui::tr(ui::S::Atmosphere), &light.atmosphere);
            ImGui::SetItemTooltip(ui::tr(ui::S::AtmosphereTip));
            ImGui::Checkbox(ui::tr(ui::S::FroxelFog), &light.froxel_fog);
            ImGui::SetItemTooltip(ui::tr(ui::S::FroxelFogTip));
            ImGui::Checkbox(ui::tr(ui::S::InteriorFog), &light.authored_interior_fog);
            ImGui::SliderFloat(ui::tr(ui::S::FogDensityScale), &light.fog_density_scale, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox(ui::trf(ui::S::MapEnvironment, game_lighting.environment_channels()).c_str(), &light.environment);
            ImGui::SetItemTooltip(ui::tr(ui::S::MapEnvironmentTip));
            ImGui::Checkbox(ui::tr(ui::S::SkyAmbient), &light.sky_ambient);
            ImGui::SetItemTooltip(ui::tr(ui::S::SkyAmbientTip));
            ImGui::Checkbox(ui::tr(ui::S::OverheadOcclusion), &light.overhead_occlusion);
            ImGui::SetItemTooltip(ui::tr(ui::S::OverheadOcclusionTip));
            ImGui::Checkbox(ui::tr(ui::S::ProbeFade), &light.probe_distance_fade);
            if (map_scene && ambient_audio && ambient_audio->emitter_count() > 0) {
                ImGui::SeparatorText("環境音");
                ImGui::Checkbox("環境音を再生", &ambient_audio_enabled);
                ImGui::BeginDisabled(!ambient_audio_enabled);
                ImGui::SliderFloat("環境音量", &ambient_audio_volume, 0.0f, 1.5f, "%.2f");
                ImGui::EndDisabled();
                ImGui::TextDisabled("再生中: %zu ボイス / %zu 音源", ambient_audio->active_voices(), ambient_audio->emitter_count());
            }
            if (map_scene && entity_audio && ai.sim) {
                ImGui::SeparatorText("敵・船の音");
                ImGui::Checkbox("敵・船の音を再生", &entity_audio_enabled);
                ImGui::SetItemTooltip("AI の兵士・メカ・降下船が鳴らす音 (アニメーションのイベントとコンポーネントのループ)");
                ImGui::BeginDisabled(!entity_audio_enabled);
                ImGui::SliderFloat("敵・船の音量", &entity_audio_volume, 0.0f, 1.5f, "%.2f");
                ImGui::EndDisabled();
                ImGui::TextDisabled("再生中: %zu ボイス / %zu イベント読込済", entity_audio->active_voices(), entity_audio->events_loaded());
            }
            ImGui::SetItemTooltip(ui::tr(ui::S::ProbeFadeTip));
            {
                bool own = light.time_of_day < 0;
                if (ImGui::Checkbox(ui::tr(ui::S::TimeOwn), &own)) light.time_of_day = own ? -1.0f : game_lighting.default_time_of_day();
                ImGui::SetItemTooltip(ui::tr(ui::S::TimeOwnTip));
                ImGui::BeginDisabled(own);
                float t = own ? game_lighting.default_time_of_day() : light.time_of_day;
                const int minutes = int(t * 24 * 60 + 0.5f);
                if (ImGui::SliderFloat(ui::tr(ui::S::Time), &t, 0.0f, 1.0f, std::format("{:02}:{:02}", minutes / 60 % 24, minutes % 60).c_str()))
                    light.time_of_day = t;
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
            if (ImGui::Button(ui::tr(ui::S::ResetDefault))) {
                ViewMode m = light.mode;
                light = LightingParams{};
                light.mode = m;
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 4 * sc);
        ImGui::SetNextItemWidth(170 * sc);
        if (begin_combo("##mode", view_mode_name(light.mode))) {
            for (uint32_t m = 0; m < uint32_t(ViewMode::Count); ++m)
                if (ImGui::Selectable(view_mode_name(ViewMode(m)), light.mode == ViewMode(m))) light.mode = ViewMode(m);
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, 4 * sc);
        ImGui::SetNextItemWidth(220 * sc);
        ImGui::TextUnformatted("GAME SHADING / STRICT");
        if (map_scene && !map.activity.is_none()) {
            ImGui::SameLine(0, 4 * sc);
            if (ai.enabled) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 120, 255));
            if (ImGui::Button(icon(ico::People).c_str())) ImGui::OpenPopup("ai");
            if (ai.enabled) ImGui::PopStyleColor();
            ImGui::SetItemTooltip(ui::tr(ui::S::AiTip));
            if (ImGui::BeginPopup("ai")) {
                ImGui::Checkbox(ui::tr(ui::S::AiEnable), &ai.enabled);
                ImGui::SetItemTooltip(ui::tr(ui::S::AiEnableTip));
                ImGui::BeginDisabled(!ai.enabled);
                ImGui::Checkbox(ui::tr(ui::S::AiAwareness), &ai.awareness);
                ImGui::Checkbox(ui::tr(ui::S::AiMarkers), &ai.markers);
                ImGui::Checkbox(ui::tr(ui::S::AiDebug), &ai.debug);
                ImGui::Checkbox(ui::tr(ui::S::AiVisible), &ai.visible);
                ImGui::SetItemTooltip(ui::tr(ui::S::AiVisibleTip));
                ImGui::SetNextItemWidth(180 * sc);
                ImGui::SliderFloat(ui::tr(ui::S::AiRange), &ai.range, 10, 200, "%.0f m");
                ImGui::EndDisabled();
                if (ai.pending.valid()) ImGui::TextDisabled("%s", ui::tr(ui::S::AiLoading));
                ImGui::EndPopup();
            }
        }
        ImGui::SameLine(0, 4 * sc);
        if (ImGui::Button(icon(ico::Frame).c_str())) frame_scene();
        ImGui::SetItemTooltip(ui::tr(ui::S::FrameAll));
        ImGui::SameLine(0, 4 * sc);
        if (ImGui::Button(icon(ico::Save).c_str()) && scene.loaded()) capture_requested = true;
        ImGui::SetItemTooltip(ui::tr(ui::S::SavePng));
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
        if (!render_error.empty()) {
            hud_text(dl, {origin.x + 12 * sc, origin.y + bar_h + 12 * sc}, IM_COL32(255, 100, 100, 255),
                     ui::tr(ui::S::RenderStopped));
            ImGui::TextWrapped("%s", render_error.c_str());
        }
        // bottom-left HUD / messages
        if (scene.loaded() && map_scene) draw_ai_overlay(dl, origin, size, sc);
        if (scene.loaded() && map_scene) {
            const ViewMatrices vm = make_view(current_view());
            std::string hud = std::format("{}   {} meshes   {} instances   ({:.0f}, {:.0f}, {:.0f})   GAME SHADING", map.name,
                                          scene.mesh_count(), scene.instance_count(), vm.eye.x, vm.eye.y, vm.eye.z);
            hud_text(dl, {origin.x + 10 * sc, origin.y + size.y - ImGui::GetTextLineHeight() - 8 * sc},
                     IM_COL32(235, 235, 235, 230), hud.c_str());
        } else if (scene.loaded()) {
            std::string hud = std::format("{}   LOD {}   r {:.2f}   {} groups{}", scene.tag().str(), scene.lod(),
                                          scene.radius(), scene.groups().size(),
                                          light.mode == ViewMode::Lit
                                              ? "   GAME SHADING"
                                              : "");
            hud_text(dl, {origin.x + 10 * sc, origin.y + size.y - ImGui::GetTextLineHeight() - 8 * sc},
                     IM_COL32(235, 235, 235, 230), hud.c_str());
        } else if (!loading) {
            const char* msg = pm ? ui::tr(ui::S::ClickCardHint) : ui::tr(ui::S::OpenInstallHint);
            ImVec2 ts = ImGui::CalcTextSize(msg);
            hud_text(dl, {origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f}, IM_COL32(200, 200, 200, 200), msg);
        }
        if (loading) {
            spinner(dl, {origin.x + size.x * 0.5f, origin.y + size.y * 0.5f}, 20 * sc, kGreen);
            std::string t = ui::trf(ui::S::LoadingItem, loading->str());
            ImVec2 ts = ImGui::CalcTextSize(t.c_str());
            hud_text(dl, {origin.x + (size.x - ts.x) * 0.5f, origin.y + size.y * 0.5f + 32 * sc}, IM_COL32_WHITE, t.c_str());
        }
        ImGui::SetCursorScreenPos(end);
    }

    void frame_scene() {
        if (!scene.loaded()) return;
        if (map_scene && map.start) {
            // Standing at the player start, looking along its facing (eye height: estimate).
            const auto f = map.start->forward();
            const V3 eye{map.start->position[0], map.start->position[1], map.start->position[2] + kStartEyeHeight};
            cam.radius = std::max(10.0f, map.typical);
            cam.distance = cam.radius * 0.02f;
            cam.yaw = std::atan2(-f[1], -f[0]);
            cam.pitch = 0;
            const V3 dir{std::cos(cam.yaw), std::sin(cam.yaw), 0};
            cam.target = eye - dir * cam.distance;
            cam.fly_speed = 8;
            return;
        }
        if (map_scene && map.typical > 0) {
            // A map without starts: around its geometry, not the far-off outliers (skyboxes).
            cam.target = V3{map.center[0], map.center[1], map.center[2]};
            cam.radius = map.typical;
            cam.distance = map.typical * 1.2f;
            cam.fly_speed = std::max(0.05f, map.typical * 0.3f);
            return;
        }
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
        if (map_scene) {
            map_details_ui();
            return;
        }
        if (!scene.loaded()) {
            if (no_drawable_tag) {
                ImGui::Text("ENTITY %s", no_drawable_tag->str().c_str());
                ImGui::TextDisabled(ui::tr(ui::S::UnsupportedPath), no_drawable_reason.c_str());
                return;
            }
            empty_state(ico::Entities, ui::tr(ui::S::NothingSelected), ui::tr(ui::S::ClickCardHint));
            return;
        }
        if (!ImGui::BeginTabBar("model_details")) return;
        if (anim && scene.skinned() && ImGui::BeginTabItem(ui::tr(ui::S::AnimationTab))) {
            animation_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui::tr(ui::S::ParametersTab))) {
            material_panel();
            channels_panel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ui::tr(ui::S::ModelInfoTab))) {
            model_info_panel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    void material_panel() {
        if (scene_permutation_count > 1) {
            ImGui::TextUnformatted(ui::tr(ui::S::MaterialVariant));
            ImGui::SetNextItemWidth(-1);
            int permutation = int(scene_permutation);
            ImGui::BeginDisabled(loading.has_value());
            if (begin_combo("##material_variant", std::to_string(permutation).c_str(), ImGuiComboFlags_HeightLarge)) {
                ImGuiListClipper list;
                list.Begin(int(scene_permutation_count));
                while (list.Step())
                    for (int i = list.DisplayStart; i < list.DisplayEnd; ++i)
                        if (ImGui::Selectable(std::to_string(i).c_str(), i == permutation) && i != permutation) {
                            loading = scene.tag();
                            worker.request_live(scene.tag(), i, channel_overrides);
                        }
                ImGui::EndCombo();
            }
            ImGui::EndDisabled();
        }
        if (ImGui::CollapsingHeader(ui::tr(ui::S::MaterialAnimation))) {
            ImGui::PushID("material_playback");
            if (ImGui::Button(material_playing ? ui::tr(ui::S::Pause) : ui::tr(ui::S::Play)))
                material_playing = !material_playing;
            ImGui::SameLine();
            if (ImGui::Button(ui::tr(ui::S::Rewind))) material_time = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##material_time", &material_time, 0.01f, 0, 86400, ui::tr(ui::S::TimeFmt),
                                  ImGuiSliderFlags_AlwaysClamp)) material_playing = false;
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##material_speed", &material_speed, 0.0f, 4.0f, ui::tr(ui::S::SpeedFmt),
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopID();
        }
    }

    void model_info_panel() {
        ImGui::TextDisabled(ui::tr(ui::S::LodLine), scene.lod(), scene.groups().size(), scene.radius());
        if (scene.dynamic()) {
            if (scene_component.is_none()) ImGui::TextDisabled("%s", ui::tr(ui::S::NoEntityComponent));
            else ImGui::TextDisabled(ui::tr(ui::S::EntityComponent), scene_component.str().c_str());
            ImGui::PushTextWrapPos(0);
            for (const auto& n : scene.notes()) ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s", n.c_str());
            ImGui::PopTextWrapPos();
        }
        estimates_panel();
        if (engine == Engine::Game && game_ready)
            ImGui::TextDisabled(ui::tr(ui::S::ShadingLine),
                                game_lighting.shading_technique().str().c_str());
        for (size_t i = 0; i < scene.groups().size(); ++i) {
            const auto& g = scene.groups()[i];
            ImGui::PushID(int(i));
            if (ImGui::CollapsingHeader(ui::trf(ui::S::PartHeader, g.part, g.index_count).c_str())) {
                ImGui::Text(ui::tr(ui::S::Technique), g.technique.str().c_str());
                ImGui::Text("VS %s   PS %s", g.vs.str().c_str(), g.ps.str().c_str());
                ImGui::Text(ui::tr(ui::S::CbufferLine), g.cbuffer_regs, g.vm_instructions);
                for (const auto& s : g.stubbed_ops) ImGui::TextColored({1, 0.75f, 0.3f, 1}, ui::tr(ui::S::StubbedOp), s.c_str());
                if (ImGui::BeginTable("tex", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("slot", ImGuiTableColumnFlags_WidthFixed, 40);
                    ImGui::TableSetupColumn(ui::tr(ui::S::ColTexture), ImGuiTableColumnFlags_WidthFixed, 90);
                    ImGui::TableSetupColumn(ui::tr(ui::S::ColFormat));
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
                        ImGui::TextDisabled(ui::tr(ui::S::EngineSide), e.offset * 8);
                    }
                    ImGui::EndTable();
                }
            }
            ImGui::PopID();
        }
    }

    // -- TEXT --
    void strings_tab() {
        if (!catalog || !catalog->strings_ready) { ImGui::TextDisabled(ui::tr(ui::S::Loading)); return; }
        const auto& all = catalog->strings.strings();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##strfilter", (icon(ico::Search) + ui::tr(ui::S::TextFilterHint)).c_str(),
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
        ImGui::TextDisabled(ui::tr(ui::S::TextCount), string_view.size(), all.size());
        const std::string lang_col(language_code(TextLanguage(language)));
        if (ImGui::BeginTable("strings", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                ImGuiTableFlags_BordersOuter)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColHash), ImGuiTableColumnFlags_WidthFixed, 100);
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
                        if (ImGui::MenuItem(ui::tr(ui::S::CopyEnglish))) ImGui::SetClipboardText(s.text[0].c_str());
                        if (ImGui::MenuItem(ui::trf(ui::S::CopyLanguage, lang_col).c_str())) ImGui::SetClipboardText(s.text[size_t(language)].c_str());
                        if (ImGui::MenuItem(ui::tr(ui::S::CopyHash))) ImGui::SetClipboardText(std::format("{:08X}", s.hash).c_str());
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

    const std::string& text(uint32_t hash, size_t lang, TagHash container = {}) {
        static const std::string empty;
        if (!catalog || !catalog->strings_ready) return empty;
        const LocalizedString* s = catalog->strings.find(hash, container);
        return s ? s->text[lang] : empty;
    }
    // Codex strings are looked up in their own container: hashes repeat across containers.
    const std::string& text(const CodexString& s, size_t lang) {
        static const std::string empty;
        return s ? text(s.hash, lang, s.container) : empty;
    }

    // -- DIALOGUE --
    // Text in any of the 13 languages, the line's audio in any installed voice language, lines
    // filed by speaker or by conversation table, and WAV export into one folder per speaker.
    size_t dialogue_lang() const { return size_t(dialogue_text_lang < 0 ? language : dialogue_text_lang); }

    std::string speaker_name(uint32_t hash, size_t lang) {
        if (hash == 0 || hash == 0x811C9DC5) return ui::tr(ui::S::NoSpeaker);
        const std::string& t = text(hash, lang);
        if (!t.empty()) return t;
        const std::string& en = text(hash, 0);
        return en.empty() ? ui::trf(ui::S::UnknownSpeaker, hash) : en;
    }

    void ensure_audio_languages() {
        if (!audio_langs.empty() || !pm) return;
        std::map<uint16_t, std::string> found;
        for (const auto& p : pm->packages())
            if (p.language != 0 && !found.contains(p.language)) {
                const auto us = p.name.rfind('_');
                found[p.language] = us == std::string::npos ? std::to_string(p.language) : p.name.substr(us + 1);
            }
        for (auto& [id, suffix] : found) {
            audio_langs.emplace_back(id, audio_language_name(suffix));
            audio_suffixes.push_back(suffix);
        }
    }

    size_t voice_index(size_t text_lang) const {
        if (audio_lang >= 0) return size_t(audio_lang);
        size_t english = 0;
        for (size_t i = 0; i < audio_suffixes.size(); ++i) {
            const int slot = audio_language_slot(audio_suffixes[i]);
            if (slot == int(text_lang)) return i;
            if (slot == 0) english = i;
        }
        return english;
    }

    void stop_line_audio() {
        if (clip) clip->stop();
        if (audio_request) audio_request->id = 0;  // discard the pending decode without blocking the UI
        audio_error.clear();
    }

    void voice_selector(const char* label, size_t text_lang) {
        if (audio_langs.empty() || !begin_combo(label, audio_langs[voice_index(text_lang)].second.c_str())) return;
        if (ImGui::Selectable(ui::tr(ui::S::VoiceFollowText), audio_lang < 0) && audio_lang >= 0) {
            stop_line_audio();
            audio_lang = -1;
        }
        for (int i = 0; i < int(audio_langs.size()); ++i) {
            if (!ImGui::Selectable(audio_langs[size_t(i)].second.c_str(), audio_lang == i) || audio_lang == i) continue;
            stop_line_audio();
            audio_lang = i;
        }
        ImGui::EndCombo();
    }

    static PcmAudio read_line_audio(PackageManager& source, TagHash tag) {
        const auto* package = source.info(tag.pkg_id());
        if (!package || (package->language && package->language != source.language()))
            throw std::runtime_error("no recording in the selected audio language");
        return decode_wem(source.read(tag));
    }

    media::AudioEngine* audio_engine() {
        if (!audio_engine_) {
            audio_engine_ = std::make_unique<media::AudioEngine>();
            if (!audio_engine_->ok()) error(audio_engine_->error());
            clip = std::make_unique<media::ClipPlayer>(*audio_engine_);
        }
        return audio_engine_->ok() ? audio_engine_.get() : nullptr;
    }

    static uint64_t clip_id(uint32_t line, size_t wem, uint16_t lang) {
        return (uint64_t(line) + 1) | uint64_t(wem & 0xFF) << 32 | uint64_t(lang) << 40;
    }

    void play_line(uint32_t line, size_t wem_index, uint16_t lang) {
        if (!catalog || line >= catalog->dialogue.size() || !pm) return;
        const auto& l = catalog->dialogue[line];
        if (wem_index >= l.wem.size()) return;
        if (tab == TabDialogue) dialogue_selected = int(line);
        const uint64_t id = clip_id(line, wem_index, lang);
        audio_engine();
        if (clip && clip->id() == id && clip->playing()) {
            clip->stop();
            return;
        }
        if (clip) clip->stop();
        std::shared_ptr<PackageManager> fork(pm->fork());
        const TagHash wem = l.wem[wem_index];
        audio_request.reset();
        audio_request = AudioRequest{id, std::async(std::launch::async, [fork, wem, lang] {
                                         fork->set_language(lang);
                                         return std::make_shared<const PcmAudio>(read_line_audio(*fork, wem));
                                     })};
    }

    // ▶ / ■ for one line in one voice language; "…" while decoding.
    void play_button(uint32_t line, size_t wem_index, uint16_t lang, const char* label = nullptr) {
        const uint64_t id = clip_id(line, wem_index, lang);
        const bool loading = audio_request && audio_request->id == id;
        const bool playing = clip && clip->id() == id && clip->playing();
        std::string text_label = loading ? "…" : icon(playing ? ico::Stop : ico::Play);
        if (label) text_label += std::string("  ") + label;
        ImGui::PushID(int(id ^ (id >> 32)));
        if (ImGui::SmallButton(text_label.c_str())) play_line(line, wem_index, lang);
        ImGui::PopID();
    }

    void rebuild_dialogue_groups() {
        const auto& lines = catalog->dialogue;
        const size_t lang = dialogue_lang();
        dialogue_groups.clear();
        std::unordered_map<uint64_t, size_t> index;
        for (uint32_t i = 0; i < lines.size(); ++i) {
            const auto& l = lines[i];
            const uint64_t key = dialogue_group_mode == 0 ? l.speaker_hash : l.table.value;
            auto [it, inserted] = index.emplace(key, dialogue_groups.size());
            if (inserted) {
                DialogueGroup g;
                g.key = key;
                g.name = dialogue_group_mode == 0 ? speaker_name(l.speaker_hash, lang) : l.table.str();
                dialogue_groups.push_back(std::move(g));
            }
            dialogue_groups[it->second].lines.push_back(i);
        }
        if (dialogue_group_mode == 1)  // a table is labelled by its speakers
            for (auto& g : dialogue_groups) {
                std::vector<std::pair<size_t, uint32_t>> counts;
                for (uint32_t li : g.lines) {
                    const uint32_t sp = lines[li].speaker_hash;
                    auto c = std::find_if(counts.begin(), counts.end(), [&](auto& p) { return p.second == sp; });
                    if (c == counts.end()) counts.push_back({1, sp});
                    else c->first++;
                }
                std::sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.first > b.first; });
                std::string who;
                for (size_t k = 0; k < counts.size() && k < 2; ++k) who += (k ? ", " : "") + speaker_name(counts[k].second, lang);
                if (counts.size() > 2) who += ui::tr(ui::S::Others);
                g.name += "  " + who;
            }
        std::sort(dialogue_groups.begin(), dialogue_groups.end(), [&](const DialogueGroup& a, const DialogueGroup& b) {
            if (dialogue_group_mode == 0 && a.lines.size() != b.lines.size()) return a.lines.size() > b.lines.size();
            return a.name < b.name;
        });
        dialogue_groups_mode = dialogue_group_mode;
        dialogue_groups_lang = int(lang);
        dialogue_group = -1;
    }

    void start_dialogue_export(const fs::path& dir) {
        if (export_running || dialogue_view.empty() || audio_langs.empty()) return;
        if (export_thread.joinable()) export_thread.join();
        const auto& lines = catalog->dialogue;
        const size_t lang = dialogue_lang();
        const auto [lang_id, lang_name] = audio_langs[voice_index(lang)];
        struct Job { TagHash wem; fs::path file; };
        auto jobs = std::make_shared<std::vector<Job>>();
        std::string csv = "\xEF\xBB\xBF" "folder,file,table,line_id,speaker_en,speaker";
        for (size_t l = 0; l < kTextLanguageCount; ++l) csv += std::format(",{}", language_code(TextLanguage(l)));
        csv += "\n";
        auto quote = [](const std::string& v) {
            std::string q = "\"";
            for (char c : v) q += c == '"' ? std::string("\"\"") : std::string(1, c);
            return q + "\"";
        };
        std::set<std::string> used_names;
        for (uint32_t li : dialogue_view) {
            const auto& l = lines[li];
            const std::string folder = safe_file_name(speaker_name(l.speaker_hash, lang));
            for (size_t w = 0; w < l.wem.size(); ++w) {
                std::string name = std::format("{}_{:08X}{}", l.table.str(), l.line_id, l.wem.size() > 1 ? std::format("_{}", w) : "");
                std::string snippet = safe_file_name(text(l.subtitle_hash, lang, l.subtitle_container));
                if (snippet.size() > 48) snippet = utf8_prefix(snippet, 48);
                if (!snippet.empty()) name += "_" + snippet;
                std::string file = name + ".wav";
                for (int n = 2; !used_names.insert(folder + "/" + file).second; ++n) file = std::format("{}_{}.wav", name, n);
                jobs->push_back({l.wem[w], dir / to_wide(folder) / to_wide(file)});
                csv += quote(folder) + "," + quote(file) + "," + l.table.str() + std::format(",{:08X},", l.line_id) +
                       quote(speaker_name(l.speaker_hash, 0)) + "," + quote(speaker_name(l.speaker_hash, lang));
                for (size_t t = 0; t < kTextLanguageCount; ++t) csv += "," + quote(text(l.subtitle_hash, t, l.subtitle_container));
                csv += "\n";
            }
        }
        export_total = jobs->size();
        export_done = 0;
        export_failed = 0;
        export_missing = 0;
        export_cancel = false;
        export_running = true;
        {
            std::lock_guard lk(export_mutex);
            export_message = ui::trf(ui::S::ExportRunning, jobs->size(), lang_name);
        }
        std::shared_ptr<PackageManager> fork(pm->fork());
        const uint16_t lid = lang_id;
        export_thread = std::thread([this, fork, jobs, dir, csv = std::move(csv), lid, lang_name] {
            fork->set_language(lid);
            try {
                fs::create_directories(dir);
                std::ofstream(dir / "index.csv", std::ios::binary) << csv;
            } catch (...) {
            }
            for (const auto& j : *jobs) {
                if (export_cancel) break;
                try {
                    fs::create_directories(j.file.parent_path());
                    const auto pcm = read_line_audio(*fork, j.wem);
                    if (!write_wav_file(j.file, pcm)) throw std::runtime_error(ui::tr(ui::S::ErrWriteFailed));
                } catch (const std::exception& ex) {
                    if (std::string_view(ex.what()).find("no recording") != std::string_view::npos) export_missing++;
                    else export_failed++;
                }
                export_done++;
            }
            std::lock_guard lk(export_mutex);
            export_message = ui::trf(ui::S::ExportDone, 
                                         export_cancel ? ui::tr(ui::S::ExportAborted) : "",
                                         export_done.load() - export_failed.load() - export_missing.load(),
                                         export_total.load(), lang_name, export_missing.load(), export_failed.load(), to_utf8(dir.wstring()));
            export_running = false;
        });
    }

    void dialogue_tab() {
        if (!catalog || !catalog->dialogue_ready) { ImGui::TextDisabled(ui::tr(ui::S::Loading)); return; }
        ensure_audio_languages();
        const auto& lines = catalog->dialogue;
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const size_t lang = dialogue_lang();

        // toolbar
        ImGui::SetNextItemWidth(std::max(160 * sc, ImGui::GetContentRegionAvail().x * 0.35f));
        ImGui::InputTextWithHint("##dlgfilter", (icon(ico::Search) + ui::tr(ui::S::DialogueFilterHint)).c_str(), dialogue_filter,
                                 sizeof(dialogue_filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * sc);
        if (begin_combo(ui::tr(ui::S::Subtitles), text_language_name(lang))) {
            for (int l = 0; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(text_language_name(size_t(l)), size_t(l) == lang) && size_t(l) != lang) {
                    stop_line_audio();
                    dialogue_text_lang = l;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox(ui::tr(ui::S::ShowEnglish), &dialogue_show_english);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130 * sc);
        if (audio_langs.empty()) {
            ImGui::TextDisabled(ui::tr(ui::S::NoVoicePackages));
        } else voice_selector(ui::tr(ui::S::Voice), lang);
        ImGui::SetItemTooltip(ui::tr(ui::S::VoiceTip));
        ImGui::SameLine();
        ImGui::BeginDisabled(export_running || dialogue_view.empty() || audio_langs.empty());
        if (ImGui::Button((icon(ico::Save) + (std::string("  ") + ui::tr(ui::S::Export))).c_str()))
            if (auto dir = pick_folder(g_win.hwnd)) start_dialogue_export(*dir);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(ui::tr(ui::S::ExportTip));
        if (export_running) {
            ImGui::SameLine();
            ImGui::ProgressBar(export_total ? float(export_done) / float(export_total) : 0, {140 * sc, 0},
                               std::format("{} / {}", export_done.load(), export_total.load()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(ui::tr(ui::S::Abort))) export_cancel = true;
        } else {
            std::lock_guard lk(export_mutex);
            if (!export_message.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", export_message.c_str());
            }
        }
        if (!audio_error.empty()) ImGui::TextColored({1, 0.4f, 0.4f, 1}, ui::tr(ui::S::AudioError), audio_error.c_str());

        if (dialogue_groups_mode != dialogue_group_mode || dialogue_groups_lang != int(lang)) rebuild_dialogue_groups();

        // left: filing by speaker / table
        ImGui::BeginChild("dlggroups", {280 * sc, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        if (ImGui::RadioButton(ui::tr(ui::S::BySpeaker), dialogue_group_mode == 0)) dialogue_group_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton(ui::tr(ui::S::ByTable), dialogue_group_mode == 1)) dialogue_group_mode = 1;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##grpfilter", ui::tr(ui::S::Filter), dialogue_group_filter, sizeof(dialogue_group_filter));
        const std::string gf = lower_ascii(dialogue_group_filter);
        if (ImGui::Selectable(ui::trf(ui::S::AllLines, lines.size()).c_str(), dialogue_group < 0)) dialogue_group = -1;
        for (int g = 0; g < int(dialogue_groups.size()); ++g) {
            const auto& grp = dialogue_groups[size_t(g)];
            if (!gf.empty() && !contains_ci(grp.name, gf) && grp.name.find(dialogue_group_filter) == std::string::npos) continue;
            if (ImGui::Selectable(std::format("{}  ({})##g{}", grp.name, grp.lines.size(), g).c_str(), dialogue_group == g))
                dialogue_group = g;
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // the filtered view
        const std::string f = lower_ascii(dialogue_filter);
        const std::string key = std::format("{}\x01{}\x01{}\x01{}", f, lang, dialogue_group, dialogue_group_mode);
        if (key != dialogue_filter_applied) {
            dialogue_filter_applied = key;
            dialogue_view.clear();
            auto match = [&](uint32_t i) {
                const auto& l = lines[i];
                return f.empty() || contains_ci(text(l.speaker_hash, 0), f) || contains_ci(text(l.subtitle_hash, 0, l.subtitle_container), f) ||
                       text(l.subtitle_hash, lang, l.subtitle_container).find(dialogue_filter) != std::string::npos ||
                       text(l.speaker_hash, lang).find(dialogue_filter) != std::string::npos;
            };
            if (dialogue_group >= 0 && size_t(dialogue_group) < dialogue_groups.size()) {
                for (uint32_t i : dialogue_groups[size_t(dialogue_group)].lines)
                    if (match(i)) dialogue_view.push_back(i);
            } else {
                for (uint32_t i = 0; i < lines.size(); ++i)
                    if (match(i)) dialogue_view.push_back(i);
            }
        }

        ImGui::BeginGroup();
        const float details_h = dialogue_selected >= 0 ? std::min(330 * sc, ImGui::GetContentRegionAvail().y * 0.45f) : 0;
        ImGui::TextDisabled(ui::tr(ui::S::RowCount), dialogue_view.size(), lines.size());
        const uint16_t voice = audio_langs.empty() ? 0 : audio_langs[voice_index(lang)].first;
        if (ImGui::BeginTable("dialogue", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                 ImGuiTableFlags_BordersOuter,
                              {0, ImGui::GetContentRegionAvail().y - details_h})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * sc);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColSpeaker), ImGuiTableColumnFlags_WidthFixed, 150 * sc);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColLine));
            ImGui::TableSetupColumn(ui::tr(ui::S::ColTable), ImGuiTableColumnFlags_WidthFixed, 80 * sc);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip_rows;
            clip_rows.Begin(int(dialogue_view.size()));
            while (clip_rows.Step())
                for (int r = clip_rows.DisplayStart; r < clip_rows.DisplayEnd; ++r) {
                    const uint32_t li = dialogue_view[size_t(r)];
                    const auto& l = lines[li];
                    ImGui::TableNextRow();
                    ImGui::PushID(r);
                    ImGui::TableNextColumn();
                    if (!l.wem.empty()) play_button(li, 0, voice);
                    ImGui::TableNextColumn();
                    ImGui::PushFont(g_fonts.bold, 0);
                    if (ImGui::Selectable(speaker_name(l.speaker_hash, lang).c_str(), dialogue_selected == int(li),
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                        if (dialogue_selected != int(li)) stop_line_audio();
                        dialogue_selected = int(li);
                    }
                    ImGui::PopFont();
                    const std::string& en = text(l.subtitle_hash, 0, l.subtitle_container);
                    const std::string& tr = text(l.subtitle_hash, lang, l.subtitle_container);
                    if (ImGui::BeginPopupContextItem("ctx")) {
                        if (ImGui::MenuItem(ui::tr(ui::S::CopyDisplayed))) ImGui::SetClipboardText(tr.c_str());
                        if (ImGui::MenuItem(ui::tr(ui::S::CopyEnglish))) ImGui::SetClipboardText(en.c_str());
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", (tr.empty() ? en : tr).c_str());
                    if (dialogue_show_english && lang != 0 && !en.empty() && en != tr) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        ImGui::TextWrapped("%s", en.c_str());
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", l.table.str().c_str());
                    ImGui::PopID();
                }
            ImGui::EndTable();
        }
        if (dialogue_selected >= 0 && size_t(dialogue_selected) < lines.size()) dialogue_details(uint32_t(dialogue_selected));
        ImGui::EndGroup();
    }

    void dialogue_details(uint32_t li) {
        const auto& l = catalog->dialogue[li];
        const size_t lang = dialogue_lang();
        ImGui::BeginChild("dlgdetails", {0, 0}, ImGuiChildFlags_Borders);
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::TextUnformatted(speaker_name(l.speaker_hash, lang).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled(ui::tr(ui::S::LineInfo), l.line_id, l.table.str().c_str(),
                            l.audio_event.is_none() ? "-" : l.audio_event.str().c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 24 * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::SmallButton("×")) dialogue_selected = -1;
        if (l.wem.empty()) {
            ImGui::TextDisabled(ui::tr(ui::S::NoAudio));
        } else {
            for (size_t w = 0; w < l.wem.size(); ++w) {
                ImGui::TextDisabled("%s", l.wem[w].str().c_str());
                for (const auto& [id, name] : audio_langs) {
                    ImGui::SameLine();
                    play_button(li, w, id, name.c_str());
                }
                ImGui::SameLine();
                ImGui::PushID(int(w));
                if (ImGui::SmallButton((icon(ico::Save) + "  WAV").c_str()) && !audio_langs.empty())
                    if (auto dir = pick_folder(g_win.hwnd)) {
                        try {
                            std::unique_ptr<PackageManager> fork(pm->fork());
                            const auto& [voice, name] = audio_langs[voice_index(lang)];
                            fork->set_language(voice);
                            const auto pcm = read_line_audio(*fork, l.wem[w]);
                            const fs::path file = *dir / to_wide(safe_file_name(std::format(
                                "{}_{:08X}_{}.wav", speaker_name(l.speaker_hash, lang), l.line_id, name)));
                            if (!write_wav_file(file, pcm)) throw std::runtime_error(ui::tr(ui::S::ErrWriteWav));
                            info(ui::trf(ui::S::LogSaved, to_utf8(file.wstring())));
                        } catch (const std::exception& ex) {
                            error(ui::trf(ui::S::ErrWav, ex.what()));
                        }
                    }
                ImGui::SetItemTooltip(ui::tr(ui::S::SaveWavTip));
                ImGui::PopID();
            }
            if (clip && clip->playing() && (clip->id() & 0xFFFFFFFF) == uint64_t(li) + 1)
                ImGui::ProgressBar(float(clip->position() / std::max(0.001, clip->length())), {-1, 4 * ImGui::GetStyle().FontScaleDpi}, "");
        }
        if (ImGui::BeginTable("langs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn(ui::tr(ui::S::ColLanguage), ImGuiTableColumnFlags_WidthFixed, 150 * ImGui::GetStyle().FontScaleDpi);
            ImGui::TableSetupColumn(ui::tr(ui::S::BySpeaker), ImGuiTableColumnFlags_WidthFixed, 140 * ImGui::GetStyle().FontScaleDpi);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColLine));
            for (size_t t = 0; t < kTextLanguageCount; ++t) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", text_language_name(t));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(speaker_name(l.speaker_hash, t).c_str());
                ImGui::TableNextColumn();
                const std::string& s = text(l.subtitle_hash, t, l.subtitle_container);
                ImGui::PushID(int(t));
                ImGui::TextWrapped("%s", s.c_str());
                if (ImGui::BeginPopupContextItem("c")) {
                    if (ImGui::MenuItem(ui::tr(ui::S::Copy))) ImGui::SetClipboardText(s.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (l.caption_hash && l.caption_hash != 0x811C9DC5 && !text(l.caption_hash, 0, l.caption_container).empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled(ui::tr(ui::S::Caption));
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", text(l.caption_hash, lang, l.caption_container).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    // -- MOVIES --
    // Localized USM channel numbers follow the text slots with the Japanese and Latin American Spanish channels swapped.
    static std::optional<TextLanguage> movie_track_language(int channel) {
        if (channel == 2) return TextLanguage::SpanishLatAm;
        if (channel == 6) return TextLanguage::Japanese;
        if (channel < 1 || size_t(channel) > kTextLanguageCount) return std::nullopt;
        return TextLanguage(channel - 1);
    }

    int default_movie_channel(const MovieEntry& m) const {
        if (m.info.audio.empty()) return 0;
        if (m.info.audio.size() > 1) {
            for (const auto& a : m.info.audio)
                if (movie_track_language(a.channel) == TextLanguage(language)) return a.channel;
            for (const auto& a : m.info.audio)
                if (movie_track_language(a.channel) == TextLanguage::English) return a.channel;
        }
        return m.info.audio.front().channel;
    }

    static std::string movie_track_label(const MovieEntry& m, const UsmInfo::Audio& a) {
        std::string name;
        const auto locale = movie_track_language(a.channel);
        if (m.info.audio.size() > 1 && locale)
            name = ui::trf(ui::S::TrackEstimated, text_language_name(size_t(*locale)));
        else
            name = m.info.audio.size() > 1 ? ui::tr(ui::S::Default) : ui::tr(ui::S::Voice);
        name += std::format("  ch{} {} {}ch", a.channel, usm_audio_codec_name(a.codec), a.channels);
        if (a.codec != 2 && a.codec != 4) name += std::string(" ") + ui::tr(ui::S::Unsupported);
        return name;
    }

    void open_movie(int index, double at, bool paused) {
        if (!catalog || index < 0 || size_t(index) >= catalog->movies.size() || !pm) return;
        const auto& m = catalog->movies[size_t(index)];
        movie.player.reset();
        movie.pending.reset();
        if (movie.selected != index) {
            movie.selected = index;
            movie.channel = default_movie_channel(m);
            movie.has_frame = false;
        }
        if (!m.error.empty()) return;
        movie.player = std::make_unique<media::MoviePlayer>(*pm, audio_engine(), m, movie.channel, at);
        if (paused) movie.player->set_paused(true);
    }

    void media_tick() {
        if (audio_request && audio_request->result.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto pcm = audio_request->result.get();
                if (audio_request->id) {
                    audio_engine();
                    if (clip) clip->play(pcm, audio_request->id);
                    audio_error.clear();
                }
            } catch (const std::exception& ex) {
                if (audio_request->id) {
                    audio_error = ex.what();
                    if (audio_error.find("no recording") != std::string::npos) audio_error = ui::tr(ui::S::NoRecording);
                    error(ui::trf(ui::S::ErrPlayAudio, audio_error));
                }
            }
            audio_request.reset();
        }
        if (movie.player)
            if (auto f = movie.player->take_due_frame()) movie.pending = std::move(f);
    }

    // Copies the newest movie frame into its texture (recorded before the UI draws).
    void record_uploads(ID3D12GraphicsCommandList* cl, uint32_t fi) {
        if (!movie.pending) return;
        auto f = std::move(movie.pending);
        if (!f->width || !f->height) return;
        try {
            if (!movie.tex || movie.w != f->width || movie.h != f->height) {
                gpu.wait_idle();
                movie.tex = gpu.texture2d(f->width, f->height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
                if (movie.srv == UINT32_MAX) movie.srv = gpu.srv().alloc(1);
                gpu.device()->CreateShaderResourceView(movie.tex.Get(), nullptr, gpu.srv().cpu(movie.srv));
                movie.w = f->width, movie.h = f->height;
                movie.tex_readable = false;
            }
            const UINT pitch = (f->width * 4 + 255u) & ~255u;
            const size_t need = size_t(pitch) * f->height;
            auto& up = movie.upload[fi];
            if (!up || movie.upload_size[fi] < need) {
                up = gpu.buffer(need, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
                movie.upload_size[fi] = need;
            }
            uint8_t* dst = nullptr;
            const D3D12_RANGE none{0, 0};
            check_hr(up->Map(0, &none, reinterpret_cast<void**>(&dst)), "movie upload Map");
            for (uint32_t y = 0; y < f->height; ++y)
                std::memcpy(dst + size_t(y) * pitch, f->rgba.data() + size_t(y) * f->width * 4, size_t(f->width) * 4);
            up->Unmap(0, nullptr);
            if (movie.tex_readable)
                Gpu::barrier(cl, movie.tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = movie.tex.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = up.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, f->width, f->height, 1, pitch};
            cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            Gpu::barrier(cl, movie.tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            movie.tex_readable = true;
            movie.has_frame = true;
        } catch (const std::exception& ex) {
            error(ui::trf(ui::S::MovieFrameError, ex.what()));
        }
    }

    static std::string clock_text(double s) {
        const int t = int(std::max(0.0, s));
        return std::format("{}:{:02}", t / 60, t % 60);
    }

    size_t movie_sub_lang() const { return size_t(movie.sub_lang < 0 ? language : movie.sub_lang); }

    // -- CODEX --
    // The in-game codex: collections in game order, each entry's title, unlock hint and body in any
    // of the 13 languages, and its audio log in any installed voice language.
    // The codex terminal palette (CyberAcme 802.11 NEURO.FILTER intel reader): deep CRT black-green,
    // phosphor green text, dark emerald ticker bars.
    static constexpr ImU32 kCxGreen = IM_COL32(34, 214, 88, 255);
    static constexpr ImU32 kCxBody = IM_COL32(28, 186, 76, 255);
    static constexpr ImU32 kCxDim = IM_COL32(34, 122, 58, 255);
    static constexpr ImU32 kCxRule = IM_COL32(22, 68, 36, 255);
    static constexpr ImU32 kCxPanel = IM_COL32(11, 15, 12, 255);
    static constexpr ImU32 kCxBarBg = IM_COL32(10, 29, 16, 255);

    // The codex illustration texture, uploaded the first time it is shown.
    const CodexUi::Art& codex_art(TagHash tag) {
        auto [it, fresh] = codex.art.try_emplace(tag.value);
        if (!fresh) return it->second;
        CodexUi::Art& a = it->second;
        try {
            if (!codex.art_pm) codex.art_pm = pm->fork();
            const Texture t = load_texture(*codex.art_pm, tag);
            gpu.wait_idle();
            a.tex = gpu.texture(t);
            gpu.flush();
            a.srv = gpu.srv().alloc(1);
            gpu.device()->CreateShaderResourceView(a.tex.Get(), nullptr, gpu.srv().cpu(a.srv));
            a.w = t.width, a.h = t.height;
        } catch (const std::exception& ex) {
            a.error = ex.what();
            error(std::format("codex image {}: {}", tag.str(), ex.what()));
        }
        return a;
    }

    void codex_tab() {
        if (!catalog || !catalog->codex_ready) {
            if (catalog && catalog->done && !catalog->codex_error.empty())
                ImGui::TextColored(col4(theme::kError), "%s", catalog->codex_error.c_str());
            else loading_state(ui::tr(ui::S::Loading));
            return;
        }
        ensure_audio_languages();
        const Codex& cx = catalog->codex;
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const size_t lang = codex.lang < 0 ? language : size_t(codex.lang);

        // toolbar: search, text language, audio only, count
        ImGui::SetNextItemWidth(std::min(360 * sc, ImGui::GetContentRegionAvail().x * 0.4f));
        ImGui::InputTextWithHint("##cxfilter", ui::tr(ui::S::CodexFilterHint), codex.filter, sizeof(codex.filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * sc);
        if (begin_combo("##cxlang", text_language_name(lang))) {
            for (int l = 0; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(text_language_name(size_t(l)), size_t(l) == lang) && size_t(l) != lang) {
                    stop_line_audio();
                    codex.lang = l;
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("%s", ui::tr(ui::S::Subtitles));
        ImGui::SameLine();
        ImGui::Checkbox(ui::tr(ui::S::CodexAudioOnly), &codex.audio_only);
        ImGui::SameLine();
        {
            const std::string count = ui::trf(ui::S::CodexCount, cx.collections.size(), cx.entries, cx.with_audio);
            const float cw = ImGui::CalcTextSize(count.c_str()).x;
            const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cw;
            if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col4(theme::kMuted), "%s", count.c_str());
        }
        ImGui::Dummy({0, 6 * sc});

        // The records in the game's codex order, as a grid of their illustrations.
        const std::string f = lower_ascii(codex.filter);
        auto match = [&](const CodexString& h) { return f.empty() || contains_ci(text(h, lang), f) || contains_ci(text(h, 0), f); };
        codex.view.clear();
        for (int ci = 0; ci < int(cx.collections.size()); ++ci) {
            const auto& c = cx.collections[size_t(ci)];
            for (int ei = 0; ei < int(c.entries.size()); ++ei) {
                const auto& e = c.entries[size_t(ei)];
                if (codex.audio_only && e.lines.empty()) continue;
                if (!match(c.title) && !match(e.title) && !match(e.body)) continue;
                codex.view.push_back({ci, ei});
            }
        }
        if (static bool once = false; !once && !codex.view.empty()) {  // NIM_CODEX_OPEN=N: open grid item N (headless checks)
            once = true;
            if (const char* v = std::getenv("NIM_CODEX_OPEN")) {
                const size_t n = std::min(size_t(std::atoi(v)), codex.view.size() - 1);
                codex.collection = codex.view[n].first, codex.entry = codex.view[n].second, codex.open_reader = true;
            }
        }
        ImGui::BeginChild("cxgrid", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* dl = ImGui::GetWindowDrawList();
        const float gap = 14 * sc;
        const float avail = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, int((avail + gap) / (180 * sc + gap)));
        const float tile = (avail - gap * float(cols - 1)) / float(cols);
        const float img_h = tile * (910.0f / 880.0f);  // the codex art is 880x910
        const float cap_h = ImGui::GetTextLineHeight() * 2 + 14 * sc;
        const float row_h = img_h + cap_h + gap;
        const int rows = (int(codex.view.size()) + cols - 1) / cols;
        int uploads = 0;  // new textures per frame, so scrolling stays smooth
        ImGuiListClipper clip_rows;
        clip_rows.Begin(rows, row_h);
        while (clip_rows.Step())
            for (int r = clip_rows.DisplayStart; r < clip_rows.DisplayEnd; ++r) {
                const ImVec2 row0 = ImGui::GetCursorPos();
                for (int k = 0; k < cols; ++k) {
                    const size_t vi = size_t(r * cols + k);
                    if (vi >= codex.view.size()) break;
                    const auto [ci, ei] = codex.view[vi];
                    const auto& c = cx.collections[size_t(ci)];
                    const auto& e = c.entries[size_t(ei)];
                    ImGui::SetCursorPos({row0.x + float(k) * (tile + gap), row0.y});
                    ImGui::PushID(int(vi));
                    const ImVec2 p = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##t", {tile, img_h + cap_h})) {
                        codex.collection = ci, codex.entry = ei;
                        codex.open_reader = true;
                    }
                    const bool hov = ImGui::IsItemHovered();
                    const ImVec2 q{p.x + tile, p.y + img_h};
                    dl->AddRectFilled(p, q, kCxPanel);
                    const TagHash art_tag = e.illustration.is_none() ? c.illustration : e.illustration;
                    const CodexUi::Art* art = nullptr;
                    if (!art_tag.is_none()) {
                        auto it = codex.art.find(art_tag.value);
                        if (it != codex.art.end()) art = &it->second;
                        else if (uploads < 3) art = &codex_art(art_tag), ++uploads;
                    }
                    if (art && art->srv != UINT32_MAX) {
                        const float pad = 6 * sc;
                        dl->AddImage(ImTextureID(gpu.srv().gpu(art->srv).ptr), {p.x + pad, p.y + pad}, {q.x - pad, q.y - pad});
                    } else if (art_tag.is_none()) {
                        ImGui::PushFont(g_fonts.mono, ImGui::GetFontSize() * 0.75f / sc);
                        const char* none = ui::tr(ui::S::CodexNoIllustration);
                        const ImVec2 ts = ImGui::CalcTextSize(none, nullptr, false, tile - 24 * sc);
                        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p.x + 12 * sc, p.y + (img_h - ts.y) * 0.5f}, kCxDim, none,
                                    nullptr, tile - 24 * sc);
                        ImGui::PopFont();
                    }
                    dl->AddRect(p, q, hov ? kCxGreen : kCxRule, 0, 0, 1 * sc);
                    if (!e.lines.empty()) {  // audio log badge
                        const std::string g = icon(ico::Volume);
                        const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
                        const ImVec2 b{q.x - gs.x - 14 * sc, p.y + 10 * sc};
                        dl->AddRectFilled({b.x - 6 * sc, b.y - 4 * sc}, {b.x + gs.x + 6 * sc, b.y + gs.y + 2 * sc}, IM_COL32(0, 0, 0, 200), 3 * sc);
                        dl->AddText(b, kCxGreen, g.c_str());
                    }
                    std::string et = text(e.title, lang);
                    if (et.empty()) et = std::format("{:08X}", e.hash);
                    const float ty = q.y + 7 * sc;
                    text_fit(dl, {p.x, ty}, hov ? kCxGreen : theme::kText, et.c_str(), p.x + tile);
                    text_fit(dl, {p.x, ty + ImGui::GetTextLineHeight() + 2 * sc}, theme::kMuted, text(c.title, lang).c_str(), p.x + tile);
                    ImGui::PopID();
                }
                ImGui::SetCursorPos(row0);
                ImGui::Dummy({avail, row_h - ImGui::GetStyle().ItemSpacing.y});
            }
        if (codex.view.empty()) {
            ImGui::PushFont(g_fonts.mono, 0);
            ImGui::TextColored(col4(kCxDim), "> %s", ui::tr(ui::S::CodexNone));
            ImGui::PopFont();
        }
        ImGui::EndChild();

        // The reader, as a popup over the grid. Esc or x closes; left / right step through the grid.
        if (codex.open_reader) {
            stop_line_audio();
            ImGui::OpenPopup("##cxreader");
            codex.open_reader = false;
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float max_w = std::min(vp->WorkSize.x - 80 * sc, 1380 * sc);
        const float max_h = vp->WorkSize.y - 100 * sc;
        constexpr float kTermAspect = 1.90f;  // CyberAcme 802.11 widescreen terminal proportion
        float pop_w = max_w, pop_h = pop_w / kTermAspect;
        if (pop_h > max_h) {
            pop_h = max_h;
            pop_w = std::min(max_w, pop_h * kTermAspect);
        }
        ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
        ImGui::SetNextWindowSize({pop_w, pop_h}, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, col4(kCxPanel));
        ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.72f));
        if (ImGui::BeginPopupModal("##cxreader", nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
            bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
            if (!ImGui::GetIO().WantTextInput && !codex.view.empty()) {
                const int step = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -1 : 0;
                if (step) {
                    int at = 0;
                    for (int v = 0; v < int(codex.view.size()); ++v)
                        if (codex.view[size_t(v)].first == codex.collection && codex.view[size_t(v)].second == codex.entry) at = v;
                    const int next = std::clamp(at + step, 0, int(codex.view.size()) - 1);
                    if (at != next) {
                        stop_line_audio();
                        codex.collection = codex.view[size_t(next)].first;
                        codex.entry = codex.view[size_t(next)].second;
                    }
                }
            }
            codex_reader(cx, lang, close);
            if (close) {
                stop_line_audio();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    // One record as the game's CyberAcme 802.11 (NEURO.FILTER) intel reader shows it.
    void codex_reader(const Codex& cx, size_t lang, bool& close) {
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const ImVec2 win_p = ImGui::GetWindowPos();
        const ImVec2 win_s = ImGui::GetWindowSize();
        auto* root_dl = ImGui::GetWindowDrawList();
        const float bar_h = std::clamp(win_s.y * 0.056f, 32 * sc, 42 * sc);
        const float ticker_font_px = std::clamp(bar_h * 0.54f / sc, 15.0f, 21.0f);
        const float term_font_px = std::clamp(win_s.x * 0.0132f / sc, 15.0f, 19.0f);

        // Full terminal background + subtle horizontal CRT scanlines.
        root_dl->AddRectFilled(win_p, {win_p.x + win_s.x, win_p.y + win_s.y}, kCxPanel);
        for (float y = win_p.y + bar_h; y < win_p.y + win_s.y - bar_h; y += std::max(2.0f, 3.0f * sc))
            root_dl->AddLine({win_p.x, y}, {win_p.x + win_s.x, y}, IM_COL32(0, 0, 0, 34), 1.0f);

        // Top & bottom continuously scrolling CyberAcme 802.11 (NEURO.FILTER) ticker bars.
        auto draw_ticker_bar = [&](float y0, const char* sep, bool with_close, float dir) {
            const ImVec2 b0{win_p.x, y0}, b1{win_p.x + win_s.x, y0 + bar_h};
            root_dl->AddRectFilled(b0, b1, kCxBarBg);
            root_dl->AddLine({b0.x, b0.y}, {b1.x, b0.y}, kCxRule, 1.0f);
            root_dl->AddLine({b0.x, b1.y - 1.0f}, {b1.x, b1.y - 1.0f}, kCxRule, 1.0f);
            const std::string unit = std::format("CYBERACME 802.11 (NEURO.FILTER)   {}   ", sep);
            ImGui::PushFont(g_fonts.wide, ticker_font_px);
            const ImVec2 us = ImGui::CalcTextSize(unit.c_str());
            const float step = std::max(us.x, 40.0f * sc);
            const float clip_r = with_close ? (b1.x - bar_h - 6 * sc) : b1.x;
            const float phase = std::fmod(float(ImGui::GetTime()) * 36.0f * sc, step);
            const float start_x = dir < 0.0f ? (b0.x - phase) : (b0.x - step + phase);
            root_dl->PushClipRect(b0, {clip_r, b1.y}, true);
            const float ty = y0 + (bar_h - us.y) * 0.5f;
            for (float x = start_x; x < clip_r; x += step)
                root_dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {x, ty}, kCxGreen, unit.c_str());
            root_dl->PopClipRect();
            ImGui::PopFont();
        };
        draw_ticker_bar(win_p.y, "+", true, -1.0f);
        draw_ticker_bar(win_p.y + win_s.y - bar_h, "-", false, 1.0f);

        // Close button [x] on the right side of the top ticker bar.
        {
            const float bs = bar_h - 6 * sc;
            ImGui::SetCursorScreenPos({win_p.x + win_s.x - bs - 5 * sc, win_p.y + 3 * sc});
            if (ImGui::InvisibleButton("##cxclose", {bs, bs})) close = true;
            const bool hov = ImGui::IsItemHovered();
            const ImVec2 a = ImGui::GetItemRectMin();
            if (hov) root_dl->AddRectFilled(a, {a.x + bs, a.y + bs}, IM_COL32(26, 74, 40, 200), 2 * sc);
            const std::string g = icon(0xE711);
            const ImVec2 gs = ImGui::CalcTextSize(g.c_str());
            root_dl->AddText({a.x + (bs - gs.x) * 0.5f, a.y + (bs - gs.y) * 0.5f}, hov ? kCxGreen : kCxDim, g.c_str());
        }

        // Main interior area between the two ticker bars.
        const float pad_x = win_s.x * 0.025f, pad_y = win_s.y * 0.042f;
        const float inner_w = std::max(100.0f, win_s.x - 2 * pad_x);
        const float inner_h = std::max(100.0f, win_s.y - 2 * bar_h - 2 * pad_y);
        ImGui::SetCursorScreenPos({win_p.x + pad_x, win_p.y + bar_h + pad_y});

        if (codex.collection < 0 || size_t(codex.collection) >= cx.collections.size() || codex.entry < 0 ||
            size_t(codex.entry) >= cx.collections[size_t(codex.collection)].entries.size()) {
            ImGui::PushFont(g_fonts.mono, term_font_px);
            ImGui::TextColored(col4(kCxDim), "> %s", ui::tr(ui::S::CodexPick));
            ImGui::PopFont();
            return;
        }
        const auto& c = cx.collections[size_t(codex.collection)];
        const auto& e = c.entries[size_t(codex.entry)];
        const bool two_col = inner_w > 640 * sc;
        const float col_gap = win_s.x * 0.024f;
        const float left_w = two_col ? std::min(inner_w * 0.345f, inner_h * 0.78f) : std::min(inner_w, 240 * sc);
        const float col_right_w = two_col ? std::max(260 * sc, inner_w - left_w - col_gap) : inner_w;

        // ---- Left column: large square CRT phosphor-tinted illustration ----
        ImGui::BeginGroup();
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float box = std::max(120 * sc, left_w);
            const TagHash art_tag = e.illustration.is_none() ? c.illustration : e.illustration;
            const CodexUi::Art* art = art_tag.is_none() ? nullptr : &codex_art(art_tag);
            const float box_h = box;
            root_dl->AddRectFilled(p, {p.x + box, p.y + box_h}, IM_COL32(6, 14, 8, 255));
            if (art && art->srv != UINT32_MAX) {
                const float aspect = float(std::max(art->w, 1)) / float(std::max(art->h, 1));
                float iw = box, ih = box;
                if (aspect > 1.0f) ih = box / aspect;
                else iw = box * aspect;
                const ImVec2 ia{p.x + (box - iw) * 0.5f, p.y + (box_h - ih) * 0.5f};
                const ImVec2 ib{ia.x + iw, ia.y + ih};
                const ImU32 tint = codex.crt_tint ? IM_COL32(48, 240, 108, 255) : IM_COL32(255, 255, 255, 255);
                root_dl->AddImage(ImTextureID(gpu.srv().gpu(art->srv).ptr), ia, ib, {0, 0}, {1, 1}, tint);
                if (codex.crt_tint) {
                    for (float y = ia.y; y < ib.y; y += std::max(2.0f, 3.0f * sc))
                        root_dl->AddLine({ia.x, y}, {ib.x, y}, IM_COL32(0, 18, 6, 64), 1.0f);
                }
            } else {
                ImGui::PushFont(g_fonts.mono, ImGui::GetFontSize() * 0.8f / sc);
                const char* none = art ? art->error.c_str() : ui::tr(ui::S::CodexNoIllustration);
                const float wrap = box - 24 * sc;
                const ImVec2 ts = ImGui::CalcTextSize(none, nullptr, false, wrap);
                root_dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p.x + 12 * sc, p.y + (box_h - ts.y) * 0.5f}, kCxDim, none, nullptr, wrap);
                ImGui::PopFont();
            }
            root_dl->AddRect(p, {p.x + box, p.y + box_h}, kCxRule, 0, 0, 1.0f);
            ImGui::Dummy({box, box_h});

            // Subtle 1-line metadata + CRT tint toggle below the artwork.
            ImGui::Dummy({0, 6 * sc});
            ImGui::PushFont(g_fonts.mono, ImGui::GetFontSize() * 0.78f / sc);
            ImGui::PushStyleColor(ImGuiCol_Button, col4(kCxBarBg));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(kCxRule));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, col4(kCxRule));
            ImGui::PushStyleColor(ImGuiCol_Text, col4(kCxGreen));
            if (ImGui::SmallButton(codex.crt_tint ? "[CRT: ON]" : "[CRT: OFF]")) codex.crt_tint = !codex.crt_tint;
            ImGui::PopStyleColor(4);
            ImGui::SameLine(0, 10 * sc);
            ImGui::TextColored(col4(kCxDim), "ID %08X", e.hash);
            if (e.unlock && !text(e.unlock, lang).empty()) {
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + box);
                ImGui::TextColored(col4(kCxDim), "%s: %s", upper_ascii(ui::tr(ui::S::CodexUnlock)).c_str(), text(e.unlock, lang).c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::PopFont();
        }
        ImGui::EndGroup();

        if (two_col) ImGui::SameLine(0, col_gap);
        else ImGui::Dummy({0, 12 * sc});

        // ---- Right column: scrollable terminal body (game text carries its own header/footer) ----
        ImGui::BeginGroup();
        ImGui::PushFont(g_fonts.mono, term_font_px);
        const float cw = std::max(1.0f, ImGui::CalcTextSize("=").x);
        const float line_h = ImGui::GetTextLineHeight();
        auto eq_rule = [&](float w) {
            const size_t n = std::max<size_t>(4, size_t(w / cw));
            const std::string s(n, '=');
            ImGui::TextColored(col4(kCxGreen), "%s", s.c_str());
        };

        // Scrollable body child with thin dark scrollbar on the far right edge.
        const float body_h = std::max(80 * sc, (win_p.y + win_s.y - bar_h - pad_y) - ImGui::GetCursorScreenPos().y);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 7 * sc);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 2 * sc);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(52, 64, 56, 210));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(40, 140, 72, 230));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, kCxGreen);
        ImGui::BeginChild("cxbody", {col_right_w, body_h}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        auto* bdl = ImGui::GetWindowDrawList();
        const float text_w = std::max(100.0f, ImGui::GetContentRegionAvail().x - 10 * sc);

        // Audio log controls (when entry has voice recordings).
        if (!e.lines.empty()) {
            const uint16_t voice = audio_langs.empty() ? 0 : audio_langs[voice_index(lang)].first;
            ImGui::TextColored(col4(kCxDim), "%s", upper_ascii(ui::tr(ui::S::CodexPlayLog)).c_str());
            if (!audio_langs.empty()) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140 * sc);
                voice_selector("##cxvoice", lang);
            }
            for (size_t li : e.lines) {
                const auto& l = catalog->dialogue[li];
                if (l.wem.empty()) continue;
                play_button(uint32_t(li), 0, voice);
                ImGui::SameLine();
                const uint64_t id = clip_id(uint32_t(li), 0, voice);
                const float bar_w = std::min(240 * sc, std::max(80 * sc, text_w - 240 * sc));
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float bh = ImGui::GetFrameHeight();
                const float t = clip && clip->id() == id && clip->length() > 0 ? float(clip->position() / clip->length()) : 0.0f;
                bdl->AddRectFilled({p.x, p.y + bh * 0.5f - 1 * sc}, {p.x + bar_w, p.y + bh * 0.5f + 1 * sc}, kCxRule);
                bdl->AddRectFilled({p.x, p.y + bh * 0.5f - 1 * sc}, {p.x + bar_w * t, p.y + bh * 0.5f + 1 * sc}, kCxGreen);
                ImGui::Dummy({bar_w, bh});
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                text_line_fit(clip && clip->id() == id ? (clock_text(clip->position()) + " / " + clock_text(clip->length())).c_str() : l.wem[0].str().c_str(), kCxDim);
                const auto& audio_label = text(l.subtitle_hash, lang, l.subtitle_container);
                if (!audio_label.empty()) ImGui::SetItemTooltip("%s", audio_label.c_str());
            }
            eq_rule(text_w);
            ImGui::Dummy({0, 4 * sc});
        }

        // Body paragraphs in phosphor green monospace.
        const std::string& body = text(e.body, lang);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 5 * sc});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_w);
        if (body.empty()) {
            ImGui::TextColored(col4(kCxDim), "%s", ui::tr(ui::S::CodexNoBody));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, col4(kCxBody));
            size_t pos = 0;
            while (pos <= body.size()) {
                size_t end = body.find('\n', pos);
                if (end == std::string::npos) end = body.size();
                std::string line = body.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const bool is_rule = line.size() > 8 && line.find_first_not_of("=-_~ ") == std::string::npos;
                if (is_rule) {
                    line.resize(std::min(line.size(), size_t(text_w / cw)));
                    ImGui::TextColored(col4(kCxGreen), "%s", line.c_str());
                } else if (line.empty()) {
                    ImGui::Dummy({0, line_h * 0.55f});
                } else {
                    ImGui::TextUnformatted(line.c_str());
                }
                pos = end + 1;
            }
            ImGui::PopStyleColor();
        }
        if (e.lines.empty() && !e.asset.is_none()) {
            ImGui::Dummy({0, 6 * sc});
            ImGui::TextColored(col4(kCxDim), "%s", ui::trf(ui::S::CodexAssetNoAudio, e.asset.str()).c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
        ImGui::EndGroup();
    }

    void movies_tab() {
        if (!catalog || !catalog->movies_ready) { ImGui::TextDisabled(ui::tr(ui::S::Loading)); return; }
        const auto& movies = catalog->movies;
        const float sc = ImGui::GetStyle().FontScaleDpi;

        // left: movie list
        ImGui::BeginChild("movielist", {330 * sc, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##moviefilter", (icon(ico::Search) + ui::tr(ui::S::MovieFilterHint)).c_str(), movie.filter, sizeof(movie.filter));
        const std::string mf = lower_ascii(movie.filter);
        ImGui::TextDisabled(ui::tr(ui::S::MovieCount), movies.size(),
                            size_t(std::count_if(movies.begin(), movies.end(), [](const MovieEntry& m) { return m.info.audio.size() > 1; })));
        for (int i = 0; i < int(movies.size()); ++i) {
            const auto& m = movies[size_t(i)];
            if (!contains_ci(m.info.filename, mf) && !contains_ci(m.usm.str(), mf)) continue;
            ImGui::PushID(i);
            const bool sel = movie.selected == i;
            if (ImGui::Selectable("##m", sel, 0, {0, ImGui::GetTextLineHeight() * 2 + 4 * sc})) open_movie(i, 0, false);
            ImGui::SameLine(4 * sc);
            ImGui::BeginGroup();
            ImGui::TextUnformatted(m.info.filename.empty() ? m.usm.str().c_str() : m.info.filename.c_str());
            if (m.info.audio.size() > 1) {
                ImGui::SameLine();
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), ui::tr(ui::S::Multilingual));
            }
            ImGui::TextDisabled("%s  %ux%u  %s  %s", m.usm.str().c_str(), m.info.width, m.info.height,
                                clock_text(m.info.duration()).c_str(), format_bytes(m.size).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::SameLine();

        ImGui::BeginGroup();
        if (movie.selected < 0 || size_t(movie.selected) >= movies.size()) {
            empty_state(ico::Movies, ui::tr(ui::S::NavMovies), ui::tr(ui::S::PickMovie));
            ImGui::EndGroup();
            return;
        }
        const auto& m = movies[size_t(movie.selected)];
        auto* p = movie.player.get();
        const double pos = p ? p->position() : 0, dur = m.info.duration();
        const size_t sub_lang = movie_sub_lang();

        // video area
        const float list_h = 150 * sc, controls_h = ImGui::GetFrameHeightWithSpacing() * 2 + 8 * sc;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float area_h = std::max(80 * sc, avail.y - controls_h - list_h);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin, {origin.x + avail.x, origin.y + area_h}, IM_COL32(0, 0, 0, 255));
        const float vw = float(m.info.display_width ? m.info.display_width : m.info.width);
        const float vh = float(m.info.display_height ? m.info.display_height : m.info.height);
        const float scale = std::min(avail.x / std::max(1.0f, vw), area_h / std::max(1.0f, vh));
        const ImVec2 sz{vw * scale, vh * scale};
        const ImVec2 a{origin.x + (avail.x - sz.x) * 0.5f, origin.y + (area_h - sz.y) * 0.5f};
        if (movie.has_frame && movie.srv != UINT32_MAX)
            dl->AddImage(ImTextureID(gpu.srv().gpu(movie.srv).ptr), a, {a.x + sz.x, a.y + sz.y});
        ImGui::InvisibleButton("video", {avail.x, area_h});
        if (ImGui::IsItemClicked() && p) p->set_paused(!p->paused());

        // subtitles over the picture
        std::vector<media::MovieSubtitle> subs = p ? p->subtitles() : std::vector<media::MovieSubtitle>{};
        if (movie.sub_lang != kSubtitlesOff)
            for (const auto& s : subs)
                if (pos >= s.start && pos < s.start + s.duration) {
                    std::string t = text(s.string_hash, sub_lang);
                    if (t.empty()) t = text(s.string_hash, 0);
                    if (t.empty()) t = s.key;
                    const float wrap = sz.x * 0.8f;
                    const ImVec2 ts = ImGui::CalcTextSize(t.c_str(), nullptr, false, wrap);
                    const ImVec2 tp{a.x + (sz.x - ts.x) * 0.5f, a.y + sz.y - ts.y - 24 * sc};
                    dl->AddRectFilled({tp.x - 8 * sc, tp.y - 4 * sc}, {tp.x + ts.x + 8 * sc, tp.y + ts.y + 4 * sc}, IM_COL32(0, 0, 0, 170));
                    dl->AddText(nullptr, 0, tp, IM_COL32(255, 255, 255, 255), t.c_str(), nullptr, wrap);
                }

        // controls
        const bool playing = p && !p->paused() && !p->finished();
        if (ImGui::Button(icon(playing ? ico::Pause : ico::Play).c_str(), {36 * sc, 0})) {
            if (!p || p->finished()) open_movie(movie.selected, 0, false);
            else p->set_paused(!p->paused());
            p = movie.player.get();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(std::format("{} / {}", clock_text(movie.seeking ? movie.seek_value : pos), clock_text(dur)).c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (!movie.seeking) movie.seek_value = float(pos);
        if (ImGui::SliderFloat("##seek", &movie.seek_value, 0, float(std::max(0.001, dur)), "")) movie.seeking = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            movie.seeking = false;
            open_movie(movie.selected, movie.seek_value, p && p->paused());
            p = movie.player.get();
        } else if (!ImGui::IsItemActive()) {
            movie.seeking = false;
        }

        ImGui::SetNextItemWidth(300 * sc);
        const UsmInfo::Audio* cur = nullptr;
        for (const auto& t : m.info.audio)
            if (t.channel == movie.channel) cur = &t;
        if (begin_combo(ui::tr(ui::S::AudioTrack), cur ? movie_track_label(m, *cur).c_str() : ui::tr(ui::S::None))) {
            for (const auto& t : m.info.audio)
                if (ImGui::Selectable(movie_track_label(m, t).c_str(), t.channel == movie.channel)) {
                    movie.channel = t.channel;
                    open_movie(movie.selected, pos, p && p->paused());
                    p = movie.player.get();
                }
            ImGui::EndCombo();
        }
        if (m.info.audio.size() > 1)
            ImGui::SetItemTooltip(ui::tr(ui::S::TrackTip));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * sc);
        if (begin_combo(ui::tr(ui::S::Subtitles), movie.sub_lang == kSubtitlesOff ? ui::tr(ui::S::Off) : text_language_name(sub_lang))) {
            if (ImGui::Selectable(ui::tr(ui::S::Off), movie.sub_lang == kSubtitlesOff)) movie.sub_lang = kSubtitlesOff;
            for (int l = 0; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(text_language_name(size_t(l)), movie.sub_lang != kSubtitlesOff && size_t(l) == sub_lang))
                    movie.sub_lang = l;
            ImGui::EndCombo();
        }
        if (p) {
            if (const std::string e = p->error(); !e.empty()) {
                ImGui::SameLine();
                ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", e.c_str());
            } else if (cur && !p->audio_supported()) {
                ImGui::SameLine();
                ImGui::TextDisabled(cur->codec != 2 && cur->codec != 4 ? ui::tr(ui::S::VideoOnlyUnsupported) : ui::tr(ui::S::AudioOutputError),
                                    usm_audio_codec_name(cur->codec));
            }
        }

        // subtitle list (the subtitles are interleaved with the video, so they appear as it plays)
        if (ImGui::BeginTable("subs", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                              {0, ImGui::GetContentRegionAvail().y})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ui::tr(ui::S::Time), ImGuiTableColumnFlags_WidthFixed, 60 * sc);
            ImGui::TableSetupColumn(text_language_name(sub_lang));
            ImGui::TableSetupColumn(ui::tr(ui::S::ColEnglishKey));
            ImGui::TableHeadersRow();
            if (subs.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled(ui::tr(ui::S::NoSubtitlesYet));
            }
            for (size_t i = 0; i < subs.size(); ++i) {
                const auto& s = subs[i];
                ImGui::TableNextRow();
                ImGui::PushID(int(i));
                ImGui::TableNextColumn();
                const bool now = pos >= s.start && pos < s.start + s.duration;
                if (ImGui::Selectable(clock_text(s.start).c_str(), now, ImGuiSelectableFlags_SpanAllColumns))
                    open_movie(movie.selected, s.start, false);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", text(s.string_hash, sub_lang).c_str());
                ImGui::TableNextColumn();
                const std::string& en = text(s.string_hash, 0);
                ImGui::TextDisabled("%s", en.empty() ? s.key.c_str() : en.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndGroup();
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
                        error(ui::trf(ui::S::ErrOpenPackage, ex.what()));
                    }
                }
            }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##entfilter", ui::tr(ui::S::EntryFilterHint), entry_filter, sizeof(entry_filter));
        std::string f = lower_ascii(entry_filter);
        ImGui::BeginChild("entries", {0, ImGui::GetContentRegionAvail().y * 0.55f}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
        if (ImGui::BeginTable("entrytable", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColTag), ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColType), ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColClass), ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn(ui::tr(ui::S::ColSize));
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
                        if (ImGui::IsMouseDoubleClicked(0) && r.type == 8 &&
                            (r.reference == kClassStaticMesh || r.reference == kClassDynamicModel)) {
                            request_load(r.tag);
                            open_tab(r.reference == kClassStaticMesh ? TabStatics : TabEntities);
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%u/%u", r.type, r.subtype);
                    ImGui::TableNextColumn();
                    ImGui::Text("%08X", r.reference);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u%s", r.size, r.type != 8                          ? ""
                                                     : r.reference == kClassStaticMesh   ? "  (static mesh)"
                                                     : r.reference == kClassDynamicModel ? "  (dynamic model)"
                                                                                         : "");
                }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        ImGui::BeginChild("hex", {0, 0}, ImGuiChildFlags_Borders);
        if (!entry_preview_error.empty()) ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s", entry_preview_error.c_str());
        else if (!entry_preview.empty()) {
            ImGui::TextDisabled(ui::tr(ui::S::Bytes), entry_preview.size());
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
        if (log.empty()) {
            empty_state(ico::Log, ui::tr(ui::S::LogEmpty), "");
            return;
        }
        for (const auto& l : log) {
            if (l.error) ImGui::TextColored({1, 0.45f, 0.45f, 1}, "%s", l.text.c_str());
            else ImGui::TextUnformatted(l.text.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    }

    void shutdown() {
        movie.player.reset();
        clip.reset();
        if (export_thread.joinable()) {
            export_cancel = true;
            export_thread.join();
        }
        worker.stop();
        map_worker.stop();
        if (!gpu.removed()) gpu.wait_idle();
        for (auto& [k, t] : thumbs)
            if (t.srv != UINT32_MAX) gpu.srv().free(t.srv, 1);
        thumbs.clear();
        for (const auto& entry : retired_thumbnails) gpu.srv().free(entry.second.srv, 1);
        retired_thumbnails.clear();
        if (movie.srv != UINT32_MAX) gpu.srv().free(movie.srv, 1);
        movie.srv = UINT32_MAX;
        movie.tex.Reset();
        for (auto& u : movie.upload) u.Reset();
        game_lighting.release(gpu);
        rain.release(gpu);
        particles.release(gpu);
        scene.release(gpu);
        gb.release(gpu);
    }
};

// ---- NIM_BENCH: unattended frame-time measurement (tools/nim/frame_profile.hpp, 3.111) -------------
struct BenchDriver {
    uint32_t frames = 0, settle = 240, settled = 0;
    std::optional<V3> eye, look;
    float radius = -1;
    bool ai = false, vsync = false, done = false;
    fs::path out;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::string waiting_for;

    static std::optional<V3> vec(const char* name) {
        const char* v = std::getenv(name);
        V3 r{};
        if (!v || std::sscanf(v, "%f,%f,%f", &r.x, &r.y, &r.z) != 3) return std::nullopt;
        return r;
    }
    void init() {
        const char* f = std::getenv("NIM_BENCH");
        if (!f || !*f) return;
        frames = uint32_t(std::max(0, std::atoi(f)));
        if (!frames) return;
        eye = vec("NIM_BENCH_EYE"), look = vec("NIM_BENCH_LOOK");
        if (const char* r = std::getenv("NIM_BENCH_RADIUS")) radius = float(std::atof(r));
        if (const char* a = std::getenv("NIM_BENCH_AI")) ai = a[0] == '1';
        if (const char* v = std::getenv("NIM_BENCH_VSYNC")) vsync = v[0] == '1';
        if (const char* st = std::getenv("NIM_BENCH_SETTLE")) settle = uint32_t(std::max(0, std::atoi(st)));
        const char* o = std::getenv("NIM_BENCH_OUT");
        out = o && *o ? fs::path(to_wide(o)) : exe_dir() / "nim_bench.txt";
    }
    bool active() const { return frames && !done; }
    void configure(App& app) {
        if (radius >= 0) app.map.radius = radius <= 0 ? App::MapView::kUnlimited : radius;
        app.ai.enabled = ai;
    }
    void place_camera(App& app) const {
        if (!eye || !look) return;
        const V3 d = *eye - *look;
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len <= 0) return;
        app.cam.target = *look;
        app.cam.distance = len;
        app.cam.yaw = std::atan2(d.y, d.x);
        app.cam.pitch = std::asin(std::clamp(d.z / len, -1.0f, 1.0f));
    }
    bool ready(const App& app) {
        if (!app.map_scene) return waiting_for = "map", false;
        if (app.map.streaming || !app.scene.loaded()) return waiting_for = "map streaming", false;
        if (ai && !app.ai.sim && !app.ai.failed) return waiting_for = "ai", false;
        return true;
    }
    // Before the frame's UI: advances the phases. Returns true once the report is written.
    bool tick(App& app) {
        if (!active()) return false;
        place_camera(app);
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        if (!g_prof.collecting) {
            if (!ready(app)) {
                if (waited > 600) return finish(app, "timed out waiting for " + waiting_for);
                return false;
            }
            if (settled++ < settle) return false;
            g_prof.collecting = true;
            return false;
        }
        if (g_prof.frames() >= frames) return finish(app, {});
        return false;
    }
    bool finish(App& app, const std::string& failure) {
        done = true;
        g_prof.collecting = false;
        std::string r = std::format("nim {} benchmark: {}\n", kVersion, app.gpu.adapter_name());
        if (!failure.empty()) r += "FAILED: " + failure + "\n";
        r += std::format("map {} ({}), radius {}, viewport {}x{}, ai {} ({} agents), present {}, {} settle frames\n",
                         app.map.bubble.str(), app.map.name, app.map.radius >= App::MapView::kUnlimited ? 0.0f : app.map.radius,
                         app.gb.width(), app.gb.height(), ai ? "on" : "off", app.ai.sim ? app.ai.sim->agents().size() : 0,
                         vsync ? "vsync" : "uncapped", settle);
        r += std::format("map load: {:.3f}s from selection to ready (first piece {:.3f}s, last batch {:.3f}s)\n", app.map.load_seconds, app.map.first_piece_seconds, app.map.last_piece_seconds);
        if (eye && look)
            r += std::format("camera eye {:.1f},{:.1f},{:.1f} look {:.1f},{:.1f},{:.1f}\n", eye->x, eye->y, eye->z, look->x, look->y,
                             look->z);
        const auto [statics, lod0, entities] = app.scene.visibility_counts();
        r += std::format("scene: {} meshes, {} instances; visible {} static ({} LOD0), {} entity\n", app.scene.mesh_count(),
                         app.scene.instance_count(), statics, lod0, entities);
        r += g_prof.report();
        std::ofstream(out, std::ios::binary) << r;
        return true;
    }
};

struct RecoveryView {
    bool retry = false;
    fs::path install;
    TagHash bubble, activity, model;
    uint32_t activity_map = 0;
    std::optional<Camera> camera;
    float map_radius = 120.0f;
    int texture_cap = 1;
    ViewMode mode = ViewMode::Lit;
    bool ai = false;
    bool fx = true;
};

int run_renderer(const std::wstring& cmdline, float scale, RecoveryView& recovery) {
    const bool recovering = recovery.retry;
    auto app = std::make_unique<App>();
    try {
        // Streamline is optional. Initialize it only when the user requests frame generation.
        app->gpu.init(false, [](ID3D12Device* device) { return g_frame_generation.attach(device); }, true);
        init_swapchain(app->gpu);
        app->lighting.init(app->gpu);
    } catch (const std::exception& ex) {
        MessageBoxW(g_win.hwnd, to_wide(ui::trf(ui::S::ErrD3D12, ex.what())).c_str(), L"nim", MB_ICONERROR);
        return 1;
    }

    // Command line: [INSTALL_DIR] [STATIC_MESH / DYNAMIC_MODEL / MAP_BUBBLE_TAG] in any order.
    std::optional<fs::path> install_arg;
    std::optional<TagHash> tag_arg;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
        for (int i = 1; argv && i < argc; ++i) {
            std::wstring a = argv[i];
            if (a == L"--frame-generation") g_frame_generation_requested = !recovering && g_frame_generation.supported();
            else if (fs::is_directory(a)) install_arg = fs::path(a);
            else if (a.size() == 8 && a.find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos)
                tag_arg = TagHash(uint32_t(std::stoul(a, nullptr, 16)));
        }
        if (g_frame_generation_requested) g_frame_generation.init();
        LocalFree(argv);
    }

    if (recovering) {
        if (!recovery.install.empty()) install_arg = recovery.install;
        if (!recovery.bubble.is_none()) tag_arg = recovery.bubble;
        else if (!recovery.model.is_none()) tag_arg = recovery.model;
        g_frame_generation_requested = false;
        // Leave headroom while rebuilding after a hang, especially with window capture active.
        app->map.radius = std::min(recovery.map_radius, 120.0f);
        app->map.texture_cap = std::min(recovery.texture_cap, 1);
        app->light.ssr = false;
        app->light.volumetrics = false;
        app->light.mode = recovery.mode;
        app->fx_enabled = recovery.fx;
        app->ai.enabled = recovery.ai;
    }

    // The interface language is read before anything that shows text (fonts, the first messages).
    load_prefs();
    ui::set_lang(g_prefs.lang);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // The layout is fixed (a rail and a page), so there is no window layout to remember.
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    apply_style(scale);

    // Open the install before building fonts: the UI is set in the game's own fonts.
    if (!recovering) {
        ShowWindow(g_win.hwnd, SW_SHOWDEFAULT);
        UpdateWindow(g_win.hwnd);
    }
    if (install_arg) app->open_install(*install_arg);
    else if (auto found = PackageManager::find_install()) app->open_install(*found);
    else app->error(ui::tr(ui::S::ErrNoSteamInstall));
    load_fonts(app->pm.get());

    ImGui_ImplWin32_Init(g_win.hwnd);
    static Gpu* s_gpu = nullptr;
    s_gpu = &app->gpu;
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
        const Entry* e = app->pm->entry(*tag_arg);
        if (e && e->reference == kClassBubbleParent) {
            app->pending_map = *tag_arg;
            if (recovering && recovery.camera) {
                app->pending_activity = std::pair{recovery.activity, recovery.activity_map};
                app->pending_camera = recovery.camera;
            }
            app->tab = TabMaps;
        } else {
            app->request_load(*tag_arg);
            app->tab = e && e->reference == kClassDynamicModel ? TabEntities : TabStatics;
        }
    }
    // NIM_PAGE=<page number 0..11> opens that page first (for headless screenshots of the layout).
    if (const char* page = std::getenv("NIM_PAGE"); page && *page) {
        const int t = std::atoi(page);
        if (t >= 0 && t < TabCount) app->tab = Tab(t);
    }

    BenchDriver bench;
    bench.init();
    if (bench.active()) {
        bench.configure(*app);
        g_prof.init(app->gpu.device());
        uint64_t frequency = 1;
        app->gpu.queue()->GetTimestampFrequency(&frequency);
        g_prof.set_frequency(frequency);
    } else if (!recovering && g_prefs.auto_update && !start_update(true)) app->error(ui::tr(ui::S::UpdateUnavailable));

    uint64_t frame = 0;
    bool done = false;
    std::string gpu_failure;
    if (recovering) app->info("GPU を再初期化しました。負荷を下げて描画を再開します。");
    try {
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(g_win.hwnd)) { g_frame_generation.set_active(false); Sleep(16); continue; }
        if (g_frame_generation_requested != g_frame_generation_proxy) {
            release_swapchain(app->gpu);
            g_frame_generation.set_active(g_frame_generation_requested);
            create_swapchain(app->gpu, g_frame_generation_requested);
            g_frame_generation.reset_history();
        }
        if (g_win.pending_w && g_win.pending_h) {
            resize_swapchain(app->gpu, g_win.pending_w, g_win.pending_h);
            g_win.pending_w = g_win.pending_h = 0;
        }

        const uint32_t fi = uint32_t(frame % Gpu::kFramesInFlight);
        if (bench.tick(*app)) {
            done = true;
            break;
        }
        g_prof.begin_frame();
        {
            const auto ps = g_prof.scope(Prof::Wait);
            if (g_win.waitable) WaitForSingleObjectEx(g_win.waitable, 1000, FALSE);
            app->gpu.wait(g_win.fence[fi]);
        }
        g_prof.read_slot(fi);
        app->gpu.begin_frame(fi);
        const bool generation_scene = g_frame_generation_proxy && app->map_scene && !app->map.streaming &&
            app->frame_generation_rect_valid && app->light.mode == ViewMode::Lit && app->scene.loaded() &&
            GetForegroundWindow() == g_win.hwnd;
        g_frame_generation.set_active(generation_scene);
        g_frame_generation.begin_frame(uint32_t(frame));
        g_frame_generation.mark(FrameGeneration::Marker::simulation_start);
        {
            const auto ps = g_prof.scope(Prof::Pending);
            app->apply_pending(app->gpu);
        }
        g_prof.subtract(Prof::Pending, g_prof.current(Prof::Stream) + g_prof.current(Prof::AiSim) + g_prof.current(Prof::AiActors));

        std::optional<Prof::Scope> ui_scope(std::in_place, g_prof, Prof::Ui);
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app->ui();
        ImGui::Render();
        ui_scope.reset();

        if (!app->frame_generation_rect_valid || !app->map_scene || app->map.streaming)
            g_frame_generation.set_active(false);
        g_frame_generation.mark(FrameGeneration::Marker::simulation_end);
        g_frame_generation.mark(FrameGeneration::Marker::render_start);
        auto* cl = g_win.list.Get();
        check_hr(g_win.alloc[fi]->Reset(), "allocator Reset");
        check_hr(cl->Reset(g_win.alloc[fi].Get(), nullptr), "list Reset");
        g_prof.set_list(cl, fi);
        try {
            app->record_uploads(cl, fi);
            if (app->viewport_visible && !app->map.streaming) app->record(cl);
        } catch (const std::exception& ex) {
            app->gpu.check_device();
            app->error(ui::trf(ui::S::ErrRender, ex.what()));
        }
        std::optional<Prof::Scope> submit_scope(std::in_place, g_prof, Prof::Submit);
        const UINT bb = g_win.swapchain->GetCurrentBackBufferIndex();
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float clear[4] = {0.016f, 0.016f, 0.016f, 1};
        cl->ClearRenderTargetView(g_win.rtv[bb], clear, 0, nullptr);
        cl->OMSetRenderTargets(1, &g_win.rtv[bb], FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {app->gpu.srv().heap()};
        cl->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl);
        Gpu::barrier(cl, g_win.back[bb].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        g_prof.gpu_mark(Prof::GUi);
        g_prof.resolve();
        check_hr(cl->Close(), "Close");
        ID3D12CommandList* lists[] = {cl};
        app->gpu.queue()->ExecuteCommandLists(1, lists);
        submit_scope.reset();
        g_frame_generation.mark(FrameGeneration::Marker::render_end);
        g_frame_generation.mark(FrameGeneration::Marker::present_start);
        std::optional<Prof::Scope> present_scope(std::in_place, g_prof, Prof::Present);
        const bool uncapped = app->map.streaming || (bench.active() && !bench.vsync);
        const HRESULT present = g_win.swapchain->Present(uncapped ? 0 : 1, uncapped && g_win.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
        present_scope.reset();
        if (FAILED(present)) {
            app->gpu.check_device();
            check_hr(present, "Present");
        }
        g_frame_generation.mark(FrameGeneration::Marker::present_end);
        g_frame_generation.after_present();
        g_win.fence[fi] = app->gpu.signal();
        ++frame;
        app->after_frame();
        g_prof.end_frame();
    }

    app->gpu.wait_idle();
    } catch (const std::exception& ex) {
        gpu_failure = ex.what();
        if (!app->gpu.removed()) {
            try {
                app->gpu.check_device();
            } catch (const std::exception& removed) {
                gpu_failure = removed.what();
            }
        }
    }

    const bool retry = !recovering && !bench.active() && app->gpu.removed();
    if (retry) {
        recovery.retry = true;
        recovery.install = app->install;
        recovery.fx = app->fx_enabled;
        recovery.ai = app->ai.enabled;
        recovery.mode = app->light.mode;
        if (app->map_scene) {
            recovery.bubble = app->map.bubble;
            recovery.activity = app->map.activity;
            recovery.activity_map = app->map.activity_map;
            recovery.camera = app->cam;
            recovery.map_radius = app->map.radius;
            recovery.texture_cap = app->map.texture_cap;
        } else if (app->scene.loaded()) {
            recovery.model = app->scene.tag();
        }
    } else if (!gpu_failure.empty()) {
        const auto error = app->gpu.removed() ? ui::trf(ui::S::ErrDeviceRemoved, gpu_failure) : gpu_failure;
        MessageBoxW(g_win.hwnd, to_wide(error).c_str(), L"nim", MB_ICONERROR);
    }
    if (app->loader.joinable()) app->loader.join();
    app->shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_window_gpu(app->gpu);
    g_frame_generation.shutdown();
    g_frame_generation_requested = g_frame_generation_proxy = false;
    return retry ? 2 : (gpu_failure.empty() ? 0 : 1);
}

int run(const std::wstring& cmdline) {
    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wnd_proc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"nim", nullptr};
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    g_win.hwnd = CreateWindowW(wc.lpszClassName, L"nim — Marathon asset viewer", WS_OVERLAPPEDWINDOW, 80, 60,
                               int(1680 * scale), int(1000 * scale), nullptr, nullptr, wc.hInstance, nullptr);
    {  // dark frame, the shadow and rounded corners kept while the UI draws the title bar
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_win.hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        const MARGINS m{0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(g_win.hwnd, &m);
        SetWindowPos(g_win.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    }

    RecoveryView recovery;
    int result = run_renderer(cmdline, scale, recovery);
    if (result == 2) result = run_renderer(cmdline, scale, recovery);
    DestroyWindow(g_win.hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdline, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int r = run(std::wstring(L"nim ") + cmdline);
    if (g_update_process) CloseHandle(g_update_process);
    CoUninitialize();
    return r;
}
