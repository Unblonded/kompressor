#include "theme.hpp"

#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace kzgui {

namespace {
constexpr float kPi = 3.14159265358979f;
std::unordered_map<ImGuiID, float> g_anims;
bool g_animating = false, g_animatingPrev = false;

float em() { return ImGui::GetFontSize(); }

ImU32 white(float a = 1.0f) { return ImGui::GetColorU32(ImVec4(1, 1, 1, a)); }
}  // namespace

Fonts& fonts() {
    static Fonts f;
    return f;
}

ImU32 u32(const ImVec4& c, float alpha) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal) {
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding);
    int v1 = dl->VtxBuffer.Size;
    ImVec2 g1 = horizontal ? ImVec2(b.x, a.y) : ImVec2(a.x, b.y);
    // Keep the vertex alpha (anti-aliased fringe) but take colour and opacity from the gradient.
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, v1, a, g1, c0, c1);
    float al0 = ((c0 >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f, al1 = ((c1 >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    if (al0 < 1.0f || al1 < 1.0f) {
        float span = horizontal ? (b.x - a.x) : (b.y - a.y);
        for (int i = v0; i < v1; ++i) {
            ImDrawVert& v = dl->VtxBuffer[i];
            float t = span > 0 ? ImClamp(((horizontal ? v.pos.x - a.x : v.pos.y - a.y)) / span, 0.0f, 1.0f) : 0.0f;
            float va = ((v.col >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f * (al0 + (al1 - al0) * t);
            v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)(va * 255.0f + 0.5f) << IM_COL32_A_SHIFT);
        }
    }
}

void softShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float size, float alpha) {
    int steps = std::max(4, (int)size);
    for (int i = 1; i <= steps; ++i) {
        float t = float(i) / steps;
        float o = size * t;
        float al = alpha * (1.0f - t) * (1.0f - t) * 0.5f;
        dl->AddRect(ImVec2(a.x - o, a.y - o + size * 0.25f), ImVec2(b.x + o, b.y + o + size * 0.25f),
                    ImGui::GetColorU32(ImVec4(0, 0, 0, al)), rounding + o, 0, size / steps + 1.0f);
    }
}

float anim(ImGuiID id, bool on, float speed) {
    auto it = g_anims.find(id);
    float target = on ? 1.0f : 0.0f;
    if (it == g_anims.end()) it = g_anims.emplace(id, target).first;
    float& v = it->second;
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    if (v < target) v = std::min(target, v + dt * speed * 0.5f + (target - v) * std::min(1.0f, dt * speed));
    else if (v > target) v = std::max(target, v - dt * speed * 0.5f - (v - target) * std::min(1.0f, dt * speed));
    if (v != target) g_animating = true;
    return v;
}

bool animating() { return g_animating || g_animatingPrev; }

void beginFrameUi() {
    g_animatingPrev = g_animating;
    g_animating = false;
}

// ============================================================================ theme
void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 12.0f;
    s.ChildRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 10.0f;
    s.ScrollbarRounding = 10.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.WindowPadding = ImVec2(16, 16);
    s.FramePadding = ImVec2(11, 7);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.CellPadding = ImVec2(10, 5);
    s.IndentSpacing = 18.0f;
    s.ScrollbarSize = 11.0f;
    s.GrabMinSize = 14.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 2.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(0, 4);
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    s.DisabledAlpha = 0.38f;
    s.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    const ImVec4 a = col::Accent;
    auto A = [](const ImVec4& v, float al) { return ImVec4(v.x, v.y, v.z, al); };

    c[ImGuiCol_Text] = col::Text;
    c[ImGuiCol_TextDisabled] = col::Dim;
    c[ImGuiCol_WindowBg] = col::Bg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.090f, 0.094f, 0.122f, 0.985f);
    c[ImGuiCol_Border] = col::Line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = col::Surface2;
    c[ImGuiCol_FrameBgHovered] = col::Surface3;
    c[ImGuiCol_FrameBgActive] = mix(col::Surface3, a, 0.12f);
    c[ImGuiCol_TitleBg] = col::Surface1;
    c[ImGuiCol_TitleBgActive] = col::Surface1;
    c[ImGuiCol_TitleBgCollapsed] = col::Surface1;
    c[ImGuiCol_MenuBarBg] = col::Surface1;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = A(col::Text, 0.10f);
    c[ImGuiCol_ScrollbarGrabHovered] = A(col::Text, 0.20f);
    c[ImGuiCol_ScrollbarGrabActive] = A(a, 0.70f);
    c[ImGuiCol_CheckMark] = ImVec4(1, 1, 1, 1);
    c[ImGuiCol_SliderGrab] = a;
    c[ImGuiCol_SliderGrabActive] = col::AccentHover;
    c[ImGuiCol_Button] = col::Surface2;
    c[ImGuiCol_ButtonHovered] = col::Surface3;
    c[ImGuiCol_ButtonActive] = mix(col::Surface3, a, 0.25f);
    c[ImGuiCol_Header] = A(a, 0.22f);
    c[ImGuiCol_HeaderHovered] = A(col::Text, 0.05f);
    c[ImGuiCol_HeaderActive] = A(a, 0.30f);
    c[ImGuiCol_Separator] = col::Line;
    c[ImGuiCol_SeparatorHovered] = A(a, 0.8f);
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = A(a, 0.50f);
    c[ImGuiCol_ResizeGripActive] = a;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = col::Surface2;
    c[ImGuiCol_TabSelected] = col::Surface1;
    c[ImGuiCol_TabSelectedOverline] = a;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = col::Surface1;
    c[ImGuiCol_TabDimmedSelectedOverline] = A(a, 0.5f);
    c[ImGuiCol_PlotHistogram] = a;
    c[ImGuiCol_PlotHistogramHovered] = col::AccentHover;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableBorderStrong] = col::Line;
    c[ImGuiCol_TableBorderLight] = A(col::Line, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = A(col::Text, 0.015f);
    c[ImGuiCol_TextSelectedBg] = A(a, 0.35f);
    c[ImGuiCol_TextLink] = col::AccentHover;
    c[ImGuiCol_DragDropTarget] = col::Teal;
    c[ImGuiCol_NavCursor] = a;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.01f, 0.01f, 0.02f, 0.62f);

    s.ScaleAllSizes(scale);
}

