#include "shell_integration.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "kz_common.hpp"
#include "../shellext/komp_shell_common.hpp"
#endif

namespace kzgui::shell {

#ifdef _WIN32
namespace {

// Everything lives under HKCU, so no admin rights are needed.
const wchar_t* kClasses = L"Software\\Classes\\";
const wchar_t* kPrefsKey = L"Software\\Kompressor";
const wchar_t* kVerb = L"Kompressor";
const wchar_t* kMenuName = kompshell::kMenuName;  // what the cascade is called in the menu
const wchar_t* kFilesMenu = L"Kompressor.Menu.Files";
const wchar_t* kArchiveMenu = L"Kompressor.Menu.Archive";
const auto& kArchiveExts = kompshell::kArchiveExts;

// The menu extension (KompShell.dll) and where it is registered.
const std::wstring kClsid = KOMP_SHELL_CLSID_STR;
const std::wstring kClsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + KOMP_SHELL_CLSID_STR;
const wchar_t* kHandlerClasses[] = {L"*", L"Directory"};
const wchar_t* kApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

std::wstring exePath() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return std::wstring(buf, n);
}

std::wstring handlerKey(const wchar_t* cls) { return std::wstring(kClasses) + cls + L"\\shellex\\ContextMenuHandlers\\Kompressor"; }

// KompShell.dll shipped next to the exe.
std::filesystem::path bundledDll() { return std::filesystem::path(exePath()).parent_path() / L"KompShell.dll"; }

// Explorer keeps the DLL it loaded open until it exits, which would block rebuilding or upgrading the
// installed copy. So Explorer gets its own copy in %LOCALAPPDATA%\Kompressor\shell, named after the
// contents: a new version is a new file, and old ones are deleted once nothing has them loaded.
std::filesystem::path installedDll(const std::filesystem::path& src) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return {};
    uint64_t h = 1469598103934665603ull;
    char buf[65536];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        for (std::streamsize i = 0; i < in.gcount(); ++i) h = (h ^ (unsigned char)buf[i]) * 1099511628211ull;
        if (!in) break;
    }
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    wchar_t name[64];
    swprintf(name, 64, L"KompShell-%016llx.dll", (unsigned long long)h);
    return std::filesystem::path(local) / L"Kompressor" / L"shell" / name;
}

std::wstring widen(const std::string& s) { return kz::u8path(s).wstring(); }

bool setStr(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

std::wstring getStr(HKEY root, const std::wstring& key, const wchar_t* name) {
    wchar_t buf[4096];
    DWORD sz = sizeof buf;
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    return buf;
}

void deleteTree(const std::wstring& key) { RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); RegDeleteKeyW(HKEY_CURRENT_USER, key.c_str()); }

std::wstring command(const std::wstring& exe, const wchar_t* action) {
    return L"\"" + exe + L"\" --shell " + action + L" \"%1\"";
}

// One sub-entry of a cascade menu.
bool addItem(const std::wstring& menu, const wchar_t* id, const wchar_t* label, const wchar_t* action,
             const std::wstring& exe, bool separatorBefore = false) {
    std::wstring k = kClasses + menu + L"\\shell\\" + id;
    bool ok = setStr(HKEY_CURRENT_USER, k, L"MUIVerb", label) &&
              // "Player" lifts Explorer's 15-item limit; the per-item launches are batched by collectShellLaunch.
              setStr(HKEY_CURRENT_USER, k, L"MultiSelectModel", L"Player") &&
              setStr(HKEY_CURRENT_USER, k + L"\\command", nullptr, command(exe, action));
    if (ok && separatorBefore) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, k.c_str(), 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            DWORD v = 0x20;  // ECF_SEPARATORBEFORE
            RegSetValueExW(h, L"CommandFlags", 0, REG_DWORD, (const BYTE*)&v, sizeof v);
            RegCloseKey(h);
        }
    }
    return ok;
}

// The "Kompressor >" entry on a class, pointing at one of the shared sub-menus.
bool addCascade(const std::wstring& cls, const std::wstring& menu, const std::wstring& exe, const std::wstring& appliesTo = {}) {
    std::wstring k = kClasses + cls + L"\\shell\\" + kVerb;
    bool ok = setStr(HKEY_CURRENT_USER, k, L"MUIVerb", kMenuName) &&
              setStr(HKEY_CURRENT_USER, k, L"ExtendedSubCommandsKey", menu) &&
              setStr(HKEY_CURRENT_USER, k, L"MultiSelectModel", L"Player") &&
              setStr(HKEY_CURRENT_USER, k, L"Position", L"Bottom");
    if (ok && !appliesTo.empty()) ok = setStr(HKEY_CURRENT_USER, k, L"AppliesTo", appliesTo);
    return ok;
}

std::wstring archiveQuery(bool negate) {
    std::wstring q;
    for (const char* e : kArchiveExts) {
        if (!q.empty()) q += L" OR ";
        q += L"System.FileExtension:=." + widen(e);
    }
    return negate ? L"NOT (" + q + L")" : q;
}

