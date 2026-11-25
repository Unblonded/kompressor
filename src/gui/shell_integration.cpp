#include "shell_integration.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "kz_common.hpp"
#include "prefs.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>

#include "../shellext/komp_shell_common.hpp"
#else
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
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
         addItem(kFilesMenu, L"3hash", L"Checksums", L"hash", exe, true) &&
         addItem(kArchiveMenu, L"1open", L"Open", L"open", exe) &&
         addItem(kArchiveMenu, L"2here", L"Extract here", L"extract-here", exe) &&
         addItem(kArchiveMenu, L"3to", L"Extract to folder", L"extract-to", exe) &&
         addItem(kArchiveMenu, L"4compress", L"Compress...", L"compress", exe, true) &&
         addItem(kArchiveMenu, L"5super", L"Super compress...", L"super", exe) &&
         addItem(kArchiveMenu, L"6hash", L"Checksums", L"hash", exe, true) &&
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

// macOS: Finder Quick Actions (Automator service workflows in ~/Library/Services).
// Linux: Nautilus scripts, a Dolphin service menu and Nemo actions. Each passes every selected path
// to one launch, so no batching is needed.
namespace {

namespace fs = std::filesystem;

struct Item {
    const char* action;  // the --shell action
    const char* label;
    bool archivesOnly;
};
const Item kItems[] = {
    {"open", "Open archive", true},
    {"extract-here", "Extract here", true},
    {"extract-to", "Extract to folder", true},
    {"test", "Test archive", true},
    {"compress", "Add to archive...", false},
    {"super", "Super compress...", false},
    {"add-zip", "Add to ZIP", false},
    {"add-7z", "Add to 7z", false},
    {"hash", "Checksums", false},
};

std::string selfExe() {
#ifdef __linux__
    if (const char* ai = std::getenv("APPIMAGE")) return ai;  // the AppImage, not its temporary mount
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).string();
#else
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return {};
    std::error_code ec;
    fs::path p = fs::weakly_canonical(buf, ec);
    return ec ? std::string(buf) : p.string();
#endif
}

fs::path home() {
    const char* h = std::getenv("HOME");
    return h ? fs::path(h) : fs::path();
}

// Text safe inside double quotes in sh and in .desktop Exec lines.
std::string shQuote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') o += '\\';
        o += c;
    }
    return o + "\"";
}

bool writeFile(const fs::path& p, const std::string& text, bool executable) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    if (executable)
        fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                               fs::perms::others_exec, ec);
    return true;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#ifdef __APPLE__
fs::path servicesDir() { return home() / "Library" / "Services"; }
fs::path workflowPath(const Item& it) { return servicesDir() / (std::string("Kompressor - ") + it.label + ".workflow"); }

std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