// ============================================================================ icons
void drawIco(ImDrawList* dl, Ico ic, ImVec2 p, float s, ImU32 c) {
    auto P = [&](float x, float y) { return ImVec2(p.x + x * s, p.y + y * s); };
    const float t = std::max(1.25f, s * 0.085f);
    ImVec4 cv = ImGui::ColorConvertU32ToFloat4(c);
    ImU32 soft = ImGui::GetColorU32(ImVec4(cv.x, cv.y, cv.z, cv.w * 0.16f));
    auto page = [&]() {
        ImVec2 pts[5] = {P(0.22f, 0.07f), P(0.60f, 0.07f), P(0.80f, 0.27f), P(0.80f, 0.93f), P(0.22f, 0.93f)};
        dl->AddConvexPolyFilled(pts, 5, soft);
        dl->AddPolyline(pts, 5, c, ImDrawFlags_Closed, t);
        dl->AddLine(P(0.60f, 0.07f), P(0.60f, 0.27f), c, t);
        dl->AddLine(P(0.60f, 0.27f), P(0.80f, 0.27f), c, t);
    };
    switch (ic) {
        case Ico::Folder:
        case Ico::FolderOpen: {
            ImVec4 fc = cv;
            dl->AddRectFilled(P(0.06f, 0.16f), P(0.44f, 0.36f), ImGui::GetColorU32(ImVec4(fc.x, fc.y, fc.z, fc.w * 0.75f)), s * 0.07f);
            dl->AddRectFilled(P(0.06f, 0.26f), P(0.94f, 0.86f), c, s * 0.09f);
            if (ic == Ico::FolderOpen) {
                ImVec2 q[4] = {P(0.18f, 0.44f), P(0.98f, 0.44f), P(0.86f, 0.86f), P(0.06f, 0.86f)};
                dl->AddConvexPolyFilled(q, 4, white(0.22f * cv.w));
            } else {
                dl->AddLine(P(0.06f, 0.40f), P(0.94f, 0.40f), white(0.18f * cv.w), std::max(1.0f, s * 0.05f));
            }
            break;
        }
        case Ico::File: page(); break;
        case Ico::Text:
            page();
            for (int i = 0; i < 3; ++i)
                dl->AddLine(P(0.34f, 0.46f + i * 0.14f), P(i == 2 ? 0.56f : 0.68f, 0.46f + i * 0.14f), c, t * 0.85f);
            break;
        case Ico::Archive:
            page();
            for (int i = 0; i < 4; ++i) {
                float y = 0.16f + i * 0.15f, x = i % 2 ? 0.44f : 0.36f;
                dl->AddRectFilled(P(x, y), P(x + 0.10f, y + 0.08f), c, 1.0f);
            }
            dl->AddRectFilled(P(0.34f, 0.74f), P(0.56f, 0.86f), c, s * 0.03f);
            break;
        case Ico::Image:
            page();
            dl->AddTriangleFilled(P(0.30f, 0.82f), P(0.46f, 0.56f), P(0.62f, 0.82f), c);
            dl->AddTriangleFilled(P(0.50f, 0.82f), P(0.62f, 0.64f), P(0.74f, 0.82f), c);
            dl->AddCircleFilled(P(0.40f, 0.40f), s * 0.06f, c);
            break;
        case Ico::Code:
            page();
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.43f, 0.48f), P(0.33f, 0.60f), P(0.43f, 0.72f)}.data(), 3, c, 0, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.59f, 0.48f), P(0.69f, 0.60f), P(0.59f, 0.72f)}.data(), 3, c, 0, t);
            break;
        case Ico::Exec:
            page();
            dl->AddTriangleFilled(P(0.40f, 0.44f), P(0.40f, 0.78f), P(0.66f, 0.61f), c);
            break;
        case Ico::Link:
            page();
            dl->AddLine(P(0.36f, 0.74f), P(0.64f, 0.46f), c, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.46f, 0.46f), P(0.64f, 0.46f), P(0.64f, 0.64f)}.data(), 3, c, 0, t);
            break;
        case Ico::Open: {
            ImVec2 q[6] = {P(0.08f, 0.20f), P(0.40f, 0.20f), P(0.50f, 0.32f), P(0.86f, 0.32f), P(0.86f, 0.82f), P(0.08f, 0.82f)};
            dl->AddPolyline(q, 6, c, ImDrawFlags_Closed, t);
            dl->AddLine(P(0.08f, 0.44f), P(0.86f, 0.44f), c, t);
            break;
        }
        case Ico::Box: {
            ImVec2 h[6] = {P(0.50f, 0.08f), P(0.88f, 0.28f), P(0.88f, 0.72f), P(0.50f, 0.92f), P(0.12f, 0.72f), P(0.12f, 0.28f)};
            dl->AddConvexPolyFilled(h, 6, soft);
            dl->AddPolyline(h, 6, c, ImDrawFlags_Closed, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.12f, 0.28f), P(0.50f, 0.48f), P(0.88f, 0.28f)}.data(), 3, c, 0, t);
            dl->AddLine(P(0.50f, 0.48f), P(0.50f, 0.92f), c, t);
            break;
        }
        case Ico::Bolt: {
            ImVec2 a[4] = {P(0.58f, 0.04f), P(0.20f, 0.56f), P(0.48f, 0.56f), P(0.40f, 0.96f)};
            dl->AddTriangleFilled(P(0.60f, 0.04f), P(0.18f, 0.58f), P(0.52f, 0.58f), c);
            dl->AddTriangleFilled(P(0.48f, 0.42f), P(0.84f, 0.42f), P(0.40f, 0.96f), c);
            (void)a;
            break;
        }
        case Ico::Extract:
        case Ico::Drop:
            dl->AddLine(P(0.50f, 0.08f), P(0.50f, 0.62f), c, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.30f, 0.44f), P(0.50f, 0.64f), P(0.70f, 0.44f)}.data(), 3, c, 0, t);
            dl->AddPolyline(std::array<ImVec2, 4>{P(0.12f, 0.62f), P(0.12f, 0.90f), P(0.88f, 0.90f), P(0.88f, 0.62f)}.data(), 4, c, 0, t);
            break;
        case Ico::Shield: {
            ImVec2 sh[7] = {P(0.50f, 0.06f), P(0.86f, 0.20f), P(0.84f, 0.52f), P(0.70f, 0.76f), P(0.50f, 0.94f), P(0.30f, 0.76f), P(0.16f, 0.52f)};
            ImVec2 sh2[8] = {sh[0], sh[1], sh[2], sh[3], sh[4], sh[5], sh[6], P(0.14f, 0.20f)};
            dl->AddConvexPolyFilled(sh2, 8, soft);
            dl->AddPolyline(sh2, 8, c, ImDrawFlags_Closed, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.34f, 0.50f), P(0.46f, 0.62f), P(0.68f, 0.38f)}.data(), 3, c, 0, t);
            break;
        }
        case Ico::Sparkle: {
            auto star = [&](float cx, float cy, float r) {
                ImVec2 st[8];
                for (int i = 0; i < 8; ++i) {
                    float ang = i * kPi / 4.0f - kPi / 2.0f;
                    float rr = (i % 2 == 0) ? r : r * 0.28f;
                    st[i] = P(cx + std::cos(ang) * rr, cy + std::sin(ang) * rr);
                }
                for (int i = 0; i < 8; i += 2) dl->AddTriangleFilled(P(cx, cy), st[(i + 7) % 8], st[i], c), dl->AddTriangleFilled(P(cx, cy), st[i], st[i + 1], c);
            };
            star(0.42f, 0.56f, 0.38f);
            star(0.80f, 0.20f, 0.16f);
            break;
        }
        case Ico::Search:
            dl->AddCircle(P(0.43f, 0.43f), s * 0.28f, c, 24, t);
            dl->AddLine(P(0.64f, 0.64f), P(0.88f, 0.88f), c, t * 1.2f);
            break;
        case Ico::Close:
            dl->AddLine(P(0.24f, 0.24f), P(0.76f, 0.76f), c, t);
            dl->AddLine(P(0.76f, 0.24f), P(0.24f, 0.76f), c, t);
            break;
        case Ico::Up:
            dl->AddLine(P(0.50f, 0.16f), P(0.50f, 0.86f), c, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.22f, 0.44f), P(0.50f, 0.16f), P(0.78f, 0.44f)}.data(), 3, c, 0, t);
            break;
        case Ico::Chevron:
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.38f, 0.22f), P(0.64f, 0.50f), P(0.38f, 0.78f)}.data(), 3, c, 0, t);
            break;
        case Ico::Lock:
            dl->PathArcTo(P(0.50f, 0.42f), s * 0.20f, kPi, 2 * kPi, 16);
            dl->PathLineTo(P(0.70f, 0.48f));
            dl->PathStroke(c, 0, t);
            dl->AddLine(P(0.30f, 0.42f), P(0.30f, 0.48f), c, t);
            dl->AddRectFilled(P(0.20f, 0.46f), P(0.80f, 0.90f), c, s * 0.08f);
            break;
        case Ico::Check:
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.18f, 0.52f), P(0.40f, 0.74f), P(0.82f, 0.28f)}.data(), 3, c, 0, t * 1.2f);
            break;
        case Ico::Warn:
            dl->AddTriangle(P(0.50f, 0.10f), P(0.92f, 0.86f), P(0.08f, 0.86f), c, t);
            dl->AddLine(P(0.50f, 0.38f), P(0.50f, 0.60f), c, t);
            dl->AddCircleFilled(P(0.50f, 0.72f), t * 0.8f, c);
            break;
        case Ico::Info:
            dl->AddCircle(P(0.5f, 0.5f), s * 0.40f, c, 24, t);
            dl->AddLine(P(0.50f, 0.46f), P(0.50f, 0.70f), c, t);
            dl->AddCircleFilled(P(0.50f, 0.32f), t * 0.8f, c);
            break;
        case Ico::Error:
            dl->AddCircle(P(0.5f, 0.5f), s * 0.40f, c, 24, t);
            dl->AddLine(P(0.36f, 0.36f), P(0.64f, 0.64f), c, t);
            dl->AddLine(P(0.64f, 0.36f), P(0.36f, 0.64f), c, t);
            break;
        case Ico::Plus:
            dl->AddLine(P(0.50f, 0.18f), P(0.50f, 0.82f), c, t);
            dl->AddLine(P(0.18f, 0.50f), P(0.82f, 0.50f), c, t);
            break;
        case Ico::Trash:
            dl->AddLine(P(0.16f, 0.24f), P(0.84f, 0.24f), c, t);
            dl->AddLine(P(0.38f, 0.12f), P(0.62f, 0.12f), c, t);
            dl->AddPolyline(std::array<ImVec2, 4>{P(0.24f, 0.24f), P(0.30f, 0.90f), P(0.70f, 0.90f), P(0.76f, 0.24f)}.data(), 4, c, 0, t);
            break;
        case Ico::Eye:
            dl->PathArcTo(P(0.5f, 0.95f), s * 0.62f, -kPi * 0.78f, -kPi * 0.22f, 16);
            dl->PathStroke(c, 0, t);
            dl->PathArcTo(P(0.5f, 0.05f), s * 0.62f, kPi * 0.22f, kPi * 0.78f, 16);
            dl->PathStroke(c, 0, t);
            dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.13f, c);
            break;
        case Ico::Panel:
            dl->AddRect(P(0.10f, 0.16f), P(0.90f, 0.84f), c, s * 0.10f, 0, t);
            dl->AddLine(P(0.10f, 0.60f), P(0.90f, 0.60f), c, t);
            break;
        case Ico::Logo:
            dl->AddTriangleFilled(P(0.14f, 0.22f), P(0.46f, 0.50f), P(0.14f, 0.78f), c);
            dl->AddTriangleFilled(P(0.86f, 0.22f), P(0.86f, 0.78f), P(0.54f, 0.50f), c);
            break;
        case Ico::Clock:
            dl->AddCircle(P(0.5f, 0.5f), s * 0.40f, c, 24, t);
            dl->AddPolyline(std::array<ImVec2, 3>{P(0.50f, 0.26f), P(0.50f, 0.52f), P(0.68f, 0.62f)}.data(), 3, c, 0, t);
            break;
        case Ico::Layers:
            for (int i = 2; i >= 0; --i) {
                float y = 0.20f + i * 0.17f;
                ImVec2 r[4] = {P(0.50f, y), P(0.90f, y + 0.18f), P(0.50f, y + 0.36f), P(0.10f, y + 0.18f)};
                dl->AddConvexPolyFilled(r, 4, i == 0 ? c : soft);
                if (i) dl->AddPolyline(r, 4, c, ImDrawFlags_Closed, t * 0.8f);
            }
            break;
        case Ico::Gauge:
            dl->PathArcTo(P(0.5f, 0.66f), s * 0.40f, kPi, 2 * kPi, 24);
            dl->PathStroke(c, 0, t);
            dl->AddLine(P(0.5f, 0.66f), P(0.74f, 0.40f), c, t);
            dl->AddCircleFilled(P(0.5f, 0.66f), t * 1.1f, c);
            break;
        case Ico::Hash:
            dl->AddLine(P(0.38f, 0.14f), P(0.30f, 0.86f), c, t);
            dl->AddLine(P(0.68f, 0.14f), P(0.60f, 0.86f), c, t);
            dl->AddLine(P(0.16f, 0.36f), P(0.86f, 0.36f), c, t);
            dl->AddLine(P(0.14f, 0.64f), P(0.84f, 0.64f), c, t);
            break;
    }
}

