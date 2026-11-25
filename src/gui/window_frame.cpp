#include "window_frame.hpp"

#include "native_files.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#include <windows.h>
#include <windowsx.h>
#endif

namespace kzgui::frame {

namespace {
GLFWwindow* g_window = nullptr;
bool g_custom = false;
float g_titleH = 0.0f;
std::vector<Rect> g_interactive;

#ifdef _WIN32
WNDPROC g_prevProc = nullptr;
constexpr UINT_PTR kRedrawTimer = 0x4B5A;

int resizeBorder(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// Which resize edge a client-area point is on (HTLEFT ... HTBOTTOMRIGHT), or 0.
LRESULT edgeAt(HWND h, POINT pt) {
    if (IsZoomed(h) || !(GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME)) return 0;
    RECT rc;
    GetClientRect(h, &rc);
    if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top || pt.y >= rc.bottom) return 0;
    int b = resizeBorder(h);
    bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
    if (T && L) return HTTOPLEFT;
    if (T && R) return HTTOPRIGHT;
    if (B && L) return HTBOTTOMLEFT;
    if (B && R) return HTBOTTOMRIGHT;
    if (L) return HTLEFT;
    if (R) return HTRIGHT;
    if (T) return HTTOP;
    if (B) return HTBOTTOM;
    return 0;
}

LRESULT CALLBACK frameProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // Client area = whole window (no caption, no visible border). When maximized Windows
                // places the window slightly off-screen by the frame size, so pull the client back in.
                if (IsZoomed(h)) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    UINT dpi = GetDpiForWindow(h);
                    int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    p->rgrc[0].left += fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            // Don't let Windows repaint a (now invisible) non-client caption on focus changes.
            return DefWindowProcW(h, msg, wp, -1);
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &pt);
            if (LRESULT edge = edgeAt(h, pt)) return edge;
            if (pt.y >= 0 && pt.y < (LONG)g_titleH) {
                for (const Rect& r : g_interactive)
                    if (pt.x >= r.x0 && pt.x < r.x1 && pt.y >= r.y0 && pt.y < r.y1) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        // Moving or resizing the window runs Windows' own loop until the mouse is released, and the
        // window stood still meanwhile (jobs' progress included). A timer keeps it drawing.
        case WM_ENTERSIZEMOVE:
            SetTimer(h, kRedrawTimer, 15, nullptr);
            break;
        case WM_EXITSIZEMOVE:
            KillTimer(h, kRedrawTimer);
            break;
        case WM_TIMER:
            if (wp == kRedrawTimer) {
                native::redraw();
                return 0;
            }
            break;
        default:
            break;
    }
    return CallWindowProcW(g_prevProc, h, msg, wp, lp);
}
#endif
}  // namespace

void install(GLFWwindow* window) {
    g_window = window;
#ifdef _WIN32
    HWND h = glfwGetWin32Window(window);
    if (!h) return;
    g_prevProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(frameProc)));
    if (!g_prevProc) return;
    g_custom = true;
    // Keep the DWM drop shadow and ask for dark, rounded (Windows 11) chrome.
    MARGINS m{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(h, &m);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int corner = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
}

bool custom() { return g_custom; }

bool maximized() { return g_window && glfwGetWindowAttrib(g_window, GLFW_MAXIMIZED); }

void minimize() {
    if (g_window) glfwIconifyWindow(g_window);
}

void toggleMaximize() {
    if (!g_window) return;
    if (maximized()) glfwRestoreWindow(g_window);
    else glfwMaximizeWindow(g_window);
}

void placeCentered(int w, int h) {
    if (!g_window) return;
    // The monitor holding the window's centre, else the primary one.
    int wx, wy, ww, wh;
    glfwGetWindowPos(g_window, &wx, &wy);
    glfwGetWindowSize(g_window, &ww, &wh);
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    for (int i = 0; i < count; ++i) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
        int cx = wx + ww / 2, cy = wy + wh / 2;
        if (cx >= mx && cx < mx + mw && cy >= my && cy < my + mh) mon = mons[i];
    }
    int mx = 0, my = 0, mw = w, mh = h;
    if (mon) glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);
    w = w < mw ? w : mw;
    h = h < mh ? h : mh;
    int x = mx + (mw - w) / 2, y = my + (mh - h) / 2;
#ifdef _WIN32
    // With the custom frame the client area is the whole window, which GLFW's sizing doesn't know.
    if (g_custom) {
        HWND hw = glfwGetWin32Window(g_window);
        if (IsZoomed(hw)) ShowWindow(hw, SW_RESTORE);
        SetWindowPos(hw, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
#endif
    glfwSetWindowSize(g_window, w, h);
    glfwSetWindowPos(g_window, x, y);
}

void setTitleBar(float height, const std::vector<Rect>& interactive) {
    g_titleH = height;
    g_interactive = interactive;
}

void edgeCursor() {
#ifdef _WIN32
    // The resize border lies inside the client area, so GLFW counts it as "in the window" and puts
    // ImGui's cursor (an arrow) back on every frame, hiding Windows' resize arrows. Ask ImGui for them.
    if (!g_custom || !g_window) return;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsAnyItemActive()) return;
    HWND h = glfwGetWin32Window(g_window);
    POINT pt;
    if (!GetCursorPos(&pt) || WindowFromPoint(pt) != h) return;
    ScreenToClient(h, &pt);
    switch (edgeAt(h, pt)) {
        case HTLEFT:
        case HTRIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW); break;
        case HTTOP:
        case HTBOTTOM: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); break;
        case HTTOPLEFT:
        case HTBOTTOMRIGHT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE); break;
        case HTTOPRIGHT:
        case HTBOTTOMLEFT: ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNESW); break;
        default: break;
    }
#endif
}

void styleDetached(void* nativeHandle) {
#ifdef _WIN32
    HWND h = static_cast<HWND>(nativeHandle);
    if (!h) return;
    if (g_window)  // owned windows stay above the main window and minimize with it
        SetWindowLongPtrW(h, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(glfwGetWin32Window(g_window)));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    int corner = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(h, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    MARGINS m{1, 1, 1, 1};  // gives the borderless popup the standard drop shadow
    DwmExtendFrameIntoClientArea(h, &m);
#else
    (void)nativeHandle;
#endif
}

}  // namespace kzgui::frame
