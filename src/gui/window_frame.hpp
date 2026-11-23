// Custom window chrome. On Windows the native caption is removed (the frame, shadow, snapping and
// edge resizing stay native) and the app's top row acts as the title bar. Elsewhere this is a no-op
// and the system title bar is kept.
#pragma once

#include <vector>

struct GLFWwindow;

namespace kzgui::frame {

struct Rect {
    float x0, y0, x1, y1;  // client-area pixels
};

void install(GLFWwindow* window);
// True when the app must draw its own minimize / maximize / close buttons.
bool custom();
bool maximized();
void minimize();
void toggleMaximize();
// Called every frame: the title bar spans [0, height) vertically; points inside `interactive`
// (menus, buttons) are normal client clicks, everything else in the bar drags the window.
void setTitleBar(float height, const std::vector<Rect>& interactive);

}  // namespace kzgui::frame
