#include "native_files.hpp"

#include <GLFW/glfw3.h>

#include <atomic>
#include <cstring>
#include <thread>

#include "kz_common.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace kzgui::native {

#ifdef _WIN32

namespace {

bool g_ole = false;

// A DROPFILES block (CF_HDROP) listing `paths` as wide strings.
HGLOBAL makeHdrop(const std::vector<std::string>& paths) {
    std::wstring list;
    for (auto& p : paths) {
        list += kz::u8path(p).make_preferred().wstring();
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    std::memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
    GlobalUnlock(h);
    return h;
}

HGLOBAL makeDword(DWORD v) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!h) return nullptr;
    *static_cast<DWORD*>(GlobalLock(h)) = v;
    GlobalUnlock(h);
    return h;
}

bool setHdrop(IDataObject* obj, const std::vector<std::string>& paths) {
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm = {};
    sm.tymed = TYMED_HGLOBAL;
    sm.hGlobal = makeHdrop(paths);
    if (!sm.hGlobal) return false;
    if (FAILED(obj->SetData(&fe, &sm, TRUE))) {
        GlobalFree(sm.hGlobal);
        return false;
    }
    return true;
}

// Runs `work` on a worker thread while this (UI) thread keeps dispatching window messages, so the
// window repaints and Windows doesn't flag it as hung.
template <class F>
void runPumping(F&& work) {
    std::atomic<bool> done{false};
    std::thread t([&] {
        work();
        done = true;
    });
    while (!done) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    t.join();
}

bool ownWindowAt(POINT pt) {
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

class DropSource : public IDropSource {
public:
    DropSource(IDataObject* obj, const std::function<std::vector<std::string>()>& prepare) : obj_(obj), prepare_(prepare) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref_; }  // lives on the stack of dragFilesOut

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (keys & MK_LBUTTON) return S_OK;
        POINT pt;
        GetCursorPos(&pt);
        if (effect_ == DROPEFFECT_NONE || ownWindowAt(pt)) return DRAGDROP_S_CANCEL;
        // Released over a target: create the files now, then let the drop go ahead.
        std::vector<std::string> real;
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        runPumping([&] { real = prepare_(); });
        if (real.empty() || !setHdrop(obj_, real)) return DRAGDROP_S_CANCEL;
        return DRAGDROP_S_DROP;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        effect_ = effect;
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    IDataObject* obj_;
    const std::function<std::vector<std::string>()>& prepare_;
    ULONG ref_ = 1;
    DWORD effect_ = DROPEFFECT_NONE;
};

}  // namespace

void init() { g_ole = SUCCEEDED(OleInitialize(nullptr)); }

void shutdown() {
    if (g_ole) OleUninitialize();
    g_ole = false;
}

void openWithDefaultApp(const std::string& path) {
    std::wstring w = kz::u8path(path).make_preferred().wstring();
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool fileTransferSupported() { return true; }

std::vector<std::string> clipboardFiles() {
    std::vector<std::string> out;
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(nullptr)) return out;
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP d = static_cast<HDROP>(h);
        UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT len = DragQueryFileW(d, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(d, i, s.data(), len + 1);
            out.push_back(kz::pathToUtf8(std::filesystem::path(s)));
        }
    }
    CloseClipboard();
    return out;
}

bool setClipboardFiles(GLFWwindow* window, const std::vector<std::string>& paths) {
    if (paths.empty() || !OpenClipboard(glfwGetWin32Window(window))) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = makeHdrop(paths)) {
        ok = SetClipboardData(CF_HDROP, h) != nullptr;
        if (!ok) GlobalFree(h);
    }
    // "Preferred DropEffect" = copy, so Explorer pastes a copy (not a move of our temp files).
    if (HGLOBAL e = makeDword(DROPEFFECT_COPY)) {
        if (!SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), e)) GlobalFree(e);
    }
    CloseClipboard();
    return ok;
}

bool dragFilesOut(GLFWwindow* window, const std::vector<std::string>& paths,
                  const std::function<std::vector<std::string>()>& prepare) {
    if (!g_ole || paths.empty()) return false;
    IDataObject* obj = nullptr;
    if (FAILED(SHCreateDataObject(nullptr, 0, nullptr, nullptr, IID_PPV_ARGS(&obj)))) return false;
    if (!setHdrop(obj, paths)) {
        obj->Release();
        return false;
    }
    HWND hwnd = glfwGetWin32Window(window);
    DragAcceptFiles(hwnd, FALSE);  // our own window is not a target while we are the source
    DropSource src(obj, prepare);
    DWORD effect = DROPEFFECT_NONE;
    HRESULT hr = DoDragDrop(obj, &src, DROPEFFECT_COPY, &effect);
    DragAcceptFiles(hwnd, TRUE);
    // The drag loop ate the button release. GLFW captured the mouse on the press and only lets go on a
    // release, so without one every mouse move would keep going to this window (another window under
    // the cursor would miss its first click). Hand it the release it missed.
    ReleaseCapture();
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    PostMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM((WORD)(short)pt.x, (WORD)(short)pt.y));
    obj->Release();
    return hr == DRAGDROP_S_DROP;
}

#else  // ------------------------------------------------------------------------------------------

void init() {}
void shutdown() {}

void openWithDefaultApp(const std::string& path) {
#ifdef __APPLE__
    const char* argv[] = {"open", path.c_str(), nullptr};
#else
    const char* argv[] = {"xdg-open", path.c_str(), nullptr};
#endif
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, const_cast<char**>(argv), environ) == 0)
        std::thread([pid] {
            int st;
            waitpid(pid, &st, 0);
        }).detach();
}

bool fileTransferSupported() { return false; }
std::vector<std::string> clipboardFiles() { return {}; }
bool setClipboardFiles(GLFWwindow*, const std::vector<std::string>&) { return false; }
bool dragFilesOut(GLFWwindow*, const std::vector<std::string>&, const std::function<std::vector<std::string>()>&) {
    return false;
}

#endif

}  // namespace kzgui::native
