#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "ui_text.hpp"

#include <algorithm>
#include <cmath>

namespace nim::chrome {

// shadcn-style neutral surfaces, white primary controls, and red only for errors.
namespace theme {
constexpr ImU32 kBg = IM_COL32(10, 10, 10, 255);           // background
constexpr ImU32 kRail = IM_COL32(17, 17, 17, 255);       // sidebar, title bar and status line
constexpr ImU32 kSurface = IM_COL32(22, 22, 22, 255);    // cards
constexpr ImU32 kSurfaceHi = IM_COL32(38, 38, 38, 255);  // secondary controls
constexpr ImU32 kHover = IM_COL32(31, 31, 31, 255);
constexpr ImU32 kActive = IM_COL32(38, 38, 38, 255);
constexpr ImU32 kBorder = IM_COL32(43, 43, 43, 255);
constexpr ImU32 kRing = IM_COL32(140, 140, 140, 255);
constexpr ImU32 kText = IM_COL32(250, 250, 250, 255);
constexpr ImU32 kMuted = IM_COL32(163, 163, 163, 255);
constexpr ImU32 kError = IM_COL32(248, 113, 113, 255);
constexpr ImU32 kAccentHi = IM_COL32(245, 245, 245, 255);
constexpr ImU32 kPrimaryText = IM_COL32(23, 23, 23, 255);
constexpr ImU32 kPrimaryHover = IM_COL32(224, 224, 224, 255);
constexpr ImU32 kPrimaryPressed = IM_COL32(204, 204, 204, 255);
} // namespace theme

inline ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

inline void apply_style(float scale) {
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
    c[ImGuiCol_PopupBg] = rgb(12, 12, 12, 0.99f);
    c[ImGuiCol_Border] = col4(theme::kBorder);
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = col4(theme::kSurface);
    c[ImGuiCol_FrameBgHovered] = col4(theme::kHover);
    c[ImGuiCol_FrameBgActive] = col4(theme::kSurfaceHi);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col4(theme::kRail);
    c[ImGuiCol_MenuBarBg] = col4(theme::kRail);
    c[ImGuiCol_Header] = col4(theme::kActive);
    c[ImGuiCol_HeaderHovered] = rgb(39, 39, 39, 0.6f);
    c[ImGuiCol_HeaderActive] = rgb(52, 52, 52);
    c[ImGuiCol_Button] = col4(theme::kSurfaceHi);
    c[ImGuiCol_ButtonHovered] = rgb(52, 52, 52);
    c[ImGuiCol_ButtonActive] = rgb(63, 63, 63);
    c[ImGuiCol_CheckMark] = col4(theme::kAccentHi);
    c[ImGuiCol_CheckboxSelectedBg] = col4(theme::kActive);
    c[ImGuiCol_InputTextCursor] = col4(theme::kText);
    c[ImGuiCol_DragDropTarget] = col4(theme::kRing);
    c[ImGuiCol_SliderGrab] = rgb(212, 212, 212);
    c[ImGuiCol_SliderGrabActive] = col4(theme::kAccentHi);
    c[ImGuiCol_Tab] = {0, 0, 0, 0};
    c[ImGuiCol_TabHovered] = rgb(39, 39, 39, 0.6f);
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
    c[ImGuiCol_ScrollbarGrab] = rgb(39, 39, 39);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(63, 63, 63);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(82, 82, 82);
    c[ImGuiCol_TableHeaderBg] = rgb(17, 17, 17);
    c[ImGuiCol_TableBorderStrong] = col4(theme::kBorder);
    c[ImGuiCol_TableBorderLight] = rgb(28, 28, 28);
    c[ImGuiCol_TableRowBgAlt] = {1, 1, 1, 0.02f};
    c[ImGuiCol_TextSelectedBg] = rgb(250, 250, 250, 0.18f);
    c[ImGuiCol_NavCursor] = col4(theme::kRing);
    c[ImGuiCol_PlotHistogram] = c[ImGuiCol_PlotLines] = rgb(212, 212, 212);
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

// Text drawn at `pos` and cut with "..." at `max_x` (list rows, labels in narrow columns).
inline void text_fit(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float max_x) {
    if (max_x <= pos.x) return;
    const ImVec2 ts = ImGui::CalcTextSize(text);
    if (pos.x + ts.x <= max_x) {
        dl->AddText(pos, col, text);
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::RenderTextEllipsis(dl, pos, {max_x, pos.y + ts.y}, max_x, text, nullptr, &ts);
    ImGui::PopStyleColor();
}


// Keep the viewer's existing Windows Segoe Fluent / MDL2 glyphs. In an installation
// without the icon font, render labels instead of private-use missing-glyph boxes.
namespace rail_glyph {
constexpr ImWchar Maps = 0xE774, Models = 0xE99A, Scenery = 0xE7B8, Dialogue = 0xE8BD,
                  Codex = 0xE8F1, Movies = 0xE714, Text = 0xE8D2, Packages = 0xE8B7,
                  Log = 0xE9D9, Settings = 0xE713, Updates = 0xE896;  // Windows Download
}

inline bool rail_glyph_text(ImWchar codepoint, char (&utf8)[5]) {
    if (!ImGui::GetFont()->IsGlyphInFont(codepoint)) return false;
    return ImTextCharToUtf8(utf8, codepoint) > 0;
}

template <class Open>
void draw_sidebar(float title_h, bool expanded, ui::S active, ImFont* bold, Open&& open) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 o = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddLine({o.x + w - 1, o.y}, {o.x + w - 1, o.y + h}, theme::kBorder);
    dl->AddLine({o.x, o.y + title_h - 1}, {o.x + w, o.y + title_h - 1}, theme::kBorder);
    ImGui::PushFont(bold, 22);
    const char* brand = expanded ? "nim" : "n";
    const float brand_x = expanded ? 17 * sc : (w - ImGui::CalcTextSize(brand).x) * 0.5f;
    dl->AddText({o.x + brand_x, o.y + (title_h - ImGui::GetTextLineHeight()) * 0.5f}, theme::kText, brand);
    ImGui::PopFont();

    auto row = [&](ui::S label, ImWchar glyph, const char* compact_label) {
        const float height = 34 * sc, inset = 8 * sc;
        const float width = std::max(1.0f, ImGui::GetWindowWidth() - 2 * inset);
        ImGui::SetCursorPosX(inset);
        ImGui::PushID(int(label));
        const bool clicked = ImGui::InvisibleButton("nav", {width, height}, ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered();
        const bool selected = active == label;
        const ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
        auto* d = ImGui::GetWindowDrawList();
        if (selected || hovered) d->AddRectFilled(p, q, selected ? theme::kActive : theme::kHover, 6 * sc);
        if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
            d->AddRect(p, q, theme::kRing, 6 * sc, 1 * sc);
        const ImU32 color = selected || hovered ? theme::kText : theme::kMuted;
        char glyph_text[5]{};
        const bool have_icon = rail_glyph_text(glyph, glyph_text);
        if (have_icon || !expanded) {
            const char* text = have_icon ? glyph_text : compact_label;
            ImGui::PushFont(nullptr, have_icon ? 16 : 11);
            const auto ts = ImGui::CalcTextSize(text);
            const float center_x = p.x + (expanded ? 18 * sc : width * 0.5f);
            d->AddText({center_x - ts.x * 0.5f, p.y + (height - ts.y) * 0.5f}, color, text);
            ImGui::PopFont();
        }
        if (expanded) {
            if (selected) ImGui::PushFont(bold, 0);
            const float ty = p.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
            text_fit(d, {p.x + (have_icon ? 35 : 11) * sc, ty}, color, ui::tr(label), q.x - 7 * sc);
            if (selected) ImGui::PopFont();
        } else if (hovered) ImGui::SetTooltip("%s", ui::tr(label));
        ImGui::PopID();
        if (clicked) open(label);
    };
    auto section = [&](ui::S label) {
        if (!expanded) { ImGui::Dummy({0, 5 * sc}); return; }
        ImGui::SetCursorPosX(17 * sc);
        ImGui::PushFont(bold, 13);
        ImGui::TextColored(col4(theme::kMuted), "%s", ui::tr(label));
        ImGui::PopFont();
        ImGui::Dummy({0, 2 * sc});
    };
    const float foot_h = 119 * sc;
    ImGui::SetCursorPos({0, title_h + 10 * sc});
    ImGui::BeginChild("##nav", {w - 1, std::max(1.0f, h - title_h - foot_h - 10 * sc)},
                     ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
    section(ui::S::GroupModels3D);
    row(ui::S::NavMaps, rail_glyph::Maps, "MP"); row(ui::S::NavEntities, rail_glyph::Models, "3D"); row(ui::S::NavStatics, rail_glyph::Scenery, "SC");
    ImGui::Dummy({0, 9 * sc});
    section(ui::S::Group2D);
    row(ui::S::NavDialogue, rail_glyph::Dialogue, "VO"); row(ui::S::NavCodex, rail_glyph::Codex, "CX");
    row(ui::S::NavMovies, rail_glyph::Movies, "MV"); row(ui::S::NavText, rail_glyph::Text, "TX");
    ImGui::Dummy({0, 9 * sc});
    section(ui::S::GroupData);
    row(ui::S::NavPackages, rail_glyph::Packages, "PK");
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const float foot_top = std::max(title_h + 11 * sc, h - foot_h);
    dl->AddLine({o.x + 12 * sc, o.y + foot_top}, {o.x + w - 12 * sc, o.y + foot_top}, theme::kBorder);
    ImGui::SetCursorPos({0, foot_top + 5 * sc});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
    row(ui::S::NavLog, rail_glyph::Log, "LG"); row(ui::S::NavSettings, rail_glyph::Settings, "ST");
    row(ui::S::SettingsUpdates, rail_glyph::Updates, "UP");
    ImGui::PopStyleVar();
}

struct HeaderLayout { float search_x, search_width, title_right; bool language; };
inline HeaderLayout header_layout(float width, float sc, float buttons_x) {
    const bool language = width >= 565 * sc;
    const float right = buttons_x - (language ? 124 : 9) * sc;
    const float available = right - 126 * sc;
    const float search_width = available >= 82 * sc ? std::min(320 * sc, available) : 0.0f;
    const float search_x = right - search_width;
    return {search_x, search_width, search_width > 0 ? search_x - 12 * sc : right, language};
}

inline void draw_title_name(const char* name, ImFont* font, float h, float right) {
    const auto o = ImGui::GetWindowPos();
    const float sc = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushFont(font, 17);
    text_fit(ImGui::GetWindowDrawList(), {o.x + 46 * sc, o.y + (h - ImGui::GetTextLineHeight()) * 0.5f},
             theme::kText, name, o.x + right);
    ImGui::PopFont();
}

inline void draw_search_key(ImVec2 p, float width, float height, float sc) {
    auto* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(nullptr, 11);
    const auto ts = ImGui::CalcTextSize("Ctrl K");
    const ImVec2 a{p.x + width - ts.x - 16 * sc, p.y + (height - ts.y) * 0.5f - 2 * sc};
    const ImVec2 b{a.x + ts.x + 9 * sc, a.y + ts.y + 4 * sc};
    dl->AddRect(a, b, theme::kBorder, 3 * sc);
    dl->AddText({a.x + 4 * sc, a.y + 2 * sc}, theme::kMuted, "Ctrl K");
    ImGui::PopFont();
}

inline float draw_status_contents(ImVec2 o, float w, float h, const char* left, const char* right, bool error, bool ready) {
    auto* dl = ImGui::GetWindowDrawList();
    const float sc = ImGui::GetStyle().FontScaleDpi;
    const float ty = o.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    const float right_width = std::min(ImGui::CalcTextSize(right).x, w * 0.46f);
    const float rx = o.x + w - right_width - 14 * sc;
    text_fit(dl, {rx, ty}, theme::kMuted, right, o.x + w - 14 * sc);
    const ImU32 color = error ? theme::kError : ready ? theme::kAccentHi : theme::kMuted;
    dl->AddCircleFilled({o.x + 16 * sc, o.y + h * 0.5f}, 3 * sc, color);
    text_fit(dl, {o.x + 27 * sc, ty}, error ? color : theme::kText, left, rx - 12 * sc);
    return std::max(1.0f, rx - o.x - 12 * sc);
}

inline void draw_page_heading(const char* name, const char* desc, ImFont* bold) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushFont(bold, 30);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(col4(theme::kMuted), "%s", desc);
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 11 * sc});
}

// The same update card is used for the settings page and the headless UI preview.
inline bool begin_update_card(float width, const char* version, ImFont* bold) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col4(theme::kRail));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * sc, 14 * sc});
    const bool visible = ImGui::BeginChild("##app-update", {width, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                                          ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (visible) {
        ImGui::PushFont(bold, 18);
        ImGui::TextUnformatted(ui::tr(ui::S::SettingsUpdates));
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, col4(theme::kMuted));
        ImGui::TextWrapped("%s", ui::trf(ui::S::SettingsVersion, version).c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    return visible;
}

inline bool update_primary_button(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, col4(theme::kPrimaryText));
    ImGui::PushStyleColor(ImGuiCol_Button, col4(theme::kAccentHi));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col4(theme::kPrimaryHover));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col4(theme::kPrimaryPressed));
    ImGui::PushStyleColor(ImGuiCol_Border, col4(theme::kAccentHi));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(5);
    return pressed;
}

} // namespace nim::chrome
