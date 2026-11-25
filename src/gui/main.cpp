// Kompressor GUI entry point: GLFW window + OpenGL 3 + Dear ImGui.
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "app.hpp"
#include "app_icon.hpp"
#include "shell_integration.hpp"
#include "kmp.hpp"
#include "kz_archive.hpp"
#include "kz_perf.hpp"
#include "kz_ramdrive.hpp"
#include "native_files.hpp"
#include "theme.hpp"
#include "window_frame.hpp"

namespace {

kzgui::App* g_app = nullptr;

void onDrop(GLFWwindow* w, int count, const char** paths) {
    if (!g_app) return;
    std::vector<std::string> v;
    for (int i = 0; i < count; ++i) v.emplace_back(paths[i]);  // GLFW hands out UTF-8 on every platform
    // Where the files landed, in ImGui coordinates (screen coordinates when viewports are enabled).
    double cx = 0, cy = 0;
    glfwGetCursorPos(w, &cx, &cy);
    int wx = 0, wy = 0;
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) glfwGetWindowPos(w, &wx, &wy);
    g_app->onDrop(v, ImVec2((float)(wx + cx), (float)(wy + cy)));
}

void onError(int code, const char* msg) { std::fprintf(stderr, "GLFW error %d: %s\n", code, msg); }

std::string firstExisting(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return {};
}

// Loads a system UI font + monospace font so text looks native and covers Latin/Greek/Cyrillic.
ImFont* loadFonts(float px, ImFont** monoOut) {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, 0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F,
        0x2000, 0x206F, 0x2190, 0x21FF, 0x2500, 0x257F, 0x2700, 0x27BF, 0,
    };
#if defined(_WIN32)
    std::string ui = firstExisting({"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"});
    std::string bold = firstExisting({"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
                                      "C:\\Windows\\Fonts\\arialbd.ttf"});
    std::string mono = firstExisting({"C:\\Windows\\Fonts\\CascadiaMono.ttf", "C:\\Windows\\Fonts\\consola.ttf",
                                      "C:\\Windows\\Fonts\\cour.ttf"});
#elif defined(__APPLE__)
    std::string ui = firstExisting({"/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/Library/Fonts/Arial Unicode.ttf",
                                    "/System/Library/Fonts/Supplemental/Arial.ttf"});
    std::string bold = firstExisting({"/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf"});
    std::string mono = firstExisting({"/System/Library/Fonts/Menlo.ttc", "/System/Library/Fonts/Monaco.ttf"});
#else
    std::string ui = firstExisting({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
                                    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                    "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf"});
    std::string bold = firstExisting({"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                                      "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                                      "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf"});
    std::string mono = firstExisting({"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                      "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
                                      "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"});
#endif
    if (bold.empty()) bold = ui;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    auto add = [&](const std::string& file, float size) -> ImFont* {
        ImFont* f = file.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, ranges);
        if (!f) {
            ImFontConfig d;
            d.SizePixels = size;
            f = io.Fonts->AddFontDefault(&d);
        }
        return f;
    };
    kzgui::Fonts& F = kzgui::fonts();
    F.body = add(ui, px);  // first font added = default
    F.bold = add(bold, px);
    F.caption = add(ui, std::round(px * 0.85f));
    F.h2 = add(bold, std::round(px * 1.3f));
    F.h1 = add(bold, std::round(px * 2.3f));
    F.mono = mono.empty() ? F.body : add(mono, std::round(px * 0.93f));
    *monoOut = F.mono;
    return F.body;
}

}  // namespace

void (*g_prevCreateWindow)(ImGuiViewport*) = nullptr;  // backend's Platform_CreateWindow

float g_pendingScale = 0.0f;  // set by the content-scale callback, applied before the next frame
float g_currentScale = 0.0f;
std::function<void()> g_render;  // renders one frame; used by the window-refresh callback

