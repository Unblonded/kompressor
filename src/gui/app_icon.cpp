#include "app_icon.hpp"

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

#include <lzma.h>

#include <cstdint>
#include <vector>

#include "app_icon_data.inc"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace kzgui {
namespace {

constexpr int kCount = (int)(sizeof(kIconSizes) / sizeof(kIconSizes[0]));

struct Images {
    bool ok = false;
    std::vector<unsigned char> raw;
    const unsigned char* px[kCount] = {};
};

const Images& images() {
    static Images im = [] {
        Images r;
        r.raw.resize(kIconRawSize);
        uint64_t memlimit = UINT64_MAX;
        size_t inPos = 0, outPos = 0;
        if (lzma_stream_buffer_decode(&memlimit, 0, nullptr, kIconXz, &inPos, sizeof(kIconXz), r.raw.data(), &outPos,
                                      r.raw.size()) != LZMA_OK ||
            outPos != r.raw.size())
            return r;
        size_t off = 0;
        for (int i = 0; i < kCount; ++i) {
            r.px[i] = r.raw.data() + off;
            off += (size_t)kIconSizes[i] * kIconSizes[i] * 4;
        }
        r.ok = true;
        return r;
    }();
    return im;
}

}  // namespace

void setWindowIcon(GLFWwindow* window) {
#if !defined(__APPLE__)  // macOS takes the icon from the bundle
    const Images& im = images();
    if (!im.ok) return;
    GLFWimage list[kCount];
    for (int i = 0; i < kCount; ++i) list[i] = {kIconSizes[i], kIconSizes[i], const_cast<unsigned char*>(im.px[i])};
    glfwSetWindowIcon(window, kCount, list);
#else
    (void)window;
#endif
}

ImTextureID appIconTexture(float px) {
    static GLuint tex[kCount] = {};
    const Images& im = images();
    if (!im.ok) return (ImTextureID)0;
    int i = 0;
    while (i < kCount - 1 && kIconSizes[i] < px) ++i;
    if (!tex[i]) {
        GLint prev = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &tex[i]);
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kIconSizes[i], kIconSizes[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, im.px[i]);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    return (ImTextureID)(intptr_t)tex[i];
}

}  // namespace kzgui