void icon(Ico ic, float s, const ImVec4& c) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawIco(ImGui::GetWindowDrawList(), ic, p, s, u32(c));
    ImGui::Dummy(ImVec2(s, s));
}

void spinner(float radius, const ImVec4& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ctr(p.x + radius, p.y + radius);
    float t = (float)ImGui::GetTime() * 6.0f;
    dl->AddCircle(ctr, radius * 0.8f, u32(c, 0.15f), 32, radius * 0.22f);
    dl->PathArcTo(ctr, radius * 0.8f, t, t + 1.8f + 0.9f * std::sin(t * 0.5f), 24);
    dl->PathStroke(u32(c), 0, radius * 0.22f);
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

// ============================================================================ widgets
namespace {
float controlHeight() { return ImGui::GetFrameHeight(); }

void textCentered(ImDrawList* dl, ImFont* f, ImVec2 a, ImVec2 b, ImU32 c, const char* txt, float xOffset = 0) {
    ImFont* font = f ? f : ImGui::GetFont();
    float sz = font->FontSize * ImGui::GetIO().FontGlobalScale;
    ImVec2 ts = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, txt, ImGui::FindRenderedTextEnd(txt));
    dl->AddText(font, sz, ImVec2(std::floor((a.x + b.x - ts.x) * 0.5f + xOffset), std::floor((a.y + b.y - ts.y) * 0.5f)), c, txt,
                ImGui::FindRenderedTextEnd(txt));
}

ImVec2 textSize(ImFont* f, const char* txt) {
    ImFont* font = f ? f : ImGui::GetFont();
    float sz = font->FontSize * ImGui::GetIO().FontGlobalScale;
    return font->CalcTextSizeA(sz, FLT_MAX, 0.0f, txt, ImGui::FindRenderedTextEnd(txt));
}

bool disabledNow() { return (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0; }
}  // namespace

bool primaryButton(const char* label, Ico ic, ImVec2 size, bool hasIcon) {
    const float e = em();
    ImFont* f = fonts().bold;
    ImVec2 ts = textSize(f, label);
    float iconS = hasIcon ? e * 1.05f : 0.0f;
    float gap = hasIcon ? e * 0.5f : 0.0f;
    if (size.y <= 0) size.y = controlHeight() * 1.1f;
    if (size.x <= 0) size.x = ts.x + iconS + gap + e * 2.2f;
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##pb", size);
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    float h = anim(ImGui::GetItemID(), hov && !disabledNow());
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding;
    if (h > 0.01f) {  // glow
        for (int i = 1; i <= 6; ++i) {
            float o = i * e * 0.09f;
            dl->AddRect(ImVec2(a.x - o, a.y - o), ImVec2(b.x + o, b.y + o), u32(col::Accent, 0.10f * h * (1.0f - i / 7.0f)), r + o, 0,
                        e * 0.1f);
        }
    }
    ImVec4 c0 = mix(col::Accent, ImVec4(1, 1, 1, 1), 0.10f * h), c1 = mix(mix(col::Accent, col::Accent2, 0.55f), ImVec4(1, 1, 1, 1), 0.10f * h);
    if (held) c0 = mix(c0, ImVec4(0, 0, 0, 1), 0.12f), c1 = mix(c1, ImVec4(0, 0, 0, 1), 0.12f);
    gradientRect(dl, a, b, u32(c0), u32(c1), r, true);
    // top sheen
    gradientRect(dl, a, ImVec2(b.x, a.y + (b.y - a.y) * 0.5f), white(0.10f), white(0.0f), r);
    float contentW = ts.x + iconS + gap;
    float x = std::floor((a.x + b.x - contentW) * 0.5f);
    if (hasIcon) drawIco(dl, ic, ImVec2(x, (a.y + b.y - iconS) * 0.5f), iconS, white());
    dl->AddText(f, f->FontSize * ImGui::GetIO().FontGlobalScale, ImVec2(x + iconS + gap, std::floor((a.y + b.y - ts.y) * 0.5f)),
                white(), label, ImGui::FindRenderedTextEnd(label));
    return clicked;
}

bool ghostButton(const char* label, Ico ic, ImVec2 size, bool hasIcon, bool enabled) {
    const float e = em();
    ImVec2 ts = textSize(nullptr, label);
    float iconS = hasIcon ? e * 1.05f : 0.0f;
    float gap = hasIcon && ts.x > 0 ? e * 0.45f : 0.0f;
    if (size.y <= 0) size.y = controlHeight() * 1.1f;
    if (size.x <= 0) size.x = ts.x + iconS + gap + e * 1.5f;
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##gb", size);
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && enabled, held = ImGui::IsItemActive();
    float h = anim(ImGui::GetItemID(), hov);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding;
    if (h > 0.0f) dl->AddRectFilled(a, b, u32(held ? col::Surface3 : mix(col::Surface2, col::Surface3, 0.6f), h), r);
    ImVec4 tc = mix(mix(col::Text, col::Dim, 0.15f), col::Text, h);
    float x = std::floor((a.x + b.x - (ts.x + iconS + gap)) * 0.5f);
    if (hasIcon) drawIco(dl, ic, ImVec2(x, (a.y + b.y - iconS) * 0.5f), iconS, u32(mix(col::Dim, col::AccentHover, h)));
    dl->AddText(ImVec2(x + iconS + gap, std::floor((a.y + b.y - ts.y) * 0.5f)), u32(tc), label, ImGui::FindRenderedTextEnd(label));
    ImGui::EndDisabled();
    return clicked && enabled;
}

bool secondaryButton(const char* label, ImVec2 size, bool enabled) {
    const float e = em();
    ImVec2 ts = textSize(nullptr, label);
    if (size.y <= 0) size.y = controlHeight() * 1.1f;
    if (size.x <= 0) size.x = ts.x + e * 2.0f;
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##sb", size);
    bool hov = ImGui::IsItemHovered() && enabled, held = ImGui::IsItemActive();
    float h = anim(ImGui::GetItemID(), hov);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(a, b, u32(held ? col::Surface3 : mix(col::Surface2, col::Surface3, h)), r);
    dl->AddRect(a, b, u32(mix(col::Line, col::Faint, h)), r, 0, 1.0f);
    textCentered(dl, nullptr, a, b, u32(col::Text), label);
    ImGui::EndDisabled();
    return clicked && enabled;
}

bool iconButton(const char* id, Ico ic, float size, const char* tip, bool enabled, const ImVec4* tint) {
    ImGui::BeginDisabled(!enabled);
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##ib", ImVec2(size, size));
    bool hov = ImGui::IsItemHovered() && enabled, held = ImGui::IsItemActive();
    float h = anim(ImGui::GetItemID(), hov);
    ImGui::PopID();
    if (tip && hov) ImGui::SetTooltip("%s", tip);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (h > 0) dl->AddRectFilled(a, b, u32(held ? col::Surface3 : mix(col::Surface2, col::Surface3, 0.5f), h), size * 0.28f);
    float is = size * 0.56f;
    ImVec4 c = tint ? *tint : mix(col::Dim, col::Text, h);
    drawIco(dl, ic, ImVec2(a.x + (size - is) * 0.5f, a.y + (size - is) * 0.5f), is, u32(c));
    ImGui::EndDisabled();
    return clicked && enabled;
}

void chip(const char* text, const ImVec4& c, bool filled) {
    ImFont* f = fonts().caption;
    ImVec2 ts = textSize(f, text);
    const float e = em();
    ImVec2 size(ts.x + e * 1.0f, ts.y + e * 0.32f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    // vertically centre against a normal text line
    float lineH = ImGui::GetTextLineHeight();
    p.y += std::floor((lineH - size.y) * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, b, u32(c, filled ? 0.95f : 0.13f), size.y * 0.5f);
    if (!filled) dl->AddRect(p, b, u32(c, 0.28f), size.y * 0.5f, 0, 1.0f);
    textCentered(dl, f, p, b, filled ? u32(col::Bg) : u32(mix(c, col::Text, 0.25f)), text);
    ImGui::Dummy(ImVec2(size.x, lineH));
}

bool pill(const char* label, bool selected, float minWidth) {
    const float e = em();
    ImVec2 ts = textSize(fonts().bold, label);
    ImVec2 size(std::max(minWidth, ts.x + e * 1.6f), controlHeight());
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##pill", size);
    bool hov = ImGui::IsItemHovered();
    float h = anim(ImGui::GetItemID(), hov);
    float s = anim(ImGui::GetItemID() + 1, selected, 18.0f);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    dl->AddRectFilled(a, b, u32(mix(mix(col::Surface2, col::Surface3, h), mix(col::Surface2, col::Accent, 0.30f), s)), r);
    dl->AddRect(a, b, u32(mix(mix(col::Line, col::Faint, h), col::Accent, s)), r, 0, 1.0f + s * 0.5f);
    textCentered(dl, fonts().bold, a, b, u32(mix(col::Dim, col::Text, std::max(h, s))), label);
    return clicked;
}

bool optionCard(const char* id, const char* title, const char* desc, bool selected, ImVec2 size, const char* badge) {
    const float e = em();
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##card", size);
    bool hov = ImGui::IsItemHovered();
    float h = anim(ImGui::GetItemID(), hov);
    float s = anim(ImGui::GetItemID() + 1, selected, 18.0f);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().FrameRounding * 1.2f;
    dl->AddRectFilled(a, b, u32(mix(mix(col::Surface1, col::Surface2, h), mix(col::Surface1, col::Accent, 0.13f), s)), r);
    dl->AddRect(a, b, u32(mix(mix(col::Line, col::Faint, h), col::Accent, s)), r, 0, 1.0f + s * 0.6f);
    float pad = e * 0.8f;
    // radio dot
    ImVec2 rc(b.x - pad - e * 0.45f, a.y + pad + e * 0.45f);
    dl->AddCircle(rc, e * 0.42f, u32(mix(col::Faint, col::Accent, s)), 20, 1.5f);
    if (s > 0.01f) dl->AddCircleFilled(rc, e * 0.24f * s, u32(col::Accent));
    ImFont* bf = fonts().bold;
    float bs = bf->FontSize * ImGui::GetIO().FontGlobalScale;
    dl->AddText(bf, bs, ImVec2(a.x + pad, a.y + pad), u32(col::Text), title);
    if (badge) {
        ImVec2 tw = textSize(bf, title);
        ImFont* sf = fonts().caption;
        ImVec2 bsz = textSize(sf, badge);
        ImVec2 ba(a.x + pad + tw.x + e * 0.45f, a.y + pad + (tw.y - bsz.y - e * 0.2f) * 0.5f);
        ImVec2 bb(ba.x + bsz.x + e * 0.7f, ba.y + bsz.y + e * 0.2f);
        dl->AddRectFilled(ba, bb, u32(col::Accent, 0.22f), (bb.y - ba.y) * 0.5f);
        textCentered(dl, sf, ba, bb, u32(col::AccentHover), badge);
    }
    ImFont* sf = fonts().caption;
    float ss = sf->FontSize * ImGui::GetIO().FontGlobalScale;
    dl->AddText(sf, ss, ImVec2(a.x + pad, a.y + pad + bs + e * 0.3f), u32(col::Dim), desc, nullptr, size.x - pad * 2);
    return clicked;
}

void caption(const char* text) {
    ImGui::PushFont(fonts().caption);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Faint);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void progressBar(float frac, ImVec2 size, const ImVec4& c) {
    const float e = em();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = e * 0.42f;
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + size.x, a.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = size.y * 0.5f;
    dl->AddRectFilled(a, b, u32(col::Surface3), r);
    ImVec4 c2 = mix(c, col::Accent2, 0.6f);
    if (frac < 0) {
        float t = std::fmod((float)ImGui::GetTime() * 0.9f, 1.0f);
        float x0 = a.x + (size.x * 1.3f) * t - size.x * 0.3f;
        float x1 = x0 + size.x * 0.3f;
        x0 = std::max(x0, a.x), x1 = std::min(x1, b.x);
        if (x1 > x0) gradientRect(dl, ImVec2(x0, a.y), ImVec2(x1, b.y), u32(c), u32(c2), r, true);
    } else if (frac > 0) {
        float w = std::max(size.y, size.x * std::min(frac, 1.0f));
        gradientRect(dl, a, ImVec2(a.x + w, b.y), u32(c), u32(c2), r, true);
    }
    ImGui::Dummy(size);
}

void ring(ImVec2 c, float radius, float thickness, float frac, const ImVec4& color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircle(c, radius, u32(col::Surface3), 64, thickness);
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (frac <= 0) return;
    float a0 = -kPi * 0.5f, a1 = a0 + 2 * kPi * frac;
    int segs = std::max(8, (int)(64 * frac));
    // gradient along the arc: draw in short pieces
    for (int i = 0; i < segs; ++i) {
        float t0 = float(i) / segs, t1 = float(i + 1) / segs;
        dl->PathArcTo(c, radius, a0 + (a1 - a0) * t0, a0 + (a1 - a0) * t1 + 0.01f, 3);
        dl->PathStroke(u32(mix(color, col::Accent2, t0 * 0.8f)), 0, thickness);
    }
    // rounded caps
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a0) * radius, c.y + std::sin(a0) * radius), thickness * 0.5f, u32(color));
    dl->AddCircleFilled(ImVec2(c.x + std::cos(a1) * radius, c.y + std::sin(a1) * radius), thickness * 0.5f,
                        u32(mix(color, col::Accent2, 0.8f)));
}

