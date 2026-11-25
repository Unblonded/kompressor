// Rendering for the Kompressor app: shell, welcome screen, explorer, dialogs and overlays.
#include <GLFW/glfw3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "app.hpp"
#include "app_util.hpp"
#include "kmp.hpp"
#include "native_files.hpp"
#include "theme.hpp"
#include "shell_integration.hpp"
#include "kz_gpu.hpp"
#include "kz_perf.hpp"
#include "prefs.hpp"
#include "window_frame.hpp"

namespace kzgui {

namespace {

float emPx() { return ImGui::GetFontSize(); }

Ico iconFor(const Node& n) {
    if (n.isDir) return Ico::Folder;
    if (n.isLink) return Ico::Link;
    std::string e = kz::lowerExt(n.name);
    if (kz::looksLikeArchive(n.name)) return Ico::Archive;
    static const char* img[] = {"png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico", "tif", "tiff", "heic", "psd",
                                "mp4", "mov", "mkv", "mp3", "wav", "flac", "ogg"};
    static const char* code[] = {"c", "h", "cpp", "hpp", "cc", "cs", "java", "py", "js", "ts", "rs", "go", "swift", "m",
                                 "mm", "kt", "lua", "rb", "php", "sh", "json", "xml", "html", "css", "yml", "yaml", "toml", "cmake"};
    static const char* exe[] = {"exe", "dll", "so", "dylib", "bin", "app", "msi", "sys", "o", "a", "lib"};
    static const char* txt[] = {"txt", "md", "log", "ini", "cfg", "csv", "rtf", "pdf", "doc", "readme"};
    for (auto x : img) if (e == x) return Ico::Image;
    for (auto x : code) if (e == x) return Ico::Code;
    for (auto x : exe) if (e == x) return Ico::Exec;
    for (auto x : txt) if (e == x) return Ico::Text;
    return Ico::File;
}

ImVec4 iconColor(Ico ic) {
    switch (ic) {
        case Ico::Folder: return col::FolderCol;
        case Ico::Archive: return col::ArchiveCol;
        case Ico::Image: return col::ImageCol;
        case Ico::Code: return col::CodeCol;
        case Ico::Exec: return col::ExecCol;
        case Ico::Link: return col::Teal;
        default: return col::FileCol;
    }
}

std::string typeLabel(const Node& n) {
    if (n.isDir) return "Folder";
    if (n.isLink) return "Link";
    std::string e = kz::lowerExt(n.name);
    for (auto& c : e) c = char(std::toupper((unsigned char)c));
    return e.empty() ? "File" : e;
}

void centerText(const char* t, const ImVec4& c, ImFont* f = nullptr) {
    if (f) ImGui::PushFont(f);
    float w = ImGui::CalcTextSize(t).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + (ImGui::GetContentRegionAvail().x - w) * 0.5f));
    ImGui::TextColored(c, "%s", t);
    if (f) ImGui::PopFont();
}

// Rounded card background behind a region.
void cardBg(ImVec2 a, ImVec2 b, const ImVec4& fill = col::Surface1) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetStyle().ChildRounding;
    dl->AddRectFilled(a, b, u32(fill), r);
    dl->AddRect(a, b, u32(col::Line), r, 0, 1.0f);
}

// Rounded rect with a dashed border (drop zone).
void dashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c, float r, float dash, float th) {
    auto line = [&](ImVec2 p0, ImVec2 p1) {
        float len = std::sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y));
        int n = std::max(1, (int)(len / (dash * 2)));
        float step = len / n;
        ImVec2 d((p1.x - p0.x) / len, (p1.y - p0.y) / len);
        for (int i = 0; i < n; ++i) {
            float s0 = i * step + step * 0.25f, s1 = s0 + step * 0.5f;
            dl->AddLine(ImVec2(p0.x + d.x * s0, p0.y + d.y * s0), ImVec2(p0.x + d.x * s1, p0.y + d.y * s1), c, th);
        }
    };
    line(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y));
    line(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
    line(ImVec2(b.x - r, b.y), ImVec2(a.x + r, b.y));
    line(ImVec2(a.x, b.y - r), ImVec2(a.x, a.y + r));
    const float pi = 3.14159265f;
    dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, pi, pi * 1.5f, 8), dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, pi * 1.5f, pi * 2, 8), dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0, pi * 0.5f, 8), dl->PathStroke(c, 0, th);
    dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, pi * 0.5f, pi, 8), dl->PathStroke(c, 0, th);
}

// Icon inside a soft tinted rounded tile.
void iconTile(Ico ic, float size, const ImVec4& tint) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos(), b(p.x + size, p.y + size);
    gradientRect(dl, p, b, u32(tint, 0.24f), u32(tint, 0.10f), size * 0.28f);
    dl->AddRect(p, b, u32(tint, 0.25f), size * 0.28f, 0, 1.0f);
    float is = size * 0.56f;
    drawIco(dl, ic, ImVec2(p.x + (size - is) * 0.5f, p.y + (size - is) * 0.5f), is, u32(mix(tint, col::Text, 0.15f)));
    ImGui::Dummy(ImVec2(size, size));
}

// A small text button used for breadcrumbs.
bool crumb(const char* label, bool current, Ico* ic = nullptr) {
    const float e = emPx();
    ImFont* f = current ? fonts().bold : fonts().body;
    ImGui::PushFont(f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImGui::PopFont();
    float iconS = ic ? e * 1.0f : 0.0f, gap = ic ? e * 0.4f : 0.0f;
    ImVec2 size(ts.x + iconS + gap + e * 0.9f, ImGui::GetFrameHeight());
    bool clicked = ImGui::InvisibleButton(label, size);
    float h = anim(ImGui::GetItemID(), ImGui::IsItemHovered() && !current);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (h > 0) dl->AddRectFilled(a, b, u32(col::Surface3, h), e * 0.4f);
    float x = a.x + e * 0.45f;
    if (ic) {
        drawIco(dl, *ic, ImVec2(x, (a.y + b.y - iconS) * 0.5f), iconS, u32(col::ArchiveCol));
        x += iconS + gap;
    }
    dl->AddText(f, f->FontSize * ImGui::GetIO().FontGlobalScale, ImVec2(x, std::floor((a.y + b.y - ts.y) * 0.5f)),
                u32(current ? col::Text : mix(col::Dim, col::Text, h)), label);
    return clicked && !current;
}

// Stat tile used in the archive overview.
void statTile(const char* label, const std::string& value, Ico ic, const ImVec4& tint, float w) {
    const float e = emPx();
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + e * 3.6f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, u32(col::Surface2), e * 0.6f);
    float is = e * 1.0f;
    drawIco(dl, ic, ImVec2(a.x + e * 0.75f, a.y + e * 0.7f), is, u32(tint));
    ImFont* sf = fonts().caption;
    dl->AddText(sf, sf->FontSize * ImGui::GetIO().FontGlobalScale, ImVec2(a.x + e * 0.75f + is + e * 0.4f, a.y + e * 0.68f),
                u32(col::Dim), label);
    ImFont* bf = fonts().bold;
    dl->AddText(bf, bf->FontSize * ImGui::GetIO().FontGlobalScale * 1.1f, ImVec2(a.x + e * 0.75f, a.y + e * 1.95f), u32(col::Text),
                value.c_str());
    ImGui::Dummy(ImVec2(w, b.y - a.y));
}

// "about 1h 20m left", rounded the way a person would say it (the estimate is rough anyway).
std::string timeLeftText(double s) {
    char buf[48];
    if (s < 60) return "under a minute left";
    if (s < 3600) {
        std::snprintf(buf, sizeof buf, "about %dm left", (int)std::lround(s / 60.0));
    } else if (s < 100 * 3600) {
        int m = (int)std::lround(s / 60.0);
        std::snprintf(buf, sizeof buf, "about %dh %02dm left", m / 60, m % 60);
    } else {
        return "many hours left";
    }
    return buf;
}

std::string clockTime(double t) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

Ico logIcon(const ImVec4& c) {
    auto same = [](const ImVec4& a, const ImVec4& b) { return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) < 0.01f; };
    if (same(c, col::Ok)) return Ico::Check;
    if (same(c, col::Error)) return Ico::Error;
    if (same(c, col::Warn)) return Ico::Warn;
    return Ico::Info;
}

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
std::string fitText(const std::string& text, float maxW, bool middle = false) {
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

#ifdef __APPLE__
const char* kMod = "Cmd";
#else
const char* kMod = "Ctrl";
#endif

}  // namespace

// ============================================================================ frame
void App::frame() {
    pumpJobs();
    syncDropJob();
    updateBoost();
    dropTab_ = -1;  // set again by the archive view drawn this frame
    if (quick_) {
        drawQuick();
        return;
    }
    if (quitWhenDone_ && directJobs_ == 0 && !directFailed_) glfwSetWindowShouldClose(window_, 1);
    const float em = emPx();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.8f, em * 0.62f));  // taller menu bar
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);

    drawMenuBar();
    drawToolbar();

    const float pad = em * 1.1f;
    const float statusH = em * 2.0f;
    const float activityH = showActivity_ ? em * 10.5f : 0.0f;
    const float gap = em * 0.75f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + gap));
    float contentH = avail.y - gap - statusH - (showActivity_ ? activityH + gap : 0.0f) - gap;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##content", ImVec2(avail.x - pad * 2, contentH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (tabs_.empty()) {
        activeTab_ = -1;
        drawWelcome();
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.95f, em * 0.55f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(em * 0.5f, em * 0.3f));
        bool bar = ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                                                     ImGuiTabBarFlags_FittingPolicyScroll);
        ImGui::PopStyleVar(2);
        if (bar) {
            int toClose = -1;
            for (auto& tp : tabs_) {
                ExplorerTab& t = *tp;
                bool open = true;
                ImGuiTabItemFlags fl = focusTab_ == t.id ? ImGuiTabItemFlags_SetSelected : 0;
                std::string label = "      " + t.title + "###tab" + std::to_string(t.id);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.95f, em * 0.55f));
                bool sel = ImGui::BeginTabItem(label.c_str(), &open, fl);
                ImGui::PopStyleVar();
                {
                    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
                    float is = em * 1.05f;
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    ImVec2 ip(a.x + em * 0.85f, (a.y + b.y - is) * 0.5f);
                    if (t.loading) {
                        float tt = (float)ImGui::GetTime() * 6.0f;
                        dl->PathArcTo(ImVec2(ip.x + is * 0.5f, ip.y + is * 0.5f), is * 0.38f, tt, tt + 4.0f, 16);
                        dl->PathStroke(u32(col::Accent), 0, em * 0.13f);
                    } else {
                        drawIco(dl, t.info.isKmp ? Ico::Sparkle : Ico::Archive, ip, is,
                                u32(t.info.isKmp ? col::Accent : col::ArchiveCol, sel ? 1.0f : 0.7f));
                    }
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) && !t.src.inMemory()) ImGui::SetTooltip("%s", t.src.path.c_str());
                if (sel) {
                    activeTab_ = t.id;
                    ImGui::Dummy(ImVec2(0, em * 0.35f));
                    drawExplorer(t);
                    ImGui::EndTabItem();
                }
                if (!open) toClose = t.id;
            }
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) actionOpen();
            ImGui::SetItemTooltip("Open another archive (%s+O)", kMod);
            focusTab_ = -1;
            ImGui::EndTabBar();
            if (toClose >= 0) closeTab(toClose);
        }
    }
    ImGui::EndChild();

    if (showActivity_) {
        ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + gap - ImGui::GetStyle().ItemSpacing.y));
        drawActivity();
    }
    drawStatusBar();
    ImGui::End();
    if (frame::custom() && !frame::maximized())
        ImGui::GetForegroundDrawList()->AddRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                u32(mix(col::Line, col::Faint, 0.3f)));

    // Global shortcuts (Ctrl on Windows/Linux, Cmd on macOS).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) actionOpen();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteGlobal)) openCompressDialog({}, false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, ImGuiInputFlags_RouteGlobal)) openCompressDialog({}, true);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal) && activeTab_ >= 0) closeTab(activeTab_);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, ImGuiInputFlags_RouteGlobal)) showSettings_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, ImGuiInputFlags_RouteGlobal))
        if (ExplorerTab* t = tabById(activeTab_); t && !t->loading && t->error.empty()) actionSearch(*t, false);

    drawCompressWindow();
    drawExtractWindow();
    drawConvertWindow();
    drawFormats();
    drawHashWindow();
    drawSearchWindow();
    drawRamDriveWindow();
    drawAbout();
    drawSettings();
    drawPasswordPopup();
    drawConfirmPopup();
    fileDlg_.draw();
    if (std::string e = fileDlg_.takeError(); !e.empty()) {
        log(col::Error, e);
        toast(col::Error, e);
    }
    drawToasts();
}

