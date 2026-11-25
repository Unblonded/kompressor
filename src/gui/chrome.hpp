// Window chrome and small text helpers shared by the app and the self-extractor.
#pragma once

#include <string>
#include <vector>

#include "theme.hpp"
#include "window_frame.hpp"

struct GLFWwindow;

namespace kzgui {

// Minimize / (maximize) / close at the right end of a custom title bar spanning [top, top + bh), ending
// at `right` (screen coordinates). Adds their rectangles to `hit`; returns the x where they begin.
float windowButtons(GLFWwindow* window, float right, float top, float bh, bool maximizable, std::vector<frame::Rect>& hit);
// `text` cut to fit `maxW` in the current font, with "..." at the end, or in the middle (paths).
std::string fitText(const std::string& text, float maxW, bool middle = false);
// Icon tile in the style of dialogHeader(): rounded gradient square with the icon in its centre.
void iconTile(Ico ic, const ImVec4& tint, float tile);

}  // namespace kzgui
