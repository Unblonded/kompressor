// Kompressor GUI entry point: GLFW window + OpenGL 3 + Dear ImGui.
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#elif defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "app.hpp"
#include "kmp.hpp"
#include "kz_archive.hpp"
#include "theme.hpp"
#include "window_frame.hpp"

namespace {

kzgui::App* g_app = nullptr;

void onDrop(GLFWwindow*, int count, const char** paths) {
    if (!g_app) return;
    std::vector<std::string> v;
    for (int i = 0; i < count; ++i) v.emplace_back(paths[i]);  // GLFW hands out UTF-8 on every platform
    g_app->onDrop(v);
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

    GLFWwindow* window = glfwCreateWindow(1320, 840, "Kompressor", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetWindowSizeLimits(window, 760, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);
    kzgui::frame::install(window);  // custom title bar (Windows)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;  // layout is fixed; nothing worth persisting

    // HiDPI: macOS works in points with a 2x framebuffer; Windows/Linux work in pixels.
    // The scale follows the monitor the window is on (e.g. 100% on a 1080p screen, 175% on 4K):
    // fonts and sizes are rebuilt whenever the window moves to a monitor with a different scale.
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window, &xs, &ys);
    ImFont* mono = nullptr;
    float uiScale = applyUiScale(xs, &mono);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float x, float) { g_pendingScale = x; });

    {
        kzgui::App app(window, mono, uiScale);
        g_app = &app;
        glfwSetDropCallback(window, onDrop);
        if (!snapshotSelect.empty()) app.debugSelect(snapshotSelect);

        // Files passed on the command line / "Open with": archives open, anything else is queued to compress.
        std::vector<std::string> toCompress;
        for (size_t i = 1; i < args.size(); ++i) {
            kz::Source s;
            s.path = args[i];
            std::error_code ec;
            if (std::filesystem::is_regular_file(kz::u8path(args[i]), ec) &&
                (kz::looksLikeArchive(args[i]) || kz::kmp::isKmp(s)))
                app.openArchive(args[i]);
            else if (args[i].rfind("-psn_", 0) != 0)  // ignore old macOS process serial args
                toCompress.push_back(args[i]);
        }
        if (!toCompress.empty()) app.openCompressDialog(toCompress, false);

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
            glfwSwapBuffers(window);
            inFrame = false;
        };
        g_render = renderFrame;
        glfwSetWindowRefreshCallback(window, [](GLFWwindow*) {
            if (g_render) g_render();
        });

        while (!glfwWindowShouldClose(window)) {
            // Sleep while idle; tick quickly only while work is running or animating.
            if (app.busy()) glfwWaitEventsTimeout(1.0 / 30.0);
            else glfwWaitEventsTimeout(0.5);
            renderFrame();
        }
        glfwSetWindowRefreshCallback(window, nullptr);
        g_render = nullptr;
        g_app = nullptr;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