// A Quick Action: one "Run Shell Script" step that gets the selection as arguments.
bool writeWorkflow(const Item& it, const std::string& exe) {
    fs::path w = workflowPath(it);
    std::string menu = std::string("Kompressor: ") + it.label;
    std::string types = it.archivesOnly ? "<string>public.archive</string><string>public.zip-archive</string>"
                                          "<string>org.7-zip.7-zip-archive</string><string>com.rarlab.rar-archive</string>"
                                          "<string>public.disk-image</string><string>com.apple.installer-package-archive</string>"
                                          "<string>public.data</string>"
                                        : "<string>public.item</string>";
    std::string info =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>NSServices</key><array><dict>\n"
        "<key>NSMenuItem</key><dict><key>default</key><string>" + xmlEsc(menu) + "</string></dict>\n"
        "<key>NSMessage</key><string>runWorkflowAsService</string>\n"
        "<key>NSRequiredContext</key><dict><key>NSApplicationIdentifier</key><string>com.apple.finder</string></dict>\n"
        "<key>NSSendFileTypes</key><array>" + types + "</array>\n"
        "</dict></array>\n"
        "</dict></plist>\n";
    std::string command = "exec " + shQuote(exe) + " --shell " + it.action + " \"$@\"";
    std::string wflow =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>AMApplicationBuild</key><string>523</string>\n"
        "<key>AMApplicationVersion</key><string>2.10</string>\n"
        "<key>AMDocumentVersion</key><string>2</string>\n"
        "<key>actions</key><array><dict><key>action</key><dict>\n"
        "<key>AMAccepts</key><dict><key>Container</key><string>List</string><key>Optional</key><true/>"
        "<key>Types</key><array><string>com.apple.cocoa.string</string></array></dict>\n"
        "<key>AMActionVersion</key><string>2.0.3</string>\n"
        "<key>AMApplication</key><array><string>Automator</string></array>\n"
        "<key>AMParameterProperties</key><dict><key>COMMAND_STRING</key><dict/><key>CheckedForUserDefaultShell</key><dict/>"
        "<key>inputMethod</key><dict/><key>shell</key><dict/><key>source</key><dict/></dict>\n"
        "<key>AMProvides</key><dict><key>Container</key><string>List</string>"
        "<key>Types</key><array><string>com.apple.cocoa.string</string></array></dict>\n"
        "<key>ActionBundlePath</key><string>/System/Library/Automator/Run Shell Script.action</string>\n"
        "<key>ActionName</key><string>Run Shell Script</string>\n"
        "<key>ActionParameters</key><dict>\n"
        "<key>COMMAND_STRING</key><string>" + xmlEsc(command) + "</string>\n"
        "<key>CheckedForUserDefaultShell</key><true/>\n"
        "<key>inputMethod</key><integer>1</integer>\n"
        "<key>shell</key><string>/bin/sh</string>\n"
        "<key>source</key><string></string>\n"
        "</dict>\n"
        "<key>BundleIdentifier</key><string>com.apple.RunShellScript</string>\n"
        "<key>CFBundleVersion</key><string>2.0.3</string>\n"
        "<key>CanShowSelectedItemsWhenRun</key><false/>\n"
        "<key>CanShowWhenRun</key><true/>\n"
        "<key>Category</key><array><string>AMCategoryUtilities</string></array>\n"
        "<key>Class Name</key><string>RunShellScriptAction</string>\n"
        "<key>InputUUID</key><string>5C2B1A7E-0D4E-4B4C-9E39-2C1F7A9D0001</string>\n"
        "<key>Keywords</key><array><string>Shell</string><string>Script</string></array>\n"
        "<key>OutputUUID</key><string>5C2B1A7E-0D4E-4B4C-9E39-2C1F7A9D0002</string>\n"
        "<key>UUID</key><string>5C2B1A7E-0D4E-4B4C-9E39-2C1F7A9D0003</string>\n"
        "<key>UnlocalizedApplications</key><array><string>Automator</string></array>\n"
        "<key>arguments</key><dict/>\n"
        "<key>isViewVisible</key><integer>1</integer>\n"
        "</dict><key>isViewVisible</key><integer>1</integer></dict></array>\n"
        "<key>connectors</key><dict/>\n"
        "<key>workflowMetaData</key><dict>\n"
        "<key>applicationBundleIDsByPath</key><dict/>\n"
        "<key>applicationPaths</key><array/>\n"
        "<key>inputTypeIdentifier</key><string>com.apple.Automator.fileSystemObject</string>\n"
        "<key>outputTypeIdentifier</key><string>com.apple.Automator.nothing</string>\n"
        "<key>presentationMode</key><integer>15</integer>\n"
        "<key>processesInput</key><false/>\n"
        "<key>serviceInputTypeIdentifier</key><string>com.apple.Automator.fileSystemObject</string>\n"
        "<key>serviceOutputTypeIdentifier</key><string>com.apple.Automator.nothing</string>\n"
        "<key>serviceProcessesInput</key><false/>\n"
        "<key>systemImageName</key><string>NSActionTemplate</string>\n"
        "<key>useAutomaticInputType</key><false/>\n"
        "<key>workflowTypeIdentifier</key><string>com.apple.Automator.servicesMenu</string>\n"
        "</dict>\n"
        "</dict></plist>\n";
    return writeFile(w / "Contents" / "Info.plist", info, false) && writeFile(w / "Contents" / "document.wflow", wflow, false);
}

void refreshServices() {
    if (fork() == 0) {
        execl("/System/Library/CoreServices/pbs", "pbs", "-update", (char*)nullptr);
        _exit(0);
    }
}
#else
fs::path dataHome() {
    const char* x = std::getenv("XDG_DATA_HOME");
    return x && *x ? fs::path(x) : home() / ".local" / "share";
}
fs::path nautilusDir() { return dataHome() / "nautilus" / "scripts" / "Kompressor"; }
fs::path nemoDir() { return dataHome() / "nemo" / "actions"; }
std::vector<fs::path> dolphinFiles() {  // KDE 6, then KDE 5
    return {dataHome() / "kio" / "servicemenus" / "kompressor.desktop",
            dataHome() / "kservices5" / "ServiceMenus" / "kompressor.desktop"};
}
const char* kArchiveMimes =
    "application/zip;application/x-7z-compressed;application/vnd.rar;application/x-rar;application/x-tar;"
    "application/x-compressed-tar;application/x-bzip-compressed-tar;application/x-xz-compressed-tar;"
    "application/x-zstd-compressed-tar;application/gzip;application/x-gzip;application/x-bzip2;application/x-xz;"
    "application/zstd;application/x-lzma;application/x-lz4;application/x-iso9660-image;application/vnd.ms-cab-compressed;"
    "application/x-rpm;application/vnd.debian.binary-package;application/x-cpio;application/java-archive;"
    "application/vnd.android.package-archive;application/x-lha;application/x-xar;application/x-apple-diskimage;";