void notifyShell() { SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); }

}  // namespace

bool supported() { return true; }

namespace {

bool staticVerbsRegistered() {
    std::wstring cmd = getStr(HKEY_CURRENT_USER, std::wstring(kClasses) + kFilesMenu + L"\\shell\\1compress\\command", nullptr);
    // Also require the current layout (name, no icon), so menus from older versions get replaced.
    std::wstring cascade = std::wstring(kClasses) + L"*\\shell\\" + kVerb;
    return !cmd.empty() && cmd == command(exePath(), L"compress") &&
           getStr(HKEY_CURRENT_USER, cascade, L"MUIVerb") == kMenuName && getStr(HKEY_CURRENT_USER, cascade, L"Icon").empty() &&
           getStr(HKEY_CURRENT_USER, cascade, L"Position") == L"Bottom";
}

void removeStaticVerbs() {
    deleteTree(std::wstring(kClasses) + L"*\\shell\\" + kVerb);
    deleteTree(std::wstring(kClasses) + L"*\\shell\\KompressorArchive");
    deleteTree(std::wstring(kClasses) + L"Directory\\shell\\" + kVerb);
    deleteTree(std::wstring(kClasses) + kFilesMenu);
    deleteTree(std::wstring(kClasses) + kArchiveMenu);
}

void removeExtension() {
    for (const wchar_t* cls : kHandlerClasses) deleteTree(handlerKey(cls));
    deleteTree(kClsidKey);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kClsid.c_str());
}

// Deletes older copies of the DLL that Explorer no longer has loaded (a loaded one can't be deleted).
void pruneOldDlls(const std::filesystem::path& keep) {
    std::error_code ec;
    for (auto& e : std::filesystem::directory_iterator(keep.parent_path(), ec)) {
        auto name = e.path().filename().wstring();
        if (e.path() != keep && name.rfind(L"KompShell-", 0) == 0) std::filesystem::remove(e.path(), ec);
    }
}

bool extensionRegistered() {
    auto src = bundledDll();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec)) return false;
    auto dll = installedDll(src);
    if (dll.empty() || !std::filesystem::is_regular_file(dll, ec)) return false;
    if (getStr(HKEY_CURRENT_USER, kClsidKey + L"\\InprocServer32", nullptr) != dll.wstring()) return false;
    if (getStr(HKEY_CURRENT_USER, kPrefsKey, L"ExePath") != exePath()) return false;
    for (const wchar_t* cls : kHandlerClasses)
        if (getStr(HKEY_CURRENT_USER, handlerKey(cls), nullptr) != kClsid) return false;
    // The old static menu must be gone too, or Komp would show up twice.
    return getStr(HKEY_CURRENT_USER, std::wstring(kClasses) + L"*\\shell\\" + kVerb, L"MUIVerb").empty();
}

bool registerExtension() {
    auto src = bundledDll();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec)) return false;
    auto dll = installedDll(src);
    if (dll.empty()) return false;
    std::filesystem::create_directories(dll.parent_path(), ec);
    if (!std::filesystem::is_regular_file(dll, ec) && !std::filesystem::copy_file(src, dll, ec)) return false;
    bool ok = setStr(HKEY_CURRENT_USER, kClsidKey, nullptr, L"Kompressor context menu") &&
              setStr(HKEY_CURRENT_USER, kClsidKey + L"\\InprocServer32", nullptr, dll.wstring()) &&
              setStr(HKEY_CURRENT_USER, kClsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment") &&
              setStr(HKEY_CURRENT_USER, kPrefsKey, L"ExePath", exePath());
    for (const wchar_t* cls : kHandlerClasses) ok = ok && setStr(HKEY_CURRENT_USER, handlerKey(cls), nullptr, kClsid);
    if (ok) setStr(HKEY_CURRENT_USER, kApprovedKey, kClsid.c_str(), L"Kompressor context menu");
    if (ok) pruneOldDlls(dll);
    return ok;
}

}  // namespace

bool registered() { return std::filesystem::exists(bundledDll()) ? extensionRegistered() : staticVerbsRegistered(); }