// ============================================================================ quick-task window
// Explorer "Extract here" / "Add to .7z" ...: a small window with the task alone - what is being done,
// progress, speed and time left, pause / cancel, a password prompt when one is needed, and how it ended.
void App::drawQuickTitleBar() {
    if (!ImGui::BeginMenuBar()) return;
    const float em = emPx();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImRect mb = w->MenuBarRect();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::vector<frame::Rect> hit;
    dl->AddLine(ImVec2(mb.Min.x, mb.Max.y - 1), ImVec2(mb.Max.x, mb.Max.y - 1), u32(col::Line));
    float ls = em * 1.2f;
    ImGui::SetCursorPosX(em * 1.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawLogo(dl, ImVec2(p.x, (mb.Min.y + mb.Max.y - ls) * 0.5f), ls);
    ImGui::Dummy(ImVec2(ls, 1));
    ImGui::SameLine(0, em * 0.5f);
    ImGui::PushFont(fonts().bold);
    ImGui::TextUnformatted("Kompressor");
    ImGui::PopFont();
    if (frame::custom()) windowButtons(window_, mb.Max.x, w->Pos.y, mb.Max.y - w->Pos.y, false, hit);
    frame::setTitleBar(mb.Max.y - w->Pos.y, hit);
    ImGui::EndMenuBar();
}

void App::drawQuick() {
    const float em = emPx();
    const double now = ImGui::GetTime();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.8f, em * 0.5f));
    ImGui::Begin("##quick", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);
    drawQuickTitleBar();

    const float pad = em * 1.1f;
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float inner = W - pad * 2;
    const float tile = em * 2.7f;
    const float bh = ImGui::GetFrameHeight() * 1.05f;
    const float textX = pad + tile + em * 0.85f;
    const float textW = W - textX - pad;
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + em * 0.9f));
    const float top = ImGui::GetCursorPosY();
    // Header: icon tile, title, and one dim line under it.
    auto header = [&](Ico ic, const ImVec4& tint, const std::string& title, const std::string& line) {
        ImGui::SetCursorPos(ImVec2(pad, top));
        iconTile(ic, tint, tile);
        ImGui::SetCursorPos(ImVec2(textX, top - em * 0.1f));
        ImGui::PushFont(fonts().h2);
        ImGui::TextUnformatted(fitText(title, textW).c_str());
        ImGui::PopFont();
        if (!line.empty()) {
            ImGui::SetCursorPosX(textX);
            ImGui::TextColored(col::Dim, "%s", fitText(line, textW).c_str());
        }
        ImGui::SetCursorPos(ImVec2(pad, top + tile + em * 0.85f));
    };
    auto buttonRow = [&] { ImGui::SetCursorPos(ImVec2(pad, H - pad - bh)); };
    // Puts `width` of buttons against the right padding (alignEnd() would run them to the window's edge).
    auto alignEnd = [&](float width) { ImGui::SetCursorPosX(W - pad - width); };
    auto openFull = [&] {
        if (ghostButton("Open in Kompressor", Ico::Open, ImVec2(0, bh))) leaveQuickMode();
        ImGui::SetItemTooltip("Show the full Kompressor window, with the activity log");
    };

    std::vector<Job*> act;
    for (auto& jp : jobs_)
        if (jp->direct && !jp->finished) act.push_back(jp.get());

    bool quit = false;
    if (pwd_.onOk) {
        // ---------------------------------------------------------------- password
        header(Ico::Lock, col::Warn, "Password needed", pwd_.prompt);
        float ib = ImGui::GetFrameHeight();
        if (pwd_.request) {
            ImGui::SetKeyboardFocusHere();
            pwd_.request = false;
        }
        ImGui::SetNextItemWidth(inner - ib - em * 0.4f);
        bool enter = ImGui::InputTextWithHint("##qpw", "Password", pwd_.buf, sizeof pwd_.buf,
                                              ImGuiInputTextFlags_EnterReturnsTrue | (pwd_.show ? 0 : ImGuiInputTextFlags_Password));
        ImGui::SameLine(0, em * 0.4f);
        const ImVec4 on = col::AccentHover;
        if (iconButton("##qshow", Ico::Eye, ib, pwd_.show ? "Hide password" : "Show password", true, pwd_.show ? &on : nullptr))
            pwd_.show = !pwd_.show;
        buttonRow();
        float pbw = em * 7.0f, cbw = em * 6.0f;
        alignEnd(pbw + cbw + em * 0.5f);
        bool cancel = secondaryButton("Cancel", ImVec2(cbw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SameLine(0, em * 0.5f);
        bool ok = primaryButton("Unlock", Ico::Lock, ImVec2(pbw, bh)) || enter;
        if (ok || cancel) {
            std::string pw = pwd_.buf;
            std::memset(pwd_.buf, 0, sizeof pwd_.buf);
            auto okCb = std::move(pwd_.onOk);
            auto cancelCb = std::move(pwd_.onCancel);
            pwd_.onOk = nullptr;
            pwd_.onCancel = nullptr;
            pwd_.request = false;
            if (ok && okCb) okCb(pw);
            else if (cancel && cancelCb) cancelCb();
        }
    } else if (!act.empty()) {
        // ---------------------------------------------------------------- working
        bool allPaused = true, det = true;
        double done = 0, total = 0, rate = 0, left = 0, elapsed = 0;
        int rated = 0;
        for (Job* j : act) {
            j->sampleRate(now);
            if (!j->paused()) allPaused = false;
            uint64_t t = j->prog->total.load();
            if (t == 0) det = false;
            total += (double)t;
            done += (double)j->prog->fraction() * (double)t;
            if (j->rateSamples >= 3) rate += j->rate, ++rated;
            double l = j->secondsLeft();
            left = (left < 0 || l < 0) ? -1.0 : std::max(left, l);
            elapsed = std::max(elapsed, j->elapsed(now));
        }
        float frac = det && total > 0 ? (float)(done / total) : -1.0f;
        std::string what, dest = act[0]->dest;
        for (size_t i = 0; i < act.size() && i < 2; ++i) what += (i ? ", " : "") + act[i]->subject;
        if (act.size() > 2) what += " and " + std::to_string(act.size() - 2) + " more";
        for (Job* j : act)
            if (j->dest != dest) dest = parentDir(dest);  // e.g. "Extract to folder" on several archives
        if (quickVerb_ != "Extracting") dest = kz::fileNameOf(dest);  // the new archive sits beside its files
        std::string arrow = "  ->  ";
        std::string line = what + arrow + dest;
        if (ImGui::CalcTextSize(line.c_str()).x > textW) {  // squeeze the path, not the names
            float room = textW - ImGui::CalcTextSize((what + arrow).c_str()).x;
            line = room > em * 6 ? what + arrow + fitText(dest, room, true) : what;
        }
        Ico ic = quickVerb_ == "Extracting" ? Ico::Extract : quickVerb_ == "Super-compressing" ? Ico::Sparkle : Ico::Box;
        header(ic, allPaused ? col::Warn : col::Accent, allPaused ? "Paused" : quickVerb_, line);
        progressBar(allPaused ? std::max(frac, 0.0f) : frac, ImVec2(inner, em * 0.42f), allPaused ? col::Warn : col::Accent);
        ImGui::Dummy(ImVec2(0, em * 0.15f));
        // 42%  -  12.3 MB/s  -  about 1m left                      0:12
        std::string st;
        if (frac >= 0) st = std::to_string((int)std::floor(frac * 100.0f)) + "%";
        if (allPaused) {
            st += st.empty() ? "Paused" : "  -  paused";
        } else {
            if (rated) st += (st.empty() ? "" : "  -  ") + (rate < 1024.0 ? std::string("under 1 KB/s") : kz::humanSize((uint64_t)rate) + "/s");
            if (left >= 0 && rated) st += "  -  " + timeLeftText(left);
            if (st.empty()) st = act[0]->prog->status();
        }
        ImGui::SetCursorPosX(pad);
        std::string el = kz::formatDuration(elapsed);
        float elW = ImGui::CalcTextSize(el.c_str()).x;
        ImGui::TextColored(allPaused ? col::Warn : col::Dim, "%s", fitText(st, inner - elW - em).c_str());
        ImGui::SameLine(pad + inner - elW);
        ImGui::TextColored(col::Faint, "%s", el.c_str());

        buttonRow();
        openFull();
        float pbw = em * 6.0f, cbw = em * 6.0f;
        ImGui::SameLine();
        alignEnd(pbw + cbw + em * 0.5f);
        if (secondaryButton(allPaused ? "Resume" : "Pause", ImVec2(pbw, bh)))
            for (Job* j : act) j->setPaused(!allPaused, now);
        ImGui::SameLine(0, em * 0.5f);
        if (secondaryButton("Cancel", ImVec2(cbw, bh)))
            for (Job* j : act) {
                j->prog->cancel = true;  // also wakes a paused worker so it can stop
                j->setPaused(false, now);
            }
    } else if (directJobs_ == 0) {
        // ---------------------------------------------------------------- finished
        if (quickDoneAt_ < 0) quickDoneAt_ = now;
        const QuickResult* failed = nullptr;
        const QuickResult* last = nullptr;
        int ok = 0;
        for (auto& r : quickResults_) {
            if (r.ok) ++ok, last = &r;
            else if (!r.cancelled && !failed) failed = &r;
        }
        const bool extracting = quickVerb_ == "Extracting";
        if (failed) {
            header(Ico::Error, col::Error, (extracting ? "Couldn't extract " : "Couldn't create ") + failed->subject, "");
            // The reason, wrapped over the lines left above the buttons.
            ImGui::SetCursorPos(ImVec2(textX, top + ImGui::GetTextLineHeightWithSpacing() * 1.35f));
            ImGui::PushTextWrapPos(W - pad);
            ImGui::PushStyleColor(ImGuiCol_Text, mix(col::Error, col::Text, 0.55f));
            std::string msg = failed->message.empty() ? std::string("Something went wrong.") : failed->message;
            if (ok) msg += "  (" + std::to_string(ok) + " other" + (ok > 1 ? "s" : "") + " finished.)";
            ImGui::TextWrapped("%s", msg.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            buttonRow();
            openFull();
            float sbw = em * 8.6f, cbw = em * 5.4f;
            ImGui::SameLine();
            alignEnd(sbw + cbw + em * 0.5f);
            if (secondaryButton("Show in folder", ImVec2(sbw, bh))) reveal(failed->reveal);
            ImGui::SameLine(0, em * 0.5f);
            if (primaryButton("Close", Ico::Close, ImVec2(cbw, bh), false) || ImGui::IsKeyPressed(ImGuiKey_Escape)) quit = true;
        } else if (!last) {
            quit = true;  // everything was cancelled
        } else {
            std::string verb = extracting ? "Extracted " : "Created ";
            std::string title = ok > 1 ? verb + std::to_string(ok) + " archives" : verb + last->subject;
            std::string line = extracting ? "to " + fitText(last->reveal, textW - ImGui::CalcTextSize("to ").x, true) : last->message;
            header(Ico::Check, col::Ok, title, line);
            progressBar(1.0f, ImVec2(inner, em * 0.42f), col::Ok);
            buttonRow();
            float sbw = em * 8.6f, cbw = em * 5.4f;
            alignEnd(sbw + cbw + em * 0.5f);
            if (secondaryButton("Show in folder", ImVec2(sbw, bh))) {
                reveal(last->reveal);
                quit = true;
            }
            ImGui::SameLine(0, em * 0.5f);
            if (primaryButton("Close", Ico::Close, ImVec2(cbw, bh), false)) quit = true;
            // Closes on its own a moment later, unless the pointer rests on the window (to click a button).
            if (glfwGetWindowAttrib(window_, GLFW_HOVERED)) quickDoneAt_ = std::max(quickDoneAt_, now - 0.6);
            if (now - quickDoneAt_ > 1.6) quit = true;
        }
    }
    if (quit) glfwSetWindowShouldClose(window_, 1);
    ImGui::End();
    if (frame::custom())
        ImGui::GetForegroundDrawList()->AddRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                u32(mix(col::Line, col::Faint, 0.3f)));
}

void App::leaveQuickMode() {
    quick_ = false;
    quitWhenDone_ = false;  // the user wants to look around: stay open
    float xs = 1.0f, ys = 1.0f;
#ifndef __APPLE__
    glfwGetWindowContentScale(window_, &xs, &ys);  // window sizes are in pixels here
#endif
    glfwSetWindowTitle(window_, "Kompressor");
    glfwSetWindowAttrib(window_, GLFW_RESIZABLE, GLFW_TRUE);
    glfwSetWindowSizeLimits(window_, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    frame::placeCentered((int)std::lround(1320 * xs), (int)std::lround(840 * xs));
}

void App::enterQuickMode(const std::string& verb) {
    quick_ = true;
    quickVerb_ = verb;
    glfwSetWindowTitle(window_, (verb + " - Kompressor").c_str());
}

void App::drawSplitOption(bool* on, int* preset, int* customMb, float inner) {
    (void)inner;
    ImGui::PushID(on);
    toggle("Split into parts", on);
    ImGui::SetItemTooltip("Cut the finished archive into .001, .002 ... files of a set size\n"
                          "(for uploads, e-mail or FAT32 drives). Open the .001 to read them back.");
    if (*on) {
        const float em = ImGui::GetFontSize();
        static const char* sizes[] = {"100 MB", "700 MB", "4 GB (FAT32)", "Custom"};
        ImGui::Indent(em * 1.6f);
        segmented("##split", preset, sizes, 4);
        if (*preset == 3) {
            ImGui::SameLine(0, em * 0.6f);
            ImGui::SetNextItemWidth(em * 6.0f);
            ImGui::InputInt("##mb", customMb, 0, 0);
            if (*customMb < 1) *customMb = 1;
            ImGui::SameLine(0, em * 0.4f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col::Dim, "MB per part");
        }
        ImGui::Unindent(em * 1.6f);
    }
    ImGui::PopID();
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
    const float em = emPx();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImRect mb = w->MenuBarRect();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::vector<frame::Rect> hit;  // clickable parts of the title bar; the rest drags the window
    auto addHit = [&] {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        hit.push_back({a.x - w->Pos.x, a.y - w->Pos.y, b.x - w->Pos.x, b.y - w->Pos.y});
    };
    dl->AddLine(ImVec2(mb.Min.x, mb.Max.y - 1), ImVec2(mb.Max.x, mb.Max.y - 1), u32(col::Line));

    // brand
    float ls = em * 1.35f;
    ImGui::SetCursorPosX(em * 1.1f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawLogo(dl, ImVec2(p.x, (mb.Min.y + mb.Max.y - ls) * 0.5f), ls);
    ImGui::Dummy(ImVec2(ls, 1));
    ImGui::SameLine(0, em * 0.55f);
    ImGui::PushFont(fonts().bold);
    ImGui::TextUnformatted("Kompressor");
    ImGui::PopFont();
    ImGui::SameLine(0, em * 1.4f);

    std::string kO = std::string(kMod) + "+O", kN = std::string(kMod) + "+N", kSN = std::string(kMod) + "+Shift+N", kSet = std::string(kMod) + "+,",
                kW = std::string(kMod) + "+W";
    ExplorerTab* at = tabById(activeTab_);
    bool ready = at && !at->loading && !at->nodes.empty();
    ImGui::PushStyleColor(ImGuiCol_Text, mix(col::Dim, col::Text, 0.4f));
    bool fileOpen = ImGui::BeginMenu("File");
    addHit();
    ImGui::PopStyleColor();
    if (fileOpen) {
        if (ImGui::MenuItem("Open Archive...", kO.c_str())) actionOpen();
        if (ImGui::MenuItem("New Archive...", kN.c_str())) openCompressDialog({}, false);
        if (ImGui::MenuItem("Super Compress...", kSN.c_str())) openCompressDialog({}, true);
        ImGui::Separator();
        if (ImGui::MenuItem("RAM Drive...", nullptr, !ram_.busy && ram_.drive != nullptr)) openRamDrive();
        ImGui::SetItemTooltip("A drive kept in memory, for fast scratch files; emptied when it stops");
        ImGui::Separator();
        if (ImGui::MenuItem("Close Tab", kW.c_str(), false, at != nullptr)) closeTab(activeTab_);
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...", kSet.c_str())) showSettings_ = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window_, 1);
        ImGui::EndMenu();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, mix(col::Dim, col::Text, 0.4f));
    bool archOpen = ImGui::BeginMenu("Archive", ready);
    addHit();
    ImGui::PopStyleColor();
    if (archOpen) {
        if (ImGui::MenuItem("Extract All...")) actionExtract(*at, false);
        if (ImGui::MenuItem("Extract Selected...", nullptr, false, !at->selection.empty())) actionExtract(*at, true);
        if (ImGui::MenuItem("Test Integrity")) actionTest(*at);
        if (ImGui::MenuItem("Checksums...")) actionHash(*at, false);
        ImGui::SetItemTooltip("CRC32, MD5, SHA-1 and SHA-256 of every file, and checking them against a list");
        std::string kF = std::string(kMod) + "+Shift+F";
        if (ImGui::MenuItem("Find in Files...", kF.c_str())) actionSearch(*at, false);
        ImGui::SetItemTooltip("Search the text (or bytes) inside every file, without extracting anything");
        ImGui::Separator();
        bool canEdit = at->editable && !at->editing;
        if (ImGui::MenuItem("Add Files...", nullptr, false, canEdit)) pickFilesToAdd(*at, false);
        if (ImGui::MenuItem("Add Folder...", nullptr, false, canEdit)) pickFilesToAdd(*at, true);
        if (native::fileTransferSupported()) {
            std::string kC = std::string(kMod) + "+C", kV = std::string(kMod) + "+V";
            if (ImGui::MenuItem("Copy Selected", kC.c_str(), false, !at->selection.empty())) copySelection(*at);
            if (ImGui::MenuItem("Paste Files", kV.c_str(), false, canEdit)) pasteFiles(*at);
        }
        if (ImGui::MenuItem("Delete Selected", "Del", false, canEdit && !at->selection.empty())) askDeleteSelection(*at);
        if (!at->editable && !at->editWhy.empty()) ImGui::SetItemTooltip("%s", at->editWhy.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Recompress As...")) actionConvert(*at);
        if (ImGui::MenuItem("Super Compress to .kmp...")) actionConvert(*at, true);
        if (ImGui::MenuItem("Reload")) loadTab(at->id);
        if (!at->src.inMemory()) {
#ifdef _WIN32
            if (ImGui::MenuItem("Open in File Explorer")) openInExplorer(*at);
            ImGui::SetItemTooltip("Browse this archive in Windows File Explorer (for the kinds Explorer can open)");
#endif
            if (ImGui::MenuItem("Show in File Manager")) reveal(at->src.path);
        }
        ImGui::EndMenu();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, mix(col::Dim, col::Text, 0.4f));
    bool viewOpen = ImGui::BeginMenu("View");
    addHit();
    ImGui::PopStyleColor();
    if (viewOpen) {
        ImGui::MenuItem("Activity Panel", nullptr, &showActivity_);
        ImGui::EndMenu();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, mix(col::Dim, col::Text, 0.4f));
    bool helpOpen = ImGui::BeginMenu("Help");
    addHit();
    ImGui::PopStyleColor();
    if (helpOpen) {
        if (ImGui::MenuItem("Supported Formats")) showFormats_ = true;
        if (ImGui::MenuItem("About Kompressor")) showAbout_ = true;
        ImGui::EndMenu();
    }

    // right side: window controls (custom title bar) and version
    float right = mb.Max.x;
    if (frame::custom()) right = windowButtons(window_, right, w->Pos.y, mb.Max.y - w->Pos.y, true, hit) - em * 0.4f;
    char ver[32];
    std::snprintf(ver, sizeof ver, "v%s", KZ_VERSION);
    ImGui::PushFont(fonts().caption);
    float vw = ImGui::CalcTextSize(ver).x + em * 1.0f;
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(right - vw - em * 0.7f, (mb.Min.y + mb.Max.y - ImGui::GetTextLineHeight()) * 0.5f));
    chip(ver, col::Dim);
    frame::setTitleBar(mb.Max.y - w->Pos.y, hit);
    ImGui::EndMenuBar();
}

void App::drawToolbar() {
    const float em = emPx();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float W = ImGui::GetWindowWidth();
    float h = em * 3.5f;
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + h), u32(col::Surface1));
    dl->AddLine(ImVec2(p0.x, p0.y + h - 1), ImVec2(p0.x + W, p0.y + h - 1), u32(col::Line));
    float bh = em * 2.35f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + em * 1.0f, p0.y + (h - bh) * 0.5f));
    const float sp = em * 0.3f;

    if (ghostButton("Open", Ico::Open, ImVec2(0, bh))) actionOpen();
    ImGui::SetItemTooltip("Open and explore an archive without extracting it (%s+O)", kMod);
    ImGui::SameLine(0, sp);
    // With an archive open, Compress and Super Compress re-pack that archive into another format.
    ExplorerTab* at = tabById(activeTab_);
    bool ready = at && !at->loading && !at->nodes.empty();
    if (ghostButton("Compress", Ico::Box, ImVec2(0, bh))) {
        if (ready) actionConvert(*at);
        else openCompressDialog({}, false);
    }
    if (ready) ImGui::SetItemTooltip("Recompress \"%s\" into another format (ZIP, 7Z, TAR.ZST...)", at->title.c_str());
    else ImGui::SetItemTooltip("Create a ZIP, 7Z, TAR.XZ, TAR.ZST... archive (%s+N)", kMod);
    ImGui::SameLine(0, sp * 2);
    if (primaryButton("Super Compress", Ico::Bolt, ImVec2(0, bh))) {
        if (ready) actionConvert(*at, true);
        else openCompressDialog({}, true);
    }
    if (ready) ImGui::SetItemTooltip("Recompress \"%s\" into Kompressor's .kmp format", at->title.c_str());
    else ImGui::SetItemTooltip("Kompressor's own .kmp format: dedup + content-aware LZMA2/Zstd");

    if (ready) {
        ImGui::SameLine(0, em * 0.9f);
        ImVec2 sp0 = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(sp0.x, p0.y + h * 0.28f), ImVec2(sp0.x, p0.y + h * 0.72f), u32(col::Line), 1.0f);
        ImGui::Dummy(ImVec2(1, bh));
        ImGui::SameLine(0, em * 0.9f);
        if (ghostButton("Extract All", Ico::Extract, ImVec2(0, bh))) actionExtract(*at, false);
        ImGui::SetItemTooltip("Extract everything in this archive");
        ImGui::SameLine(0, sp);
        if (ghostButton("Extract Selected", Ico::Extract, ImVec2(0, bh), true, !at->selection.empty())) actionExtract(*at, true);
        ImGui::SameLine(0, sp);
        if (ghostButton("Test", Ico::Shield, ImVec2(0, bh))) actionTest(*at);
        ImGui::SetItemTooltip("Decompress everything in memory and verify checksums");
        ImGui::SameLine(0, sp);
        if (ghostButton("Checksums", Ico::Hash, ImVec2(0, bh))) actionHash(*at, !at->selection.empty());
        ImGui::SetItemTooltip("CRC32 / MD5 / SHA-1 / SHA-256 of the files (the selected ones, or all), and checking a list");
        ImGui::SameLine(0, sp);
        if (ghostButton("Find", Ico::Search, ImVec2(0, bh))) actionSearch(*at, !at->selection.empty());
        ImGui::SetItemTooltip("Search the text or bytes inside the files (the selected ones, or all)");
        if (at->editable) {
            ImGui::SameLine(0, em * 0.9f);
            ImVec2 sp1 = ImGui::GetCursorScreenPos();
            dl->AddLine(ImVec2(sp1.x, p0.y + h * 0.28f), ImVec2(sp1.x, p0.y + h * 0.72f), u32(col::Line), 1.0f);
            ImGui::Dummy(ImVec2(1, bh));
            ImGui::SameLine(0, em * 0.9f);
            if (ghostButton("Add", Ico::Plus, ImVec2(0, bh), true, !at->editing)) pickFilesToAdd(*at, false);
            ImGui::SetItemTooltip("Add files to the folder you're in (or drop them onto the list)");
            ImGui::SameLine(0, sp);
            if (ghostButton("Delete", Ico::Trash, ImVec2(0, bh), true, !at->editing && !at->selection.empty()))
                askDeleteSelection(*at);
            ImGui::SetItemTooltip("Remove the selected entries from the archive (Del)");
        }
    }

    // right: settings + activity toggle
    float is = em * 2.2f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + W - is * 2 - em * 1.4f, p0.y + (h - is) * 0.5f));
    if (iconButton("##settings", Ico::Gear, is, "Settings")) showSettings_ = true;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + W - is - em * 1.0f, p0.y + (h - is) * 0.5f));
    const ImVec4 on = col::AccentHover;
    if (iconButton("##activity", Ico::Panel, is, showActivity_ ? "Hide activity panel" : "Show activity panel", true,
                   showActivity_ ? &on : nullptr))
        showActivity_ = !showActivity_;

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
    ImGui::Dummy(ImVec2(0, 0));
}

