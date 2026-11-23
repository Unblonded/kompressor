// Small helpers shared by app.cpp (logic) and app_draw.cpp (rendering).
#pragma once

#include <cstddef>
#include <string>

#include "kz_formats.hpp"

namespace kzgui {

std::string lower(std::string s);
void copyTo(char* dst, size_t n, const std::string& s);
std::string parentDir(const std::string& p);
// Shows a file or folder in Finder / Explorer / the Linux file manager.
void reveal(const std::string& path);
int formatIndex(kz::OutFormat f);

}  // namespace kzgui