bool searchInput(const char* id, const char* hint, char* buf, size_t n, float width) {
    const float e = em();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(e * 2.1f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(width);
    bool changed = ImGui::InputTextWithHint(id, hint, buf, n);
    ImGui::PopStyleVar(2);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float is = e * 0.95f;
    bool act = ImGui::IsItemActive();
    drawIco(ImGui::GetWindowDrawList(), Ico::Search, ImVec2(a.x + e * 0.75f, (a.y + b.y - is) * 0.5f), is,
            u32(act ? col::AccentHover : col::Dim));
    if (act) ImGui::GetWindowDrawList()->AddRect(a, b, u32(col::Accent, 0.7f), (b.y - a.y) * 0.5f, 0, 1.5f);
    if (buf[0]) {
        float cs = (b.y - a.y) * 0.8f;
        ImGui::SetCursorScreenPos(ImVec2(b.x - cs - e * 0.25f, a.y + (b.y - a.y - cs) * 0.5f));
        if (iconButton("##clr", Ico::Close, cs, "Clear")) {
            buf[0] = 0;
            changed = true;
        }
    }
    return changed;
}

void kvRow(const char* key, const char* value, float keyWidth) {
    float x = ImGui::GetCursorPosX();
    ImGui::TextColored(col::Dim, "%s", key);
    ImGui::SameLine(x + keyWidth);
    ImGui::TextWrapped("%s", value);
}

bool beginDialog(const char* id, const char* title, const char* subtitle, Ico ic, bool* open, float width, const ImVec4& tint) {
    const float e = em();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, vp->WorkSize.y * 0.94f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(e * 1.5f, e * 1.3f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.075f, 0.078f, 0.102f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, mix(col::Line, col::Faint, 0.3f));
    bool vis = ImGui::Begin(id, open,
                            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - e * 3.0f);  // popped by endDialog
    if (!vis) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->PushClipRectFullScreen();
    softShadow(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetStyle().WindowRounding, e * 1.6f, 0.55f);
    dl->PopClipRect();
    dialogHeader(title, subtitle, ic, tint, width, open);
    return true;
}

void dialogHeader(const char* title, const char* subtitle, Ico ic, const ImVec4& tint, float width, bool* open) {
    const float e = em();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    // top accent hairline
    gradientRect(dl, ImVec2(wp.x + e * 1.2f, wp.y), ImVec2(wp.x + ws.x - e * 1.2f, wp.y + 2.0f), u32(tint, 0.0f), u32(tint, 0.9f), 0, true);

    float tile = e * 2.7f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    gradientRect(dl, p, ImVec2(p.x + tile, p.y + tile), u32(tint, 0.30f), u32(mix(tint, col::Accent2, 0.5f), 0.14f), e * 0.7f);
    dl->AddRect(p, ImVec2(p.x + tile, p.y + tile), u32(tint, 0.35f), e * 0.7f, 0, 1.0f);
    float is = tile * 0.52f;
    drawIco(dl, ic, ImVec2(p.x + (tile - is) * 0.5f, p.y + (tile - is) * 0.5f), is, u32(mix(tint, col::Text, 0.35f)));
    ImGui::Dummy(ImVec2(tile, tile));
    ImGui::SameLine(0, e * 0.9f);
    ImGui::BeginGroup();
    ImGui::PushFont(fonts().h2);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (subtitle && *subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - tile - e * 6.5f);
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    if (open) {
        float bs = e * 1.9f;
        ImVec2 keep = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(wp.x + ws.x - bs - e * 0.9f, wp.y + e * 0.9f));
        if (iconButton("##dlgclose", Ico::Close, bs, nullptr)) *open = false;
        ImGui::SetCursorScreenPos(keep);
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Dummy(ImVec2(width - e * 3.0f, e * 0.2f));
}

bool toggle(const char* label, bool* v) {
    const float e = em();
    float h = e * 1.15f, w = h * 1.85f;
    ImVec2 ts = textSize(nullptr, label);
    float lineH = std::max(h, ImGui::GetTextLineHeight());
    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##tg", ImVec2(w + e * 0.6f + ts.x, lineH));
    bool hov = ImGui::IsItemHovered();
    if (clicked) *v = !*v;
    float on = anim(ImGui::GetItemID(), *v, 16.0f);
    float hv = anim(ImGui::GetItemID() + 1, hov);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ta(a.x, a.y + (lineH - h) * 0.5f), tb(ta.x + w, ta.y + h);
    ImVec4 off = mix(col::Surface3, mix(col::Surface3, col::Faint, 0.5f), hv);
    if (on > 0.01f) gradientRect(dl, ta, tb, u32(col::Accent, on), u32(mix(col::Accent, col::Accent2, 0.5f), on), h * 0.5f, true);
    if (on < 0.99f) dl->AddRectFilled(ta, tb, u32(off, 1.0f - on), h * 0.5f);
    float kr = h * 0.5f - 2.5f;
    ImVec2 kc(ta.x + h * 0.5f + (w - h) * on, ta.y + h * 0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.0f), kr + 0.5f, IM_COL32(0, 0, 0, 60));
    dl->AddCircleFilled(kc, kr, white());
    dl->AddText(ImVec2(tb.x + e * 0.6f, a.y + (lineH - ts.y) * 0.5f), u32(col::Text), label, ImGui::FindRenderedTextEnd(label));
    return clicked;
}