void App::drawStatusBar() {
    const float em = emPx();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    ImVec2 ws = ImGui::GetWindowSize();
    float h = em * 2.0f;
    ImVec2 a(wp.x, wp.y + ws.y - h), b(wp.x + ws.x, wp.y + ws.y);
    dl->AddRectFilled(a, b, u32(col::Surface1));
    dl->AddLine(a, ImVec2(b.x, a.y), u32(col::Line));

    int running = 0, pausedJobs = 0;
    for (auto& j : jobs_)
        if (j->visible && !j->finished) (j->paused() ? pausedJobs : running)++;
    float cy = (a.y + b.y) * 0.5f;
    float x = a.x + em * 1.1f;
    ImVec4 dot = running ? col::Teal : col::Ok;
    float pulse = running ? 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f) : 0.0f;
    if (running) dl->AddCircleFilled(ImVec2(x + em * 0.3f, cy), em * (0.3f + 0.25f * pulse), u32(dot, 0.25f * (1.0f - pulse)));
    dl->AddCircleFilled(ImVec2(x + em * 0.3f, cy), em * 0.24f, u32(dot));
    char left[96];
    if (running) std::snprintf(left, sizeof left, "%d task%s running%s%s", running, running > 1 ? "s" : "",
                               pausedJobs ? ", some paused" : "", kz::perf::boosted() ? "  -  maximum performance" : "");
    else if (pausedJobs) std::snprintf(left, sizeof left, "%d task%s paused", pausedJobs, pausedJobs > 1 ? "s" : "");
    else std::snprintf(left, sizeof left, "Ready");
    ImFont* sf = fonts().caption;
    float fs = sf->FontSize * ImGui::GetIO().FontGlobalScale;
    dl->AddText(sf, fs, ImVec2(x + em * 0.95f, cy - fs * 0.55f), u32(running ? col::Teal : col::Dim), left);

    ExplorerTab* at = tabById(activeTab_);
    if (at && !at->loading && !at->nodes.empty()) {
        char right[256];
        std::snprintf(right, sizeof right, "%s    %zu entries    %s unpacked    %d selected", at->info.format.c_str(),
                      at->info.entries.size(), kz::humanSize(at->info.totalSize).c_str(), (int)at->selection.size());
        ImVec2 ts = sf->CalcTextSizeA(fs, FLT_MAX, 0, right);
        dl->AddText(sf, fs, ImVec2(b.x - ts.x - em * 1.1f, cy - fs * 0.55f), u32(col::Dim), right);
    } else {
        char right[96];
        std::snprintf(right, sizeof right, "Drop files anywhere    %s+O open    %s+N compress", kMod, kMod);
        ImVec2 ts = sf->CalcTextSizeA(fs, FLT_MAX, 0, right);
        dl->AddText(sf, fs, ImVec2(b.x - ts.x - em * 1.1f, cy - fs * 0.55f), u32(col::Faint), right);
    }
}

// ============================================================================ welcome
void App::drawWelcome() {
    const float em = emPx();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float w = std::min(avail.x - em * 2, em * 50);
    float contentH = em * 33.0f;
    float top = std::max(em * 0.8f, (avail.y - contentH) * 0.4f);
    float left = origin.x + (avail.x - w) * 0.5f;

    // ambient glow behind the hero
    {
        ImVec2 c(origin.x + avail.x * 0.5f, origin.y + top + em * 2.1f);
        float maxR = std::max(em * 4.0f, c.y - origin.y);
        const int steps = 28;
        for (int i = steps; i >= 1; --i) {
            float t = float(i) / steps;
            dl->AddCircleFilled(c, maxR * t, u32(mix(col::Accent, col::Accent2, t), 0.012f), 64);
        }
    }

    // hero
    float ls = em * 4.2f;
    ImVec2 lp(origin.x + (avail.x - ls) * 0.5f, origin.y + top);
    for (int i = 1; i <= 8; ++i)
        dl->AddRect(ImVec2(lp.x - i, lp.y - i + 2), ImVec2(lp.x + ls + i, lp.y + ls + i + 2), u32(col::Accent, 0.07f * (1.0f - i / 9.0f)),
                    ls * 0.28f + i, 0, 1.5f);
    drawLogo(dl, lp, ls);
    ImGui::SetCursorScreenPos(ImVec2(origin.x, lp.y + ls + em * 1.0f));
    centerText("Kompressor", col::Text, fonts().h1);
    ImGui::Dummy(ImVec2(0, em * 0.05f));
    centerText("Extract anything. Compress everything. Super-compress the rest.", col::Dim);
    ImGui::Dummy(ImVec2(0, em * 1.6f));

    // action cards
    struct CardDef { Ico ic; ImVec4 tint; const char* title; const char* desc; std::string key; const char* badge; int action; };
    const CardDef cards[3] = {
        {Ico::Open, col::Accent2, "Open archive", "Browse ZIP, 7Z, RAR, TAR, ISO and 20+ more without extracting a thing.",
         std::string(kMod) + "+O", nullptr, 0},
        {Ico::Box, col::Teal, "Compress files", "Create ZIP, 7Z, TAR.XZ, TAR.ZST, ISO... with optional AES-256 encryption.",
         std::string(kMod) + "+N", nullptr, 1},
        {Ico::Bolt, col::Accent, "Super compress", "The .kmp format: dedup plus content-aware LZMA2/Zstd for the smallest files.",
         std::string(kMod) + "+Shift+N", "SMALLEST", 2},
    };
    float gap = em * 1.0f;
    float cw = (w - gap * 2) / 3.0f, ch = em * 10.2f;
    float y0 = ImGui::GetCursorScreenPos().y;
    for (int i = 0; i < 3; ++i) {
        const CardDef& c = cards[i];
        ImVec2 a(left + i * (cw + gap), y0);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##card", ImVec2(cw, ch));
        float h = anim(ImGui::GetItemID(), ImGui::IsItemHovered());
        ImGui::PopID();
        float lift = h * em * 0.2f;
        ImVec2 ca(a.x, a.y - lift), cb(a.x + cw, a.y + ch - lift);
        float r = em * 0.9f;
        softShadow(dl, ca, cb, r, em * (0.6f + h * 0.8f), 0.30f + h * 0.15f);
        dl->AddRectFilled(ca, cb, u32(mix(col::Surface1, col::Surface2, h)), r);
        if (i == 2) {  // featured: gradient wash + gradient border
            gradientRect(dl, ca, cb, u32(col::Accent, 0.10f + 0.05f * h), u32(col::Accent2, 0.02f), r);
            dl->AddRect(ca, cb, u32(mix(col::Accent, col::Text, 0.1f * h), 0.55f + 0.35f * h), r, 0, 1.5f);
        } else {
            dl->AddRect(ca, cb, u32(mix(col::Line, c.tint, 0.55f * h)), r, 0, 1.0f + 0.4f * h);
        }
        float pad = em * 1.25f;
        float ts = em * 2.7f;
        ImVec2 tp(ca.x + pad, ca.y + pad);
        gradientRect(dl, tp, ImVec2(tp.x + ts, tp.y + ts), u32(c.tint, 0.30f + 0.10f * h), u32(c.tint, 0.10f), ts * 0.3f);
        dl->AddRect(tp, ImVec2(tp.x + ts, tp.y + ts), u32(c.tint, 0.30f), ts * 0.3f);
        float is = ts * 0.54f;
        drawIco(dl, c.ic, ImVec2(tp.x + (ts - is) * 0.5f, tp.y + (ts - is) * 0.5f), is, u32(mix(c.tint, col::Text, 0.2f)));
        if (c.badge) {
            ImFont* sf = fonts().caption;
            float fs = sf->FontSize * ImGui::GetIO().FontGlobalScale;
            ImVec2 bts = sf->CalcTextSizeA(fs, FLT_MAX, 0, c.badge);
            ImVec2 ba(cb.x - pad - bts.x - em * 0.9f, tp.y + em * 0.2f), bb(cb.x - pad, tp.y + em * 0.2f + bts.y + em * 0.35f);
            gradientRect(dl, ba, bb, u32(col::Accent), u32(col::Accent2), (bb.y - ba.y) * 0.5f, true);
            dl->AddText(sf, fs, ImVec2(ba.x + em * 0.45f, ba.y + em * 0.17f), u32(ImVec4(1, 1, 1, 1)), c.badge);
        }
        ImFont* bf = fonts().bold;
        float bfs = bf->FontSize * ImGui::GetIO().FontGlobalScale * 1.08f;
        dl->AddText(bf, bfs, ImVec2(ca.x + pad, tp.y + ts + em * 0.9f), u32(col::Text), c.title);
        ImFont* sf = fonts().caption;
        float sfs = sf->FontSize * ImGui::GetIO().FontGlobalScale;
        dl->AddText(sf, sfs * 1.05f, ImVec2(ca.x + pad, tp.y + ts + em * 0.9f + bfs + em * 0.35f), u32(col::Dim), c.desc, nullptr,
                    cw - pad * 2);
        // shortcut + arrow
        ImVec2 kts = sf->CalcTextSizeA(sfs, FLT_MAX, 0, c.key.c_str());
        dl->AddText(sf, sfs, ImVec2(ca.x + pad, cb.y - pad - kts.y + em * 0.2f), u32(col::Faint), c.key.c_str());
        float as = em * 1.0f;
        drawIco(dl, Ico::Chevron, ImVec2(cb.x - pad - as + h * em * 0.25f, cb.y - pad - as + em * 0.25f), as,
                u32(mix(col::Faint, c.tint, h)));
        if (clicked) {
            if (c.action == 0) actionOpen();
            else openCompressDialog({}, c.action == 2);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(left, y0 + ch + em * 1.3f));

    // drop zone
    {
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + em * 5.2f);
        bool hov = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered();
        float h = anim(ImGui::GetID("##dropzone"), hov);
        dl->AddRectFilled(a, b, u32(col::Accent, 0.03f + 0.03f * h), em * 0.9f);
        dashedRect(dl, a, b, u32(mix(col::Faint, col::Accent, 0.35f + 0.65f * h), 0.8f), em * 0.9f, em * 0.32f, 1.4f);
        float is = em * 1.7f;
        const char* t1 = "Drop files, folders or archives anywhere";
        const char* t2 = "Archives open instantly. Everything else goes straight to the compressor.";
        ImFont* bf = fonts().bold;
        ImFont* sf = fonts().caption;
        float bfs = bf->FontSize * ImGui::GetIO().FontGlobalScale, sfs = sf->FontSize * ImGui::GetIO().FontGlobalScale;
        ImVec2 s1 = bf->CalcTextSizeA(bfs, FLT_MAX, 0, t1), s2 = sf->CalcTextSizeA(sfs, FLT_MAX, 0, t2);
        float blockW = is + em * 1.0f + std::max(s1.x, s2.x);
        float x = a.x + (w - blockW) * 0.5f, cy = (a.y + b.y) * 0.5f;
        drawIco(dl, Ico::Drop, ImVec2(x, cy - is * 0.5f), is, u32(mix(col::Dim, col::AccentHover, h)));
        dl->AddText(bf, bfs, ImVec2(x + is + em, cy - s1.y - em * 0.08f), u32(col::Text), t1);
        dl->AddText(sf, sfs, ImVec2(x + is + em, cy + em * 0.12f), u32(col::Dim), t2);
        ImGui::Dummy(ImVec2(w, b.y - a.y));
    }

    // format chips
    ImGui::SetCursorScreenPos(ImVec2(left, ImGui::GetCursorScreenPos().y + em * 0.9f));
    {
        static const char* fmts[] = {"ZIP", "7Z", "RAR", "TAR", "GZ", "BZ2", "XZ", "ZST", "LZ4", "ISO", "CAB", "LHA", "XAR", "CPIO", "RPM", "DEB", "KMP"};
        caption("READS NATIVELY");
        ImGui::SameLine(0, em * 0.8f);
        float x0 = ImGui::GetCursorPosX();
        float maxX = ImGui::GetCursorPosX() + (left + w - ImGui::GetCursorScreenPos().x);
        for (size_t i = 0; i < sizeof fmts / sizeof *fmts; ++i) {
            ImGui::PushFont(fonts().caption);
            float cwid = ImGui::CalcTextSize(fmts[i]).x + em * 1.0f;
            ImGui::PopFont();
            if (i && ImGui::GetCursorPosX() + cwid > maxX) {
                ImGui::NewLine();
                ImGui::SetCursorPosX(x0);
            }
            chip(fmts[i], i + 1 == sizeof fmts / sizeof *fmts ? col::Accent : col::FileCol);
            ImGui::SameLine(0, em * 0.35f);
        }
        ImGui::SameLine(0, em * 0.6f);
        if (ImGui::TextLink("All formats")) showFormats_ = true;
    }
}

