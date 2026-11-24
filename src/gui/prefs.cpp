#include "prefs.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#endif

namespace kzgui::prefs {

#ifdef _WIN32
namespace {
const wchar_t* kKey = L"Software\\Kompressor";
std::wstring wide(const char* s) { return std::wstring(s, s + strlen(s)); }  // names are ASCII
}  // namespace

bool getBool(const char* name, bool fallback) {
    wchar_t buf[16];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, wide(name).c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return fallback;
    return buf[0] == L'1';
}

void setBool(const char* name, bool value) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    const wchar_t* v = value ? L"1" : L"0";
    RegSetValueExW(h, wide(name).c_str(), 0, REG_SZ, (const BYTE*)v, 2 * sizeof(wchar_t));
    RegCloseKey(h);
}

#else
namespace {
namespace fs = std::filesystem;

fs::path file() {
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    return fs::path(home ? home : ".") / "Library/Application Support/Kompressor/settings";
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    fs::path base = xdg && *xdg ? fs::path(xdg) : fs::path(home ? home : ".") / ".config";
    return base / "kompressor/settings";
#endif
}

std::map<std::string, std::string> load() {
    std::map<std::string, std::string> m;
    std::ifstream in(file());
    for (std::string line; std::getline(in, line);) {
        size_t eq = line.find('=');
        if (eq != std::string::npos) m[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return m;
}
}  // namespace

bool getBool(const char* name, bool fallback) {
    auto m = load();
    auto it = m.find(name);
    return it == m.end() ? fallback : it->second == "1";
}

void setBool(const char* name, bool value) {
    auto m = load();
    m[name] = value ? "1" : "0";
    std::error_code ec;
    fs::create_directories(file().parent_path(), ec);
    std::ofstream out(file(), std::ios::trunc);
    for (auto& [k, v] : m) out << k << '=' << v << '\n';
}
#endif

}  // namespace kzgui::prefs