bool slider(const char* id, int* v, int lo, int hi, float width) {
    const float e = em();
    float h = e * 1.5f, knob = e * 0.62f;
    ImGui::PushID(id);
    ImGui::InvisibleButton("##sl", ImVec2(width, h));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    float hv = anim(ImGui::GetItemID(), hov || act);
    ImGui::PopID();
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    float x0 = a.x + knob, x1 = b.x - knob;
    bool changed = false;
    if (act && hi > lo) {
        float t = ImClamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        int nv = lo + (int)std::lround(t * (hi - lo));
        if (nv != *v) *v = nv, changed = true;
    }
    if (hov && ImGui::IsItemFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && *v > lo) --*v, changed = true;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && *v < hi) ++*v, changed = true;
    }
    float t = hi > lo ? float(*v - lo) / float(hi - lo) : 0.0f;
    float cy = (a.y + b.y) * 0.5f, th = e * 0.32f;
    float kx = x0 + (x1 - x0) * t;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0 - th, cy - th * 0.5f), ImVec2(x1 + th, cy + th * 0.5f), u32(col::Surface3), th);
    // tick marks for small ranges
    if (hi - lo > 0 && hi - lo <= 24)
        for (int i = 0; i <= hi - lo; ++i) {
            float tx = x0 + (x1 - x0) * i / float(hi - lo);
            dl->AddCircleFilled(ImVec2(tx, cy), 1.5f, u32(col::Faint, 0.8f));
        }
    gradientRect(dl, ImVec2(x0 - th, cy - th * 0.5f), ImVec2(std::max(kx, x0 - th + th * 2), cy + th * 0.5f), u32(col::Accent),
                 u32(mix(col::Accent, col::Accent2, 0.6f)), th, true);
    if (hv > 0.01f) dl->AddCircleFilled(ImVec2(kx, cy), knob * (1.0f + 0.55f * hv), u32(col::Accent, 0.18f * hv), 24);
    dl->AddCircleFilled(ImVec2(kx, cy + 1.0f), knob, IM_COL32(0, 0, 0, 70), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), knob, white(), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), knob * 0.38f, u32(col::Accent), 16);
    return changed;
}