// ============================================================================ explorer
void App::drawExplorer(ExplorerTab& t) {
    const float em = emPx();
    if (!debugSelect_.empty() && !t.loading && !t.nodes.empty()) {
        for (int i = 1; i < (int)t.nodes.size(); ++i)
            if (t.nodes[i].path == debugSelect_) {
                t.cwd = std::max(0, t.nodes[i].parent);
                computeRows(t);
                t.selection = {i};
                requestPreview(t, i);
            }
        debugSelect_.clear();
    }
    if (debugRecompress_ && !t.loading && !t.nodes.empty()) {
        debugRecompress_ = false;
        actionConvert(t);
    }
    if (!debugSearch_.empty() && !t.loading && !t.nodes.empty()) {
        actionSearch(t, false);
        std::snprintf(search_.query, sizeof search_.query, "%s", debugSearch_.c_str());
        debugSearch_.clear();
        runSearch();
    }
    if (debugHash_ && !t.loading && !t.nodes.empty()) {
        debugHash_ = false;
        actionHash(t, false);
        if (!debugHashList_.empty()) loadHashList(debugHashList_);
    }
    if (t.loading) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float w = std::min(avail.x, em * 24);
        ImGui::Dummy(ImVec2(0, std::max(em * 2, avail.y * 0.28f)));
        ImGui::SetCursorPosX((avail.x - em * 2.4f) * 0.5f);
        spinner(em * 1.2f, col::Accent);
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        centerText(("Reading " + t.title).c_str(), col::Text, fonts().bold);
        if (t.loadProg) {
            std::string st = t.loadProg->status();
            if (!st.empty()) centerText(st.c_str(), col::Dim, fonts().caption);
            ImGui::Dummy(ImVec2(0, em * 0.4f));
            ImGui::SetCursorPosX((avail.x - w) * 0.5f);
            progressBar(t.loadProg->total.load() ? t.loadProg->fraction() : -1.0f, ImVec2(w, 0), col::Accent);
            ImGui::Dummy(ImVec2(0, em * 0.5f));
            ImGui::SetCursorPosX((avail.x - em * 7) * 0.5f);
            if (secondaryButton("Cancel", ImVec2(em * 7, 0))) t.loadProg->cancel = true;
        }
        return;
    }
    if (t.nodes.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        bool locked = t.error.find("encrypted") != std::string::npos;
        ImGui::Dummy(ImVec2(0, std::max(em * 2, avail.y * 0.25f)));
        float ts = em * 3.6f;
        ImGui::SetCursorPosX((avail.x - ts) * 0.5f);
        iconTile(locked ? Ico::Lock : (t.error.empty() ? Ico::Box : Ico::Error), ts, locked ? col::Warn : (t.error.empty() ? col::Dim : col::Error));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        centerText(locked ? "This archive is encrypted" : (t.error.empty() ? "This archive is empty" : "Couldn't open this archive"), col::Text,
                   fonts().h2);
        if (!t.error.empty() && !locked) {
            float ww = std::min(avail.x, em * 34);
            ImGui::SetCursorPosX((avail.x - ww) * 0.5f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ww);
            ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
            ImGui::TextWrapped("%s", t.error.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy(ImVec2(0, em));
        float bw = em * 8;
        ImGui::SetCursorPosX((avail.x - bw * 2 - em * 0.6f) * 0.5f);
        if (secondaryButton("Retry", ImVec2(bw, 0))) loadTab(t.id);
        ImGui::SameLine(0, em * 0.6f);
        int id = t.id;
        if (primaryButton("Password", Ico::Lock, ImVec2(bw, ImGui::GetFrameHeight() * 1.1f)))
            askPassword("Password for \"" + t.title + "\":", [this, id](const std::string& pw) {
                if (ExplorerTab* t = tabById(id)) {
                    t->password = pw;
                    loadTab(id);
                }
            });
        return;
    }

    // --- navigation row: up, breadcrumbs, search
    float rowH = ImGui::GetFrameHeight();
    bool goBk = iconButton("##back", Ico::Back, rowH, "Back (Alt+Left, mouse back button)", !t.back.empty());
    ImGui::SameLine(0, em * 0.1f);
    bool goFw = iconButton("##fwd", Ico::Forward, rowH, "Forward (Alt+Right, mouse forward button)", !t.forward.empty());
    ImGui::SameLine(0, em * 0.1f);
    bool up = iconButton("##up", Ico::Up, rowH, "Up one level (Backspace)", t.cwd != 0);
    // Side mouse buttons (X1 = back, X2 = forward) work anywhere in the window, plus Alt+Left / Alt+Right.
    bool noModal = !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    bool overMain = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (noModal && ((overMain && ImGui::IsMouseClicked(3)) ||
                    ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow, ImGuiInputFlags_RouteGlobal)))
        goBk = !t.back.empty();
    if (noModal && ((overMain && ImGui::IsMouseClicked(4)) ||
                    ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_RightArrow, ImGuiInputFlags_RouteGlobal)))
        goFw = !t.forward.empty();
    if (goBk) goBack(t);
    else if (goFw) goForward(t);
    else if (up || (t.cwd != 0 && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        navigateTo(t, std::max(0, t.nodes[t.cwd].parent), t.cwd);
    ImGui::SameLine(0, em * 0.4f);
    std::vector<int> crumbs;
    for (int n = t.cwd; n >= 0; n = t.nodes[n].parent) crumbs.push_back(n);
    std::reverse(crumbs.begin(), crumbs.end());
    float searchW = std::min(em * 17, ImGui::GetContentRegionAvail().x * 0.35f);
    float crumbsMaxX = ImGui::GetContentRegionMax().x - searchW - em;
    // collapse the middle of very deep paths
    size_t firstShown = 0;
    if (crumbs.size() > 5) firstShown = crumbs.size() - 4;
    for (size_t i = 0; i < crumbs.size(); ++i) {
        if (i > 0 && i < firstShown) continue;
        if (i) {
            ImGui::SameLine(0, 0);
            float cs = em * 0.75f;
            ImVec2 cp = ImGui::GetCursorScreenPos();
            drawIco(ImGui::GetWindowDrawList(), Ico::Chevron, ImVec2(cp.x, cp.y + (rowH - cs) * 0.5f), cs, u32(col::Faint));
            ImGui::Dummy(ImVec2(cs, rowH));
            ImGui::SameLine(0, 0);
            if (i == firstShown && firstShown > 1) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(col::Faint, "...");
                ImGui::SameLine(0, 0);
                ImVec2 cp2 = ImGui::GetCursorScreenPos();
                drawIco(ImGui::GetWindowDrawList(), Ico::Chevron, ImVec2(cp2.x, cp2.y + (rowH - cs) * 0.5f), cs, u32(col::Faint));
                ImGui::Dummy(ImVec2(cs, rowH));
                ImGui::SameLine(0, 0);
            }
        }
        if (ImGui::GetCursorPosX() > crumbsMaxX) break;
        ImGui::PushID((int)i);
        bool last = i + 1 == crumbs.size();
        Ico ai = t.info.isKmp ? Ico::Sparkle : Ico::Archive;
        if (crumb(t.nodes[crumbs[i]].name.c_str(), last, i == 0 ? &ai : nullptr)) navigateTo(t, crumbs[i]);
        ImGui::PopID();
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - searchW);
    if (searchInput("##filter", "Search this folder", t.filter, sizeof t.filter, searchW)) computeRows(t);

    // --- archive summary chips
    {
        const auto& in = t.info;
        ImGui::Dummy(ImVec2(0, em * 0.1f));
        chip(in.format.c_str(), in.isKmp ? col::Accent : col::Accent2);
        if (!in.filter.empty() && in.filter != "none") {
            ImGui::SameLine(0, em * 0.4f);
            chip(in.filter.c_str(), col::Teal);
        }
        ImGui::SameLine(0, em * 0.4f);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%zu entries", in.entries.size());
        chip(buf, col::FileCol);
        if (in.hasEncrypted) {
            ImGui::SameLine(0, em * 0.4f);
            chip("encrypted", col::Warn);
        }
        ImGui::SameLine(0, em * 0.9f);
        std::snprintf(buf, sizeof buf, "%s packed  /  %s unpacked", kz::humanSize(in.archiveSize).c_str(), kz::humanSize(in.totalSize).c_str());
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%s", buf);
        ImGui::PopFont();
        if (in.totalSize && in.archiveSize < in.totalSize) {
            ImGui::SameLine(0, em * 0.5f);
            std::snprintf(buf, sizeof buf, "-%.1f%%", 100.0 * (1.0 - double(in.archiveSize) / double(in.totalSize)));
            chip(buf, col::Ok);
        }
    }
    ImGui::Dummy(ImVec2(0, em * 0.15f));

    // --- table | splitter | preview
    ImVec2 avail = ImGui::GetContentRegionAvail();
    // previewWidth is a fraction of the row, so it survives window resizes and DPI changes.
    if (t.previewWidth <= 0 || t.previewWidth >= 1) t.previewWidth = 0.37f;
    float previewPx = std::clamp(std::floor(avail.x * t.previewWidth), em * 16, std::max(em * 16, avail.x - em * 22));
    float split = em * 0.7f;
    float tableW = avail.x - previewPx - split;
    {
        // Files dropped anywhere on the list or preview are added to this archive.
        ImVec2 a = ImGui::GetCursorScreenPos();
        dropTab_ = t.id;
        dropMin_ = a;
        dropMax_ = ImVec2(a.x + avail.x, a.y + avail.y);
    }
    if (t.editing) {
        // An update is rewriting the archive: show it over the list, and keep hands off meanwhile.
        ImGui::BeginDisabled();
    }
    drawTable(t, tableW);
    if (t.editing) ImGui::EndDisabled();
    ImGui::SameLine(0, 0);
    ImGui::InvisibleButton("##split", ImVec2(split, avail.y));
    bool sh = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (sh) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive() && avail.x > 0) t.previewWidth = std::clamp((previewPx - ImGui::GetIO().MouseDelta.x) / avail.x, 0.05f, 0.95f);
    {
        float h = anim(ImGui::GetItemID(), sh);
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float x = std::floor((a.x + b.x) * 0.5f), cy = (a.y + b.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x - 1.5f, cy - em * 1.2f), ImVec2(x + 1.5f, cy + em * 1.2f),
                                                  u32(mix(col::Line, col::Accent, h)), 2.0f);
    }
    ImGui::SameLine(0, 0);
    drawPreview(t);
}

// The file list clears the highlight colours so it can draw its own rounded row highlight. Its
// right-click menus would inherit that and show no hover, so they put the theme's colours back.
// ImGui wants them popped inside the popup, before EndPopup().
static void pushListMenuColors() {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(col::Text.x, col::Text.y, col::Text.z, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(col::Accent.x, col::Accent.y, col::Accent.z, 0.30f));
}
static void endListMenu() {
    ImGui::PopStyleColor(3);
    ImGui::EndPopup();
}

void App::drawTable(ExplorerTab& t, float width) {
    const float em = emPx();
    const float rowH = em * 1.95f;
    ImVec2 a = ImGui::GetCursorScreenPos();
    float height = ImGui::GetContentRegionAvail().y;
    cardBg(a, ImVec2(a.x + width, a.y + height));

    ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_Hideable | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoBordersInBody;
    int openDir = -1, openFile = -1;
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(em * 0.6f, 0));
    ImGui::SetCursorScreenPos(ImVec2(a.x + 1, a.y + em * 0.3f));
    bool open = ImGui::BeginTable("##entries", 4, flags, ImVec2(width - 2, height - em * 0.6f));
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::PopStyleColor(4);
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("NAME", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, em * 6.2f);
    ImGui::TableSetupColumn("MODIFIED", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, em * 8.8f);
    ImGui::TableSetupColumn("TYPE", ImGuiTableColumnFlags_WidthFixed, em * 4.6f);
    // custom header row: small caption font, hairline underneath
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, em * 2.0f);
    ImGui::PushFont(fonts().caption);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Faint);
    for (int c = 0; c < 4; ++c) {
        if (!ImGui::TableSetColumnIndex(c)) continue;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.5f);
        ImGui::TableHeader(ImGui::TableGetColumnName(c));
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    {
        ImGuiTable* tb = ImGui::GetCurrentTable();
        float y = tb->RowPosY2;
        ImGui::GetWindowDrawList()->PushClipRect(ImVec2(tb->OuterRect.Min.x, y - 2), ImVec2(tb->OuterRect.Max.x, y + 2), false);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(tb->OuterRect.Min.x + em * 0.5f, y), ImVec2(tb->OuterRect.Max.x - em * 0.5f, y),
                                            u32(col::Line));
        ImGui::GetWindowDrawList()->PopClipRect();
    }
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty && ss->SpecsCount > 0) {
            t.sortCol = ss->Specs[0].ColumnIndex;
            t.sortAsc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            computeRows(t);
            ss->SpecsDirty = false;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    bool searching = t.filter[0] != 0;
    ImGuiListClipper clip;
    clip.Begin((int)t.rows.size());  // let the clipper measure real row height (with spacing) so every row is reachable
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            int id = t.rows[r];
            Node& n = t.nodes[id];
            ImGui::TableNextRow(0, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(id);
            bool sel = t.selection.count(id) > 0;
            ImVec2 cp = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                                    ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) {
                if (io.KeyShift && t.anchor >= 0 && t.anchor < (int)t.rows.size()) {
                    t.selection.clear();
                    for (int k = std::min(t.anchor, r); k <= std::max(t.anchor, r); ++k) t.selection.insert(t.rows[k]);
                } else if (io.KeyCtrl || io.KeySuper) {
                    if (sel) t.selection.erase(id);
                    else t.selection.insert(id);
                    t.anchor = r;
                } else {
                    t.selection = {id};
                    t.anchor = r;
                }
                if (t.selection.size() == 1 && !n.isDir && t.previewNode != id) requestPreview(t, id);
                if (t.selection.size() != 1) requestPreview(t, -1);
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (n.isDir) openDir = id;
                    else openFile = id;
                }
            }
            // Pulling a row out starts a drag of the selection to the file manager (after this frame).
            if (!dragging_ && !t.editing && native::fileTransferSupported() && ImGui::IsItemActive() &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Left, em * 0.6f)) {
                if (!sel) {
                    t.selection = {id};
                    t.anchor = r;
                }
                pendingDrag_ = t.id;
                ImGui::ClearActiveID();  // the row must not see the release as a click
            }
            bool hov = ImGui::IsItemHovered();
            sel = t.selection.count(id) > 0;
            {
                // rounded row highlight across all columns
                ImGuiTable* tb = ImGui::GetCurrentTable();
                ImVec2 ra(tb->WorkRect.Min.x + em * 0.35f, ImGui::GetItemRectMin().y + 1),
                    rb(tb->WorkRect.Max.x - em * 0.35f, ImGui::GetItemRectMax().y - 1);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                // Escape the column clip (the highlight spans all columns) but stay inside the scrolling body:
                // never over the frozen header row or past the bottom of the list.
                const ImRect& body = tb->Bg2ClipRectForDrawCmd;
                dl->PushClipRect(body.Min, body.Max, false);
                if (sel) {
                    dl->AddRectFilled(ra, rb, u32(col::Accent, 0.20f), em * 0.45f);
                    dl->AddRect(ra, rb, u32(col::Accent, 0.35f), em * 0.45f);
                } else if (hov) {
                    dl->AddRectFilled(ra, rb, u32(col::Text, 0.045f), em * 0.45f);
                }
                dl->PopClipRect();
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                pushListMenuColors();
                if (!sel) t.selection = {id};
                if (n.isDir && ImGui::MenuItem("Open folder")) openDir = id;
                if (!n.isDir && ImGui::MenuItem("Open")) openEntry(t, id);
                if (!n.isDir && ImGui::MenuItem("Open as archive")) openNested(t, id);
                if (!n.isDir && ImGui::MenuItem("Preview")) requestPreview(t, id);
                ImGui::Separator();
                if (ImGui::MenuItem("Extract...")) actionExtract(t, true);
                if (ImGui::MenuItem("Checksums...")) actionHash(t, true);
                if (ImGui::MenuItem("Find in Selected...")) actionSearch(t, true);
                std::string kC = std::string(kMod) + "+C";
                if (native::fileTransferSupported() && ImGui::MenuItem("Copy", kC.c_str())) copySelection(t);
                if (ImGui::MenuItem("Delete", "Del", false, t.editable && !t.editing)) askDeleteSelection(t);
                if (!t.editable && !t.editWhy.empty()) ImGui::SetItemTooltip("%s", t.editWhy.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(n.path.c_str());
                endListMenu();
            }
            Ico ic = iconFor(n);
            float is = em * 1.2f;
            ImGui::SetCursorScreenPos(ImVec2(cp.x + em * 0.15f, cp.y + (rowH - is) * 0.5f));
            icon(ic, is, iconColor(ic));
            ImGui::SameLine(0, em * 0.6f);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cp.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f));
            if (n.isDir) ImGui::PushFont(fonts().bold);
            ImGui::TextUnformatted(n.name.c_str());
            if (n.isDir) ImGui::PopFont();
            if (searching && n.parent > 0) {
                ImGui::SameLine(0, em * 0.5f);
                ImGui::PushFont(fonts().caption);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.08f);
                ImGui::TextColored(col::Faint, "in %s", t.nodes[n.parent].path.c_str());
                ImGui::PopFont();
            }
            if (n.encrypted) {
                ImGui::SameLine(0, em * 0.4f);
                icon(Ico::Lock, em * 0.85f, col::Warn);
            }
            auto vcenter = [&](float h) { ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cp.y + (rowH - h) * 0.5f)); };
            ImGui::TableSetColumnIndex(1);
            {
                std::string s = n.isDir ? (std::to_string(n.files) + " items") : kz::humanSize(n.size);
                vcenter(ImGui::GetTextLineHeight());
                float tw = ImGui::CalcTextSize(s.c_str()).x;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - tw));
                ImGui::TextColored(n.isDir ? col::Dim : col::Text, "%s", s.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            vcenter(ImGui::GetTextLineHeight());
            ImGui::TextColored(col::Dim, "%s", kz::formatTime(n.mtime).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::PushFont(fonts().caption);
            vcenter(ImGui::GetTextLineHeight());
            ImGui::TextColored(mix(iconColor(ic), col::Dim, 0.35f), "%s", typeLabel(n).c_str());
            ImGui::PopFont();
            ImGui::PopID();
        }
    }
    // Right-click on the empty part of the list: add or paste into this folder.
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##bgctx");
    if (ImGui::BeginPopup("##bgctx")) {
        pushListMenuColors();
        bool canEdit = t.editable && !t.editing;
        if (ImGui::MenuItem("Add Files...", nullptr, false, canEdit)) pickFilesToAdd(t, false);
        if (ImGui::MenuItem("Add Folder...", nullptr, false, canEdit)) pickFilesToAdd(t, true);
        if (native::fileTransferSupported()) {
            std::string kV = std::string(kMod) + "+V";
            if (ImGui::MenuItem("Paste", kV.c_str(), false, canEdit)) pasteFiles(t);
        }
        if (!t.editable && !t.editWhy.empty()) {
            ImGui::Separator();
            ImGui::PushTextWrapPos(em * 22);
            ImGui::TextColored(col::Dim, "%s", t.editWhy.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", (std::string(kMod) + "+A").c_str()))
            t.selection = std::set<int>(t.rows.begin(), t.rows.end());
#ifdef _WIN32
        if (!t.src.inMemory()) {
            ImGui::Separator();
            if (ImGui::MenuItem("Open in File Explorer")) openInExplorer(t);
        }
#endif
        endListMenu();
    }
    ImGui::EndTable();
    ImGui::PopStyleColor(4);
    ImGui::SetCursorScreenPos(a);  // report the card (not the inset table) as this item for SameLine()
    ImGui::Dummy(ImVec2(width, height));
    if (t.rows.empty()) {
        ImVec2 c(a.x + width * 0.5f, a.y + height * 0.42f);
        const char* msg = searching ? "No matches in this folder" : "This folder is empty";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        float is = em * 2.2f;
        drawIco(ImGui::GetWindowDrawList(), searching ? Ico::Search : Ico::FolderOpen, ImVec2(c.x - is * 0.5f, c.y - is - em * 0.6f), is,
                u32(col::Faint));
        ImGui::GetWindowDrawList()->AddText(ImVec2(c.x - ts.x * 0.5f, c.y), u32(col::Dim), msg);
    }

    // keyboard: Enter opens, Ctrl+A selects all
    if (!io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && t.selection.size() == 1) {
            int id = *t.selection.begin();
            if (t.nodes[id].isDir) openDir = id;
            else openFile = id;
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteGlobal))
            t.selection = std::set<int>(t.rows.begin(), t.rows.end());
        if (!t.editing) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !t.selection.empty() && t.editable) askDeleteSelection(t);
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, ImGuiInputFlags_RouteGlobal) && !t.selection.empty()) copySelection(t);
            if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_RouteGlobal) && native::fileTransferSupported())
                pasteFiles(t);
        }
    }
    if (openDir >= 0) {
        navigateTo(t, openDir);
    } else if (openFile >= 0) {
        // Archives inside the archive open as a new tab; anything else opens in its usual app.
        if (nestedArchive(t, openFile)) openNested(t, openFile);
        else openEntry(t, openFile);
    }
}

