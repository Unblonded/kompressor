// KompShell.dll: Kompressor's entry in Explorer's (classic) right-click menu.
//
// Static menu verbs in the registry can only go at the top or bottom of the menu, so the "Komp" submenu
// is added by this small context-menu handler instead, in the first group (where Explorer puts menu
// extensions, above "Open with"). The commands start Kompressor.exe
// with "--shell <action> <paths...>" (or "--shell-list <action> <file>" for very long selections).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#include <cstring>
#include <cwctype>
#include <new>
#include <string>
#include <vector>

#include "komp_shell_common.hpp"

namespace {

// {4FF8516A-3A6F-4DBB-8CA9-E67FD21BF5D7}
const CLSID kClsid = {0x4FF8516A, 0x3A6F, 0x4DBB, {0x8C, 0xA9, 0xE6, 0x7F, 0xD2, 0x1B, 0xF5, 0xD7}};

HINSTANCE g_inst = nullptr;
LONG g_objects = 0, g_locks = 0;

// The --shell actions Kompressor.exe understands (see main.cpp).
enum Cmd { COpen, CExtract, CHere, CTo, CTest, CCompress, CSuper, CAdd7z, CAddZip, CAddKmp };
const wchar_t* const kActions[] = {L"open", L"extract", L"extract-here", L"extract-to", L"test",
                                   L"compress", L"super", L"add-7z", L"add-zip", L"add-kmp"};
const wchar_t* const kHelp[] = {L"Browse the archive in Kompressor",
                                L"Choose where to extract the archive",
                                L"Extract next to the archive",
                                L"Extract into a folder named after the archive",
                                L"Check that every file in the archive decompresses correctly",
                                L"Choose a format and options, then compress",
                                L"Pack into Kompressor's .kmp format, with options",
                                L"Compress into a 7z archive next to the selection",
                                L"Compress into a ZIP archive next to the selection",
                                L"Super compress into a .kmp archive next to the selection"};

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::wstring fileName(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Name for "Extract to": the archive name without its archive extension. Same rule as kz::stripArchiveExt.
std::wstring extractStem(const std::wstring& path) {
    std::wstring f = fileName(path), l = lower(f);
    for (const wchar_t* c : {L".tar.gz", L".tar.bz2", L".tar.xz", L".tar.zst", L".tar.lz4", L".tar.lzma", L".tar.lz",
                             L".tar.z", L".tar.br"}) {
        size_t n = wcslen(c);
        if (l.size() > n && l.compare(l.size() - n, n, c) == 0) return f.substr(0, f.size() - n);
    }
    size_t d = f.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? f : f.substr(0, d);
}

// Name for the quick "Add to" entries. Same rule as the app's quickArchiveName(): one file -> its name
// without the extension, one folder -> its name, several items -> the name of the folder they're in.
std::wstring addStem(const std::vector<std::wstring>& paths) {
    std::wstring name;
    if (paths.size() == 1) {
        name = fileName(paths[0]);
        DWORD a = GetFileAttributesW(paths[0].c_str());
        bool dir = a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        size_t d = name.find_last_of(L'.');
        if (!dir && d != std::wstring::npos && d > 0) name.erase(d);
    } else {
        std::wstring p = paths[0];
        size_t s = p.find_last_of(L"\\/");
        if (s != std::wstring::npos) name = fileName(p.substr(0, s));
    }
    if (name.empty() || name.back() == L':' || name.find(L'\\') != std::wstring::npos) name = L"Archive";  // drive root
    return name;
}

// A name for display in a menu: '&' would otherwise underline the next letter, and long names are shortened.
std::wstring menuText(std::wstring s) {
    if (s.size() > 48) s = s.substr(0, 45) + L"...";
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'&') r += L'&';
        r += c;
    }
    return r;
}

bool isArchive(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
    std::wstring ext = lower(path.substr(dot + 1));
    for (const char* e : kompshell::kArchiveExts) {
        std::wstring w(e, e + strlen(e));
        if (w == ext) return true;
    }
    return false;
}

// Kompressor.exe: recorded by the app when it registers this handler, else next to this DLL.
std::wstring exePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Kompressor", L"ExePath", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    DWORD n = GetModuleFileNameW(g_inst, buf, MAX_PATH * 4);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L'\\') + 1) + L"Kompressor.exe";
}

// Quotes a path for a command line (a trailing backslash would otherwise escape the closing quote).
std::wstring quoted(const std::wstring& s) {
    std::wstring q = L"\"" + s;
    if (!s.empty() && s.back() == L'\\') q += L'\\';
    return q + L"\"";
}

