// The Kompressor app icon, compiled into the binary (see tools/make_icon.py).
#pragma once
#include <imgui.h>

struct GLFWwindow;

namespace kzgui {

// Sets the title-bar / taskbar icon on platforms where GLFW supports it.
void setWindowIcon(GLFWwindow* window);

// GL texture of the icon closest to (but not smaller than) `px` pixels; 0 if unavailable.
// Needs a current GL context.
ImTextureID appIconTexture(float px);

}  // namespace kzgui