// An entry to treat as an archive: by its name, or by its content once its preview bytes are loaded.
bool App::nestedArchive(const ExplorerTab& t, int node) const {
    if (node < 0 || node >= (int)t.nodes.size() || t.nodes[node].isDir) return false;
    if (kz::looksLikeArchive(t.nodes[node].name)) return true;
    return t.previewNode == node && !t.previewLoading && (bool)kz::sniff(t.preview.data(), t.preview.size());
}

void App::drawPreview(ExplorerTab& t) {
    const float em = emPx();
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImVec2 sz(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
    cardBg(a, ImVec2(a.x + sz.x, a.y + sz.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.2f, em * 1.1f));
    ImGui::BeginChild("##preview", sz, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();
    int nid = t.previewNode;
    float fullW = ImGui::GetContentRegionAvail().x;

    if (nid <= 0 || nid >= (int)t.nodes.size()) {
        const auto& in = t.info;
        // header
        iconTile(in.isKmp ? Ico::Sparkle : Ico::Archive, em * 2.6f, in.isKmp ? col::Accent : col::ArchiveCol);
        ImGui::SameLine(0, em * 0.8f);
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().bold);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fullW - em * 3.6f);
        ImGui::TextWrapped("%s", t.title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%s", in.format.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, em * 0.6f));

        if (!t.selection.empty()) {
            uint64_t sz2 = 0;
            uint32_t files = 0;
            for (int id : t.selection) {
                sz2 += t.nodes[id].size;
                files += t.nodes[id].isDir ? t.nodes[id].files : 1;
            }
            ImVec2 ca = ImGui::GetCursorScreenPos();
            float ch = em * 6.4f;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            gradientRect(dl, ca, ImVec2(ca.x + fullW, ca.y + ch), u32(col::Accent, 0.16f), u32(col::Accent2, 0.06f), em * 0.7f, true);
            dl->AddRect(ca, ImVec2(ca.x + fullW, ca.y + ch), u32(col::Accent, 0.35f), em * 0.7f);
            ImGui::SetCursorScreenPos(ImVec2(ca.x + em * 0.9f, ca.y + em * 0.8f));
            ImGui::BeginGroup();
            caption("SELECTION");
            ImGui::PushFont(fonts().bold);
            ImGui::Text("%d item%s  -  %u file%s", (int)t.selection.size(), t.selection.size() == 1 ? "" : "s", files, files == 1 ? "" : "s");
            ImGui::PopFont();
            ImGui::TextColored(col::Dim, "%s uncompressed", kz::humanSize(sz2).c_str());
            ImGui::EndGroup();
            ImGui::SetCursorScreenPos(ImVec2(ca.x + fullW - em * 8.6f, ca.y + (ch - em * 2.3f) * 0.5f));
            if (primaryButton("Extract", Ico::Extract, ImVec2(em * 7.8f, em * 2.3f))) actionExtract(t, true);
            ImGui::SetCursorScreenPos(ImVec2(ca.x, ca.y + ch + em * 0.9f));
            ImGui::Dummy(ImVec2(0, 0));
        }

        // ratio ring
        if (in.totalSize) {
            float frac = float(double(in.archiveSize) / double(in.totalSize));
            float saved = std::max(0.0f, 1.0f - frac);
            float rr = std::min(em * 3.9f, fullW * 0.26f);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImVec2 c(p.x + rr + em * 0.5f, p.y + rr + em * 0.4f);
            ring(c, rr, em * 0.65f, saved, saved > 0.5f ? col::Ok : col::Accent);
            char pct[32];
            std::snprintf(pct, sizeof pct, "%.0f%%", saved * 100.0f);
            ImFont* hf = fonts().h1;
            float hs = hf->FontSize * ImGui::GetIO().FontGlobalScale * 0.8f;
            ImVec2 ts = hf->CalcTextSizeA(hs, FLT_MAX, 0, pct);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(hf, hs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.62f), u32(col::Text), pct);
            ImFont* sf = fonts().caption;
            float ss = sf->FontSize * ImGui::GetIO().FontGlobalScale;
            const char* lbl = "saved";
            ImVec2 ls = sf->CalcTextSizeA(ss, FLT_MAX, 0, lbl);
            dl->AddText(sf, ss, ImVec2(c.x - ls.x * 0.5f, c.y + ts.y * 0.38f), u32(col::Dim), lbl);
            // legend to the right of the ring
            float lx = c.x + rr + em * 1.6f;
            if (lx + em * 8 < p.x + fullW) {
                ImGui::SetCursorScreenPos(ImVec2(lx, c.y - em * 2.6f));
                ImGui::BeginGroup();
                caption("PACKED");
                ImGui::PushFont(fonts().h2);
                ImGui::TextUnformatted(kz::humanSize(in.archiveSize).c_str());
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, em * 0.2f));
                caption("ORIGINAL");
                ImGui::PushFont(fonts().h2);
                ImGui::TextColored(col::Dim, "%s", kz::humanSize(in.totalSize).c_str());
                ImGui::PopFont();
                ImGui::EndGroup();
            }
            ImGui::SetCursorScreenPos(ImVec2(p.x, c.y + rr + em * 1.0f));
            ImGui::Dummy(ImVec2(0, 0));
        }

        // stat tiles
        uint64_t files = 0, dirs = 0;
        for (auto& e : in.entries) (e.isDir ? dirs : files)++;
        float tw = (fullW - em * 0.6f) * 0.5f;
        statTile("Files", std::to_string(files), Ico::File, col::FileCol, tw);
        ImGui::SameLine(0, em * 0.6f);
        statTile("Folders", std::to_string(dirs), Ico::Folder, col::FolderCol, tw);
        statTile("Unpacked", kz::humanSize(in.totalSize), Ico::Layers, col::Accent2, tw);
        ImGui::SameLine(0, em * 0.6f);
        char ratio[32] = "-";
        if (in.archiveSize && in.totalSize) std::snprintf(ratio, sizeof ratio, "%.2f : 1", double(in.totalSize) / double(in.archiveSize));
        statTile("Ratio", ratio, Ico::Gauge, col::Teal, tw);
        if (!in.extra.empty()) {
            ImGui::Dummy(ImVec2(0, em * 0.2f));
            ImGui::PushStyleColor(ImGuiCol_Text, col::Teal);
            ImGui::TextWrapped("%s", in.extra.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        ImGui::PushFont(fonts().caption);
        ImGui::PushStyleColor(ImGuiCol_Text, col::Faint);
        ImGui::TextWrapped("%s", t.editable
                                     ? "Select a file to preview it, double-click to open it. Drag files out to copy them, "
                                       "or drop files onto the list to add them."
                                     : "Select a file to preview it, double-click to open it. Drag files out to copy them.");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::EndChild();
        return;
    }

    Node& n = t.nodes[nid];
    // What the content really is, when the name says something else (e.g. a ZIP named .png).
    kz::Sniffed real = t.previewNode == nid && !t.previewLoading ? kz::sniff(t.preview.data(), t.preview.size()) : kz::Sniffed();
    bool misnamed = real && !kz::nameFits(n.name.c_str(), real);
    Ico ic = misnamed ? Ico::Archive : iconFor(n);
    iconTile(ic, em * 2.6f, iconColor(ic));
    ImGui::SameLine(0, em * 0.8f);
    ImGui::BeginGroup();
    ImGui::PushFont(fonts().bold);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + fullW - em * 3.6f);
    ImGui::TextWrapped("%s", n.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::PushFont(fonts().caption);
    std::string kind = misnamed ? std::string(real.name) + " archive (named as " + typeLabel(n) + ")" : typeLabel(n);
    ImGui::TextColored(misnamed ? col::Warn : col::Dim, "%s  -  %s  -  %s", kind.c_str(), kz::humanSize(n.size).c_str(),
                       kz::formatTime(n.mtime).c_str());
    ImGui::PopFont();
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    if (ghostButton("Open", Ico::Open, ImVec2(0, ImGui::GetFrameHeight()))) openEntry(t, nid);
    ImGui::SetItemTooltip("Open in its usual app (double-click does the same)");
    ImGui::SameLine(0, em * 0.2f);
    if (ghostButton("Extract", Ico::Extract, ImVec2(0, ImGui::GetFrameHeight()))) {
        t.selection = {nid};
        actionExtract(t, true);
    }
    if (nestedArchive(t, nid)) {
        ImGui::SameLine(0, em * 0.2f);
        if (ghostButton("Explore", Ico::Archive, ImVec2(0, ImGui::GetFrameHeight()))) openNested(t, nid);
        kz::Sniffed real = t.previewNode == nid ? kz::sniff(t.preview.data(), t.preview.size()) : kz::Sniffed();
        if (real && !kz::nameFits(n.name.c_str(), real))
            ImGui::SetItemTooltip("Its content is a %s archive, whatever the name says. Open it in a new tab.", real.name);
        else
            ImGui::SetItemTooltip("Open this archive in a new tab");
    }
    {
        static const char* modes[] = {"Auto", "Text", "Hex"};
        ImGui::PushFont(fonts().caption);
        float sw = 0;
        for (auto m : modes) sw += ImGui::CalcTextSize(m).x + em * 1.3f;
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - sw - 6.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.04f);
        segmented("##mode", &t.previewMode, modes, 3);
    }
    ImGui::Dummy(ImVec2(0, em * 0.15f));

    if (t.previewLoading) {
        ImGui::Dummy(ImVec2(0, em * 2));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (fullW - em * 1.8f) * 0.5f);
        spinner(em * 0.9f, col::Accent);
        centerText("Decompressing...", col::Dim);
        if (t.previewProg) {
            ImGui::Dummy(ImVec2(0, em * 0.3f));
            progressBar(t.previewProg->total.load() ? t.previewProg->fraction() : -1.0f, ImVec2(fullW, 0), col::Accent);
            ImGui::Dummy(ImVec2(0, em * 0.3f));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (fullW - em * 6) * 0.5f);
            if (secondaryButton("Cancel", ImVec2(em * 6, 0))) t.previewProg->cancel = true;
        }
        ImGui::EndChild();
        return;
    }
    if (!t.previewError.empty()) {
        ImGui::Dummy(ImVec2(0, em));
        bool locked = t.previewError.find("assword") != std::string::npos;
        icon(locked ? Ico::Lock : Ico::Error, em * 1.3f, locked ? col::Warn : col::Error);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, locked ? col::Warn : col::Error);
        ImGui::TextWrapped("%s", t.previewError.c_str());
        ImGui::PopStyleColor();
        int id = t.id;
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        if (primaryButton("Enter password", Ico::Lock))
            askPassword("Password for \"" + t.title + "\":", [this, id, nid](const std::string& pw) {
                if (ExplorerTab* t = tabById(id)) {
                    t->password = pw;
                    requestPreview(*t, nid);
                }
            });
        ImGui::EndChild();
        return;
    }
    if (t.preview.empty()) {
        ImGui::Dummy(ImVec2(0, em * 2));
        centerText("Empty file", col::Faint);
        ImGui::EndChild();
        return;
    }
    if (t.previewTruncated) {
        char b[64];
        std::snprintf(b, sizeof b, "showing first %s", kz::humanSize(t.preview.size()).c_str());
        chip(b, col::Warn);
    }

    bool asText = t.previewMode == 1 || (t.previewMode == 0 && t.previewIsText);
    ImVec2 ca = ImGui::GetCursorScreenPos();
    ImVec2 csz = ImGui::GetContentRegionAvail();
    ImDrawList* pdl = ImGui::GetWindowDrawList();
    pdl->AddRectFilled(ca, ImVec2(ca.x + csz.x, ca.y + csz.y), u32(col::Bg), em * 0.6f);
    pdl->AddRect(ca, ImVec2(ca.x + csz.x, ca.y + csz.y), u32(col::Line), em * 0.6f);
    ImGui::PushFont(fonts().mono);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.6f, em * 0.12f));
    ImGui::BeginChild("##content", csz, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    const char* base = (const char*)t.preview.data();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const bool marked = t.previewMark < t.preview.size();
    auto markRow = [&]() {  // tints the row the cursor is on
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetWindowWidth());
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(ImGui::GetWindowPos().x, p.y - em * 0.06f),
                                                  ImVec2(ImGui::GetWindowPos().x + w + ImGui::GetScrollX(), p.y + lineH - em * 0.06f),
                                                  u32(col::Accent, 0.22f));
    };
    int markAt = -1;  // line (text) or row (hex) holding the mark
    if (marked) {
        if (asText)
            markAt = (int)(std::upper_bound(t.lineStarts.begin(), t.lineStarts.end(), (uint32_t)t.previewMark) - t.lineStarts.begin()) - 1;
        else
            markAt = (int)(t.previewMark / 16);
        if (t.previewJump > 0) {
            --t.previewJump;
            ImGui::SetScrollY(std::max(0.0f, markAt * lineH - ImGui::GetWindowHeight() * 0.35f));
        }
    }
    if (asText) {
        char num[16];
        std::snprintf(num, sizeof num, "%zu", t.lineStarts.size());
        float gutter = ImGui::CalcTextSize(num).x;
        ImGuiListClipper clip;
        clip.Begin((int)t.lineStarts.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                size_t b = t.lineStarts[i];
                size_t e = i + 1 < (int)t.lineStarts.size() ? t.lineStarts[i + 1] - 1 : t.preview.size();
                if (e > b && base[e - 1] == '\r') --e;
                if (e - b > 4000) e = b + 4000;
                std::snprintf(num, sizeof num, "%d", i + 1);
                if (i == markAt) markRow();
                float x = ImGui::GetCursorPosX();
                ImGui::SetCursorPosX(x + gutter - ImGui::CalcTextSize(num).x);
                ImGui::TextColored(col::Faint, "%s", num);
                ImGui::SameLine(x + gutter + em * 1.0f);
                ImGui::TextUnformatted(base + b, base + e);
            }
    } else {
        int rows = (int)((t.preview.size() + 15) / 16);
        ImGuiListClipper clip;
        clip.Begin(rows);
        char hex[64], asc[20];
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                size_t off = (size_t)r * 16;
                int k = 0;
                for (int c = 0; c < 16; ++c) {
                    if (off + c < t.preview.size()) k += std::snprintf(hex + k, sizeof hex - k, "%02X ", t.preview[off + c]);
                    else k += std::snprintf(hex + k, sizeof hex - k, "   ");
                    if (c == 7) hex[k++] = ' ', hex[k] = 0;
                }
                int m = 0;
                for (int c = 0; c < 16 && off + c < t.preview.size(); ++c) {
                    uint8_t ch = t.preview[off + c];
                    asc[m++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
                }
                asc[m] = 0;
                if (r == markAt) markRow();
                ImGui::TextColored(col::Faint, "%08zX", off);
                ImGui::SameLine(0, em * 1.0f);
                ImGui::TextUnformatted(hex);
                ImGui::SameLine(0, em * 0.4f);
                ImGui::TextColored(col::Dim, "%s", asc);
            }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    ImGui::EndChild();
}

