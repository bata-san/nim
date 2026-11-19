// nim — Marathon asset viewer (GUI).
//
// Dear ImGui (docking) over D3D12, laid out after goriath: a row of document tabs, a HOME page of
// big buttons, and browsers whose right-hand panel is a live 3D view. Meshes are drawn with the
// game's own G-buffer shaders and, by default, shaded by the game's own deferred lighting pass;
// the UI itself is set in the game's fonts, read from the user's install at startup.
#include "media.hpp"
#include "nim/content/animation.hpp"
#include "nim/content/audio.hpp"
#include "nim/content/movies.hpp"
#include "nim/content/dialogue.hpp"
#include "nim/content/fonts.hpp"
#include "nim/content/map.hpp"
#include "nim/content/strings.hpp"
#include "nim/content/weather.hpp"
#include "nim/core/package_manager.hpp"
#include "nim/render/game_lighting.hpp"
#include "nim/content/object_channels.hpp"
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
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

using namespace nim;
namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.3.1";
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
// Display names of the string-table language slots (TextLanguage order).
const char* text_language_name(size_t l) {
    static constexpr const char* k[] = {"英語", "日本語", "ドイツ語", "フランス語", "スペイン語", "スペイン語 (中南米)", "イタリア語",
                                        "韓国語", "繁体字中国語", "簡体字中国語", "ポルトガル語 (ブラジル)", "ポーランド語", "ロシア語"};
    return l < std::size(k) ? k[l] : "?";
}
// Localized audio packages end in a language suffix (w64_sr_audio_015a_jpn).
std::string audio_language_name(const std::string& suffix) {
    static const std::pair<const char*, size_t> k[] = {{"en", 0},   {"jpn", 1},  {"ja", 1},   {"de", 2},  {"fr", 3},
                                                       {"es", 4},   {"esmx", 5}, {"mx", 5},   {"it", 6},  {"ko", 7},
                                                       {"kor", 7},  {"cht", 8},  {"zht", 8},  {"chs", 9}, {"zhs", 9},
                                                       {"ptbr", 10}, {"pt", 10}, {"pl", 11},  {"ru", 12}};
    for (const auto& [s, l] : k)
        if (suffix == s) return text_language_name(l);
    return suffix;
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
                   Movies = 0xE714, Play = 0xE768, Pause = 0xE769, Stop = 0xE71A, Volume = 0xE767, Folder = 0xE8B7;
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
    std::atomic<bool> meshes_ready{false}, strings_ready{false}, dialogue_ready{false}, movies_ready{false}, done{false};
    std::vector<MeshEntry> meshes;
    std::vector<PackageGroup> packages;  // packages that hold static meshes
    std::vector<MeshEntry> dynamics;     // dynamic models (entity meshes)
    std::vector<PackageGroup> dyn_packages;
    StringTable strings;
    std::vector<DialogueLine> dialogue;
    std::vector<MovieEntry> movies;
    std::string error;
};

