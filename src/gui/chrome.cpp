// Window chrome and small text helpers shared by the app and the self-extractor.
#include "chrome.hpp"

#include <GLFW/glfw3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace kzgui {

namespace {
float emPx() { return ImGui::GetFontSize(); }
}  // namespace

// Minimize / maximize / close at the right end of a custom title bar spanning [top, top + bh), ending at
// `right` (screen coordinates). Adds their rectangles to `hit`; returns the x where they begin.
float windowButtons(GLFWwindow* window, float right, float top, float bh, bool maximizable, std::vector<frame::Rect>& hit) {
    const float em = emPx();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bw = std::round(em * 3.1f), th = std::max(1.0f, std::round(em / 15.0f));
    int kinds[3] = {0, 1, 2}, n = 3;
    if (!maximizable) kinds[1] = 2, n = 2;
    for (int k = 0; k < n; ++k) {
        int i = kinds[k];
        float x = right - bw * (n - k);
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##wbtn", ImVec2(bw, bh));
        bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        float h = anim(ImGui::GetItemID(), hov, 20.0f);
        ImGui::PopID();
        hit.push_back({x - w->Pos.x, top - w->Pos.y, x + bw - w->Pos.x, top + bh - w->Pos.y});
        ImVec2 a(x, top), b(x + bw, top + bh);
        if (i == 2) {
            if (h > 0) dl->AddRectFilled(a, b, u32(ImVec4(0.78f, 0.17f, 0.20f, held ? 0.75f : 1.0f), h));
        } else if (h > 0) {
            dl->AddRectFilled(a, b, u32(held ? col::Line : col::Surface3, h));
        }
        ImU32 gc = u32(i == 2 ? mix(col::Dim, ImVec4(1, 1, 1, 1), h) : mix(col::Dim, col::Text, h));
        float g = std::round(em * 0.68f);
        ImVec2 c(std::round((a.x + b.x) * 0.5f), std::round((a.y + b.y) * 0.5f));
        if (i == 0) {
            dl->AddLine(ImVec2(c.x - g * 0.5f, c.y + 0.5f), ImVec2(c.x + g * 0.5f, c.y + 0.5f), gc, th);
        } else if (i == 1) {
            if (frame::maximized()) {
                float o = std::round(g * 0.22f);
                dl->AddRect(ImVec2(c.x - g * 0.5f, c.y - g * 0.5f + o), ImVec2(c.x + g * 0.5f - o, c.y + g * 0.5f), gc, 1.5f, 0, th);
                dl->PathLineTo(ImVec2(c.x - g * 0.5f + o, c.y - g * 0.5f + o));
                dl->PathLineTo(ImVec2(c.x - g * 0.5f + o, c.y - g * 0.5f));
                dl->PathLineTo(ImVec2(c.x + g * 0.5f, c.y - g * 0.5f));
                dl->PathLineTo(ImVec2(c.x + g * 0.5f, c.y + g * 0.5f - o));
                dl->PathLineTo(ImVec2(c.x + g * 0.5f - o, c.y + g * 0.5f - o));
                dl->PathStroke(gc, 0, th);
            } else {
                dl->AddRect(ImVec2(c.x - g * 0.5f, c.y - g * 0.5f), ImVec2(c.x + g * 0.5f, c.y + g * 0.5f), gc, 1.5f, 0, th);
            }
        } else {
            dl->AddLine(ImVec2(c.x - g * 0.5f, c.y - g * 0.5f), ImVec2(c.x + g * 0.5f, c.y + g * 0.5f), gc, th);
            dl->AddLine(ImVec2(c.x + g * 0.5f, c.y - g * 0.5f), ImVec2(c.x - g * 0.5f, c.y + g * 0.5f), gc, th);
        }
        if (clicked) {
            if (i == 0) frame::minimize();
            else if (i == 1) frame::toggleMaximize();
            else glfwSetWindowShouldClose(window, 1);
        }
    }
    return right - bw * n;
}

// `text` cut to fit `maxW` in the current font, with "..." at the end, or in the middle (paths: keeps
// the drive and the last folder).
std::string fitText(const std::string& text, float maxW, bool middle) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxW) return text;
    auto back = [&](size_t i) {  // start of the UTF-8 character before i
        do --i; while (i > 0 && (text[i] & 0xC0) == 0x80);
        return i;
    };
    auto fwd = [&](size_t i) {
        do ++i; while (i < text.size() && (text[i] & 0xC0) == 0x80);
        return i;
    };
    if (!middle) {
        size_t n = text.size();
        while (n > 0) {
            n = back(n);
            std::string t = text.substr(0, n) + "...";
            if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
        }
        return "...";
    }
    // drop characters around the middle until it fits
    size_t a = text.size() / 2, b = a;
    for (bool left = true; a > 0 || b < text.size(); left = !left) {
        if ((left && a > 0) || b >= text.size()) a = back(a);
        else b = fwd(b);
        std::string t = text.substr(0, a) + "..." + text.substr(b);
        if (ImGui::CalcTextSize(t.c_str()).x <= maxW) return t;
    }
    return "...";
}

// Icon tile in the style of dialogHeader(): rounded gradient square with the icon in its centre.
void iconTile(Ico ic, const ImVec4& tint, float tile) {
    const float e = emPx();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    gradientRect(dl, p, ImVec2(p.x + tile, p.y + tile), u32(tint, 0.30f), u32(mix(tint, col::Accent2, 0.5f), 0.14f), e * 0.7f);
    dl->AddRect(p, ImVec2(p.x + tile, p.y + tile), u32(tint, 0.35f), e * 0.7f, 0, 1.0f);
    float is = tile * 0.52f;
    drawIco(dl, ic, ImVec2(p.x + (tile - is) * 0.5f, p.y + (tile - is) * 0.5f), is, u32(mix(tint, col::Text, 0.35f)));
    ImGui::Dummy(ImVec2(tile, tile));
}

}  // namespace kzgui