bool segmented(const char* id, int* v, const char* const* items, int count) {
    const float e = em();
    ImFont* f = fonts().caption;
    float h = ImGui::GetFrameHeight() * 0.92f, pad = 3.0f;
    float widths[8] = {}, total = pad * 2;
    for (int i = 0; i < count && i < 8; ++i) total += widths[i] = textSize(f, items[i]).x + e * 1.3f;
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + total, a.y + h), u32(col::Bg), h * 0.5f);
    dl->AddRect(a, ImVec2(a.x + total, a.y + h), u32(col::Line), h * 0.5f);
    bool changed = false;
    float x = a.x + pad;
    ImGui::PushID(id);
    for (int i = 0; i < count && i < 8; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, a.y + pad));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(widths[i], h - pad * 2)) && *v != i) *v = i, changed = true;
        bool hov = ImGui::IsItemHovered();
        float s = anim(ImGui::GetItemID(), *v == i, 18.0f);
        ImGui::PopID();
        ImVec2 ia = ImGui::GetItemRectMin(), ib = ImGui::GetItemRectMax();
        if (s > 0.01f) dl->AddRectFilled(ia, ib, u32(col::Surface3, s), (ib.y - ia.y) * 0.5f);
        textCentered(dl, f, ia, ib, u32(mix(hov ? col::Text : col::Dim, col::Text, s)), items[i]);
        x += widths[i];
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(a);
    ImGui::Dummy(ImVec2(total, h));
    return changed;
}

void drawLogo(ImDrawList* dl, ImVec2 p, float s) {
    ImVec2 b(p.x + s, p.y + s);
    gradientRect(dl, p, b, u32(col::Accent), u32(col::Accent2), s * 0.28f);
    gradientRect(dl, p, ImVec2(b.x, p.y + s * 0.5f), white(0.16f), white(0.0f), s * 0.28f);
    float in = s * 0.2f;
    drawIco(dl, Ico::Logo, ImVec2(p.x + in, p.y + in), s - in * 2, white());
}

void endDialog() {
    ImGui::PopTextWrapPos();
    ImGui::End();
}

void alignRight(float width) {
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

}  // namespace kzgui