// (Re)builds fonts and style for a monitor content scale. Returns the style scale.
float applyUiScale(float xs, ImFont** mono) {
    if (xs <= 0.0f) xs = 1.0f;
    g_currentScale = xs;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const float basePx = 15.0f;
    io.FontDefault = loadFonts(basePx * xs, mono);
#if defined(__APPLE__)
    io.FontGlobalScale = 1.0f / xs;  // fonts are rasterised at framebuffer resolution, laid out in points
    float uiScale = 1.0f;
#else
    float uiScale = xs;
#endif
    kzgui::applyTheme(uiScale);
    return uiScale;
}

int main(int argc, char** argv) {
    kz::initLocale();
    std::vector<std::string> args = kz::utf8Args(argc, argv);
    if (int rc = kz::RamDrive::helperMain(args); rc >= 0) return rc;  // the elevated RAM drive helper (Windows)
    // Debug aid: "--snapshot out.ppm" renders ~3 s of frames, saves the framebuffer and exits.
    std::string snapshotPath, snapshotSelect;
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "--select") {
            snapshotSelect = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "--snapshot") {
            snapshotPath = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }
    // "--snapshot-size 1320x1400": a different window size, to check layouts that need more room.
    int winW = 1320, winH = 840;
    bool sizeGiven = false;
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "--snapshot-size") {
            std::sscanf(args[i + 1].c_str(), "%dx%d", &winW, &winH);
            sizeGiven = true;
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }

    // Explorer menu maintenance: "--register-shell" / "--unregister-shell".
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--register-shell" || args[i] == "--unregister-shell") {
            std::string err;
            bool ok = args[i] == "--register-shell" ? kzgui::shell::registerMenu(err) : kzgui::shell::unregisterMenu(err);
            if (!ok) std::fprintf(stderr, "kompressor: %s\n", err.c_str());
            return ok ? 0 : 1;
        }
    // Launched from the Explorer menu: "--shell <action> <path>". Explorer starts one process per selected
    // item; the first one collects the rest and the others exit right here.
    bool openSettings = false, debugDetach = false, debugRecompress = false;
    for (size_t i = 1; i < args.size();)
        if (args[i] == "--settings" || args[i] == "--debug-detach" || args[i] == "--debug-recompress") {
            (args[i] == "--settings" ? openSettings : args[i] == "--debug-detach" ? debugDetach : debugRecompress) = true;
            args.erase(args.begin() + i);
        } else {
            ++i;
        }
    std::string debugHashList;
    bool debugHash = false;
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--debug-hash") {
            debugHash = true;
            size_t n = 1;
            if (i + 2 < args.size()) debugHashList = args[i + 1], n = 2;  // a list, then the archive
            args.erase(args.begin() + i, args.begin() + i + n);
            break;
        }
    std::string debugSearch;
    for (size_t i = 1; i + 1 < args.size(); ++i)
        if (args[i] == "--debug-search") {
            debugSearch = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }
    int debugRam = 0;  // 1 open the RAM Drive window, 2 also start a drive (not mounted)
    for (size_t i = 1; i < args.size(); ++i)
        if (args[i] == "--debug-ram" || args[i] == "--debug-ram-run") {
            debugRam = args[i] == "--debug-ram" ? 1 : 2;
            args.erase(args.begin() + i);
            break;
        }
    std::string shellAction;
    std::vector<std::string> shellPaths;
    if (args.size() >= 3 && args[1] == "--shell") {
        shellAction = args[2];
        shellPaths.assign(args.begin() + 3, args.end());
        args.resize(1);
        if (!kzgui::shell::collectShellLaunch(shellAction, shellPaths)) return 0;
    } else if (args.size() == 4 && args[1] == "--shell-list") {
        // The menu extension passes very long selections as a UTF-8 list file (one path per line).
        shellAction = args[2];
        std::error_code ec;
        auto listPath = kz::u8path(args[3]);
        {
            std::ifstream in(listPath, std::ios::binary);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) shellPaths.push_back(line);
            }
        }
        std::filesystem::remove(listPath, ec);
        args.resize(1);
    }

    glfwSetErrorCallback(onError);
