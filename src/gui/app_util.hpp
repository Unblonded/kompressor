// Small helpers shared by app.cpp (logic) and app_draw.cpp (rendering).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "kz_formats.hpp"

namespace kzgui {

std::string lower(std::string s);
void copyTo(char* dst, size_t n, const std::string& s);
std::string parentDir(const std::string& p);
// Shows a file or folder in Finder / Explorer / the Linux file manager.
void reveal(const std::string& path);
int formatIndex(kz::OutFormat f);
// Deepest folder containing every input (e.g. C:/a/b for C:/a/b/x.txt and C:/a/b/c/y). Falls back to
// the first input's folder when they share nothing (different drives).
std::string commonParent(const std::vector<std::string>& inputs);
// If the output path still sits in `oldDir` (i.e. the user did not pick another folder), move it into
// the inputs' common folder, keeping the file name.
void retargetOutput(char* out, size_t n, const std::string& oldDir, const std::vector<std::string>& inputs);

}  // namespace kzgui
