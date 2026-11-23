#include "window_frame.hpp"

#include <GLFW/glfw3.h>

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

int resizeBorder(HWND h) {
    UINT dpi = GetDpiForWindow(h);
    return GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
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
            RECT rc;
            GetClientRect(h, &rc);
            if (!IsZoomed(h)) {
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
            }
            if (pt.y >= 0 && pt.y < (LONG)g_titleH) {
                for (const Rect& r : g_interactive)
                    if (pt.x >= r.x0 && pt.x < r.x1 && pt.y >= r.y0 && pt.y < r.y1) return HTCLIENT;
                return HTCAPTION;
            }
            return HTCLIENT;
        }
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

void setTitleBar(float height, const std::vector<Rect>& interactive) {
    g_titleH = height;
    g_interactive = interactive;
}

}  // namespace kzgui::frame