bool registerMenu(std::string& err) {
    std::string ignored;
    unregisterMenu(ignored);
    // The menu extension places Komp in the first group of the menu. Without the DLL (a bare exe copied
    // somewhere), fall back to static verbs, which Explorer can only put at the bottom.
    if (std::filesystem::exists(bundledDll())) {
        if (registerExtension()) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrefsKey, L"ShellMenuOff");
            notifyShell();
            return true;
        }
        removeExtension();
    }
    std::wstring exe = exePath();
    // Plain files and folders get the compress entries; archives (a second verb, since one class cannot
    // hold two cascades with the same name) get open / extract on top.
    bool ok = addItem(kFilesMenu, L"1compress", L"Compress...", L"compress", exe) &&
         addItem(kFilesMenu, L"2super", L"Super compress...", L"super", exe) &&
         addItem(kArchiveMenu, L"1open", L"Open", L"open", exe) &&
         addItem(kArchiveMenu, L"2here", L"Extract here", L"extract-here", exe) &&
         addItem(kArchiveMenu, L"3to", L"Extract to folder", L"extract-to", exe) &&
         addItem(kArchiveMenu, L"4compress", L"Compress...", L"compress", exe, true) &&
         addItem(kArchiveMenu, L"5super", L"Super compress...", L"super", exe) &&
         addCascade(L"*", kFilesMenu, exe, archiveQuery(true));
    if (ok) {
        std::wstring k = std::wstring(kClasses) + L"*\\shell\\KompressorArchive";
        ok = setStr(HKEY_CURRENT_USER, k, L"MUIVerb", kMenuName) &&
             setStr(HKEY_CURRENT_USER, k, L"ExtendedSubCommandsKey", kArchiveMenu) &&
             setStr(HKEY_CURRENT_USER, k, L"MultiSelectModel", L"Player") &&
             setStr(HKEY_CURRENT_USER, k, L"AppliesTo", archiveQuery(false)) &&
             setStr(HKEY_CURRENT_USER, k, L"Position", L"Bottom");
    }
    ok = ok && addCascade(L"Directory", kFilesMenu, exe);
    if (!ok) {
        unregisterMenu(ignored);
        err = "could not write the Explorer menu entries to the registry";
        return false;
    }
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kPrefsKey, L"ShellMenuOff");
    notifyShell();
    return true;
}

bool unregisterMenu(std::string& err) {
    (void)err;
    removeStaticVerbs();
    removeExtension();
    setStr(HKEY_CURRENT_USER, kPrefsKey, L"ShellMenuOff", L"1");
    notifyShell();
    return true;
}

void autoRegister() {
    if (getStr(HKEY_CURRENT_USER, kPrefsKey, L"ShellMenuOff") == L"1" || registered()) return;
    std::string err;
    registerMenu(err);
}

// ---------------------------------------------------------------------------------------------------
// Batching. The first process of a burst owns a named mutex and a message-only window; the others find
// that window and pass their paths with WM_COPYDATA. The owner keeps collecting until the burst goes
// quiet, then closes the window so later, unrelated menu clicks start their own batch.

namespace {
std::vector<std::string>* g_sink = nullptr;
bool g_got = false;

LRESULT CALLBACK sinkProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA && g_sink) {
        auto* cd = (COPYDATASTRUCT*)l;
        const char* p = (const char*)cd->lpData;
        size_t n = cd->cbData, start = 0;
        for (size_t i = 0; i <= n; ++i)
            if (i == n || p[i] == '\0') {
                if (i > start) g_sink->emplace_back(p + start, i - start);
                start = i + 1;
            }
        g_got = true;
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}
}  // namespace

bool collectShellLaunch(const std::string& action, std::vector<std::string>& paths) {
    std::wstring name = L"KompressorShell." + widen(action);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (L"Local\\" + name).c_str());
    bool other = GetLastError() == ERROR_ALREADY_EXISTS;

    if (other) {
        // Hand our paths to the process that got here first.
        std::string blob;
        for (auto& p : paths) blob += p + '\0';
        for (int tries = 0; tries < 40; ++tries) {
            if (HWND target = FindWindowExW(HWND_MESSAGE, nullptr, L"KompressorShellSink", name.c_str())) {
                COPYDATASTRUCT cd{1, (DWORD)blob.size(), blob.data()};
                DWORD_PTR res = 0;
                if (SendMessageTimeoutW(target, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &res) && res) {
                    if (mutex) CloseHandle(mutex);
                    return false;
                }
            }
            Sleep(50);
        }
        // The owner already finished collecting: run this batch on our own.
        if (mutex) CloseHandle(mutex);
        return true;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = sinkProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KompressorShellSink";
    RegisterClassW(&wc);
    HWND sink = CreateWindowExW(0, wc.lpszClassName, name.c_str(), 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    g_sink = &paths;
    using clock = std::chrono::steady_clock;
    auto start = clock::now(), last = start;
    // Wait until no new paths arrived for a moment (and at most a few seconds for huge selections).
    while (clock::now() - last < std::chrono::milliseconds(400) && clock::now() - start < std::chrono::seconds(5)) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        MSG msg;
        g_got = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_got) last = clock::now();
    }
    DestroyWindow(sink);
    g_sink = nullptr;
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return true;
}

#else  // !_WIN32

bool supported() { return false; }
bool registered() { return false; }
bool registerMenu(std::string& err) { err = "not supported on this platform"; return false; }
bool unregisterMenu(std::string&) { return true; }
void autoRegister() {}
bool collectShellLaunch(const std::string&, std::vector<std::string>&) { return true; }

#endif

}  // namespace kzgui::shell
