// Small per-user settings store: HKCU\Software\Kompressor on Windows, a key=value file elsewhere
// (~/Library/Application Support/Kompressor/settings on macOS, $XDG_CONFIG_HOME/kompressor/settings on Linux).
#pragma once

#include <string>

namespace kzgui::prefs {

bool getBool(const char* name, bool fallback);
void setBool(const char* name, bool value);

}  // namespace kzgui::prefs
