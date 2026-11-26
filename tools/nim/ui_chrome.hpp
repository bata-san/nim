#pragma once

#include "imgui.h"
#include "imgui_internal.h"
#include "ui_text.hpp"

#include <algorithm>
#include <cmath>

namespace nim::chrome {

// Neutral graphite chrome with a restrained Marathon-green accent.
namespace theme {
constexpr ImU32 kBg = IM_COL32(13, 16, 18, 255);           // background
constexpr ImU32 kRail = IM_COL32(19, 23, 25, 255);       // sidebar, title bar and status line
constexpr ImU32 kSurface = IM_COL32(22, 27, 29, 255);    // cards
constexpr ImU32 kSurfaceHi = IM_COL32(34, 41, 44, 255);  // secondary controls
constexpr ImU32 kHover = IM_COL32(31, 38, 40, 255);
constexpr ImU32 kActive = IM_COL32(39, 49, 34, 255);
constexpr ImU32 kBorder = IM_COL32(47, 57, 60, 255);
constexpr ImU32 kRing = IM_COL32(160, 200, 82, 255);
constexpr ImU32 kText = IM_COL32(238, 243, 244, 255);
constexpr ImU32 kMuted = IM_COL32(159, 174, 178, 255);
constexpr ImU32 kError = IM_COL32(248, 113, 113, 255);
constexpr ImU32 kAccentHi = IM_COL32(203, 246, 107, 255);
constexpr ImU32 kPrimaryText = IM_COL32(19, 27, 13, 255);
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
    c[ImGuiCol_PopupBg] = rgb(12, 12, 14, 0.99f);
    c[ImGuiCol_Border] = col4(theme::kBorder);
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = col4(theme::kSurface);
    c[ImGuiCol_FrameBgHovered] = col4(theme::kHover);
    c[ImGuiCol_FrameBgActive] = col4(theme::kSurfaceHi);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = col4(theme::kRail);
    c[ImGuiCol_MenuBarBg] = col4(theme::kRail);
    c[ImGuiCol_Header] = col4(theme::kActive);
    c[ImGuiCol_HeaderHovered] = rgb(39, 39, 42, 0.6f);
    c[ImGuiCol_HeaderActive] = rgb(52, 52, 56);
    c[ImGuiCol_Button] = col4(theme::kSurfaceHi);
    c[ImGuiCol_ButtonHovered] = rgb(52, 52, 56);
    c[ImGuiCol_ButtonActive] = rgb(63, 63, 70);
    c[ImGuiCol_CheckMark] = col4(theme::kAccentHi);
    c[ImGuiCol_CheckboxSelectedBg] = col4(theme::kActive);
    c[ImGuiCol_SliderGrab] = rgb(212, 212, 216);
    c[ImGuiCol_SliderGrabActive] = col4(theme::kAccentHi);
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


enum class RailIcon { Map, Models, Scenery, Dialogue, Codex, Movies, Text, Package, Log, Settings };

inline void rail_icon(ImDrawList* dl, ImVec2 center, float sc, RailIcon kind, ImU32 color) {
    auto at = [&](float x, float y) { return ImVec2{center.x + x * sc, center.y + y * sc}; };
    auto line = [&](float x, float y, float xx, float yy) { dl->AddLine(at(x, y), at(xx, yy), color, 1.35f * sc); };
    auto box = [&](float x, float y, float xx, float yy) { dl->AddRect(at(x, y), at(xx, yy), color, 2 * sc, 1.35f * sc); };
    switch (kind) {
    case RailIcon::Map:
        line(-7, -4, -2, -6); line(-2, -6, 2, -4); line(2, -4, 7, -6); line(7, -6, 7, 5);
        line(7, 5, 2, 7); line(2, 7, -2, 5); line(-2, 5, -7, 7); line(-7, 7, -7, -4);
        line(-2, -6, -2, 5); line(2, -4, 2, 7); break;
    case RailIcon::Models:
        line(0, -7, 7, -3); line(7, -3, 7, 4); line(7, 4, 0, 8); line(0, 8, -7, 4); line(-7, 4, -7, -3);
        line(-7, -3, 0, -7); line(-7, -3, 0, 1); line(0, 1, 7, -3); line(0, 1, 0, 8); break;
    case RailIcon::Scenery:
        box(-7, -6, 7, 6); line(-6, 4, -2, -1); line(-2, -1, 1, 2); line(1, 2, 4, -2); line(4, -2, 7, 2);
        dl->AddCircleFilled(at(-3, -3), 1 * sc, color); break;
    case RailIcon::Dialogue:
        box(-7, -6, 7, 4); line(-4, 4, -4, 7); line(-4, 7, 0, 4);
        line(-4, -2, 4, -2); line(-4, 1, 1, 1); break;
    case RailIcon::Codex:
        box(-7, -6, 7, 6); line(0, -6, 0, 6); line(-5, -2, -2, -2); line(2, -2, 5, -2); break;
    case RailIcon::Movies:
        box(-7, -6, 7, 6); dl->AddTriangleFilled(at(-2, -3), at(-2, 3), at(4, 0), color); break;
    case RailIcon::Text:
        line(-6, -5, 6, -5); line(-6, 0, 6, 0); line(-6, 5, 2, 5); break;
    case RailIcon::Package:
        box(-7, -6, 7, 6); line(-7, -2, 7, -2); line(-2, 1, 2, 1); break;
    case RailIcon::Log:
        box(-7, -6, 7, 6); line(-4, -2, -1, 0); line(-1, 0, -4, 2); line(1, 2, 4, 2); break;
    case RailIcon::Settings:
        line(-7, -5, 7, -5); line(-7, 0, 7, 0); line(-7, 5, 7, 5);
        dl->AddCircleFilled(at(-2, -5), 2 * sc, color); dl->AddCircleFilled(at(3, 0), 2 * sc, color);
        dl->AddCircleFilled(at(0, 5), 2 * sc, color); break;
    }
}

template <class Open>
void draw_sidebar(float title_h, bool expanded, ui::S active, ImFont* bold, Open&& open) {
    const float sc = ImGui::GetStyle().FontScaleDpi;
    const ImVec2 o = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth(), h = ImGui::GetWindowHeight();
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddLine({o.x + w - 1, o.y}, {o.x + w - 1, o.y + h}, theme::kBorder);
    dl->AddLine({o.x, o.y + title_h - 1}, {o.x + w, o.y + title_h - 1}, theme::kBorder);
    const float mark_x = o.x + (expanded ? 15 * sc : (w - 26 * sc) * 0.5f);
    const float mark_y = o.y + (title_h - 26 * sc) * 0.5f;
    dl->AddRectFilled({mark_x, mark_y}, {mark_x + 26 * sc, mark_y + 26 * sc}, theme::kAccentHi, 6 * sc);
    dl->AddQuadFilled({mark_x + 8 * sc, mark_y + 19 * sc}, {mark_x + 12 * sc, mark_y + 7 * sc},
                     {mark_x + 17 * sc, mark_y + 7 * sc}, {mark_x + 13 * sc, mark_y + 19 * sc}, theme::kPrimaryText);
    dl->AddCircleFilled({mark_x + 18 * sc, mark_y + 17 * sc}, 2 * sc, theme::kPrimaryText);
    if (expanded) {
        ImGui::PushFont(bold, 22);
        dl->AddText({o.x + 51 * sc, o.y + (title_h - ImGui::GetTextLineHeight()) * 0.5f}, theme::kText, "nim");
        ImGui::PopFont();
    }
    auto row = [&](ui::S label, RailIcon glyph) {
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
        if (selected)
            d->AddRectFilled({p.x + 2 * sc, p.y + 8 * sc}, {p.x + 4 * sc, q.y - 8 * sc}, theme::kAccentHi, sc);
        const ImU32 color = selected ? theme::kAccentHi : hovered ? theme::kText : theme::kMuted;
        rail_icon(d, {p.x + (expanded ? 18 * sc : width * 0.5f), p.y + height * 0.5f}, sc, glyph, color);
        if (expanded) {
            if (selected) ImGui::PushFont(bold, 0);
            const float ty = p.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
            text_fit(d, {p.x + 35 * sc, ty}, selected || hovered ? theme::kText : theme::kMuted, ui::tr(label), q.x - 7 * sc);
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
    const float foot_h = 83 * sc;
    ImGui::SetCursorPos({0, title_h + 10 * sc});
    ImGui::BeginChild("##nav", {w - 1, std::max(1.0f, h - title_h - foot_h - 10 * sc)},
                     ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
    section(ui::S::GroupModels3D);
    row(ui::S::NavMaps, RailIcon::Map); row(ui::S::NavEntities, RailIcon::Models); row(ui::S::NavStatics, RailIcon::Scenery);
    ImGui::Dummy({0, 9 * sc});
    section(ui::S::Group2D);
    row(ui::S::NavDialogue, RailIcon::Dialogue); row(ui::S::NavCodex, RailIcon::Codex);
    row(ui::S::NavMovies, RailIcon::Movies); row(ui::S::NavText, RailIcon::Text);
    ImGui::Dummy({0, 9 * sc});
    section(ui::S::GroupData);
    row(ui::S::NavPackages, RailIcon::Package);
    ImGui::PopStyleVar();
    ImGui::EndChild();

    const float foot_top = std::max(title_h + 11 * sc, h - foot_h);
    dl->AddLine({o.x + 12 * sc, o.y + foot_top}, {o.x + w - 12 * sc, o.y + foot_top}, theme::kBorder);
    ImGui::SetCursorPos({0, foot_top + 5 * sc});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * sc, 2 * sc});
    row(ui::S::NavLog, RailIcon::Log); row(ui::S::NavSettings, RailIcon::Settings);
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
    const auto p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + 33 * sc, p.y + 3 * sc}, theme::kAccentHi, sc);
    ImGui::Dummy({0, 12 * sc});
    ImGui::PushFont(bold, 30);
    ImGui::TextUnformatted(name);
    ImGui::PopFont();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(col4(theme::kMuted), "%s", desc);
    ImGui::PopTextWrapPos();
    ImGui::Dummy({0, 11 * sc});
}

} // namespace nim::chrome