std::string utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool launch(const wchar_t* action, const std::vector<std::wstring>& paths) {
    std::wstring exe = exePath();
    std::wstring cmd = quoted(exe) + L" --shell " + action;
    for (auto& p : paths) cmd += L" " + quoted(p);
    if (cmd.size() > 30000) {  // too long for a command line: hand the list over in a file
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring list = std::wstring(tmp) + L"kompressor-shell-" + std::to_wstring(GetTickCount64()) + L".txt";
        HANDLE f = CreateFileW(list.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        std::string body;
        for (auto& p : paths) body += utf8(p) + "\n";
        DWORD written = 0;
        WriteFile(f, body.data(), (DWORD)body.size(), &written, nullptr);
        CloseHandle(f);
        cmd = quoted(exe) + L" --shell-list " + action + L" " + quoted(list);
    }
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

class ContextMenu : public IShellExtInit, public IContextMenu {
public:
    ContextMenu() { InterlockedIncrement(&g_objects); }
    virtual ~ContextMenu() { InterlockedDecrement(&g_objects); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) *out = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu) *out = static_cast<IContextMenu*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // IShellExtInit: the selected files and folders.
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override {
        paths_.clear();
        if (!data) return E_INVALIDARG;  // folder background: nothing selected
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm;
        if (FAILED(data->GetData(&fe, &sm))) return E_INVALIDARG;
        if (HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal))) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring s(len, L'\0');
                DragQueryFileW(drop, i, s.data(), len + 1);
                paths_.push_back(s);
            }
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
        // Archives get Open / Extract on top; a selection with anything else only gets the compress entries.
        archives_ = !paths_.empty();
        for (auto& p : paths_) {
            DWORD a = GetFileAttributesW(p.c_str());
            if ((a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) || !isArchive(p)) archives_ = false;
        }
        return paths_.empty() ? E_INVALIDARG : S_OK;
    }

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override {
        if ((flags & CMF_DEFAULTONLY) || paths_.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        cmds_.clear();
        HMENU sub = CreatePopupMenu();
        UINT id = first;
        auto add = [&](Cmd c, const std::wstring& label) {
            if (id > last) return;
            AppendMenuW(sub, MF_STRING, id++, label.c_str());
            cmds_.push_back(c);
        };
        bool one = paths_.size() == 1;
        if (archives_) {
            add(COpen, L"Open archive");
            if (one) add(CExtract, L"Extract files...");
            add(CHere, L"Extract here");
            add(CTo, one ? L"Extract to \"" + menuText(extractStem(paths_[0])) + L"\\\"" : L"Extract to \"*\\\"");
            add(CTest, L"Test archive");
            AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        }
        std::wstring stem = menuText(addStem(paths_));
        add(CCompress, L"Add to archive...");
        add(CSuper, L"Super compress...");
        add(CAdd7z, L"Add to \"" + stem + L".7z\"");
        add(CAddZip, L"Add to \"" + stem + L".zip\"");
        add(CAddKmp, L"Add to \"" + stem + L".kmp\"");

        // Where Explorer asks: in the first group, after the other extensions' entries and above "Open with".
        int count = GetMenuItemCount(menu);
        UINT pos = index > (UINT)count ? (UINT)count : index;
        MENUITEMINFOW mi{sizeof mi};
        mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        mi.wID = id++;
        mi.hSubMenu = sub;
        mi.dwTypeData = const_cast<wchar_t*>(kompshell::kMenuName);
        InsertMenuItemW(menu, pos, TRUE, &mi);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, id - first);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ci) override {
        if (!ci || HIWORD(ci->lpVerb)) return E_FAIL;  // only offsets, no verb strings
        UINT off = LOWORD(ci->lpVerb);
        if (off >= cmds_.size()) return E_FAIL;
        return launch(kActions[cmds_[off]], paths_) ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR cmd, UINT type, UINT*, CHAR* name, UINT cch) override {
        if (cmd >= cmds_.size()) return E_INVALIDARG;
        switch (type) {
            case GCS_VALIDATEW:
            case GCS_VALIDATEA: return S_OK;
            case GCS_VERBW: return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, kActions[cmds_[cmd]]);
            case GCS_HELPTEXTW: return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, kHelp[cmds_[cmd]]);
            default: return E_NOTIMPL;
        }
    }

private:
    LONG ref_ = 1;
    std::vector<std::wstring> paths_;
    bool archives_ = false;
    std::vector<Cmd> cmds_;  // menu id offset -> command
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *out = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* m = new (std::nothrow) ContextMenu();
        if (!m) return E_OUTOFMEMORY;
        HRESULT hr = m->QueryInterface(riid, out);
        m->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_locks);
        else InterlockedDecrement(&g_locks);
        return S_OK;
    }
    Factory() { InterlockedIncrement(&g_objects); }
    virtual ~Factory() { InterlockedDecrement(&g_objects); }

private:
    LONG ref_ = 1;
};

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (clsid != kClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) Factory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, out);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_objects == 0 && g_locks == 0 ? S_OK : S_FALSE; }