#if defined(__linux__) && GLFW_VERSION_MINOR >= 4
    glfwInitHint(GLFW_PLATFORM, GLFW_ANY_PLATFORM);
#endif
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

    // Explorer menu tasks that need no dialog run in a small window of their own, not the full app.
    std::string quickVerb;
    if (shellAction == "extract-here" || shellAction == "extract-to") quickVerb = "Extracting";
    else if (shellAction == "add-7z" || shellAction == "add-zip") quickVerb = "Compressing";
    else if (shellAction == "add-kmp") quickVerb = "Super-compressing";
    if (shellPaths.empty()) quickVerb.clear();
    const bool quick = !quickVerb.empty();
    if (quick) {
        if (!sizeGiven) winW = 460, winH = 200;
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown once placed
    }

    GLFWwindow* window = glfwCreateWindow(winW, winH, "Kompressor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
#ifdef _WIN32
    // In a window, the desktop compositor shows frames at the monitor's refresh rate. GL's own vsync
    // there drifts against it and skips frames (138 fps on a 155 Hz screen), so frames are paced by
    // waiting for each composition pass instead (DwmFlush after the swap).
    glfwSwapInterval(0);
#else
    glfwSwapInterval(1);
#endif
    if (!quick) glfwSetWindowSizeLimits(window, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    kzgui::setWindowIcon(window);
    kzgui::frame::install(window);  // custom title bar (Windows)
    if (quick) {
        float sx = 1.0f, sy = 1.0f;
#ifndef __APPLE__
        glfwGetWindowContentScale(window, &sx, &sy);  // window sizes are in pixels here
#endif
        kzgui::frame::placeCentered((int)std::lround(winW * sx), (int)std::lround(winH * sx));
        glfwShowWindow(window);
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // layout is fixed; nothing worth persisting
    // Dialogs can be dragged out of the main window and become windows of their own.
    // (Not on Wayland: clients there cannot position their windows.)
    bool viewports = true;
#if defined(__linux__) && GLFW_VERSION_MINOR >= 4
    viewports = glfwGetPlatform() != GLFW_PLATFORM_WAYLAND;
#endif
    if (viewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        io.ConfigViewportsNoTaskBarIcon = true;  // they belong to the main window, not the taskbar
    }

    // HiDPI: macOS works in points with a 2x framebuffer; Windows/Linux work in pixels.
    // The scale follows the monitor the window is on (e.g. 100% on a 1080p screen, 175% on 4K):
    // fonts and sizes are rebuilt whenever the window moves to a monitor with a different scale.
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    ImFont* mono = nullptr;
    float uiScale = applyUiScale(xs, &mono);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);
    if (viewports) {
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        g_prevCreateWindow = pio.Platform_CreateWindow;
        pio.Platform_CreateWindow = [](ImGuiViewport* vp) {
            g_prevCreateWindow(vp);
            kzgui::frame::styleDetached(vp->PlatformHandleRaw);
        };
    }
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float x, float) { g_pendingScale = x; });

    kzgui::native::init();  // OLE: dragging files out, file clipboard
    kz::perf::recoverPowerPlan();  // a crash during a boosted job may have left the power plan switched
    {
        kzgui::App app(window, mono, uiScale);
        g_app = &app;
        glfwSetDropCallback(window, onDrop);
        if (!snapshotSelect.empty()) app.debugSelect(snapshotSelect);
        if (openSettings) app.showSettings();
        if (debugDetach) app.debugDetach();
        if (debugRecompress) app.debugRecompress();
        if (debugHash) app.debugHash(debugHashList);
        if (!debugSearch.empty()) app.debugSearch(debugSearch);
        if (debugRam) app.debugRam(debugRam == 2);

        // Files passed on the command line / "Open with": archives open, anything else is queued to compress.
        std::vector<std::string> toCompress;
        for (size_t i = 1; i < args.size(); ++i) {
            if (kz::isArchiveFile(args[i]))  // by content too, so a misnamed archive still opens
                app.openArchive(args[i]);
            else if (args[i].rfind("-psn_", 0) != 0)  // ignore old macOS process serial args
                toCompress.push_back(args[i]);
        }
        if (!toCompress.empty()) app.openCompressDialog(toCompress, false);

        if (!shellPaths.empty()) {
            namespace fs = std::filesystem;
            if (quick) app.enterQuickMode(quickVerb);
            if (shellAction == "compress" || shellAction == "super") {
                app.openCompressDialog(shellPaths, shellAction == "super");
            } else if (shellAction == "open") {
                for (auto& p : shellPaths) app.openArchive(p);
            } else if (shellAction == "extract-here" || shellAction == "extract-to") {
                for (auto& p : shellPaths) {
                    fs::path dir = kz::u8path(p).parent_path();
                    if (shellAction == "extract-to") dir /= kz::u8path(kz::stripArchiveExt(kz::fileNameOf(p)));
                    app.extractFile(p, kz::pathToUtf8(dir));
                }
                app.quitWhenDone();
            } else if (shellAction == "extract") {
                for (auto& p : shellPaths) app.openExtractDialog(p);
            } else if (shellAction == "hash") {
                app.hashFiles(shellPaths);
            } else if (shellAction == "test") {
                for (auto& p : shellPaths) app.testArchiveFile(p);
            } else if (shellAction == "add-7z" || shellAction == "add-zip" || shellAction == "add-kmp") {
                app.quickCompress(shellPaths, shellAction == "add-7z"    ? kz::OutFormat::SevenZip
                                              : shellAction == "add-zip" ? kz::OutFormat::Zip
                                                                         : kz::OutFormat::Kmp);
                app.quitWhenDone();
            }
        } else if (snapshotPath.empty()) {
            kzgui::shell::autoRegister();  // put Kompressor in the right-click menu unless the user turned it off
        }

        // One UI frame. Called from the main loop and from the window-refresh callback: while the user
        // drags or resizes the window, Windows runs its own modal loop and our loop is paused, so
        // repaint requests (e.g. the resize when crossing to a monitor with another DPI) render here.
        bool inFrame = false;
        auto renderFrame = [&] {
            if (inFrame) return;  // e.g. a native file dialog dispatching WM_PAINT mid-frame
            inFrame = true;
            if (g_pendingScale > 0.0f) {
                float ns = g_pendingScale;
                g_pendingScale = 0.0f;
                if (std::fabs(ns - g_currentScale) > 0.01f) {
                    ImGui_ImplOpenGL3_DestroyFontsTexture();
                    applyUiScale(ns, &mono);
                    ImGui_ImplOpenGL3_CreateFontsTexture();
                }
            }
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            kzgui::beginFrameUi();
            kzgui::smoothScroll();
            app.frame();
            ImGui::Render();
            int w, h;
            glfwGetFramebufferSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.055f, 0.059f, 0.075f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            if (!snapshotPath.empty() && ImGui::GetTime() > 3.0) {
                std::vector<unsigned char> px((size_t)w * h * 3);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
                if (FILE* f = std::fopen(snapshotPath.c_str(), "wb")) {
                    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
                    for (int y = h - 1; y >= 0; --y) std::fwrite(px.data() + (size_t)y * w * 3, 1, (size_t)w * 3, f);
                    std::fclose(f);
                }
                glfwSetWindowShouldClose(window, 1);
            }
            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();  // dialogs dragged outside the main window
                ImGui::RenderPlatformWindowsDefault();
                glfwMakeContextCurrent(window);
            }
            glfwSwapBuffers(window);
#ifdef _WIN32
            DwmFlush();
#endif
            inFrame = false;
        };
        g_render = renderFrame;
        kzgui::native::setRedraw(renderFrame);
        glfwSetWindowRefreshCallback(window, [](GLFWwindow*) {
            if (g_render) g_render();
        });

        // ImGui needs a few frames to settle after input: a menu or popup that just opened stays hidden for
        // one frame while it is measured. Waiting for the next event there leaves a visible blank (the
        // "flash" when moving between File / Archive / View), so draw on until things are still.
        auto unsettled = [] {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.InputEventsTrail.Size > 0) return true;
            for (ImGuiWindow* win : g.Windows)
                if (win->Active && (win->HiddenFramesCannotSkipItems > 0 || win->HiddenFramesCanSkipItems > 0 || win->Appearing))
                    return true;
            return false;
        };
        int settleFrames = 0;
        // Vsync paces the loop while the window is on screen. Covered or minimized, Windows returns from
        // SwapBuffers at once and an animation (the job spinner) would redraw as fast as a core allows, so
        // a frame that came back far too early is held to the refresh rate by the clock instead. Only then:
        // sleeping on top of vsync would miss vblanks and drop the frame rate.
        // The refresh rate is the one of the monitor the window is on (checked twice a second).
        auto windowRefresh = [&]() -> int {
            int wx, wy, ww, wh, n = 0, best = 0, hz = 60;
            glfwGetWindowPos(window, &wx, &wy);
            glfwGetWindowSize(window, &ww, &wh);
            GLFWmonitor** mons = glfwGetMonitors(&n);
            for (int i = 0; i < n; ++i) {
                const GLFWvidmode* vm = glfwGetVideoMode(mons[i]);
                if (!vm) continue;
                int mx, my;
                glfwGetMonitorPos(mons[i], &mx, &my);
                int ox = std::max(0, std::min(wx + ww, mx + vm->width) - std::max(wx, mx));
                int oy = std::max(0, std::min(wy + wh, my + vm->height) - std::max(wy, my));
                if (ox * oy > best && vm->refreshRate > 0) best = ox * oy, hz = vm->refreshRate;
            }
            return std::clamp(hz, 30, 500);
        };
