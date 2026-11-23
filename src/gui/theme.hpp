// Kompressor visual language: palette, fonts and custom-drawn widgets on top of Dear ImGui.
#pragma once

#include <imgui.h>

namespace kzgui {

namespace col {
// Surfaces (dark, faintly blue-tinted neutrals), from the window background upwards.
inline const ImVec4 Bg{0.055f, 0.059f, 0.075f, 1.00f};
inline const ImVec4 Surface1{0.078f, 0.082f, 0.106f, 1.00f};  // panels, cards
inline const ImVec4 Surface2{0.106f, 0.110f, 0.141f, 1.00f};  // inputs, raised items
inline const ImVec4 Surface3{0.141f, 0.145f, 0.188f, 1.00f};  // hover
inline const ImVec4 Line{0.157f, 0.165f, 0.212f, 1.00f};      // hairlines
inline const ImVec4 Text{0.925f, 0.933f, 0.961f, 1.00f};
inline const ImVec4 Dim{0.553f, 0.573f, 0.651f, 1.00f};
inline const ImVec4 Faint{0.365f, 0.380f, 0.447f, 1.00f};
// Brand: violet -> indigo -> cyan.
inline const ImVec4 Accent{0.549f, 0.427f, 1.000f, 1.00f};
inline const ImVec4 AccentHover{0.639f, 0.537f, 1.000f, 1.00f};
inline const ImVec4 AccentActive{0.455f, 0.337f, 0.937f, 1.00f};
inline const ImVec4 Accent2{0.243f, 0.729f, 0.996f, 1.00f};  // gradient end (sky)
inline const ImVec4 Teal{0.180f, 0.851f, 0.761f, 1.00f};
inline const ImVec4 Ok{0.243f, 0.851f, 0.537f, 1.00f};
inline const ImVec4 Warn{1.000f, 0.741f, 0.290f, 1.00f};
inline const ImVec4 Error{1.000f, 0.420f, 0.443f, 1.00f};
// File kinds.
inline const ImVec4 FolderCol{0.996f, 0.788f, 0.369f, 1.00f};
inline const ImVec4 ArchiveCol{1.000f, 0.553f, 0.341f, 1.00f};
inline const ImVec4 ImageCol{0.329f, 0.855f, 0.580f, 1.00f};
inline const ImVec4 CodeCol{0.416f, 0.690f, 1.000f, 1.00f};
inline const ImVec4 ExecCol{0.957f, 0.447f, 0.714f, 1.00f};
inline const ImVec4 FileCol{0.710f, 0.737f, 0.820f, 1.00f};
}  // namespace col

struct Fonts {
    ImFont* body = nullptr;   // UI text
    ImFont* bold = nullptr;   // emphasis, buttons, headings
    ImFont* caption = nullptr;  // small text: captions, chips, table headers
    ImFont* h2 = nullptr;     // dialog / section titles
    ImFont* h1 = nullptr;     // hero title, big numbers
    ImFont* mono = nullptr;   // preview
};
Fonts& fonts();

void applyTheme(float scale);

// ------------------------------------------------------------------ drawing helpers
ImU32 u32(const ImVec4& c, float alpha = 1.0f);
ImVec4 mix(const ImVec4& a, const ImVec4& b, float t);
// Rounded rectangle with a linear gradient (vertical by default).
void gradientRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 c0, ImU32 c1, float rounding, bool horizontal = false);
// Soft drop shadow drawn as rings outside the rectangle (safe to draw after a window background).
void softShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float size, float alpha = 0.35f);
// Smoothly animated 0..1 value keyed by id (hover/press transitions).
float anim(ImGuiID id, bool on, float speed = 14.0f);
// True while any animation is still moving: the app keeps rendering until it settles.
bool animating();
void beginFrameUi();

// ------------------------------------------------------------------ icons
enum class Ico {
    Folder, FolderOpen, File, Archive, Image, Code, Exec, Text, Link,
    Open, Box, Bolt, Extract, Shield, Sparkle, Search, Close, Up, Chevron, Lock, Check, Warn, Info, Error,
    Plus, Trash, Eye, Panel, Logo, Drop, Clock, Layers, Gauge, Hash, Gear,
};
// Draws a vector icon centred in the square at p (top-left) of side s.
void drawIco(ImDrawList* dl, Ico ic, ImVec2 p, float s, ImU32 c);
// Draws an icon at the cursor and advances layout by s x s.
void icon(Ico ic, float s, const ImVec4& c);
void spinner(float radius, const ImVec4& c);

// ------------------------------------------------------------------ widgets
// Gradient call-to-action button. Height 0 = default control height.
bool primaryButton(const char* label, Ico ic, ImVec2 size = ImVec2(0, 0), bool hasIcon = true);
inline bool accentButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    return primaryButton(label, Ico::Check, size, false);
}
// Quiet button: transparent until hovered, optional icon.
bool ghostButton(const char* label, Ico ic, ImVec2 size = ImVec2(0, 0), bool hasIcon = true, bool enabled = true);
// Bordered secondary button.
bool secondaryButton(const char* label, ImVec2 size = ImVec2(0, 0), bool enabled = true);
// Square icon-only button with tooltip.
bool iconButton(const char* id, Ico ic, float size, const char* tip = nullptr, bool enabled = true,
                const ImVec4* tint = nullptr);
// Small rounded label.
void chip(const char* text, const ImVec4& c, bool filled = false);
// Selectable pill (format pickers). Returns true when clicked.
bool pill(const char* label, bool selected, float minWidth = 0.0f);
// Option card with a title and wrapped description. Returns true when clicked.
bool optionCard(const char* id, const char* title, const char* desc, bool selected, ImVec2 size, const char* badge = nullptr);
// Uppercase-ish small dim caption used above groups.
void caption(const char* text);
// Thin progress bar; frac < 0 animates as indeterminate.
void progressBar(float frac, ImVec2 size, const ImVec4& c);
// Donut ring showing frac (0..1).
void ring(ImVec2 centre, float radius, float thickness, float frac, const ImVec4& c);
// Text input with an icon drawn inside on the left.
bool searchInput(const char* id, const char* hint, char* buf, size_t n, float width);
// Key/value row for info panels.
void kvRow(const char* key, const char* value, float keyWidth);

// Thin track slider with a round knob. Returns true while the value changes.
bool slider(const char* id, int* v, int lo, int hi, float width);
// iOS-style switch with a label. Returns true when toggled.
bool toggle(const char* label, bool* v);
// Compact segmented control. Returns true when the value changed.
bool segmented(const char* id, int* v, const char* const* items, int count);
// The gradient app mark.
void drawLogo(ImDrawList* dl, ImVec2 p, float size);
// Header used by dialogs and modals: icon tile, title, subtitle and an optional close button.
void dialogHeader(const char* title, const char* subtitle, Ico ic, const ImVec4& tint, float width, bool* open);

// Floating dialog with a custom header (icon tile, title, subtitle, close button).
bool beginDialog(const char* id, const char* title, const char* subtitle, Ico ic, bool* open, float width,
                 const ImVec4& tint = col::Accent);
void endDialog();
// Moves the cursor so `width` of content ends at the right edge (footer buttons).
void alignRight(float width);

}  // namespace kzgui