// ============================================================================ activity panel
void App::drawActivity() {
    const float em = emPx();
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetWindowWidth() - em * 2.2f;
    float h = em * 10.5f;
    cardBg(a, ImVec2(a.x + w, a.y + h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.0f, em * 0.7f));
    ImGui::BeginChild("##activity", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    // header
    int running = 0, pausedJobs = 0;
    for (auto& jp : jobs_)
        if (jp->visible && !jp->finished) (jp->paused() ? pausedJobs : running)++;
    float chipY = ImGui::GetCursorPosY();  // the header line: chips sit on it, not on the line below
    ImGui::PushFont(fonts().bold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Activity");
    ImGui::PopFont();
    if (running) {
        ImGui::SameLine(0, em * 0.5f);
        char b[32];
        std::snprintf(b, sizeof b, "%d running", running);
        ImGui::SetCursorPosY(chipY + ImGui::GetStyle().FramePadding.y);
        chip(b, col::Teal);
    }
    if (pausedJobs) {
        ImGui::SameLine(0, em * 0.5f);
        char b[32];
        std::snprintf(b, sizeof b, "%d paused", pausedJobs);
        ImGui::SetCursorPosY(chipY + ImGui::GetStyle().FramePadding.y);
        chip(b, col::Warn);
    }
    float ib = ImGui::GetFrameHeight() * 0.95f;
    bool anyJobs = running + pausedJobs > 0;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - ib * (anyJobs ? 3 : 2) - em * (anyJobs ? 0.4f : 0.2f));
    ImGui::SetCursorPosY(em * 0.7f);
    if (anyJobs) {
        // Pause everything while any job runs; resume everything once all are paused.
        bool pauseAll = running > 0;
        if (iconButton("##pauseall", pauseAll ? Ico::Pause : Ico::Play, ib, pauseAll ? "Pause all" : "Resume all"))
            for (auto& jp : jobs_)
                if (jp->visible && !jp->finished) jp->setPaused(pauseAll, ImGui::GetTime());
        ImGui::SameLine(0, em * 0.2f);
    }
    if (iconButton("##clearlog", Ico::Trash, ib, "Clear log", !log_.empty())) log_.clear();
    ImGui::SameLine(0, em * 0.2f);
    if (iconButton("##hideact", Ico::Close, ib, "Hide panel")) showActivity_ = false;

    ImGui::BeginChild("##actbody", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    for (auto& jp : jobs_) {
        Job& j = *jp;
        if (!j.visible || j.finished) continue;
        ImGui::PushID(j.id);
        float f = j.prog->fraction();
        bool det = j.prog->total.load() != 0;
        bool paused = j.paused();
        std::string st = paused ? "Paused - resume to continue" : j.prog->status();
        double el = j.elapsed(ImGui::GetTime());
        j.sampleRate(ImGui::GetTime());
        double left = paused ? -1.0 : j.secondsLeft();
        if (!paused && det && j.rateSamples >= 3) {  // speed after the status, so a slow stretch reads as working
            std::string sp = j.rate < 1024.0 ? "under 1 KB/s" : kz::humanSize((uint64_t)j.rate) + "/s";
            st = st.empty() ? sp : st + "  -  " + sp;
        }
        float avail = ImGui::GetContentRegionAvail().x;
        if (paused) icon(Ico::Pause, em * 1.1f, col::Warn);
        else spinner(em * 0.55f, col::Teal);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::BeginGroup();
        ImGui::PushFont(fonts().bold);
        ImGui::TextUnformatted(j.title.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, em * 0.6f);
        char ov[160];
        if (det && left >= 0)
            std::snprintf(ov, sizeof ov, "%.0f%%  -  %s  -  %s", f * 100.0f, kz::formatDuration(el).c_str(),
                          timeLeftText(left).c_str());
        else if (det) std::snprintf(ov, sizeof ov, "%.0f%%  -  %s", f * 100.0f, kz::formatDuration(el).c_str());
        else std::snprintf(ov, sizeof ov, "%s", kz::formatDuration(el).c_str());
        ImGui::TextColored(col::Dim, "%s", ov);
        progressBar(det ? f : (paused ? 0.0f : -1.0f), ImVec2(avail - em * 6.5f, em * 0.36f), paused ? col::Warn : col::Accent);
        if (!st.empty()) {
            ImGui::PushFont(fonts().caption);
            ImGui::TextColored(paused ? col::Warn : col::Faint, "%s", st.c_str());
            ImGui::PopFont();
        }
        ImGui::EndGroup();
        if (j.controls) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - em * 3.9f);
            if (iconButton("##pause", paused ? Ico::Play : Ico::Pause, em * 1.8f, paused ? "Resume" : "Pause"))
                j.setPaused(!paused, ImGui::GetTime());
            ImGui::SameLine(0, em * 0.2f);
            if (iconButton("##cancel", Ico::Close, em * 1.8f, "Cancel")) {
                j.prog->cancel = true;  // also wakes a paused worker so it can stop
                j.setPaused(false, ImGui::GetTime());
            }
        }
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, em * 0.2f));
    }
    // log
    for (size_t i = 0; i < log_.size(); ++i) {
        auto& l = log_[i];
        ImGui::PushID((int)i);
        // Lay the row out by hand so the small timestamp, the icon and the message share one centre line.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float L = ImGui::GetTextLineHeight();
        ImFont* cf = fonts().caption;
        float cs = cf->FontSize * ImGui::GetIO().FontGlobalScale;
        std::string ts = clockTime(l.time);
        ImVec2 tsz = cf->CalcTextSizeA(cs, FLT_MAX, 0, ts.c_str());
        float timeW = cf->CalcTextSizeA(cs, FLT_MAX, 0, "00:00:00").x;
        dl->AddText(cf, cs, ImVec2(p.x, std::floor(p.y + (L - tsz.y) * 0.5f)), u32(col::Faint), ts.c_str());
        float is = em * 0.95f;
        float x = p.x + timeW + em * 0.8f;
        drawIco(dl, logIcon(l.color), ImVec2(x, std::floor(p.y + (L - is) * 0.5f)), is, u32(l.color));
        ImGui::SetCursorScreenPos(ImVec2(x + is + em * 0.55f, p.y));
        bool plain = l.color.x == col::Dim.x && l.color.y == col::Dim.y;
        ImGui::TextColored(plain ? col::Dim : mix(l.color, col::Text, 0.55f), "%s", l.text.c_str());
        if (!l.openPath.empty()) {
            ImGui::SameLine(0, em * 0.7f);
            if (ImGui::TextLink("Open")) openArchive(l.openPath);
        }
        if (!l.revealPath.empty()) {
            ImGui::SameLine(0, em * 0.7f);
            if (ImGui::TextLink("Show in folder")) reveal(l.revealPath);
        }
        ImGui::PopID();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
}

// ============================================================================ dialogs
bool App::drawAdvice(const kz::ContentProfile* p, bool measuring, int* goal, int curFormat, int curLevel, int curMode,
                     unsigned threads, float inner, kz::Recommendation& out) {
    const float em = emPx();
    bool apply = false;
    caption("RECOMMENDED FOR YOU");
    static const char* goals[] = {"Best overall", "Smallest", "Fastest", "Share", "Password"};
    segmented("##goal", goal, goals, 5);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("What matters most for this archive?\nBest overall: good size, quick, opens in common tools\n"
                          "Smallest: the smallest file, however long it takes\nFastest: done in seconds\n"
                          "Share: opens anywhere without installing anything\nPassword: encrypted with AES-256");
    ImGui::Dummy(ImVec2(0, em * 0.15f));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetCursorScreenPos();
    const float pad = em * 0.9f, x0 = a.x + pad, w = inner - pad * 2;
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(x0, a.y + em * 0.75f));
    ImGui::BeginGroup();
    if (measuring || !p) {
        spinner(em * 0.55f, col::Accent);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::TextColored(col::Dim, "Looking at your files...");
    } else {
        kz::Recommendation r = kz::recommend(*p, (kz::Goal)*goal, threads);
        bool inUse = formatIndex(r.format) == curFormat && (r.format == kz::OutFormat::Kmp ? curMode == r.level : curLevel == r.level);
        // title row: icon + format on the left, "Use this" on the right
        float rowY = ImGui::GetCursorScreenPos().y, bh = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(x0, rowY + (bh - em * 1.1f) * 0.5f));
        icon(Ico::Sparkle, em * 1.1f, col::Accent);
        ImGui::SameLine(0, em * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowY + (bh - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::PushFont(fonts().bold);
        ImGui::TextUnformatted(r.title.c_str());
        ImGui::PopFont();
        float bw = em * 7.2f;
        ImGui::SetCursorScreenPos(ImVec2(x0 + w - bw, rowY));
        if (inUse) {
            ImGui::SetCursorScreenPos(ImVec2(x0 + w - em * 4.6f, rowY + (bh - ImGui::GetTextLineHeight()) * 0.5f));
            chip("In use", col::Ok);
        } else if (primaryButton("Use this", Ico::Check, ImVec2(bw, bh))) {
            out = r;
            apply = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, rowY + bh + em * 0.35f));
        ImGui::PushTextWrapPos(x0 - ImGui::GetWindowPos().x + w);
        ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
        ImGui::TextWrapped("%s", r.why.c_str());
        ImGui::PopStyleColor();
        // estimate
        ImGui::Dummy(ImVec2(0, em * 0.1f));
        ImGui::PushFont(fonts().caption);
        char est[256];
        double share = p->totalBytes ? double(r.estBytes) / double(p->totalBytes) : 1.0;
        std::snprintf(est, sizeof est, "About %s (%.0f%% of %s), roughly %s on this PC%s", kz::humanSize(r.estBytes).c_str(),
                      share * 100.0, kz::humanSize(p->totalBytes).c_str(), kz::formatDuration(r.estSeconds).c_str(),
                      p->measured ? "" : " - guessed from the file names");
        ImGui::TextColored(mix(col::Accent, col::Text, 0.45f), "%s", est);
        // what's in it
        std::string mix_;
        auto part = [&](uint64_t b, const char* what) {
            if (!p->totalBytes || b * 20 < p->totalBytes) return;
            if (!mix_.empty()) mix_ += ", ";
            mix_ += std::to_string((int)std::lround(100.0 * double(b) / double(p->totalBytes))) + "% " + what;
        };
        part(p->textBytes, "text & code");
        part(p->mediaBytes, "already compressed");
        part(p->exeBytes, "programs");
        part(p->otherBytes, "other data");
        if (!mix_.empty()) ImGui::TextColored(col::Faint, "%llu files: %s", (unsigned long long)p->files, mix_.c_str());
        for (auto& n : r.notes) {
            ImGui::TextColored(col::Faint, "-");
            ImGui::SameLine(0, em * 0.4f);
            ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
            ImGui::TextWrapped("%s", n.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    float bottom = ImGui::GetItemRectMax().y + em * 0.75f;
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(a, ImVec2(a.x + inner, bottom), u32(col::Accent, 0.07f), em * 0.7f);
    dl->AddRect(a, ImVec2(a.x + inner, bottom), u32(col::Accent, 0.28f), em * 0.7f);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(a.x, bottom));
    ImGui::Dummy(ImVec2(inner, em * 0.3f));
    return apply;
}

void App::drawCompressWindow() {
    if (!cmp_.open) return;
    const float em = emPx();
    if (cmp_.focus) {
        ImGui::SetNextWindowFocus();
        cmp_.focus = false;
    }
    const auto& formats = kz::outputFormats();
    const auto& fd = formats[cmp_.format];
    bool isKmp = fd.id == kz::OutFormat::Kmp;
    float W = em * 44;
    bool vis = beginDialog("###compress", isKmp ? "Super compress" : "Create archive",
                           isKmp ? "Kompressor's .kmp format: deduplication plus content-aware LZMA2/Zstd for the smallest output."
                                 : "Pack files and folders into a standard archive that any tool can open.",
                           isKmp ? Ico::Bolt : Ico::Box, &cmp_.open, W, isKmp ? col::Accent : col::Teal);
    if (!vis) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;

    // --- inputs
    caption("FILES & FOLDERS");
    if (!cmp_.inputs.empty()) {
        ImGui::SameLine();
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%zu item%s", cmp_.inputs.size(), cmp_.inputs.size() == 1 ? "" : "s");
        ImGui::PopFont();
    }
    {
        ImVec2 la = ImGui::GetCursorScreenPos();
        float lh = em * 7.2f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(la, ImVec2(la.x + inner, la.y + lh), u32(col::Bg), em * 0.6f);
        dl->AddRect(la, ImVec2(la.x + inner, la.y + lh), u32(col::Line), em * 0.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.6f, em * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.5f, em * 0.15f));
        ImGui::BeginChild("##inputs", ImVec2(inner, lh), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        if (cmp_.inputs.empty()) {
            ImGui::Dummy(ImVec2(0, em * 1.3f));
            centerText("Nothing added yet", col::Dim, fonts().bold);
            centerText("Use Add files / Add folder below, or drop them onto the window.", col::Faint, fonts().caption);
        }
        int remove = -1;
        float rowW = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < cmp_.inputs.size(); ++i) {
            ImGui::PushID((int)i);
            const std::string& in = cmp_.inputs[i];
            std::error_code ec;
            bool dir = std::filesystem::is_directory(kz::u8path(in), ec);
            ImVec2 ra = ImGui::GetCursorScreenPos();
            float rh = em * 1.9f;
            bool hov = ImGui::IsMouseHoveringRect(ra, ImVec2(ra.x + rowW, ra.y + rh)) && ImGui::IsWindowHovered();
            if (hov) ImGui::GetWindowDrawList()->AddRectFilled(ra, ImVec2(ra.x + rowW, ra.y + rh), u32(col::Text, 0.04f), em * 0.4f);
            float is = em * 1.1f;
            ImGui::SetCursorScreenPos(ImVec2(ra.x + em * 0.4f, ra.y + (rh - is) * 0.5f));
            icon(dir ? Ico::Folder : Ico::File, is, dir ? col::FolderCol : col::FileCol);
            ImGui::SameLine(0, em * 0.55f);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ra.y + (rh - ImGui::GetTextLineHeight()) * 0.5f));
            std::string name = kz::fileNameOf(in);
            if (name.empty()) name = in;
            ImGui::TextUnformatted(name.c_str());
            ImGui::SameLine(0, em * 0.5f);
            float bs = em * 1.5f;
            ImGui::PushClipRect(ra, ImVec2(ra.x + rowW - bs - em * 0.6f, ra.y + rh), true);
            ImGui::PushFont(fonts().caption);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + em * 0.1f);
            ImGui::TextColored(col::Faint, "%s", parentDir(in).c_str());
            ImGui::PopFont();
            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos(ImVec2(ra.x + rowW - bs - em * 0.2f, ra.y + (rh - bs) * 0.5f));
            if (iconButton("##rm", Ico::Close, bs, "Remove")) remove = (int)i;
            ImGui::SetCursorScreenPos(ImVec2(ra.x, ra.y + rh));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        if (remove >= 0) {
            std::string oldDir = commonParent(cmp_.inputs);
            cmp_.inputs.erase(cmp_.inputs.begin() + remove);
            retargetOutput(cmp_.out, sizeof cmp_.out, oldDir, cmp_.inputs);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }
    {
        auto addPicked = [this](const std::vector<std::string>& p) {
            bool wasEmpty = cmp_.inputs.empty() && cmp_.out[0] == 0;
            std::string oldDir = commonParent(cmp_.inputs);
            for (auto& x : p) cmp_.inputs.push_back(x);
            if (!wasEmpty) retargetOutput(cmp_.out, sizeof cmp_.out, oldDir, cmp_.inputs);
            if (wasEmpty) {
                int f = cmp_.format, k = cmp_.kmpMode;
                openCompressDialog(cmp_.inputs, false);
                cmp_.format = f;
                cmp_.kmpMode = k;
                copyTo(cmp_.out, sizeof cmp_.out, kz::withFormatExtension(cmp_.out, kz::outputFormats()[f].id));
            }
        };
        std::string start = cmp_.inputs.empty() ? lastDir_ : parentDir(cmp_.inputs.back());
        if (ghostButton("Add files", Ico::Plus, ImVec2(0, ImGui::GetFrameHeight())))
            fileDlg_.open("Add Files", FileDialog::Mode::AddFiles, start, addPicked);
        ImGui::SameLine(0, em * 0.2f);
        if (ghostButton("Add folder", Ico::Folder, ImVec2(0, ImGui::GetFrameHeight())))
            fileDlg_.open("Add Folders", FileDialog::Mode::AddFolders, start, addPicked);
    }
    if (!cmp_.inputs.empty()) {
        ImGui::SameLine(0, em * 0.2f);
        if (ghostButton("Clear", Ico::Trash, ImVec2(0, ImGui::GetFrameHeight()))) cmp_.inputs.clear();
    }
    ImGui::Dummy(ImVec2(0, em * 0.3f));

    // --- recommendation, from a quick look at the files (re-measured whenever the list changes)
    if (!cmp_.inputs.empty()) {
        std::string key;
        for (auto& s : cmp_.inputs) key += s + '\n';
        if (key != cmp_.adviceKey) {
            if (cmp_.adviceProg) cmp_.adviceProg->cancel = true;
            cmp_.adviceKey = key;
            cmp_.profile.reset();
            auto prof = std::make_shared<kz::ContentProfile>();
            auto inputs = cmp_.inputs;
            Job& j = startJob("Looking at the files",
                [inputs, prof](kz::Progress& p, std::string& err) { return kz::profileInputs(inputs, *prof, p, err); },
                [this, key, prof](Job& j) {
                    if (cmp_.adviceKey == key && j.message != "Cancelled") cmp_.profile = prof;  // a failed look still advises by defaults
                },
                false);
            cmp_.adviceProg = j.prog;
        }
        kz::Recommendation rec;
        if (drawAdvice(cmp_.profile.get(), !cmp_.profile, &cmp_.goal, cmp_.format, cmp_.level, cmp_.kmpMode, (unsigned)cmp_.threads,
                       inner, rec)) {
            cmp_.format = formatIndex(rec.format);
            if (rec.format == kz::OutFormat::Kmp) cmp_.kmpMode = rec.level;
            else cmp_.level = rec.level;
            if (cmp_.out[0]) copyTo(cmp_.out, sizeof cmp_.out, kz::withFormatExtension(cmp_.out, rec.format));
            cmp_.focusPassword = rec.wantsPassword;
        }
    }

    // --- format pills
    caption("FORMAT");
    {
        float x0 = ImGui::GetCursorPosX(), maxX = x0 + inner;
        for (size_t i = 0; i < formats.size(); ++i) {
            ImGui::PushFont(fonts().bold);
            float pw = ImGui::CalcTextSize(formats[i].name).x + em * 1.6f;
            ImGui::PopFont();
            if (i) {
                ImGui::SameLine(0, em * 0.4f);
                if (ImGui::GetCursorPosX() + pw > maxX) {
                    ImGui::NewLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.4f);
                }
            }
            if (pill(formats[i].name, (int)i == cmp_.format)) {
                cmp_.format = (int)i;
                cmp_.level = formats[i].defLevel;
                if (cmp_.out[0]) copyTo(cmp_.out, sizeof cmp_.out, kz::withFormatExtension(cmp_.out, formats[i].id));
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s  (%s)", formats[i].description, formats[i].ext);
        }
    }
    ImGui::PushFont(fonts().caption);
    ImGui::TextColored(col::Dim, "%s", fd.description);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, em * 0.3f));

    // --- mode / level
    if (isKmp) {
        caption("MODE");
        struct M { const char* name; const char* desc; const char* badge; };
        static const M modes[] = {
            {"Fast", "Zstd + dedup. Faster than ZIP and usually smaller.", nullptr},
            {"Balanced", "Zstd-19 with long-range matching. Quick to extract.", nullptr},
            {"Ultra", "LZMA2 + x86/ARM64 code filters + text tuning.", "BEST"},
            {"Max", "Tries every codec on every block, keeps the smallest. Slow.", nullptr},
            {"Extreme", "Context mixing on every core. ~15% smaller than Max; extracting is just as slow.", nullptr},
            {"Ultimate", "Heaviest model, 256 MB blocks, GBs of RAM. Smallest possible; for archiving.", "SMALLEST"},
        };
        float cw = (inner - em * 0.6f) * 0.5f, chh = em * 3.9f;
        for (int i = 0; i < 6; ++i) {
            if (i % 2) ImGui::SameLine(0, em * 0.6f);
            ImVec2 size(cw, chh);
            if (optionCard(modes[i].name, modes[i].name, modes[i].desc, cmp_.kmpMode == i, size, modes[i].badge))
                cmp_.kmpMode = i;
        }
        ImGui::Dummy(ImVec2(0, em * 0.1f));
        toggle("Deduplicate repeated data (content-defined chunking)", &cmp_.dedup);
    } else if (fd.minLevel < fd.maxLevel) {
        caption("LEVEL");
        ImGui::SameLine();
        ImGui::PushFont(fonts().caption);
        const char* ln = cmp_.level <= fd.minLevel + (fd.maxLevel - fd.minLevel) / 3
                             ? "faster"
                             : (cmp_.level >= fd.maxLevel - (fd.maxLevel - fd.minLevel) / 4 ? "smallest" : "balanced");
        ImGui::TextColored(col::AccentHover, "%d  -  %s", cmp_.level, ln);
        ImGui::PopFont();
        slider("##level", &cmp_.level, fd.minLevel, fd.maxLevel, inner);
    }
    caption("THREADS");
    ImGui::SameLine();
    ImGui::PushFont(fonts().caption);
    if (cmp_.threads == 0) ImGui::TextColored(col::AccentHover, "auto (%u cores)", kz::hardwareThreads());
    else ImGui::TextColored(col::AccentHover, "%d", cmp_.threads);
    ImGui::PopFont();
    slider("##threads", &cmp_.threads, 0, (int)kz::hardwareThreads(), inner);

    // --- password
    bool pwMismatch = false;
    if (fd.password) {
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        caption("ENCRYPTION  -  AES-256 (OPTIONAL)");
        float hw = (inner - em * 0.6f) * 0.5f;
        ImGui::SetNextItemWidth(hw);
        if (cmp_.focusPassword) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetScrollHereY(0.5f);
            cmp_.focusPassword = false;
        }
        ImGui::InputTextWithHint("##pw", "Password", cmp_.pw, sizeof cmp_.pw, ImGuiInputTextFlags_Password);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::SetNextItemWidth(hw);
        ImGui::InputTextWithHint("##pw2", "Confirm password", cmp_.pw2, sizeof cmp_.pw2, ImGuiInputTextFlags_Password);
        pwMismatch = std::strcmp(cmp_.pw, cmp_.pw2) != 0;
        if (pwMismatch && cmp_.pw2[0]) {
            icon(Ico::Warn, em * 0.95f, col::Error);
            ImGui::SameLine(0, em * 0.4f);
            ImGui::TextColored(col::Error, "Passwords do not match");
        } else if (cmp_.pw[0] && !pwMismatch) {
            icon(Ico::Lock, em * 0.95f, col::Ok);
            ImGui::SameLine(0, em * 0.4f);
            ImGui::TextColored(col::Ok, "Files will be encrypted with AES-256");
        }
    }

    // --- output
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    caption("SAVE TO");
    float bw = em * 6.5f;
    ImGui::SetNextItemWidth(inner - bw - em * 0.5f);
    ImGui::InputText("##out", cmp_.out, sizeof cmp_.out);
    ImGui::SameLine(0, em * 0.5f);
    if (secondaryButton("Browse...", ImVec2(bw, ImGui::GetFrameHeight()))) {
        std::string cur = cmp_.out;
        fileDlg_.open("Save Archive As", FileDialog::Mode::SaveFile, cur.empty() ? lastDir_ : parentDir(cur),
                      [this](const std::vector<std::string>& p) {
                          std::string o = p[0];
                          kz::OutFormat f;
                          if (kz::formatFromFileName(o, f)) cmp_.format = formatIndex(f);
                          else o = kz::withFormatExtension(o, kz::outputFormats()[cmp_.format].id);
                          copyTo(cmp_.out, sizeof cmp_.out, o);
                      },
                      cur.empty() ? std::string("Archive") + fd.ext : kz::fileNameOf(cur));
    }
    if (cmp_.out[0]) {
        // Long paths get cut off in the field: show where it actually lands (".../folder/name.ext").
        std::filesystem::path op = kz::u8path(cmp_.out);
        std::string folder = kz::pathToUtf8(op.parent_path().filename());
        std::string tail = (folder.empty() ? std::string() : "..." + std::string(1, (char)std::filesystem::path::preferred_separator) + folder +
                                                                 std::string(1, (char)std::filesystem::path::preferred_separator)) +
                           kz::pathToUtf8(op.filename());
        icon(Ico::FolderOpen, em * 0.9f, col::FolderCol);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%s", tail.c_str());
        ImGui::PopFont();
        ImGui::SetItemTooltip("%s", cmp_.out);
    }
    drawSplitOption(&cmp_.split, &cmp_.splitPreset, &cmp_.splitCustomMb, inner);
    toggle("Open the archive when finished", &cmp_.openWhenDone);

    // --- footer
    dialogFooter();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - em * 1.5f, p.y), ImVec2(p.x + inner + em * 1.5f, p.y), u32(col::Line));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
    }
    bool can = !cmp_.inputs.empty() && cmp_.out[0] && !(fd.password && pwMismatch);
    if (!can) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Faint, cmp_.inputs.empty() ? "Add something to compress" : "Choose where to save");
        ImGui::PopFont();
        ImGui::SameLine();
    }
    float pbw = em * 11.5f, cbw = em * 6.5f, bh = ImGui::GetFrameHeight() * 1.15f;
    alignRight(pbw + cbw + em * 0.6f);
    if (secondaryButton("Cancel", ImVec2(cbw, bh))) cmp_.open = false;
    ImGui::SameLine(0, em * 0.6f);
    ImGui::BeginDisabled(!can);
    if (primaryButton(isKmp ? "Super compress" : "Create archive", isKmp ? Ico::Bolt : Ico::Box, ImVec2(pbw, bh))) runCompress();
    ImGui::EndDisabled();
    endDialog();
}