std::string dolphinMenu(const std::string& exe, bool archives) {
    std::string actions, body;
    for (const Item& it : kItems) {
        if (it.archivesOnly != archives) continue;
        std::string id = it.action;
        id.erase(std::remove(id.begin(), id.end(), '-'), id.end());
        actions += id + ";";
        body += "\n[Desktop Action " + id + "]\nName=" + it.label + "\nIcon=package-x-generic\nExec=" + shQuote(exe) +
                " --shell " + it.action + " %F\n";
    }
    return std::string("[Desktop Entry]\nType=Service\nX-KDE-ServiceTypes=KonqPopupMenu/Plugin\n") +
           "MimeType=" + (archives ? kArchiveMimes : "all/allfiles;inode/directory;") + "\n" +
           "X-KDE-Submenu=Kompressor\nX-KDE-Priority=TopLevel\nActions=" + actions + "\n" + body;
}
#endif

const char* kMarker = "# Kompressor right-click menu";

}  // namespace

bool supported() { return true; }

bool registered() {
    std::string exe = selfExe();
#ifdef __APPLE__
    std::string w = readFile(workflowPath(kItems[0]) / "Contents" / "document.wflow");
    return !w.empty() && w.find(xmlEsc(shQuote(exe))) != std::string::npos;
#else
    std::string sc = readFile(nautilusDir() / kItems[0].label);
    std::string d = readFile(dolphinFiles()[0]);
    return sc.find(shQuote(exe)) != std::string::npos && d.find(shQuote(exe)) != std::string::npos;
#endif
}

bool unregisterMenu(std::string&) {
    std::error_code ec;
#ifdef __APPLE__
    for (const Item& it : kItems) fs::remove_all(workflowPath(it), ec);
    refreshServices();
#else
    fs::remove_all(nautilusDir(), ec);
    for (auto& f : dolphinFiles()) {
        fs::remove(f, ec);
        fs::remove(f.parent_path() / "kompressor-archives.desktop", ec);
    }
    for (const Item& it : kItems) fs::remove(nemoDir() / (std::string("kompressor-") + it.action + ".nemo_action"), ec);
#endif
    prefs::setBool("ShellMenuOff", true);
    return true;
}

bool registerMenu(std::string& err) {
    std::string exe = selfExe();
    if (exe.empty()) return err = "could not find Kompressor's own location", false;
    std::string ignored;
    unregisterMenu(ignored);
    bool ok = true;
#ifdef __APPLE__
    for (const Item& it : kItems) ok = ok && writeWorkflow(it, exe);
    refreshServices();
#else
    // Nautilus (GNOME Files): Scripts > Kompressor > ...
    for (const Item& it : kItems)
        ok = ok && writeFile(nautilusDir() / it.label,
                             std::string("#!/bin/sh\n") + kMarker + "\nexec " + shQuote(exe) + " --shell " + it.action + " \"$@\"\n", true);
    // Dolphin (KDE): a Kompressor submenu; archives get a second file with their own entries.
    for (auto& f : dolphinFiles()) {
        ok = ok && writeFile(f, dolphinMenu(exe, false), true);
        ok = ok && writeFile(f.parent_path() / "kompressor-archives.desktop", dolphinMenu(exe, true), true);
    }
    // Nemo (Cinnamon)
    for (const Item& it : kItems)
        ok = ok && writeFile(nemoDir() / (std::string("kompressor-") + it.action + ".nemo_action"),
                             std::string("[Nemo Action]\nName=Kompressor: ") + it.label + "\nExec=" + shQuote(exe) + " --shell " +
                                 it.action + " %F\nIcon-Name=package-x-generic\nSelection=notnone\n" +
                                 (it.archivesOnly ? std::string("Mimetypes=") + kArchiveMimes + "\n" : "Extensions=any;\n"),
                             false);
#endif
    if (!ok) {
        unregisterMenu(ignored);
        return err = "could not write the menu entries", false;
    }
    prefs::setBool("ShellMenuOff", false);
    return true;
}

void autoRegister() {
    if (prefs::getBool("ShellMenuOff", false) || registered()) return;
    std::string err;
    registerMenu(err);
}

bool collectShellLaunch(const std::string&, std::vector<std::string>&) { return true; }

#endif

}  // namespace kzgui::shell
