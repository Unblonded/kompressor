// Explorer right-click menu ("Kompressor >" cascade) and batching of multi-selection launches.
//
// Explorer starts one process per selected item, so every launch made from the menu goes through
// collectShellLaunch(): the first process of a burst gathers the paths from the others and runs the
// action once; the rest hand their paths over and exit.
#pragma once
#include <string>
#include <vector>

namespace kzgui::shell {

bool supported();   // Windows only for now
bool registered();  // menu entries exist and point at this executable
bool registerMenu(std::string& err);
bool unregisterMenu(std::string& err);

// First run: add the menu unless the user removed it before. Also repairs entries that point at an
// old location of the executable.
void autoRegister();

// `action` is one of: compress, super, open, extract-here, extract-to.
// Returns false when this process forwarded its paths to another one and should exit;
// otherwise `paths` holds every path of the burst.
bool collectShellLaunch(const std::string& action, std::vector<std::string>& paths);

}  // namespace kzgui::shell