void App::drawExtractWindow() {
    if (!ext_.open) return;
    ExplorerTab* t = tabById(ext_.tabId);
    if (!t) {
        ext_.open = false;
        return;
    }
    const float em = emPx();
    if (ext_.focus) {
        ImGui::SetNextWindowFocus();
        ext_.focus = false;
    }
    char sub[512];
    if (ext_.selectionOnly) std::snprintf(sub, sizeof sub, "%s  -  %zu selected item%s", t->title.c_str(), ext_.only.size(), ext_.only.size() == 1 ? "" : "s");
    else std::snprintf(sub, sizeof sub, "%s  -  all %zu entries", t->title.c_str(), t->info.entries.size());
    float W = em * 38;
    if (!beginDialog("###extract", "Extract files", sub, Ico::Extract, &ext_.open, W, col::Teal)) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;
    caption("DESTINATION");
    float bw = em * 6.5f;
    ImGui::SetNextItemWidth(inner - bw - em * 0.5f);
    ImGui::InputText("##dest", ext_.dest, sizeof ext_.dest);
    ImGui::SameLine(0, em * 0.5f);
    if (secondaryButton("Browse...", ImVec2(bw, ImGui::GetFrameHeight()))) {
        fileDlg_.open("Extract To", FileDialog::Mode::PickFolder, ext_.dest,
                      [this](const std::vector<std::string>& p) { copyTo(ext_.dest, sizeof ext_.dest, p[0]); });
    }
    ImGui::Dummy(ImVec2(0, em * 0.1f));
    std::string stem = kz::stripArchiveExt(t->src.name());
    std::string subl = "Put everything in a new folder \"" + stem + "\"";
    toggle(subl.c_str(), &ext_.subfolder);
    toggle("Skip files that already exist", &ext_.skipExisting);
    toggle("Show the folder when done", &ext_.reveal);
    std::string dest = ext_.dest;
    if (ext_.subfolder) dest = kz::pathToUtf8(kz::u8path(dest) / kz::u8path(stem));
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    {
        ImVec2 a = ImGui::GetCursorScreenPos();
        float h = em * 2.4f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + inner, a.y + h), u32(col::Teal, 0.07f), em * 0.6f);
        dl->AddRect(a, ImVec2(a.x + inner, a.y + h), u32(col::Teal, 0.25f), em * 0.6f);
        float is = em * 1.1f;
        drawIco(dl, Ico::FolderOpen, ImVec2(a.x + em * 0.7f, a.y + (h - is) * 0.5f), is, u32(col::FolderCol));
        ImGui::PushClipRect(a, ImVec2(a.x + inner - em * 0.6f, a.y + h), true);
        ImFont* sf = fonts().caption;
        float fs = sf->FontSize * ImGui::GetIO().FontGlobalScale;
        dl->AddText(sf, fs, ImVec2(a.x + em * 2.2f, a.y + (h - fs) * 0.5f), u32(mix(col::Teal, col::Text, 0.5f)), dest.c_str());
        ImGui::PopClipRect();
        ImGui::Dummy(ImVec2(inner, h));
    }
    dialogFooter();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - em * 1.5f, p.y), ImVec2(p.x + inner + em * 1.5f, p.y), u32(col::Line));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
    }
    float pbw = em * 9.0f, cbw = em * 6.5f, bh = ImGui::GetFrameHeight() * 1.15f;
    alignRight(pbw + cbw + em * 0.6f);
    if (secondaryButton("Cancel", ImVec2(cbw, bh))) ext_.open = false;
    ImGui::SameLine(0, em * 0.6f);
    ImGui::BeginDisabled(ext_.dest[0] == 0);
    if (primaryButton("Extract", Ico::Extract, ImVec2(pbw, bh))) {
        kz::ExtractOptions o;
        o.destDir = dest;
        o.only = ext_.only;
        o.stripPrefix = ext_.stripPrefix;
        o.password = t->password;
        o.skipExisting = ext_.skipExisting;
        lastDir_ = ext_.dest;
        runExtract(t->id, o, ext_.reveal);
        ext_.open = false;
    }
    ImGui::EndDisabled();
    endDialog();
}

