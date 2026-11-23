#include "theme.hpp"

namespace kzgui {

void applyTheme(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 6.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 7);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.CellPadding = ImVec2(8, 4);
    s.ScrollbarSize = 13.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;
    s.TabBarBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    ImVec4* c = s.Colors;
    const ImVec4 bg0{0.067f, 0.071f, 0.090f, 1.0f};  // window
    const ImVec4 bg1{0.098f, 0.102f, 0.129f, 1.0f};  // panels
    const ImVec4 bg2{0.137f, 0.141f, 0.180f, 1.0f};  // frames
    const ImVec4 bg3{0.180f, 0.184f, 0.235f, 1.0f};  // hover
    const ImVec4 line{0.200f, 0.204f, 0.259f, 1.0f};
    const ImVec4 a = col::Accent;

    c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.96f, 1.0f);
    c[ImGuiCol_TextDisabled] = col::Dim;
    c[ImGuiCol_WindowBg] = bg0;
    c[ImGuiCol_ChildBg] = bg1;
    c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.114f, 0.145f, 0.98f);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.21f, 0.30f, 1.0f);
    c[ImGuiCol_TitleBg] = bg1;
    c[ImGuiCol_TitleBgActive] = bg1;
    c[ImGuiCol_TitleBgCollapsed] = bg1;
    c[ImGuiCol_MenuBarBg] = bg1;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.26f, 0.26f, 0.33f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = a;
    c[ImGuiCol_CheckMark] = a;
    c[ImGuiCol_SliderGrab] = a;
    c[ImGuiCol_SliderGrabActive] = col::AccentHover;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = ImVec4(a.x, a.y, a.z, 0.55f);
    c[ImGuiCol_Header] = ImVec4(a.x, a.y, a.z, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(a.x, a.y, a.z, 0.18f);
    c[ImGuiCol_HeaderActive] = ImVec4(a.x, a.y, a.z, 0.40f);
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_SeparatorHovered] = a;
    c[ImGuiCol_SeparatorActive] = a;
    c[ImGuiCol_ResizeGrip] = ImVec4(a.x, a.y, a.z, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(a.x, a.y, a.z, 0.60f);
    c[ImGuiCol_ResizeGripActive] = a;
    c[ImGuiCol_Tab] = bg1;
    c[ImGuiCol_TabHovered] = bg3;
    c[ImGuiCol_TabSelected] = bg2;
    c[ImGuiCol_TabSelectedOverline] = a;
    c[ImGuiCol_TabDimmed] = bg1;
    c[ImGuiCol_TabDimmedSelected] = bg2;
    c[ImGuiCol_PlotHistogram] = a;
    c[ImGuiCol_PlotHistogramHovered] = col::AccentHover;
    c[ImGuiCol_TableHeaderBg] = bg1;
    c[ImGuiCol_TableBorderStrong] = line;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.16f, 0.21f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.018f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(a.x, a.y, a.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = col::Teal;
    c[ImGuiCol_NavCursor] = a;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.65f);

    s.ScaleAllSizes(scale);
}

bool accentButton(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, col::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::AccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, col::AccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

}  // namespace kzgui