void load_catalog(fs::path install, Catalog* cat) {
    try {
        PackageManager pm(install);
        {
            std::lock_guard lk(cat->mutex);
            cat->status = "静的メッシュを列挙中…";
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
            cat->status = "ムービーを列挙中…";
        }
        cat->dialogue_ready = true;
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
        // A preview may hold more than one model. Each clip belongs to one model's rig.
        std::optional<PreviewAnimation> animation;
        std::optional<PreviewAnimation> part_animation;
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
        thumbnail_cache_ = PrepareCache{512};
        materials_.reset();
        rigs_.reset();
        clips_.reset();
        assemblies_.clear();
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
            int permutation = -1;
            ChannelValues channels;
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return quit_ || live_ || !thumbs_.empty(); });
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
                const Entry* e = pm->entry(tag);
                if (e && e->reference == kClassDynamicModel) {
                    // The entity components hold the materials; index them once.
                    if (!materials_) materials_ = index_dynamic_model_materials(*pm);
                    if (!rigs_) rigs_ = index_model_skeletons(*pm, *materials_, &assemblies_);
                    r.mesh = prepare_entity_preview(*pm, tag, *materials_, assemblies_, permutation,
                                                    channels.empty() ? nullptr : &channels, thumb ? &thumbnail_cache_ : nullptr);
                    const auto skinned = [](const PreparedMesh& m) {
                        return std::ranges::any_of(m.buffers, [](const auto& b) { return !b.skin.bones.empty(); });
                    };
                    if (!thumb && !r.mesh->no_drawable &&
                        (rigs_->contains(tag.value) || !r.mesh->attachments.empty() || skinned(*r.mesh))) {
                        try {
                            if (!clips_) clips_ = index_animation_clips(*pm);
                            r.animation = prepare_preview_animation(*pm, *r.mesh, *rigs_, *clips_);
                            if (r.animation && r.animation->model != tag) {
                                auto local = prepare_preview_animation(*pm, *r.mesh, *rigs_, *clips_, tag);
                                if (local && !local->clips.empty()) r.part_animation = std::move(local);
                            }
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
            if (thumb) thumbnail_cache_.textures.clear();
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
    int live_permutation_ = -1;
    ChannelValues live_channels_;
    std::optional<std::unordered_map<uint32_t, DynamicModelMaterials>> materials_;
    std::optional<ModelRigIndex> rigs_;
    ModelAssemblyIndex assemblies_;
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
        std::shared_ptr<const MapWeather> weather;  // the map's world channel controllers
        std::shared_ptr<const MapAtmosphere> atmosphere;  // the map's sky lookups and sun path
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
    uint64_t request(const MapEntry& selection, std::optional<std::array<float, 3>> center, float radius, uint32_t texture_cap) {
        std::lock_guard lk(m_);
        request_ = Request{++generation_, selection.bubble, center, radius, texture_cap, selection.activity, selection.bubble_name, selection.activity_map};
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
        pieces_.pop_front();
        cv_.notify_all();
        return p;
    }
    Progress progress() {
        std::lock_guard lk(m_);
        return progress_;
    }
    bool drained(uint64_t generation) {
        std::lock_guard lk(m_);
        return progress_.generation == generation && progress_.done && pieces_.empty();
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
                const bool selection_changed = !layout || layout->bubble != req.bubble || layout_activity != req.activity || layout_activity_map != req.activity_map;
                if (selection_changed) {
                    const auto tables = req.activity.is_none() ? std::vector<TagHash>{} : load_activity_node_tables(*pm, req.activity, req.bubble_name, req.activity_map);
                    layout = load_map_layout(*pm, req.bubble, tables);
                    layout_activity = req.activity;
                    layout_activity_map = req.activity_map;
                    entities.reset();
                    starts = find_map_starts(*pm, *layout);
                }
                if (!cache || cache->max_texture_dim != req.texture_cap) {
                    cache.emplace();
                    cache->max_texture_dim = req.texture_cap;
                    cache->all_lods = true;
                }
                const auto center = req.center ? *req.center : !starts.empty() ? starts.front().position : map_focus(*layout);
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
                const size_t first_sky = region.size();
                for (auto& model : load_map_sky_objects(*pm, *layout)) region.push_back(std::move(model));
                std::sort(world_jobs.begin(), world_jobs.end());
                std::vector<size_t> jobs;
                jobs.reserve(terrain.size() + region.size());
                // The skyline must be visible while the thousands of world meshes are loading.
                for (size_t i = first_sky; i < region.size(); ++i) jobs.push_back(terrain.size() + i);
                for (size_t i = 0; i < terrain.size(); ++i) jobs.push_back(i);
                for (const auto& [distance, index] : world_jobs) jobs.push_back(index);
                auto lights = std::make_shared<std::vector<MapLight>>();
                try {
                    const auto all = load_map_lights(*pm, *layout);
                    for (const MapLight* l : map_light_region(all, center, req.radius)) lights->push_back(*l);
                } catch (const std::exception&) {
                    // a map whose node tables cannot be read is drawn without local lights
                }
                auto cubemaps = std::make_shared<std::vector<MapCubemap>>();
                // Surface probe failures through the worker's existing error path; silently
                // continuing with zero probes makes a data failure look like missing lighting.
                *cubemaps = load_map_cubemaps(*pm, *layout);
                if (selection_changed || !weather || weather_bubble_ != req.bubble) {
                    try {
                        weather = std::make_shared<const MapWeather>(load_map_weather(*pm, req.bubble));
                    } catch (const std::exception&) {
                        weather = std::make_shared<const MapWeather>();  // no weather controls
                    }
                    try {
                        atmosphere = std::make_shared<const MapAtmosphere>(load_map_atmosphere(*pm, *layout));
                    } catch (const std::exception&) {
                        atmosphere = std::make_shared<const MapAtmosphere>();  // drawn without a sky
                    }
                    weather_bubble_ = req.bubble;
                }
                {
                    std::lock_guard lk(m_);
                    if (generation_ != req.generation) continue;
                    progress_.weather = weather;
                    progress_.atmosphere = atmosphere;
                    progress_.total = region.size() + terrain.size();
                    progress_.center = center;
                    progress_.extent = extent;
                    progress_.typical = typical;
                    progress_.start = starts.empty() || req.center ? std::nullopt : std::optional<MapStart>(starts.front());
                    progress_.lights = std::move(lights);
                    progress_.cubemaps = std::move(cubemaps);
                }
                // Prepare sky, terrain and then nearby world assets in parallel.
                // Every core but the UI thread's.
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
                            if (j < terrain.size()) {
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
                                    &*cache);
                                if (piece.mesh.no_drawable) throw std::runtime_error("entity model with no drawable parts");
                                piece.rows = std::move(model.rows);
                                piece.sky_order = std::move(model.sky_order);
                            }
                        } catch (const std::exception& ex) {
                            failure = (j < terrain.size() ? "terrain: " : j - terrain.size() >= static_count ? "entity: " : "")
                                    + std::string(ex.what()).substr(0, 110);
                        }
                        std::unique_lock lk(m_);
                        // Back-pressure: the UI adds a few meshes per frame.
                        cv_.wait(lk, [&] { return quit_ || generation_ != req.generation || pieces_.size() < 64; });
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
                work(*pm);
                for (auto& t : threads) t.join();
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

enum class Engine : int { Game };

enum Tab : int { TabHome, TabSettings, TabStatics, TabEntities, TabMaps, TabText, TabDialogue, TabMovies, TabPackages, TabLog, TabCount };
struct TabInfo { uint32_t icon; const char* title; const char* id; bool closable; };
constexpr TabInfo kTabs[TabCount] = {
    {ico::Home, "HOME", "home", false},         {ico::Settings, "", "settings", false},
    {ico::Statics, "STATICS", "statics", true}, {ico::Entities, "ENTITIES", "entities", true},
    {ico::Maps, "MAPS", "maps", true},
    {ico::Text, "TEXT", "text", true},
    {ico::Dialogue, "DIALOGUE", "dialogue", true}, {ico::Movies, "MOVIES", "movies", true},
    {ico::Packages, "PACKAGES", "packages", true},
    {ico::Log, "LOG", "log", true},
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
        int selection_mode = 0;  // DEIMOS MapTab / ActivityTab
        char filter[64] = "";
        int selected = -1;     // index into map_worker.maps()
        static constexpr float kUnlimited = 5000;
        float radius = kUnlimited;  // default to the user's whole-world workflow
        bool lights_set = false;
        int texture_cap = 3;   // kMapTextureCaps index (full resolution)
        uint64_t generation = 0;
        bool streaming = false, framed = false;
        TagHash bubble;
        std::string name;
        TagHash activity;
        uint32_t activity_map = 0;
        std::array<float, 3> center{};
        float typical = 0;              // Progress::typical
        std::optional<MapStart> start;  // Progress::start
    };
    MapView map;
    bool map_scene = false;  // the live scene is a map region
    std::optional<TagHash> pending_map;  // from the command line, loaded once the maps are listed
    static constexpr uint32_t kMapTextureCaps[] = {256, 512, 1024, 0};

    // live view
    StaticMeshScene scene;
    GBuffer gb;
    LightingPass lighting;
    GameLighting game_lighting;
    bool game_ready = false;
    Engine engine = Engine::Game;
    std::string render_error;
    LightingParams light;
    Camera cam;
    uint32_t view_w = 0, view_h = 0;
    bool viewport_visible = false;
    std::optional<TagHash> loading;
    std::optional<TagHash> no_drawable_tag;
    std::string no_drawable_reason;
    bool capture_requested = false;
    bool show_fps = true;
    TagHash scene_component;
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
        AnimationClip loaded;
        std::string error;
        bool playing = true, dirty = false;
        float frame = 0, speed = 1;
    };
    std::optional<Animation> anim;
    static constexpr float kClipFps = 30;

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
    bool tab_open[TabCount] = {true, true, true, true, true, true, true, true, true, false};
    int focus_tab = -1, focus_frames = 0;

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
    std::vector<std::pair<uint16_t, std::string>> audio_langs;  // installed localized audio packages
    int audio_lang = 0;           // index into audio_langs

    // audio playback (dialogue) and WAV export
    std::unique_ptr<media::AudioEngine> audio_engine_;
    std::unique_ptr<media::ClipPlayer> clip;
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
            map_worker.start(dir);
            map = MapView{};
            map_scene = false;
            game_lighting.clear_lights(gpu);
            game_lighting.clear_cubemaps(gpu);
            game_lighting.clear_atmosphere(), map_atmosphere.reset();
            entity_preview_active = thumb_preview_active = false;
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
            error(std::format("ゲームのライティングを使えません (代替描画は無効): {}", ex.what()));
        }
        try {
            thumb_lighting.release(gpu);
            thumb_lighting.init(*pm, gpu);
            thumb_game_ready = true;
        } catch (...) {
            thumb_game_ready = false;
        }
    }

    bool set_entity_preview(GameLighting& renderer, bool& active) {
        if (active) return true;
        try {
            active = renderer.set_entity_preview(*pm, gpu);
        } catch (const std::exception& ex) {
            error(std::format("エンティティの環境光: {}", ex.what()));
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
        if (!ImGui::CollapsingHeader(std::format("推定値 ({})###estimates", all.size()).c_str())) return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("ゲームデータが無い入力 (マップ未読込・実行時の状態) に nim が入れた値");
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
        if (!ImGui::CollapsingHeader(std::format("オブジェクトの状態 (チャネル {})###channels", used.size()).c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen))
            return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("マテリアルが読む値 (氷結・ヒートカスケード・ダメージ・ディゾルブなど)。名前が不明なものはハッシュ表示");
        ImGui::PopTextWrapPos();
        bool changed = false;
        if (ImGui::BeginTable("##channels", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("チャネル", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("段", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("値", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();
            for (const auto& [hash, use] : used) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const char* name = object_channel_name(hash);
                const bool overridden = channel_overrides.count(hash) != 0;
                if (overridden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.8f, 0.3f, 1});
                ImGui::TextUnformatted(name ? name : std::format("{:08X}", hash).c_str());
                if (overridden) ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s%s", use.vs ? "VS" : "", use.ps ? (use.vs ? "+PS" : "PS") : "");
                ImGui::TableNextColumn();
                float4 v = overridden ? channel_overrides[hash] : use.value;
                ImGui::PushID(int(hash));
                const ImGuiStyle& st = ImGui::GetStyle();
                ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("01").x + st.FramePadding.x * 4 + st.ItemSpacing.x * 2));
                if (ImGui::DragFloat4("##v", v.data(), 0.01f, -10.0f, 10.0f, "%.2f")) {
                    channel_overrides[hash] = v;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("0")) { channel_overrides[hash] = {0, 0, 0, 0}; changed = true; }
                ImGui::SameLine();
                if (ImGui::SmallButton("1")) { channel_overrides[hash] = {1, 1, 1, 1}; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!channel_overrides.empty() && ImGui::SmallButton("既定値に戻す")) {
            channel_overrides.clear();
            changed = true;
        }
        if (changed) scene.set_channels(channel_overrides);
    }

    void request_load(TagHash tag) {
        if (loading && *loading == tag) return;
        stop_map();
        channel_overrides.clear();
        loading = tag;
        no_drawable_tag.reset();
        no_drawable_reason.clear();
        worker.request_live(tag);
    }

    // Runs at the top of a frame, before any UI references GPU resources.
    void apply_pending(Gpu& g) {
        if (view_w && view_h && (view_w != gb.width() || view_h != gb.height())) {
            g.wait_idle();
            gb.resize(g, view_w, view_h);
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
                    info(std::format("{} を読込 ({} グループ, 準備 {:.0f} ms + GPU {:.0f} ms){}", r->tag.str(),
                                     scene.groups().size(), r->ms, up,
                                     stubbed ? std::format(", 未実装 TFX op {} 種", stubbed) : ""));
                } catch (const std::exception& ex) {
                    scene.release(g);
                    error(std::format("{} を描画できません: {}", r->tag.str(), ex.what()));
                }
            } else if (!no_drawable) {
                error(std::format("{} を読めません: {}", r->tag.str(), r->error));
            }
        }
        // Preparing a card submits its uploads synchronously. Never put that work on each
        // viewport frame or interrupt mouse-driven camera movement.
        if (!map.streaming && frame_no % 6 == 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
            make_thumbnail(g);
        try {
            stream_map(g);
        } catch (const std::exception& ex) {
            stop_map();
            error(std::format("マップの読込を停止: {}", ex.what()));
        }
    }

    // ---- maps ----
    void stop_map() {
        if (!map.streaming) return;
        map_worker.cancel();
        map.streaming = false;
    }
    void load_map(int index, std::optional<std::array<float, 3>> center) {
        const auto& maps = map_worker.maps();
        if (index < 0 || index >= int(maps.size())) return;
        const auto& m = maps[size_t(index)];
        loading.reset();  // a live mesh still being prepared is discarded when it arrives
        map_add_failures.clear();
        gpu.wait_idle();
        scene.begin(gpu);
        anim.reset();
        channel_overrides.clear();
        const bool same = map_scene && map.bubble == m.bubble;
        map.framed = same && center.has_value();  // reloading around the camera keeps the view
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
        const bool whole = map.radius >= MapView::kUnlimited;
        map.generation = map_worker.request(m, center, whole ? 1e9f : map.radius, kMapTextureCaps[map.texture_cap]);
        info(whole ? std::format("マップ {} ({}) を読込: 全域", m.package, m.bubble.str())
                   : std::format("マップ {} ({}) を読込: 半径 {:.0f} m", m.package, m.bubble.str(), map.radius));
    }
    void stream_map(Gpu& g) {
        if (!map.streaming) return;
        const auto t0 = std::chrono::steady_clock::now();
        bool added = false;
        const auto upload_budget = std::chrono::milliseconds(map.framed ? 6 : 12);
        // Only new GPU objects are created (nothing the frame in flight uses), so no GPU wait.
        while (std::chrono::steady_clock::now() - t0 < upload_budget) {
            auto piece = map_worker.take(map.generation);
            if (!piece) break;
            added = true;
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
        if (pr.weather && map_weather != pr.weather) {
            map_weather = pr.weather;
            weather_states.assign(map_weather->controllers.size(), -1);
            apply_weather();
        }
        if (pr.atmosphere && map_atmosphere != pr.atmosphere) {
            map_atmosphere = pr.atmosphere;
            game_lighting.set_atmosphere(*pm, g, map_atmosphere.get());
            for (const auto& n : game_lighting.atmosphere_notes()) ++map_add_failures[n.substr(0, 120)];
        }
        if (pr.lights && !map.lights_set) {
            map.lights_set = true;
            std::vector<const MapLight*> ptrs;
            for (const auto& l : *pr.lights) ptrs.push_back(&l);
            map_lights = pr.lights;  // kept alive while the lighting refers to it
            try {
                game_lighting.set_lights(*pm, g, ptrs);
            } catch (const std::exception& ex) {
                ++map_add_failures[std::string("local lights: ") + std::string(ex.what()).substr(0, 100)];
            }
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
        if (added) {
            scene.finish(g, map.bubble, V3{pr.center[0], pr.center[1], pr.center[2]}, std::min(map.radius, pr.extent) * 0.6f);
            if (!map.framed && scene.loaded()) {
                frame_scene();
                if (map.start)
                    info(std::format("カメラ: プレイヤー開始地点 (推定, 80802D32) ({:.1f}, {:.1f}, {:.1f}) + 目の高さ {:.1f} m (推定)", map.start->position[0],
                                     map.start->position[1], map.start->position[2], kStartEyeHeight));
                map.framed = true;
            }
        }
        if (map_worker.drained(map.generation)) {
            map.streaming = false;
            info(std::format("マップ {}: {} メッシュ / {} インスタンス{}", map.name, scene.mesh_count(), scene.instance_count(),
                             pr.failed ? std::format(" ({} スキップ)", pr.failed) : ""));
        }
    }
    std::map<std::string, size_t> map_add_failures;
    std::shared_ptr<const std::vector<MapLight>> map_lights;
    std::shared_ptr<const std::vector<MapCubemap>> map_cubemaps;
    std::shared_ptr<const MapAtmosphere> map_atmosphere;
    bool entity_preview_active = false, thumb_preview_active = false;
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
        if (ImGui::Checkbox("ヒートカスケード", &heat_cascade_enabled)) apply_weather();
        ImGui::EndDisabled();
        if (map_weather->heat_cascade_controller)
            ImGui::SetItemTooltip("%s\n%s\n状態の値は範囲の中央 (推定)。対応する元のマテリアルに適用。",
                map_weather->heat_cascade_controller->name.c_str(),
                map_weather->heat_cascade_shared ? "Outpost の元の状態を共用 (他マップへの適用は推定)" : "このマップの元の状態");
        else ImGui::SetItemTooltip("インストール内に元のヒートカスケード状態が見つかりません");
        if (ImGui::CollapsingHeader(std::format("天候とワールドチャンネル ({})###mapweather", cs.size()).c_str())) {
            const float w = ImGui::GetContentRegionAvail().x * 0.6f;
            for (size_t i = 0; i < cs.size(); ++i) {
                const auto& c = cs[i];
                if (c.heat_cascade()) continue; // controlled by the persistent toggle above
                ImGui::PushID(int(i));
                const int cur = weather_states[i];
                const std::string preview = cur < 0 ? "既定" : weather_state_label(c.states[size_t(cur)]);
                ImGui::SetNextItemWidth(w);
                if (ImGui::BeginCombo("##state", preview.c_str())) {
                    if (ImGui::Selectable("既定", cur < 0)) weather_states[i] = -1, apply_weather();
                    for (size_t k = 0; k < c.states.size(); ++k)
                        if (ImGui::Selectable(std::format("{}##{}", weather_state_label(c.states[k]), k).c_str(), cur == int(k)))
                            weather_states[i] = int(k), apply_weather();
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", c.name.c_str());
                ImGui::PopID();
            }
            ImGui::TextDisabled("状態の値は範囲の中央 (推定: ゲームは範囲内で選び、遷移時間で補間)");
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
            ImGui::TextDisabled("マップを列挙中…");
            return;
        }
        if (const auto err = map_worker.list_error(); !err.empty()) {
            ImGui::SetCursorPos({20 * sc, 20 * sc});
            ImGui::TextColored({1, 0.45f, 0.45f, 1}, "マップを列挙できません: %s", err.c_str());
            return;
        }
        const auto& maps = map_worker.maps();
        if (pending_map) {
            for (size_t i = 0; i < maps.size(); ++i)
                if (maps[i].bubble == *pending_map && maps[i].activity.is_none()) { load_map(int(i), std::nullopt); break; }
            pending_map.reset();
        }
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float left_w = 300 * sc, pad = 10 * sc;
        const auto grey = IM_COL32(150, 150, 156, 255);

        // left: maps, then the region controls
        ImGui::BeginChild("maps", {left_w, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos({pad, pad});
        ImGui::SetNextItemWidth(left_w - 2 * pad);
        if (ImGui::Combo("##map_view_kind", &map.selection_mode, "map_view\0activity_view\0")) map.selected = -1;
        ImGui::SetItemTooltip("DEIMOS と同じ選択元: map_view = SBubbleParent / activity_view = SActivity のマップ参照 + ambient + phase");
        ImGui::SetNextItemWidth(left_w - 2 * pad);
        ImGui::InputTextWithHint("##mfilter", (icon(ico::Search) + "  マップ / タグ").c_str(), map.filter, sizeof(map.filter));
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
            dl->AddText({pos.x + pad, pos.y + 2 * sc}, IM_COL32_WHITE, (m.name.empty() ? m.package : m.name).c_str());
            dl->AddText({pos.x + pad, pos.y + 2 * sc + lh}, grey,
                        (m.activity.is_none() ? std::format("{}   {} メッシュ · {} 配置", m.bubble.str(), m.statics, m.instances) :
                        std::format("activity {} / map[{}] → {}", m.activity.str(), m.activity_map, m.bubble.str())).c_str());
            ImGui::PopID();
        }
        if (maps.empty()) {
            ImGui::SetCursorPosX(pad);
            ImGui::TextDisabled("静的ジオメトリを持つマップがありません");
        }
        ImGui::EndChild();

        // region controls
        ImGui::SetCursorPos({pad, ImGui::GetCursorPosY() + pad});
        ImGui::BeginGroup();
        const float w = left_w - 2 * pad;
        ImGui::SetNextItemWidth(w);
        ImGui::SliderFloat("##radius", &map.radius, 50, MapView::kUnlimited,
                           map.radius >= MapView::kUnlimited ? "範囲  無制限" : "範囲  %.0f m", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("領域の中心からこの距離にある配置を読み込みます (右端 = マップ全域)");
        ImGui::SetNextItemWidth(w);
        const char* caps[] = {"テクスチャ  256", "テクスチャ  512", "テクスチャ  1024", "テクスチャ  原寸"};
        ImGui::Combo("##texcap", &map.texture_cap, caps, 4);
        ImGui::SetItemTooltip("テクスチャの最大辺 (VRAM と読込時間の節約。原寸 = ゲームのデータそのまま)");
        ImGui::BeginDisabled(map.selected < 0);
        if (dbutton("カメラの位置で読込", w)) {
            const bool here = map_scene && map.selected >= 0 && maps[size_t(map.selected)].bubble == map.bubble;
            load_map(map.selected, here ? std::optional(std::array<float, 3>{cam.target.x, cam.target.y, cam.target.z}) : std::nullopt);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("いまのカメラの注視点を中心に、範囲内を読み込み直します");
        if (map_scene) {
            const auto pr = map_worker.progress();
            if (map.streaming && pr.generation == map.generation)
                ImGui::TextDisabled("読込中  %zu / %zu", pr.prepared, pr.total);
            else
                ImGui::TextDisabled("%zu メッシュ · %zu 配置", scene.mesh_count(), scene.instance_count());
            ImGui::TextDisabled("中心 (%.0f, %.0f, %.0f)", map.center[0], map.center[1], map.center[2]);
            ImGui::TextDisabled("原プローブ %zu 領域 / 描画 %zu", game_lighting.cubemap_count(), game_lighting.cubemaps_drawn());
        }
        ImGui::EndGroup();
        ImGui::EndChild();

        // right: the live view, map details below
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("mapscene", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        if (details_h <= 0) details_h = 220 * sc;
        const float view_h_px = std::max(120 * sc, ImGui::GetContentRegionAvail().y - details_h - 6 * sc);
        const ImVec2 view_origin = ImGui::GetCursorScreenPos();
        const float view_w_px = ImGui::GetContentRegionAvail().x;
        viewport_ui({view_w_px, view_h_px});
        if (map.streaming) {
            const auto pr = map_worker.progress();
            const float t = pr.total ? float(pr.prepared) / float(pr.total) : 0.0f;
            const float y = view_origin.y + view_h_px - 2 * sc;
            auto* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled({view_origin.x, y}, {view_origin.x + view_w_px, y + 2 * sc}, IM_COL32(255, 255, 255, 30));
            dl->AddRectFilled({view_origin.x, y}, {view_origin.x + view_w_px * t, y + 2 * sc}, kGreen);
        }
        ImGui::InvisibleButton("##mhsplit", {ImGui::GetContentRegionAvail().x, 6 * sc});
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) details_h = std::max(40 * sc, details_h - ImGui::GetIO().MouseDelta.y);
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::IsItemActive() ? kGreen : IM_COL32(4, 4, 4, 255));
        ImGui::BeginChild("mapdetails", {0, 0}, ImGuiChildFlags_None);
        map_details_ui();
        ImGui::EndChild();
        ImGui::EndChild();
    }

    void map_details_ui() {
        if (!map_scene) {
            ImGui::TextDisabled("マップを選ぶと、周辺の静的ジオメトリをゲームのシェーダで描画します");
            ImGui::TextDisabled("右ドラッグ + WASD / QE で移動 (Shift 速く · Ctrl 遅く · ホイールで速度)");
            return;
        }
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::TextUnformatted(map.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", map.bubble.str().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("コピー")) ImGui::SetClipboardText(map.bubble.str().c_str());
        if (map.activity.is_none()) ImGui::TextDisabled("map_view · SBubbleParent %s", map.bubble.str().c_str());
        else ImGui::TextDisabled("activity_view · SActivity %s / map[%u] → %s", map.activity.str().c_str(), map.activity_map, map.bubble.str().c_str());
        weather_ui();
        ImGui::TextDisabled("右ドラッグ + WASD / QE で移動 (Shift 速く · Ctrl 遅く · ホイールで速度)");
        const auto pr = map_worker.progress();
        std::map<std::string, size_t> skipped = map_add_failures;
        if (pr.generation == map.generation)
            for (const auto& [why, n] : pr.failures) skipped[why] += n;
        size_t total = 0;
        for (const auto& [why, n] : skipped) total += n;
        if (total) {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader(std::format("スキップしたメッシュ ({})###mapskipped", total).c_str())) {
                ImGui::PushTextWrapPos(0);
                for (const auto& [why, n] : skipped) ImGui::TextDisabled("%zu  %s", n, why.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("未対応: マップの大気 / 空・雨や VFX のパーティクル・ギミックの動き (推定値は下に記載)");
        ImGui::PopTextWrapPos();
        estimates_panel();
    }

    void make_thumbnail(Gpu& g) {
        auto r = worker.take(true);
        if (!r) return;
        Thumb& t = thumbs[r->tag.value];
        t.last_used = frame_no;
        t.failed = false;
        t.no_drawable = false;
        if (r->mesh && r->mesh->no_drawable) {
            t.no_drawable = true;
            t.error = r->mesh->notes.empty() ? "no drawable geometry" : r->mesh->notes.front();
            return;
        }
        if (!r->mesh) {
            t.failed = true;
            t.error = r->error;
            return;
        }
        try {
            g.wait_idle();
            thumb_scene.upload(g, std::move(*r->mesh), layouts);
            const bool preview = thumb_game_ready && set_entity_preview(thumb_lighting, thumb_preview_active);
            thumb_gb.resize(g, kThumbSize, kThumbSize);
            ViewMatrices vm = make_view(frame_sphere(thumb_scene.center(), thumb_scene.radius(), kThumbSize, kThumbSize));
            auto* cl = g.immediate();
            thumb_scene.render_gbuffer(g, cl, thumb_gb, vm);
            LightingParams lp = light;
            lp.mode = ViewMode::Lit;
            lp.time = lp.delta_time = 0;
            lp.atmosphere = false;
            lp.entity_preview = preview;
            lp.auto_exposure = lp.temporal_aa = false;
            if (thumb_game_ready) {
                GameLighting::TransparentFn transparents;
                if (thumb_scene.has_transparent()) transparents = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
                    thumb_scene.render_transparent(g, c, thumb_gb, hdr, vm, thumb_lighting.externs());
                };
                thumb_lighting.render(g, cl, thumb_gb, vm, lp, transparents, nullptr, thumb_scene.has_visible_light_grid());
            } else throw std::runtime_error("Original game lighting is unavailable; thumbnail rendering stopped");
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
        // Inside a map the camera walks among the geometry: a near plane at human scale.
        v.near_z = map_scene ? 0.05f : std::max(0.001f, cam.radius * 0.005f);
        return v;
    }

    void select_clip(int i) {
        if (!anim || i < 0 || i >= int(anim->binding.clips.size())) return;
        anim->clip = i;
        anim->frame = 0;
        anim->error.clear();
        try {
            anim->loaded = load_animation_clip(*pm, anim->binding.clips[size_t(i)].tag);
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
        scene.set_skin_poses(a.binding.poses(a.loaded, a.frame));
    }

    void animation_panel() {
        if (!anim || !scene.skinned()) return;
        Animation& a = *anim;
        ImGui::Spacing();
        if (!ImGui::CollapsingHeader(std::format("アニメーション (クリップ {})###anim", a.binding.clips.size()).c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen))
            return;
        if (a.alternate && ImGui::Checkbox("選択パーツのクリップ", &a.part_only)) {
            std::swap(a.binding, *a.alternate);
            a.clip = -1;
            a.error.clear();
            a.dirty = true;
        }
        const auto& binding = a.binding;
        const auto& clips = binding.clips;
        ImGui::TextDisabled("スケルトン %s (%zu ボーン)   リグ %s", binding.skeleton.component.str().c_str(), binding.skeleton.size(),
                            binding.rig.component.str().c_str());
        if (binding.inferred_binding) ImGui::TextDisabled("スケルトンとリグの対応は推定 (共通ボーン・スキンの一致)");
        if (!binding.followers.empty()) ImGui::TextDisabled("連動パーツ %zu", binding.followers.size());
        if (binding.model != a.model) ImGui::TextDisabled("再生元パーツ %s", binding.model.str().c_str());
        if (clips.empty()) {
            ImGui::TextDisabled("対応するクリップが見つかりません");
            return;
        }
        if (clips.front().bone_groups != binding.rig.bone_groups)
            ImGui::TextDisabled("ボーン構成が同じリグのクリップを使用");
        const float em = ImGui::GetFontSize();
        ImGui::SetNextItemWidth(-1);
        const std::string preview = a.clip < 0 ? std::string("バインドポーズ")
                                               : std::format("{}  ({} フレーム)", clips[size_t(a.clip)].tag.str(),
                                                             clips[size_t(a.clip)].frames);
        if (ImGui::BeginCombo("##clip", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::Selectable("バインドポーズ", a.clip < 0)) {
                a.clip = -1;
                a.dirty = true;
            }
            ImGuiListClipper clip;
            clip.Begin(int(clips.size()));
            while (clip.Step())
                for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                    const auto& c = clips[size_t(i)];
                    if (ImGui::Selectable(std::format("{}  {} フレーム##{}", c.tag.str(), c.frames, i).c_str(), a.clip == i)) select_clip(i);
                }
            ImGui::EndCombo();
        }
        if (!a.error.empty()) ImGui::TextColored({1, 0.45f, 0.4f, 1}, "%s", a.error.c_str());
        if (a.clip < 0) return;
        if (ImGui::Button(a.playing ? "一時停止" : "再生", {em * 5, 0})) a.playing = !a.playing;
        ImGui::SameLine();
        if (ImGui::Button("<")) select_clip(a.clip > 0 ? a.clip - 1 : int(clips.size()) - 1);
        ImGui::SameLine();
        if (ImGui::Button(">")) select_clip((a.clip + 1) % int(clips.size()));
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %s%s", clip_codec_name(a.loaded.codec), is_additive(binding.rig, a.loaded) ? "加算  " : "",
                            std::format("{:.1f} 秒", float(a.loaded.frame_count) / kClipFps).c_str());
        ImGui::SetNextItemWidth(-1);
        const float last = float(std::max(1u, a.loaded.frame_count) - 1);
        if (ImGui::SliderFloat("##frame", &a.frame, 0, last, "フレーム %.1f")) {
            a.playing = false;
            a.dirty = true;
        }
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##speed", &a.speed, 0.1f, 2.0f, "速度 %.2fx");
    }

    void record(ID3D12GraphicsCommandList* cl) {
        if (!gb.lit) return;
        update_animation(ImGui::GetIO().DeltaTime);
        const float material_dt = material_playing ? ImGui::GetIO().DeltaTime * material_speed : 0;
        material_time += material_dt;
        scene.set_material_frame(material_time, material_dt);
        if (game_ready) scene.set_exposure(game_lighting.exposure());
        light.time = material_time;
        light.delta_time = material_dt;
        light.entity_preview = entity_preview_active && !map_scene;
        if (game_ready) {
            scene.set_global_channel_values(game_lighting.update_global_channels(light));
            scene.set_global_channels(game_lighting.world_channels());
            scene.set_material_time_of_day(game_lighting.time_of_day(light));
        }
        ViewMatrices vm = game_ready && light.mode == ViewMode::Lit ? game_lighting.temporal_view(current_view(), light) : make_view(current_view());
        try {
            SunShadow* shadow = nullptr;
            if (shadows && light.mode == ViewMode::Lit && game_ready) {
                if (!sun_shadow.depth) sun_shadow.resize(gpu, 4096);
                sun_shadow.fit_view(game_lighting.sun_toward(light), vm.eye, cam.target, scene.center(), scene.radius(), map_scene);
                scene.render_sun_shadow(gpu, cl, sun_shadow, map_scene);
                shadow = &sun_shadow;
            }
            // Shadowing local lights: a couple of depth maps per frame for the lights on screen,
            // once the map is complete (a growing scene would discard them again).
            game_lighting.set_light_shadows(shadows);
            if (shadows && light.mode == ViewMode::Lit && game_ready && map_scene && !map.streaming && scene.loaded())
                for (SunShadow* s : game_lighting.light_shadows_to_render(gpu, cl, vm, scene.item_count(), 2)) scene.render_shadow(gpu, cl, *s);
            if (map_scene && !map.streaming && scene.loaded() && (!rain.valid || rain.items != scene.item_count())) {
                if (!rain.mask) rain.resize(gpu, 4096);
                scene.render_rain_occlusion(gpu, cl, rain, scene.center(), scene.radius());
            }
            scene.set_rain_occlusion(map_scene && rain.valid ? &rain : nullptr);
            game_lighting.set_overhead_occlusion(map_scene && rain.valid ? &rain : nullptr);
            scene.render_gbuffer(gpu, cl, gb, vm);
            if (light.mode == ViewMode::Lit) {
                if (!game_ready) throw std::runtime_error("Original game lighting is unavailable; no substitute renderer");
                GameLighting::TransparentFn transparents;
                if (scene.has_transparent()) transparents = [&](ID3D12GraphicsCommandList* c, D3D12_CPU_DESCRIPTOR_HANDLE hdr) {
                    scene.render_transparent(gpu, c, gb, hdr, vm, game_lighting.externs());
                };
                game_lighting.render(gpu, cl, gb, vm, light, transparents, shadow, scene.has_visible_light_grid());
            } else lighting.render(gpu, cl, gb, vm, light);
            render_error.clear();
        } catch (const std::exception& ex) {
            if (render_error != ex.what()) error(std::format("描画停止 (代替なし): {}", ex.what()));
            render_error = ex.what();
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
        media_tick();
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
            const bool pad0 = t == TabStatics || t == TabEntities || t == TabMaps;
            if (pad0) ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            bool visible = ImGui::Begin(tab_title(t).c_str(), kTabs[t].closable ? &tab_open[t] : nullptr,
                                        ImGuiWindowFlags_NoCollapse);
            if (pad0) ImGui::PopStyleVar();
            if (visible) {
                switch (t) {
                case TabHome: home_tab(); break;
                case TabSettings: settings_tab(); break;
                case TabStatics: statics_tab(false); break;
                case TabEntities: statics_tab(true); break;
                case TabMaps: maps_tab(); break;
                case TabText: strings_tab(); break;
                case TabDialogue: dialogue_tab(); break;
                case TabMovies: movies_tab(); break;
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
            if (dbutton((icon(ico::Entities) + "  ENTITIES").c_str(), bw)) open_tab(TabEntities);
            ImGui::SetItemTooltip("エンティティのダイナミックモデル (剛体・スキン) をエンティティのマテリアルで表示");
            if (dbutton((icon(ico::Maps) + "  MAPS").c_str(), bw)) open_tab(TabMaps);
            ImGui::SetItemTooltip("マップの配置済み静的ジオメトリを読み込んで歩き回る");
            ImGui::TableNextColumn();
            heading("2D");
            ImGui::Dummy({0, 4 * sc});
            if (dbutton((icon(ico::Text) + "  TEXT").c_str(), bw)) open_tab(TabText);
            ImGui::SetItemTooltip("ローカライズ文字列 (全言語)");
            if (dbutton((icon(ico::Dialogue) + "  DIALOGUE").c_str(), bw)) open_tab(TabDialogue);
            ImGui::SetItemTooltip("台詞を全言語で表示し、音声を再生・話者ごとに書き出す");
            if (dbutton((icon(ico::Movies) + "  MOVIES").c_str(), bw)) open_tab(TabMovies);
            ImGui::SetItemTooltip("ムービー (USM) を音声トラック・字幕の言語を選んで再生");
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
        ImGui::TextUnformatted("ゲームのシェーダのみ (代替描画なし)");
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
            ImGui::TextDisabled("メッシュを列挙中…");
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
        } else if (it != thumbs.end() && it->second.no_drawable) {
            const char* t = "未対応パス";
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
        ImGui::SetItemTooltip("ライト・露出");
        if (ImGui::BeginPopup("tune")) {
            ImGui::SliderFloat("露出", &light.exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderAngle("太陽の方位", &light.sun_azimuth, -180, 180);
            ImGui::SliderAngle("太陽の高度", &light.sun_elevation, -10, 90);
            ImGui::SliderFloat("太陽の強さ", &light.sun_intensity, 0, 10);
            ImGui::Checkbox("太陽の影", &shadows);
            ImGui::SetItemTooltip("nim のシャドウマップ (ゲームのカスケードは未解読のため推定)");
            ImGui::Checkbox("SSAO", &light.ssao);
            ImGui::SetItemTooltip("ゲームの hdao と apply_ssao_to_light_buffers (調整値は推定)");
            ImGui::Checkbox("スクリーンスペース反射", &light.ssr);
            ImGui::SetItemTooltip("ゲームの hi_z_generate_* / ssr_* コンピュートパス (設定値は推定)");
            ImGui::Checkbox("ライトシャフト", &light.volumetrics);
            ImGui::SetItemTooltip("ライトの volumetrics 技法 → blur → upres (霧密度スライスは生成元未特定のため 0 = 推定)");
            ImGui::Checkbox("ポストプロセス", &light.post);
            ImGui::SetItemTooltip("ゲームの screen_area_global_lut3d (トーンマップ・グレーディング)。オフで final_combine");
            ImGui::BeginDisabled(!light.post);
            ImGui::Checkbox("自動露出", &light.auto_exposure);
            ImGui::SetItemTooltip("autoexposure_sample_columns の読み戻し (キー値・適応速度は推定)。露出スライダーは補正");
            ImGui::SameLine();
            ImGui::Checkbox("ブルーム", &light.bloom);
            ImGui::SetItemTooltip("ゲームの縮小・ガウスぼかし・加算パス (強さ・カーネルは推定)");
            ImGui::Checkbox("TAA", &light.temporal_aa);
            ImGui::SetItemTooltip("ゲームの resolve_taa (ジッター + 動きベクトル + 履歴)。重み・ジッター列は推定。オンの間は FXAA の代わり");
            ImGui::SameLine();
            ImGui::Checkbox("FXAA", &light.antialias);
            ImGui::EndDisabled();
            if (light.post && light.auto_exposure && game_ready) ImGui::TextDisabled("露出 %.3f", game_lighting.exposure());
            ImGui::ColorEdit3("太陽の色", &light.sun_color.x);
            ImGui::SliderFloat("空の強さ", &light.sky_intensity, 0, 4);
            ImGui::ColorEdit3("空 (天頂)", &light.sky_top.x);
            ImGui::ColorEdit3("空 (地平)", &light.sky_horizon.x);
            ImGui::ColorEdit3("地面", &light.ground.x);
            ImGui::ColorEdit3("背景 (上)", &light.background_top.x);
            ImGui::ColorEdit3("背景 (下)", &light.background_bottom.x);
            ImGui::SeparatorText("大気 (マップの空とフォグ)");
            ImGui::BeginDisabled(!game_lighting.has_atmosphere());
            ImGui::Checkbox("大気", &light.atmosphere);
            ImGui::SetItemTooltip("ゲームの sky_lookup_generate_near/far と sky。フォグ値はグローバルチャンネルの写し (推定)");
            ImGui::Checkbox("フロクセル大気", &light.froxel_fog);
            ImGui::SetItemTooltip("ゲームの密度計算・散乱積分CS。格子の解像度・距離・時間履歴は推定");
            ImGui::Checkbox(std::format("マップ環境 ({} チャンネル)", game_lighting.environment_channels()).c_str(), &light.environment);
            ImGui::SetItemTooltip("マップの環境エンティティのチャンネル式 (太陽・環境光・霧の時刻カーブ) を時刻で評価");
            ImGui::Checkbox("空の環境光", &light.sky_ambient);
            ImGui::SetItemTooltip("ゲームの空半球マップ (hemisphere_sky_color_generate ほか) と cubemap_apply_sky_copy_ao。日陰の環境光");
            ImGui::Checkbox("頭上の遮蔽", &light.overhead_occlusion);
            ImGui::SetItemTooltip("cubemap_apply_sky_copy_ao_with_overhead_occlusion: 屋根の下などで空の環境光を弱める。真上からの深度は nim の描画 (推定)、分散マップはゲームの sample_and_blur_variance_map_9x9");
            ImGui::Checkbox("プローブの距離フェード (推定)", &light.probe_distance_fade);
            ImGui::SetItemTooltip("既定では全域の原プローブ照明を描画。オンでカメラ距離によるフェードを使用 (エンジンの判定条件は未解明)。各プローブの空間範囲・色・強度は元データのまま");
            {
                bool own = light.time_of_day < 0;
                if (ImGui::Checkbox("初期時刻を使う (推定)", &own)) light.time_of_day = own ? -1.0f : game_lighting.default_time_of_day();
                ImGui::SetItemTooltip("太陽パス成分 +0x08 の値を時刻と仮定。アクティビティ実行時の開始時刻は未特定のため、ゲーム本来の時刻を保証しない");
                ImGui::BeginDisabled(own);
                float t = own ? game_lighting.default_time_of_day() : light.time_of_day;
                const int minutes = int(t * 24 * 60 + 0.5f);
                if (ImGui::SliderFloat("時刻", &t, 0.0f, 1.0f, std::format("{:02}:{:02}", minutes / 60 % 24, minutes % 60).c_str()))
                    light.time_of_day = t;
                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
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
        ImGui::TextUnformatted("GAME SHADING / STRICT");
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
        if (!render_error.empty()) {
            hud_text(dl, {origin.x + 12 * sc, origin.y + bar_h + 12 * sc}, IM_COL32(255, 100, 100, 255),
                     "描画停止: ゲーム由来の入力が不足しています (代替描画なし)");
            ImGui::TextWrapped("%s", render_error.c_str());
        }
        // bottom-left HUD / messages
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
                ImGui::TextDisabled("未対応パス: %s", no_drawable_reason.c_str());
                return;
            }
            ImGui::TextDisabled("メッシュ未選択");
            return;
        }
        ImGui::PushFont(g_fonts.bold, 0);
        ImGui::Text("%s %s", scene.dynamic() ? "DYNAMIC" : "STATIC", scene.tag().str().c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("コピー")) ImGui::SetClipboardText(scene.tag().str().c_str());
        ImGui::TextDisabled("LOD %u   グループ %zu   半径 %.3f", scene.lod(), scene.groups().size(), scene.radius());
        if (scene.dynamic()) {
            if (scene_component.is_none()) {
                ImGui::TextDisabled("エンティティコンポーネント: なし (パート自身のテクニックのみ)");
            } else {
                ImGui::TextDisabled("エンティティコンポーネント %s", scene_component.str().c_str());
                if (scene_permutation_count > 1) {
                    int perm = int(scene_permutation);
                    ImGui::SetNextItemWidth(200 * ImGui::GetStyle().FontScaleDpi);
                    if (ImGui::SliderInt("マテリアル バリアント", &perm, 0, int(scene_permutation_count) - 1) &&
                        perm != int(scene_permutation)) {
                        loading = scene.tag();
                        worker.request_live(scene.tag(), perm, channel_overrides);
                        scene_permutation = uint32_t(perm);
                    }
                }
            }
            for (const auto& n : scene.notes()) ImGui::TextColored({1, 0.75f, 0.3f, 1}, "%s", n.c_str());
        }
        animation_panel();
        if (ImGui::CollapsingHeader("マテリアルのアニメーション", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("再生##material", &material_playing);
            ImGui::SameLine();
            if (ImGui::SmallButton("先頭に戻す##material")) material_time = 0;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##material_time", &material_time, 0.01f, 0, 86400, "時刻 %.2f 秒")) material_playing = false;
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##material_speed", &material_speed, 0.0f, 4.0f, "速度 %.2fx");
        }
        channels_panel();
        estimates_panel();
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
    // Text in any of the 13 languages, the line's audio in any installed voice language, lines
    // filed by speaker or by conversation table, and WAV export into one folder per speaker.
    size_t dialogue_lang() const { return size_t(dialogue_text_lang < 0 ? language : dialogue_text_lang); }

    std::string speaker_name(uint32_t hash, size_t lang) {
        if (hash == 0 || hash == 0x811C9DC5) return "(話者なし)";
        const std::string& t = text(hash, lang);
        if (!t.empty()) return t;
        const std::string& en = text(hash, 0);
        return en.empty() ? std::format("(不明 {:08X})", hash) : en;
    }

    void ensure_audio_languages() {
        if (!audio_langs.empty() || !pm) return;
        std::map<uint16_t, std::string> found;
        for (const auto& p : pm->packages())
            if (p.language != 0 && !found.contains(p.language)) {
                const auto us = p.name.rfind('_');
                found[p.language] = us == std::string::npos ? std::to_string(p.language) : p.name.substr(us + 1);
            }
        for (auto& [id, suffix] : found) audio_langs.emplace_back(id, audio_language_name(suffix));
        // default to the voice that matches the second text language, else English
        const std::string want = text_language_name(size_t(language));
        for (size_t i = 0; i < audio_langs.size(); ++i)
            if (audio_langs[i].second == want) audio_lang = int(i);
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
                                         auto bytes = fork->read(wem);
                                         return std::make_shared<const PcmAudio>(decode_wem(bytes));
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
                if (counts.size() > 2) who += " 他";
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
        const auto [lang_id, lang_name] = audio_langs[size_t(audio_lang)];
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
                std::string snippet = safe_file_name(text(l.subtitle_hash, lang));
                if (snippet.size() > 48) snippet = utf8_prefix(snippet, 48);
                if (!snippet.empty()) name += "_" + snippet;
                std::string file = name + ".wav";
                for (int n = 2; !used_names.insert(folder + "/" + file).second; ++n) file = std::format("{}_{}.wav", name, n);
                jobs->push_back({l.wem[w], dir / to_wide(folder) / to_wide(file)});
                csv += quote(folder) + "," + quote(file) + "," + l.table.str() + std::format(",{:08X},", l.line_id) +
                       quote(speaker_name(l.speaker_hash, 0)) + "," + quote(speaker_name(l.speaker_hash, lang));
                for (size_t t = 0; t < kTextLanguageCount; ++t) csv += "," + quote(text(l.subtitle_hash, t));
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
            export_message = std::format("{} 本を {} 音声で書き出し中…", jobs->size(), lang_name);
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
                    const auto pcm = decode_wem(fork->read(j.wem));
                    if (!write_wav_file(j.file, pcm)) throw std::runtime_error("書き込み失敗");
                } catch (const std::exception& ex) {
                    if (std::string_view(ex.what()).find("no recording") != std::string_view::npos) export_missing++;
                    else export_failed++;
                }
                export_done++;
            }
            std::lock_guard lk(export_mutex);
            export_message = std::format("{}{} / {} 本を書き出しました ({} 音声, 未収録 {}, 失敗 {}): {}",
                                         export_cancel ? "中止: " : "",
                                         export_done.load() - export_failed.load() - export_missing.load(),
                                         export_total.load(), lang_name, export_missing.load(), export_failed.load(), to_utf8(dir.wstring()));
            export_running = false;
        });
    }

    void dialogue_tab() {
        if (!catalog || !catalog->dialogue_ready) { ImGui::TextDisabled("読込中…"); return; }
        ensure_audio_languages();
        const auto& lines = catalog->dialogue;
        const float sc = ImGui::GetStyle().FontScaleDpi;
        const size_t lang = dialogue_lang();

        // toolbar
        ImGui::SetNextItemWidth(std::max(160 * sc, ImGui::GetContentRegionAvail().x * 0.35f));
        ImGui::InputTextWithHint("##dlgfilter", (icon(ico::Search) + "  話者・台詞で検索").c_str(), dialogue_filter,
                                 sizeof(dialogue_filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * sc);
        if (ImGui::BeginCombo("字幕", text_language_name(lang))) {
            for (int l = 0; l < int(kTextLanguageCount); ++l)
                if (ImGui::Selectable(text_language_name(size_t(l)), size_t(l) == lang)) dialogue_text_lang = l;
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::Checkbox("英語を併記", &dialogue_show_english);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130 * sc);
        if (audio_langs.empty()) {
            ImGui::TextDisabled("音声パッケージなし");
        } else if (ImGui::BeginCombo("音声", audio_langs[size_t(audio_lang)].second.c_str())) {
            for (int i = 0; i < int(audio_langs.size()); ++i)
                if (ImGui::Selectable(audio_langs[size_t(i)].second.c_str(), audio_lang == i)) audio_lang = i;
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("インストール済みの音声言語パッケージ (Steam の言語設定で追加できます)");
        ImGui::SameLine();
        ImGui::BeginDisabled(export_running || dialogue_view.empty() || audio_langs.empty());
        if (ImGui::Button((icon(ico::Save) + "  書き出し…").c_str()))
            if (auto dir = pick_folder(g_win.hwnd)) start_dialogue_export(*dir);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("表示中の台詞を WAV で話者ごとのフォルダに書き出し (index.csv に全言語のテキスト)");
        if (export_running) {
            ImGui::SameLine();
            ImGui::ProgressBar(export_total ? float(export_done) / float(export_total) : 0, {140 * sc, 0},
                               std::format("{} / {}", export_done.load(), export_total.load()).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("中止")) export_cancel = true;
        } else {
            std::lock_guard lk(export_mutex);
            if (!export_message.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", export_message.c_str());
            }
        }
        if (!audio_error.empty()) ImGui::TextColored({1, 0.4f, 0.4f, 1}, "音声エラー: %s", audio_error.c_str());

        if (dialogue_groups_mode != dialogue_group_mode || dialogue_groups_lang != int(lang)) rebuild_dialogue_groups();

        // left: filing by speaker / table
        ImGui::BeginChild("dlggroups", {280 * sc, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        if (ImGui::RadioButton("話者", dialogue_group_mode == 0)) dialogue_group_mode = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("会話テーブル", dialogue_group_mode == 1)) dialogue_group_mode = 1;
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##grpfilter", "絞り込み", dialogue_group_filter, sizeof(dialogue_group_filter));
        const std::string gf = lower_ascii(dialogue_group_filter);
        if (ImGui::Selectable(std::format("すべて  ({})", lines.size()).c_str(), dialogue_group < 0)) dialogue_group = -1;
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
                return f.empty() || contains_ci(text(l.speaker_hash, 0), f) || contains_ci(text(l.subtitle_hash, 0), f) ||
                       text(l.subtitle_hash, lang).find(dialogue_filter) != std::string::npos ||
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
        ImGui::TextDisabled("%zu / %zu 行", dialogue_view.size(), lines.size());
        const uint16_t voice = audio_langs.empty() ? 0 : audio_langs[size_t(audio_lang)].first;
        if (ImGui::BeginTable("dialogue", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                                                 ImGuiTableFlags_BordersOuter,
                              {0, ImGui::GetContentRegionAvail().y - details_h})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30 * sc);
            ImGui::TableSetupColumn("SPEAKER", ImGuiTableColumnFlags_WidthFixed, 150 * sc);
            ImGui::TableSetupColumn("LINE");
            ImGui::TableSetupColumn("TABLE", ImGuiTableColumnFlags_WidthFixed, 80 * sc);
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
                                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                        dialogue_selected = int(li);
                    ImGui::PopFont();
                    const std::string& en = text(l.subtitle_hash, 0);
                    const std::string& tr = text(l.subtitle_hash, lang);
                    if (ImGui::BeginPopupContextItem("ctx")) {
                        if (ImGui::MenuItem("表示言語をコピー")) ImGui::SetClipboardText(tr.c_str());
                        if (ImGui::MenuItem("英語をコピー")) ImGui::SetClipboardText(en.c_str());
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
        ImGui::TextDisabled("行 %08X  テーブル %s  音声イベント %s", l.line_id, l.table.str().c_str(),
                            l.audio_event.is_none() ? "-" : l.audio_event.str().c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 24 * ImGui::GetStyle().FontScaleDpi);
        if (ImGui::SmallButton("×")) dialogue_selected = -1;
        if (l.wem.empty()) {
            ImGui::TextDisabled("音声なし");
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
                            fork->set_language(audio_langs[size_t(audio_lang)].first);
                            const auto pcm = decode_wem(fork->read(l.wem[w]));
                            const fs::path file = *dir / to_wide(safe_file_name(std::format(
                                "{}_{:08X}_{}.wav", speaker_name(l.speaker_hash, lang), l.line_id, audio_langs[size_t(audio_lang)].second)));
                            if (!write_wav_file(file, pcm)) throw std::runtime_error("書き込めません");
                            info("保存しました: " + to_utf8(file.wstring()));
                        } catch (const std::exception& ex) {
                            error(std::string("WAV を保存できません: ") + ex.what());
                        }
                    }
                ImGui::SetItemTooltip("選択中の音声言語で WAV に保存");
                ImGui::PopID();
            }
            if (clip && clip->playing() && (clip->id() & 0xFFFFFFFF) == uint64_t(li) + 1)
                ImGui::ProgressBar(float(clip->position() / std::max(0.001, clip->length())), {-1, 4 * ImGui::GetStyle().FontScaleDpi}, "");
        }
        if (ImGui::BeginTable("langs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("言語", ImGuiTableColumnFlags_WidthFixed, 150 * ImGui::GetStyle().FontScaleDpi);
            ImGui::TableSetupColumn("話者", ImGuiTableColumnFlags_WidthFixed, 140 * ImGui::GetStyle().FontScaleDpi);
            ImGui::TableSetupColumn("台詞");
            for (size_t t = 0; t < kTextLanguageCount; ++t) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", text_language_name(t));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(speaker_name(l.speaker_hash, t).c_str());
                ImGui::TableNextColumn();
                const std::string& s = text(l.subtitle_hash, t);
                ImGui::PushID(int(t));
                ImGui::TextWrapped("%s", s.c_str());
                if (ImGui::BeginPopupContextItem("c")) {
                    if (ImGui::MenuItem("コピー")) ImGui::SetClipboardText(s.c_str());
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            if (l.caption_hash && l.caption_hash != 0x811C9DC5 && !text(l.caption_hash, 0).empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("キャプション");
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", text(l.caption_hash, lang).c_str());
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    }

    // -- MOVIES --
    // Default track of a localized movie: the one for the second text language (estimated
    // mapping, channel = text language slot + 1), else the first language track.
    int default_movie_channel(const MovieEntry& m) const {
        if (m.info.audio.empty()) return 0;
        if (m.info.audio.size() > 1) {
            for (const auto& a : m.info.audio)
                if (a.channel == language + 1) return a.channel;
            for (const auto& a : m.info.audio)
                if (a.channel == 1) return 1;
        }
        return m.info.audio.front().channel;
    }

    static std::string movie_track_label(const MovieEntry& m, const UsmInfo::Audio& a) {
        std::string name;
        if (m.info.audio.size() > 1 && a.channel >= 1 && size_t(a.channel) <= kTextLanguageCount)
            name = std::format("{} (推定)", text_language_name(size_t(a.channel - 1)));
        else
            name = m.info.audio.size() > 1 ? "既定" : "音声";
        name += std::format("  ch{} {} {}ch", a.channel, usm_audio_codec_name(a.codec), a.channels);
        if (a.codec != 2) name += " 未対応";
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
                audio_engine();
                if (clip) clip->play(pcm, audio_request->id);
                audio_error.clear();
            } catch (const std::exception& ex) {
                audio_error = ex.what();
                if (audio_error.find("no recording") != std::string::npos) audio_error = "この言語の音声は収録されていません";
                error("音声を再生できません: " + audio_error);
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
            error(std::string("ムービーのフレームを転送できません: ") + ex.what());
        }
    }

    static std::string clock_text(double s) {
        const int t = int(std::max(0.0, s));
        return std::format("{}:{:02}", t / 60, t % 60);
    }

    size_t movie_sub_lang() const { return size_t(movie.sub_lang < 0 ? language : movie.sub_lang); }

    void movies_tab() {
        if (!catalog || !catalog->movies_ready) { ImGui::TextDisabled("読込中…"); return; }
        const auto& movies = catalog->movies;
        const float sc = ImGui::GetStyle().FontScaleDpi;

        // left: movie list
        ImGui::BeginChild("movielist", {330 * sc, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##moviefilter", (icon(ico::Search) + "  ファイル名で検索").c_str(), movie.filter, sizeof(movie.filter));
        const std::string mf = lower_ascii(movie.filter);
        ImGui::TextDisabled("%zu 本 (多言語音声 %zu)", movies.size(),
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
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kGreen), "多言語");
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
            ImGui::TextDisabled("左の一覧からムービーを選ぶと再生します。");
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
        if (ImGui::BeginCombo("音声トラック", cur ? movie_track_label(m, *cur).c_str() : "なし")) {
            for (const auto& t : m.info.audio)
                if (ImGui::Selectable(movie_track_label(m, t).c_str(), t.channel == movie.channel)) {
                    movie.channel = t.channel;
                    open_movie(movie.selected, pos, p && p->paused());
                    p = movie.player.get();
                }
            ImGui::EndCombo();
        }
        if (m.info.audio.size() > 1)
            ImGui::SetItemTooltip("言語トラックの割り当てはゲームデータに無いため推定です\n(チャンネル番号 = 文字列の言語スロット + 1。繁体字中国語の ch9 は欠番)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * sc);
        if (ImGui::BeginCombo("字幕", movie.sub_lang == kSubtitlesOff ? "オフ" : text_language_name(sub_lang))) {
            if (ImGui::Selectable("オフ", movie.sub_lang == kSubtitlesOff)) movie.sub_lang = kSubtitlesOff;
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
                ImGui::TextDisabled(cur->codec != 2 ? "この音声 (%s) は未対応のため映像のみ" : "音声出力を開けません",
                                    usm_audio_codec_name(cur->codec));
            }
        }

        // subtitle list (the subtitles are interleaved with the video, so they appear as it plays)
        if (ImGui::BeginTable("subs", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter,
                              {0, ImGui::GetContentRegionAvail().y})) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("時刻", ImGuiTableColumnFlags_WidthFixed, 60 * sc);
            ImGui::TableSetupColumn(text_language_name(sub_lang));
            ImGui::TableSetupColumn("英語 / キー");
            ImGui::TableHeadersRow();
            if (subs.empty()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("字幕は再生に合わせて読み込まれます (このムービーにはまだ字幕がありません)");
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
        movie.player.reset();
        clip.reset();
        if (export_thread.joinable()) {
            export_cancel = true;
            export_thread.join();
        }
        worker.stop();
        map_worker.stop();
        gpu.wait_idle();
        for (auto& [k, t] : thumbs)
            if (t.srv != UINT32_MAX) gpu.srv().free(t.srv, 1);
        thumbs.clear();
        if (movie.srv != UINT32_MAX) gpu.srv().free(movie.srv, 1);
        movie.srv = UINT32_MAX;
        movie.tex.Reset();
        for (auto& u : movie.upload) u.Reset();
        game_lighting.release(gpu);
        thumb_lighting.release(gpu);
        rain.release(gpu);
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

    // Command line: [INSTALL_DIR] [STATIC_MESH / DYNAMIC_MODEL / MAP_BUBBLE_TAG] in any order.
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
        const Entry* e = app->pm->entry(*tag_arg);
        if (e && e->reference == kClassBubbleParent) {
            app->pending_map = *tag_arg;
            app->focus_tab = TabMaps;
        } else {
            app->request_load(*tag_arg);
            app->focus_tab = e && e->reference == kClassDynamicModel ? TabEntities : TabStatics;
        }
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
            app->record_uploads(cl, fi);
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
        const HRESULT present = g_win.swapchain->Present(1, 0);
        if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
            // The GPU faulted: every later frame would only wait out timeouts, so say so and quit.
            std::string why = "GPU error";
            try {
                app->gpu.check_device();
            } catch (const std::exception& ex) {
                why = ex.what();
            }
            MessageBoxW(g_win.hwnd,
                        to_wide("GPU がエラーで停止したため終了します (device removed)。\n"
                                "NIM_D3D_DEBUG=1 を設定して起動すると nim_d3d.log に詳細が出ます。\n\n" + why)
                            .c_str(),
                        L"nim", MB_ICONERROR);
            break;
        }
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