void App::drawConvertWindow() {
    if (!conv_.open) return;
    ExplorerTab* t = tabById(conv_.tabId);
    if (!t) {
        conv_.open = false;
        return;
    }
    const float em = emPx();
    if (conv_.focus) {
        ImGui::SetNextWindowFocus();
        conv_.focus = false;
    }
    const auto& formats = kz::outputFormats();
    const auto& fd = formats[conv_.format];
    bool isKmp = fd.id == kz::OutFormat::Kmp;
    size_t files = 0;
    for (auto& e : t->info.entries) files += (!e.isDir && !e.isSymlink) ? 1 : 0;

    std::string sub = "Re-pack \"" + t->title + "\" into another format. Entries stream straight across; nothing is extracted to disk.";
    if (!beginDialog("###convert", "Recompress archive", sub.c_str(), isKmp ? Ico::Bolt : Ico::Box, &conv_.open, em * 44,
                     isKmp ? col::Accent : col::Teal)) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;

    // --- source summary
    {
        ImGui::PushFont(fonts().caption);
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s  -  %zu files, %s", t->info.format.c_str(), files,
                      kz::humanSize(t->info.totalSize).c_str());
        icon(Ico::Archive, em * 0.95f, col::Dim);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::TextColored(col::Dim, "%s", buf);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, em * 0.2f));
    }

    // --- recommendation (from the listing: names, sizes and how well the archive packs today)
    {
        kz::Recommendation rec;
        if (drawAdvice(&conv_.profile, false, &conv_.goal, conv_.format, conv_.level, conv_.mode, 0, inner, rec)) {
            conv_.format = formatIndex(rec.format);
            if (rec.format == kz::OutFormat::Kmp) conv_.mode = rec.level;
            else conv_.level = rec.level;
            if (conv_.out[0]) {
                std::string o = kz::withFormatExtension(conv_.out, rec.format);
                if (!t->src.inMemory() && kz::u8path(o) == kz::u8path(t->src.path))
                    o = kz::pathToUtf8(kz::u8path(parentDir(o)) /
                                       kz::u8path(kz::stripArchiveExt(kz::fileNameOf(o)) + "-recompressed" + kz::formatDesc(rec.format).ext));
                copyTo(conv_.out, sizeof conv_.out, o);
            }
            conv_.focusPassword = rec.wantsPassword;
        }
    }

    // --- format
    caption("FORMAT");
    {
        float x0 = ImGui::GetCursorPosX(), maxX = x0 + inner;
        for (size_t i = 0; i < formats.size(); ++i) {
            ImGui::PushFont(fonts().bold);
            float pw = ImGui::CalcTextSize(formats[i].name).x + em * 1.6f;
            ImGui::PopFont();
            if (i) {
                ImGui::SameLine(0, em * 0.4f);
                if (ImGui::GetCursorPosX() + pw > maxX) {
                    ImGui::NewLine();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.4f);
                }
            }
            // GZ, XZ, ... hold a single file: only offered when the archive has exactly one.
            bool usable = !formats[i].singleFile || files == 1;
            ImGui::BeginDisabled(!usable);
            if (pill(formats[i].name, (int)i == conv_.format)) {
                conv_.format = (int)i;
                conv_.level = formats[i].defLevel;
                if (conv_.out[0]) {
                    std::string o = kz::withFormatExtension(conv_.out, formats[i].id);
                    if (!t->src.inMemory() && kz::u8path(o) == kz::u8path(t->src.path))  // don't target the source itself
                        o = kz::pathToUtf8(kz::u8path(parentDir(o)) /
                                           kz::u8path(kz::stripArchiveExt(kz::fileNameOf(o)) + "-recompressed" + formats[i].ext));
                    copyTo(conv_.out, sizeof conv_.out, o);
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(usable ? "%s  (%s)" : "%s  (%s)\nOnly for archives that hold a single file - use TAR.* for several.",
                                  formats[i].description, formats[i].ext);
        }
    }
    ImGui::PushFont(fonts().caption);
    ImGui::TextColored(col::Dim, "%s", fd.description);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, em * 0.3f));

    // --- mode / level
    if (isKmp) {
        caption("MODE");
        struct M { const char* name; const char* desc; const char* badge; };
        static const M modes[] = {
            {"Fast", "Zstd + dedup. Seconds, not minutes.", nullptr},
            {"Balanced", "Zstd-19 with long-range matching.", nullptr},
            {"Ultra", "LZMA2 + code filters + text tuning.", "BEST"},
            {"Max", "Every codec on every block. Slow.", nullptr},
            {"Extreme", "Context mixing on every core. Slow both ways.", nullptr},
            {"Ultimate", "Heaviest model, huge blocks. Smallest; very slow.", "SMALLEST"},
        };
        float cw = (inner - em * 0.6f) * 0.5f, chh = em * 3.6f;
        for (int i = 0; i < 6; ++i) {
            if (i % 2) ImGui::SameLine(0, em * 0.6f);
            ImVec2 size(cw, chh);
            if (optionCard(modes[i].name, modes[i].name, modes[i].desc, conv_.mode == i, size, modes[i].badge)) conv_.mode = i;
        }
        ImGui::Dummy(ImVec2(0, em * 0.1f));
        toggle("Deduplicate repeated data (content-defined chunking)", &conv_.dedup);
    } else if (fd.minLevel < fd.maxLevel) {
        caption("LEVEL");
        ImGui::SameLine();
        ImGui::PushFont(fonts().caption);
        const char* ln = conv_.level <= fd.minLevel + (fd.maxLevel - fd.minLevel) / 3
                             ? "faster"
                             : (conv_.level >= fd.maxLevel - (fd.maxLevel - fd.minLevel) / 4 ? "smallest" : "balanced");
        ImGui::TextColored(col::AccentHover, "%d  -  %s", conv_.level, ln);
        ImGui::PopFont();
        slider("##clevel", &conv_.level, fd.minLevel, fd.maxLevel, inner);
    }

    // --- password for the new archive
    bool pwMismatch = false;
    if (fd.password) {
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        caption("ENCRYPTION  -  AES-256 (OPTIONAL)");
        float hw = (inner - em * 0.6f) * 0.5f;
        ImGui::SetNextItemWidth(hw);
        if (conv_.focusPassword) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetScrollHereY(0.5f);
            conv_.focusPassword = false;
        }
        ImGui::InputTextWithHint("##cpw", "Password", conv_.pw, sizeof conv_.pw, ImGuiInputTextFlags_Password);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::SetNextItemWidth(hw);
        ImGui::InputTextWithHint("##cpw2", "Confirm password", conv_.pw2, sizeof conv_.pw2, ImGuiInputTextFlags_Password);
        pwMismatch = std::strcmp(conv_.pw, conv_.pw2) != 0;
        if (pwMismatch && conv_.pw2[0]) {
            icon(Ico::Warn, em * 0.95f, col::Error);
            ImGui::SameLine(0, em * 0.4f);
            ImGui::TextColored(col::Error, "Passwords do not match");
        } else if (conv_.pw[0] && !pwMismatch) {
            icon(Ico::Lock, em * 0.95f, col::Ok);
            ImGui::SameLine(0, em * 0.4f);
            ImGui::TextColored(col::Ok, "The new archive will be encrypted with AES-256");
        }
    }

    // --- output
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    caption("SAVE TO");
    float bw = em * 6.5f;
    ImGui::SetNextItemWidth(inner - bw - em * 0.5f);
    ImGui::InputText("##cout", conv_.out, sizeof conv_.out);
    ImGui::SameLine(0, em * 0.5f);
    if (secondaryButton("Browse...", ImVec2(bw, ImGui::GetFrameHeight()))) {
        std::string cur = conv_.out;
        fileDlg_.open("Save Archive As", FileDialog::Mode::SaveFile, parentDir(cur),
                      [this, files](const std::vector<std::string>& p) {
                          std::string o = p[0];
                          kz::OutFormat f;
                          if (kz::formatFromFileName(o, f) && (!kz::formatDesc(f).singleFile || files == 1)) {
                              conv_.format = formatIndex(f);
                              conv_.level = kz::formatDesc(f).defLevel;
                          } else {
                              o = kz::withFormatExtension(o, kz::outputFormats()[conv_.format].id);
                          }
                          copyTo(conv_.out, sizeof conv_.out, o);
                      },
                      kz::fileNameOf(cur));
    }
    bool overwritesSource = !t->src.inMemory() && conv_.out[0] && kz::u8path(conv_.out) == kz::u8path(t->src.path);
    if (conv_.out[0] && !overwritesSource) {
        std::filesystem::path op = kz::u8path(conv_.out);
        std::string sep(1, (char)std::filesystem::path::preferred_separator);
        std::string folder = kz::pathToUtf8(op.parent_path().filename());
        std::string tail = (folder.empty() ? std::string() : "..." + sep + folder + sep) + kz::pathToUtf8(op.filename());
        icon(Ico::FolderOpen, em * 0.9f, col::FolderCol);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%s", tail.c_str());
        ImGui::PopFont();
        ImGui::SetItemTooltip("%s", conv_.out);
    }
    if (overwritesSource) {
        icon(Ico::Warn, em * 0.95f, col::Error);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::TextColored(col::Error, "That is the archive being converted - pick another name");
    }
    drawSplitOption(&conv_.split, &conv_.splitPreset, &conv_.splitCustomMb, inner);
    toggle("Open the new archive when finished", &conv_.openWhenDone);

    // --- footer
    dialogFooter();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - em * 1.5f, p.y), ImVec2(p.x + inner + em * 1.5f, p.y), u32(col::Line));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
    }
    float bh = ImGui::GetFrameHeight() * 1.15f;
    if (ghostButton("Compress files instead", Ico::Plus, ImVec2(0, bh))) {
        conv_.open = false;
        openCompressDialog({}, isKmp);
    }
    ImGui::SetItemTooltip("Make a new archive from files and folders on disk");
    ImGui::SameLine();
    float pbw = em * 10.5f, cbw = em * 6.5f;
    alignRight(pbw + cbw + em * 0.6f);
    if (secondaryButton("Cancel", ImVec2(cbw, bh))) conv_.open = false;
    ImGui::SameLine(0, em * 0.6f);
    bool can = conv_.out[0] && !overwritesSource && !(fd.password && pwMismatch);
    ImGui::BeginDisabled(!can);
    if (primaryButton("Recompress", isKmp ? Ico::Bolt : Ico::Box, ImVec2(pbw, bh))) {
        kz::CompressOptions o;
        o.format = fd.id;
        o.level = isKmp ? conv_.mode : conv_.level;
        o.dedup = conv_.dedup;
        o.password = fd.password ? conv_.pw : "";
        o.volumeSize = conv_.split ? splitBytes(conv_.splitPreset, conv_.splitCustomMb) : 0;
        runConvert(t->id, conv_.out, o, conv_.openWhenDone);
        conv_.open = false;
    }
    ImGui::EndDisabled();
    endDialog();
}

void App::drawPasswordPopup() {
    if (pwd_.request && !fileDlg_.isOpen()) {
        ImGui::OpenPopup("Password Required");
        pwd_.request = false;
    }
    const float em = emPx();
    float W = em * 30;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0), ImVec2(W, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.5f, em * 1.3f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.075f, 0.078f, 0.102f, 1.0f));
    bool open = ImGui::BeginPopupModal("Password Required", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (open) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        if (ImGui::GetWindowViewport() == ImGui::GetMainViewport()) {  // dragged out, DWM draws the shadow
            dl->PushClipRectFullScreen();
            softShadow(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetStyle().WindowRounding, em * 1.6f, 0.55f);
            dl->PopClipRect();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W - em * 3.0f);
        dialogHeader("Password required", pwd_.prompt.c_str(), Ico::Lock, col::Warn, W, nullptr);
        float inner = W - em * 3.0f;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        float ib = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(inner - ib - em * 0.4f);
        bool enter = ImGui::InputTextWithHint("##pw", "Password", pwd_.buf, sizeof pwd_.buf,
                                              ImGuiInputTextFlags_EnterReturnsTrue | (pwd_.show ? 0 : ImGuiInputTextFlags_Password));
        ImGui::SameLine(0, em * 0.4f);
        const ImVec4 on = col::AccentHover;
        if (iconButton("##show", Ico::Eye, ib, pwd_.show ? "Hide password" : "Show password", true, pwd_.show ? &on : nullptr))
            pwd_.show = !pwd_.show;
        ImGui::Dummy(ImVec2(0, em * 0.5f));
        float pbw = em * 7.5f, cbw = em * 6.5f, bh = ImGui::GetFrameHeight() * 1.15f;
        alignRight(pbw + cbw + em * 0.6f);
        bool cancel = secondaryButton("Cancel", ImVec2(cbw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
        ImGui::SameLine(0, em * 0.6f);
        bool ok = primaryButton("Unlock", Ico::Lock, ImVec2(pbw, bh)) || enter;
        ImGui::PopTextWrapPos();
        if (ok) {
            std::string pw = pwd_.buf;
            std::memset(pwd_.buf, 0, sizeof pwd_.buf);
            auto cb = std::move(pwd_.onOk);
            pwd_.onOk = nullptr;
            pwd_.onCancel = nullptr;
            ImGui::CloseCurrentPopup();
            if (cb) cb(pw);
        } else if (cancel) {
            std::memset(pwd_.buf, 0, sizeof pwd_.buf);
            pwd_.onOk = nullptr;
            auto cb = std::move(pwd_.onCancel);
            pwd_.onCancel = nullptr;
            ImGui::CloseCurrentPopup();
            if (cb) cb();
        }
        ImGui::EndPopup();
    }
}

void App::drawFormats() {
    if (!showFormats_) return;
    const float em = emPx();
    if (!beginDialog("###formats", "Supported formats", "Everything below is handled natively - no external tools, no plugins.",
                     Ico::Layers, &showFormats_, em * 48, col::Accent2)) {
        endDialog();
        return;
    }
    caption("OPEN, EXPLORE & EXTRACT");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(em * 0.6f, em * 0.35f));
    if (ImGui::BeginTable("##rf", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, em * 15);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthStretch);
        for (auto& f : kz::readableFormats()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushFont(fonts().bold);
            ImGui::TextWrapped("%s", f.first.c_str());
            ImGui::PopFont();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(col::Dim, "%s", f.second.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    caption("CREATE");
    if (ImGui::BeginTable("##wf", 3, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed, em * 7);
        ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
        ImGui::TableSetupColumn("c", ImGuiTableColumnFlags_WidthStretch);
        for (auto& f : kz::outputFormats()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushFont(fonts().bold);
            ImGui::TextUnformatted(f.name);
            ImGui::PopFont();
            ImGui::TableSetColumnIndex(1);
            chip(f.ext, f.id == kz::OutFormat::Kmp ? col::Accent : col::FileCol);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(col::Dim, "%s", f.description);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    icon(Ico::Info, em * 0.95f, col::Dim);
    ImGui::SameLine(0, em * 0.4f);
    ImGui::TextColored(col::Dim, "RAR is written stored (uncompressed): RAR's compression method is proprietary.");
    endDialog();
}

void App::drawAbout() {
    if (!showAbout_) return;
    const float em = emPx();
    char ver[64];
    std::snprintf(ver, sizeof ver, "Version %s", KZ_VERSION);
    if (!beginDialog("###about", "Kompressor", ver, Ico::Logo, &showAbout_, em * 34, col::Accent)) {
        endDialog();
        return;
    }
    ImGui::TextUnformatted("Universal archive extractor, compressor and super compressor.");
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    float kw = em * 6;
    kvRow("Engines", kz::libraryVersions().c_str(), kw);
    kvRow("KMP", "FastCDC dedup, XXH3, LZMA2 (+BCJ x86/ARM64), Zstandard", kw);
    char ui[96];
    std::snprintf(ui, sizeof ui, "Dear ImGui %s, GLFW %s", IMGUI_VERSION, glfwGetVersionString());
    kvRow("UI", ui, kw);
    endDialog();
}

void App::drawSettings() {
    if (!showSettings_) return;
    const float em = emPx();
    if (!beginDialog("###settings", "Settings", "Preferences for this computer", Ico::Gear, &showSettings_, em * 32,
                     col::Accent)) {
        endDialog();
        return;
    }
    if (debugDetach_) {
        debugDetach_ = false;
        ImGuiViewport* mv = ImGui::GetMainViewport();
        ImGui::SetWindowPos(ImVec2(mv->Pos.x + mv->Size.x + em * 2, mv->Pos.y + em * 4));
    }
    if (shell::supported()) {
        caption("INTEGRATION");
        if (ImGui::IsWindowAppearing()) shellMenu_ = shell::registered();  // the installer may have changed it
        bool on = shellMenu_;
        if (toggle("Show Kompressor in the Explorer right-click menu", &on)) {
            std::string err;
            if (on ? shell::registerMenu(err) : shell::unregisterMenu(err)) {
                shellMenu_ = on;
                log(col::Dim, on ? "Added Kompressor to the Explorer right-click menu."
                                 : "Removed Kompressor from the Explorer right-click menu.");
                toast(col::Ok, on ? "Added to the right-click menu" : "Removed from the right-click menu");
            } else {
                log(col::Error, "Explorer menu: " + err);
                toast(col::Error, "Could not change the right-click menu");
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
        ImGui::TextWrapped("Adds a \"Komp\" submenu to files and folders: compress, super compress, and for "
                           "archives open and extract. On Windows 11 it sits under \"Show more options\".");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, em * 0.6f));
    }
    caption("PERFORMANCE");
    if (toggle("Maximum performance while compressing and extracting", &maxPerf_)) {
        prefs::setBool("MaxPerformance", maxPerf_);
        kz::perf::setMaxPerformance(maxPerf_);
        updateBoost();  // applies to (or releases) a job that is already running
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
#ifdef _WIN32
    ImGui::TextWrapped("While a job runs, Kompressor uses every core at high priority, stops Windows from throttling "
                       "it onto efficiency cores or lower clocks, switches to the High performance power plan (yours "
                       "comes back afterwards) and keeps the PC awake. The PC may feel slower and run hotter and louder "
                       "meanwhile. It does not overclock: clock limits stay as your BIOS sets them.");
#else
    ImGui::TextWrapped("While a job runs, Kompressor lets more threads work in parallel by using more of the free "
                       "memory. The computer may feel slower and run hotter meanwhile.");
#endif
    ImGui::PopStyleColor();
    if (kz::gpu::supported()) {
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        if (toggle("Use the graphics card too", &useGpu_)) {
            if (useGpu_ && !kz::gpu::available()) {
                useGpu_ = false;
                log(col::Error, "No graphics card that Kompressor can use was found (it needs Direct3D 11).");
                toast(col::Error, "No usable graphics card found");
            }
            prefs::setBool("UseGpu", useGpu_);
            kz::gpu::setEnabled(useGpu_);
            if (useGpu_) log(col::Dim, "The graphics card (" + kz::gpu::adapterName() + ") will help with ZIP compression.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
        std::string card = kz::gpu::adapterName();
        ImGui::TextWrapped("%s", ("While a ZIP is made, " + (card.empty() ? std::string("the graphics card") : card) +
                                  " searches for repeated data alongside the CPU. It takes the pieces it can finish "
                                  "before the CPU would get to them, so it only ever speeds a job up (most on big "
                                  "jobs and high levels). The ZIP stays a normal one any app opens, about the same "
                                  "size. Other formats can't be split up like this and stay on the CPU.")
                                     .c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0, em * 0.6f));
    caption("INTERFACE");
    toggle("Show the activity panel", &showActivity_);
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    alignRight(em * 6);
    if (secondaryButton("Done", ImVec2(em * 6, 0))) showSettings_ = false;
    endDialog();
}

void App::drawToasts() {
    const float em = emPx();
    double now = ImGui::GetTime();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return now - t.time > 5.0; }), toasts_.end());
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - em * 3.2f;
    int i = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++i) {
        float age = float(now - it->time);
        float alpha = age > 4.4f ? (5.0f - age) / 0.6f : std::min(1.0f, age * 5.0f);
        float slide = (1.0f - std::min(1.0f, age * 5.0f)) * em * 2.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 1.2f + slide, y), ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowViewport(vp->ID);
        std::string name = "##toast" + std::to_string(i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::max(0.0f, alpha));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.1f, em * 0.8f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, mix(col::Surface2, it->color, 0.06f));
        ImGui::PushStyleColor(ImGuiCol_Border, mix(col::Line, it->color, 0.35f));
        ImGui::Begin(name.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        dl->PushClipRectFullScreen();
        softShadow(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetStyle().WindowRounding, em * 1.0f, 0.45f * alpha);
        dl->PopClipRect();
        dl->AddRectFilled(ImVec2(wp.x + 1, wp.y + em * 0.6f), ImVec2(wp.x + 4, wp.y + ws.y - em * 0.6f), u32(it->color), 2.0f);
        icon(logIcon(it->color), em * 1.15f, it->color);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + em * 24);
        ImGui::TextUnformatted(it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ws.y + em * 0.6f;
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

}  // namespace kzgui
