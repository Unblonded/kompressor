#pragma once

#include <imgui.h>

namespace kzgui {

namespace col {
// Brand palette (dark UI).
inline const ImVec4 Accent{0.51f, 0.40f, 1.00f, 1.00f};      // electric violet
inline const ImVec4 AccentHover{0.60f, 0.50f, 1.00f, 1.00f};
inline const ImVec4 AccentActive{0.42f, 0.31f, 0.92f, 1.00f};
inline const ImVec4 Teal{0.20f, 0.85f, 0.75f, 1.00f};
inline const ImVec4 Ok{0.35f, 0.85f, 0.45f, 1.00f};
inline const ImVec4 Warn{1.00f, 0.74f, 0.25f, 1.00f};
inline const ImVec4 Error{1.00f, 0.38f, 0.38f, 1.00f};
inline const ImVec4 Dim{0.58f, 0.60f, 0.68f, 1.00f};
inline const ImVec4 FolderCol{0.98f, 0.78f, 0.33f, 1.00f};
inline const ImVec4 ArchiveCol{1.00f, 0.55f, 0.30f, 1.00f};
inline const ImVec4 ImageCol{0.35f, 0.85f, 0.55f, 1.00f};
inline const ImVec4 CodeCol{0.40f, 0.70f, 1.00f, 1.00f};
inline const ImVec4 ExecCol{0.95f, 0.45f, 0.65f, 1.00f};
inline const ImVec4 FileCol{0.75f, 0.78f, 0.86f, 1.00f};
}  // namespace col

void applyTheme(float scale);

// Big accent-coloured button. Returns true when clicked.
bool accentButton(const char* label, const ImVec2& size = ImVec2(0, 0));

}  // namespace kzgui