#ifdef _WIN32
        timeBeginPeriod(1);  // sleeps accurate to ~1 ms (the default is ~15 ms)
#endif
        auto minFrame = std::chrono::duration<double>(1.0 / windowRefresh());
        auto lastFrame = std::chrono::steady_clock::now(), lastRefreshCheck = lastFrame;
        while (!glfwWindowShouldClose(window)) {
            // Sleep while idle. Animations (scrolling, hovers, toasts) and running jobs draw every frame
            // at the display's refresh rate.
            bool hidden = glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0;
            if (hidden) glfwWaitEventsTimeout(app.busy() ? 0.25 : 0.5);
            else if (settleFrames > 0 || kzgui::animating() || app.busy() || std::getenv("KZ_FPS_LOG")) glfwPollEvents();
            else glfwWaitEventsTimeout(0.5);
            auto now = std::chrono::steady_clock::now();
            if (now - lastRefreshCheck > std::chrono::milliseconds(500)) {
                minFrame = std::chrono::duration<double>(1.0 / windowRefresh());
                lastRefreshCheck = now;
            }
            auto since = now - lastFrame;
            if (hidden || since < minFrame * 0.5) {
                if (since < minFrame) std::this_thread::sleep_for(minFrame - since);
            }
            lastFrame = std::chrono::steady_clock::now();
            static const char* fpsLog = std::getenv("KZ_FPS_LOG");  // debug aid: frames per second to a file
            if (fpsLog) {
                static int frames = 0;
                static auto t0 = lastFrame;
                ++frames;
                if (lastFrame - t0 >= std::chrono::seconds(1)) {
                    if (FILE* f = std::fopen(fpsLog, "a")) {
                        std::fprintf(f, "%d fps (display %.0f Hz)\n", frames, 1.0 / minFrame.count());
                        std::fclose(f);
                    }
                    frames = 0;
                    t0 = lastFrame;
                }
            }
            renderFrame();
            app.afterFrame();
            if (unsettled()) settleFrames = 3;
            else if (settleFrames > 0) --settleFrames;
        }
#ifdef _WIN32
        timeEndPeriod(1);
#endif
        glfwSetWindowRefreshCallback(window, nullptr);
        kzgui::native::setRedraw(nullptr);
        g_render = nullptr;
        g_app = nullptr;
    }
    kzgui::native::shutdown();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
