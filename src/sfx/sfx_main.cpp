// Kompressor self-extractor: the small program at the front of a self-extracting .exe.
//
// The file is laid out as [this program][name][an archive][trailer]. The trailer (the last 32 bytes)
// says where the archive starts, so the program reads its own file, asks where to put the files and
// extracts them: KMP with Kompressor's own reader, everything else (ZIP, 7z, RAR, TAR and its
// compressed kinds, ISO, CPIO, and single compressed files) with libarchive. `name` is only there for
// a single compressed file (.gz, .xz ...), which doesn't record what the file was called.
//
//   name.exe               asks where to extract (default: a folder named after the .exe beside it)
//   name.exe -y            extracts there without asking
//   name.exe -o<folder>    extracts to <folder> without asking ("-o <folder>" works too)
//   name.exe -p<password>  for an encrypted ZIP
//
// Its window is drawn with Dear ImGui in Kompressor's own look (src/gui/theme.cpp and friends).
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "kmp.hpp"
#include "app_icon.hpp"
#include "chrome.hpp"
#include "native_files.hpp"
#include "theme.hpp"
#include "window_frame.hpp"

namespace {

constexpr char kMagic[8] = {'K', 'Z', 'S', 'F', 'X', 0, 1, 0};
constexpr uint32_t kFlagEncrypted = 1;

struct Trailer {
    uint64_t offset = 0, size = 0;
    uint32_t flags = 0;
    uint32_t nameLen = 0;  // a single compressed file's name, stored just before the archive
};

std::wstring g_self, g_title;
std::wstring g_inner;  // that name (UTF-16), empty for real archives
Trailer g_tr;

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring fromUtf8(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

bool readTrailer(const std::wstring& path, Trailer& t) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{}, at{};
    uint8_t b[32];
    DWORD got = 0;
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 32;
    if (ok) {
        at.QuadPart = size.QuadPart - 32;
        ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, b, 32, &got, nullptr) && got == 32 &&
             std::memcmp(b, kMagic, 8) == 0;
    }
    if (ok) {
        std::memcpy(&t.offset, b + 8, 8);
        std::memcpy(&t.size, b + 16, 8);
        std::memcpy(&t.flags, b + 24, 4);
        std::memcpy(&t.nameLen, b + 28, 4);
        ok = t.offset + t.size + 32 == (uint64_t)size.QuadPart && t.nameLen <= 4096 && t.nameLen <= t.offset;
    }
    if (ok && t.nameLen) {
        std::string name(t.nameLen, '\0');
        at.QuadPart = (LONGLONG)(t.offset - t.nameLen);
        ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, name.data(), t.nameLen, &got, nullptr) && got == t.nameLen;
        if (ok) g_inner = fromUtf8(name.c_str());
    }
    CloseHandle(h);
    return ok;
}

// libarchive reads the archive straight out of this .exe, through these callbacks.
struct Region {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t base = 0, size = 0, pos = 0;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1 << 20);
    std::atomic<uint64_t>* done = nullptr;
    std::atomic<bool>* cancel = nullptr;
};

la_ssize_t regionRead(archive* a, void* p, const void** out) {
    Region* r = (Region*)p;
    if (r->cancel->load()) {
        archive_set_error(a, ECANCELED, "Cancelled");
        return -1;
    }
    uint64_t left = r->size - r->pos;
    DWORD want = (DWORD)std::min<uint64_t>(left, r->buf.size()), got = 0;
    if (!want) return 0;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)(r->base + r->pos);
    if (!SetFilePointerEx(r->h, at, nullptr, FILE_BEGIN) || !ReadFile(r->h, r->buf.data(), want, &got, nullptr)) {
        archive_set_error(a, EIO, "Can't read the archive from this file");
        return -1;
    }
    r->pos += got;
    r->done->store(r->pos);
    *out = r->buf.data();
    return got;
}
la_int64_t regionSeek(archive*, void* p, la_int64_t off, int whence) {
    Region* r = (Region*)p;
    int64_t to = whence == SEEK_SET ? off : whence == SEEK_CUR ? (int64_t)r->pos + off : (int64_t)r->size + off;
    if (to < 0 || (uint64_t)to > r->size) return ARCHIVE_FATAL;
    r->pos = (uint64_t)to;
    return to;
}
la_int64_t regionSkip(archive*, void* p, la_int64_t n) {
    Region* r = (Region*)p;
    uint64_t step = std::min<uint64_t>((uint64_t)std::max<la_int64_t>(n, 0), r->size - r->pos);
    r->pos += step;
    return (la_int64_t)step;
}

// Whether an entry name stays inside the destination (no drive, no leading slash, no "..").
bool safeName(const std::wstring& n) {
    if (n.empty() || n[0] == L'/' || n[0] == L'\\' || n.find(L':') != std::wstring::npos) return false;
    size_t s = 0;
    while (s <= n.size()) {
        size_t e = n.find_first_of(L"/\\", s);
        if (e == std::wstring::npos) e = n.size();
        if (n.compare(s, e - s, L"..") == 0 && e - s == 2) return false;
        s = e + 1;
    }
    return true;
}

struct Job {
    std::wstring dest, password;
    std::atomic<uint64_t> done{0};
    std::atomic<bool> cancel{false}, finished{false};
    bool ok = false, badPassword = false;
    uint64_t files = 0;
    std::wstring error;
};

// Unix seconds to a FILETIME.
FILETIME fileTime(int64_t unix) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)((unix + 11644473600LL) * 10000000LL);
    FILETIME ft{u.LowPart, u.HighPart};
    return ft;
}

bool isKmp() {
    HANDLE h = CreateFileW(g_self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t m[8] = {};
    DWORD got = 0;
    LARGE_INTEGER at;
    at.QuadPart = (LONGLONG)g_tr.offset;
    bool ok = g_tr.size >= 8 && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, m, 8, &got, nullptr) && got == 8 &&
              std::memcmp(m, "KMPR\x1A\r\n\0", 8) == 0;
    CloseHandle(h);
    return ok;
}

// Kompressor's own format, read with its own reader (blocks are decoded on every core).
void extractKmp(Job& job, const std::wstring& base) {
    kz::kmp::Reader rd;
    std::string err;
    if (!rd.openEmbedded(toUtf8(g_self), g_tr.offset, g_tr.size, err)) {
        job.error = fromUtf8(err.c_str());
        return;
    }
    kz::Progress prog;
    rd.setSequential(&prog);
    auto progress = [&] {
        uint64_t t = prog.workTotal.load();
        job.done = t ? (uint64_t)((double)prog.workDone.load() / (double)t * (double)g_tr.size) : 0;
        if (job.cancel.load()) prog.cancel = true;
    };
    bool ok = true;
    const auto& entries = rd.entries();
    for (size_t i = 0; i < entries.size() && ok; ++i) {
        const kz::kmp::Entry& e = entries[i];
        std::wstring name = fromUtf8(e.path.c_str());
        std::replace(name.begin(), name.end(), L'/', L'\\');
        if (!safeName(name) || e.type == kz::kmp::EntryType::Symlink) continue;
        std::wstring full = base + L"\\" + name;
        if (e.type == kz::kmp::EntryType::Dir) {
            SHCreateDirectoryExW(nullptr, full.c_str(), nullptr);
            continue;
        }
        size_t cut = full.find_last_of(L'\\');
        if (cut != std::wstring::npos) SHCreateDirectoryExW(nullptr, full.substr(0, cut).c_str(), nullptr);
        HANDLE f = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            job.error = L"Can't write " + full;
            ok = false;
            break;
        }
        bool wrote = true;
        ok = rd.readFile(i, [&](const uint8_t* p, size_t n) {
            DWORD put = 0;
            if (!WriteFile(f, p, (DWORD)n, &put, nullptr) || put != n) wrote = false;
            progress();
            return wrote && !job.cancel.load();
        }, &prog, err);
        if (ok && wrote && e.mtime) {
            FILETIME ft = fileTime(e.mtime);
            SetFileTime(f, nullptr, nullptr, &ft);
        }
        CloseHandle(f);
        if (!wrote) {
            job.error = L"Can't write " + full + L" (disk full?)";
            ok = false;
        } else if (!ok && !job.cancel.load()) {
            job.error = fromUtf8(err.c_str());
        }
        if (ok) job.files++;
    }
    progress();
    job.ok = ok && !job.cancel.load();
    if (job.cancel.load()) job.error = L"Cancelled.";
}

void extract(Job& job) {
    if (isKmp()) {
        std::wstring base = job.dest;
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
        SHCreateDirectoryExW(nullptr, base.c_str(), nullptr);
        extractKmp(job, base);
        return;
    }
    Region rg;
    rg.h = CreateFileW(g_self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    rg.base = g_tr.offset;
    rg.size = g_tr.size;
    rg.done = &job.done;
    rg.cancel = &job.cancel;
    if (rg.h == INVALID_HANDLE_VALUE) {
        job.error = L"Can't open this file to read the archive inside it.";
        return;
    }
    archive* a = archive_read_new();
    archive_read_support_format_7zip(a);
    archive_read_support_format_zip(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_cpio(a);
    archive_read_support_format_iso9660(a);
    archive_read_support_format_raw(a);  // a single compressed file; only when nothing else matches
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_zstd(a);
    archive_read_support_filter_lz4(a);
    if (!job.password.empty()) archive_read_add_passphrase(a, toUtf8(job.password).c_str());
    archive_read_set_read_callback(a, regionRead);
    archive_read_set_seek_callback(a, regionSeek);
    archive_read_set_skip_callback(a, regionSkip);
    archive_read_set_callback_data(a, &rg);

    archive* w = archive_write_disk_new();
    archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                          ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(w);

    std::wstring base = job.dest;
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
    SHCreateDirectoryExW(nullptr, base.c_str(), nullptr);

    auto fail = [&](archive* from) {
        const char* m = archive_error_string(from);
        std::string s = m ? m : "unknown error";
        job.error = fromUtf8(s.c_str());
        if (s.find("assphrase") != std::string::npos || s.find("ncrypted") != std::string::npos ||
            s.find("password") != std::string::npos)
            job.badPassword = true;
    };

    bool ok = archive_read_open1(a) == ARCHIVE_OK;
    if (!ok) fail(a);
    archive_entry* e = nullptr;
    while (ok) {
        int r = archive_read_next_header(a, &e);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) {
            ok = false;
            fail(a);
            break;
        }
        const wchar_t* nm = archive_entry_pathname_w(e);
        std::wstring name = nm ? nm : fromUtf8(archive_entry_pathname_utf8(e));
        if (archive_format(a) == ARCHIVE_FORMAT_RAW) {  // a lone compressed file: use the name it was made from
            name = g_inner;
            if (name.empty()) {
                name = g_title;
                size_t dot = name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot > 0) name.resize(dot);
            }
        }
        if (!safeName(name)) continue;  // never write outside the chosen folder
        archive_entry_copy_pathname_w(e, (base + L"\\" + name).c_str());
        if (const wchar_t* hl = archive_entry_hardlink_w(e)) {
            if (!safeName(hl)) continue;
            archive_entry_copy_hardlink_w(e, (base + L"\\" + hl).c_str());
        }
        if (archive_write_header(w, e) < ARCHIVE_WARN) {
            ok = false;
            fail(w);
            break;
        }
        if (archive_entry_size(e) > 0 || !archive_entry_size_is_set(e)) {  // a lone compressed file's size isn't known
            const void* buf;
            size_t len;
            la_int64_t off;
            while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
                if (archive_write_data_block(w, buf, len, off) < ARCHIVE_WARN) {
                    r = ARCHIVE_FATAL;
                    fail(w);
                    break;
                }
            }
            if (r != ARCHIVE_EOF) {
                if (job.error.empty()) fail(a);
                ok = false;
                break;
            }
        }
        if (archive_write_finish_entry(w) < ARCHIVE_WARN) {
            ok = false;
            fail(w);
            break;
        }
        if (archive_entry_filetype(e) == AE_IFREG) job.files++;
    }
    archive_write_close(w);
    archive_write_free(w);
    archive_read_free(a);
    CloseHandle(rg.h);
    if (job.cancel.load()) {
        job.error = L"Cancelled.";
        ok = false;
    }
    job.ok = ok;
}

// ------------------------------------------------------------------ the window
// Drawn with Dear ImGui in Kompressor's own look (its theme, title bar and widgets), like the app's
// quick-task window: no stock Windows controls.

std::wstring defaultDest() {
    std::wstring d = g_self;
    if (!g_inner.empty()) {  // a single file goes next to the .exe
        size_t slash = d.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"." : d.substr(0, slash);
    }
    size_t dot = d.find_last_of(L'.'), slash = d.find_last_of(L"\\/");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) d.resize(dot);
    // "photos.zip.exe" -> "photos"
    for (const wchar_t* x : {L".zip", L".7z", L".rar", L".kmp", L".iso", L".cpio", L".tar", L".tar.gz", L".tar.xz",
                             L".tar.zst", L".tar.bz2", L".tar.lz4"}) {
        size_t n = wcslen(x);
        if (d.size() > n && _wcsicmp(d.c_str() + d.size() - n, x) == 0) {
            d.resize(d.size() - n);
            break;
        }
    }
    return d;
}

std::string humanSize(uint64_t b) {
    const char* u[] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)b;
    int i = 0;
    while (v >= 1024.0 && i < 4) v /= 1024.0, ++i;
    char s[32];
    std::snprintf(s, sizeof s, i == 0 ? "%.0f %s" : v < 10 ? "%.2f %s" : v < 100 ? "%.1f %s" : "%.0f %s", v, u[i]);
    return s;
}

using namespace kzgui;

enum class Screen { Ask, Working, Failed, Broken };

struct Ui {
    GLFWwindow* window = nullptr;
    Screen screen = Screen::Ask;
    char dest[4096] = {};
    char pw[512] = {};
    bool showPw = false, focusPw = false;
    std::string error;       // shown on the Ask screen (wrong password) or the Failed screen
    std::unique_ptr<Job> job;
    std::thread worker;
    double startedAt = 0;
    bool quit = false;
} g_ui;

std::function<void()> g_render;

}  // namespace

namespace kzgui::native {
void redraw() {  // window_frame.cpp: keeps drawing while the window is dragged
    if (g_render) g_render();
}
}  // namespace kzgui::native

namespace {

std::string browseFolder(const std::string& start) {
    std::string out;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return out;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Extract to");
    IShellItem* at = nullptr;
    std::wstring ws = fromUtf8(start.c_str());
    while (!ws.empty() && FAILED(SHCreateItemFromParsingName(ws.c_str(), nullptr, IID_PPV_ARGS(&at)))) {
        size_t s = ws.find_last_of(L"\\/");
        if (s == std::wstring::npos) break;
        ws.resize(s);
    }
    if (at) {
        d->SetFolder(at);
        at->Release();
    }
    if (SUCCEEDED(d->Show(glfwGetWin32Window(g_ui.window)))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = toUtf8(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
    return out;
}

void startExtract() {
    std::wstring dest = fromUtf8(g_ui.dest);
    if (dest.empty()) return;
    wchar_t full[MAX_PATH * 4];
    if (GetFullPathNameW(dest.c_str(), MAX_PATH * 4, full, nullptr)) dest = full;
    g_ui.job = std::make_unique<Job>();
    g_ui.job->dest = dest;
    g_ui.job->password = fromUtf8(g_ui.pw);
    g_ui.error.clear();
    g_ui.screen = Screen::Working;
    g_ui.startedAt = ImGui::GetTime();
    Job* j = g_ui.job.get();
    g_ui.worker = std::thread([j] {
        extract(*j);
        j->finished = true;
        glfwPostEmptyEvent();
    });
}

void finishExtract() {
    if (g_ui.worker.joinable()) g_ui.worker.join();
    std::unique_ptr<Job> j = std::move(g_ui.job);
    if (j->ok) {
        ShellExecuteW(nullptr, L"open", j->dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        g_ui.quit = true;
        return;
    }
    if (j->cancel.load()) {
        g_ui.screen = Screen::Ask;
        return;
    }
    if (j->badPassword && (g_tr.flags & kFlagEncrypted)) {
        g_ui.error = g_ui.pw[0] ? "That password is wrong. Try again." : "Enter the password first.";
        std::memset(g_ui.pw, 0, sizeof g_ui.pw);
        g_ui.focusPw = true;
        g_ui.screen = Screen::Ask;
        return;
    }
    g_ui.error = toUtf8(j->error);
    if (g_ui.error.empty()) g_ui.error = "Something went wrong.";
    g_ui.screen = Screen::Failed;
}

void drawTitleBar() {
    if (!ImGui::BeginMenuBar()) return;
    const float em = ImGui::GetFontSize();
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImRect mb = w->MenuBarRect();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::vector<frame::Rect> hit;
    dl->AddLine(ImVec2(mb.Min.x, mb.Max.y - 1), ImVec2(mb.Max.x, mb.Max.y - 1), u32(col::Line));
    float ls = em * 1.2f;
    ImGui::SetCursorPosX(em * 1.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    drawLogo(dl, ImVec2(p.x, (mb.Min.y + mb.Max.y - ls) * 0.5f), ls);
    ImGui::Dummy(ImVec2(ls, 1));
    ImGui::SameLine(0, em * 0.5f);
    ImGui::PushFont(fonts().bold);
    ImGui::TextUnformatted("Kompressor");
    ImGui::PopFont();
    ImGui::SameLine(0, em * 0.5f);
    ImGui::TextColored(col::Faint, "self-extracting archive");
    if (frame::custom()) windowButtons(g_ui.window, mb.Max.x, w->Pos.y, mb.Max.y - w->Pos.y, false, hit);
    frame::setTitleBar(mb.Max.y - w->Pos.y, hit);
    ImGui::EndMenuBar();
}

void drawUi() {
    const float em = ImGui::GetFontSize();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.8f, em * 0.5f));
    ImGui::Begin("##sfx", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(4);
    drawTitleBar();

    const float pad = em * 1.1f;
    const float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    const float inner = W - pad * 2;
    const float tile = em * 2.7f;
    const float bh = ImGui::GetFrameHeight() * 1.05f;
    const float textX = pad + tile + em * 0.85f;
    const float textW = W - textX - pad;
    ImGui::SetCursorPos(ImVec2(pad, ImGui::GetCursorPosY() + em * 0.9f));
    const float top = ImGui::GetCursorPosY();
    auto header = [&](Ico ic, const ImVec4& tint, const std::string& title, const std::string& line) {
        ImGui::SetCursorPos(ImVec2(pad, top));
        iconTile(ic, tint, tile);
        ImGui::SetCursorPos(ImVec2(textX, top - em * 0.1f));
        ImGui::PushFont(fonts().h2);
        ImGui::TextUnformatted(fitText(title, textW).c_str());
        ImGui::PopFont();
        if (!line.empty()) {
            ImGui::SetCursorPosX(textX);
            ImGui::TextColored(col::Dim, "%s", fitText(line, textW).c_str());
        }
        ImGui::SetCursorPos(ImVec2(pad, top + tile + em * 0.85f));
    };
    auto buttonRow = [&] { ImGui::SetCursorPos(ImVec2(pad, H - pad - bh)); };
    auto alignEnd = [&](float width) { ImGui::SetCursorPosX(W - pad - width); };
    std::string name = toUtf8(g_title);  // "photos.exe" -> "photos"
    if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".exe") == 0) name.resize(name.size() - 4);
    if (!g_inner.empty()) name = toUtf8(g_inner);  // a single compressed file: its own name
    for (const char* x : {".zip", ".7z", ".rar", ".kmp", ".iso", ".cpio", ".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2",
                          ".tar.lz4", ".tar", ".gz", ".bz2", ".xz", ".zst", ".lz4"}) {
        if (!g_inner.empty()) break;
        size_t n = std::strlen(x);
        if (name.size() > n && _stricmp(name.c_str() + name.size() - n, x) == 0) {
            name.resize(name.size() - n);
            break;
        }
    }
    const bool encrypted = g_tr.flags & kFlagEncrypted;

    if (g_ui.screen == Screen::Broken) {
        header(Ico::Error, col::Error, "Nothing to extract", name);
        ImGui::SetCursorPosX(pad);
        ImGui::PushTextWrapPos(W - pad);
        ImGui::TextColored(mix(col::Error, col::Text, 0.55f), "%s",
                           "There is no archive in this file, or it is damaged or incomplete (a download that "
                           "didn't finish?).");
        ImGui::PopTextWrapPos();
        buttonRow();
        alignEnd(em * 5.4f);
        if (primaryButton("Close", Ico::Close, ImVec2(em * 5.4f, bh), false) || ImGui::IsKeyPressed(ImGuiKey_Escape))
            g_ui.quit = true;
    } else if (g_ui.screen == Screen::Ask) {
        header(Ico::Extract, col::Accent, "Extract " + name, humanSize(g_tr.size) + " archive" + (encrypted ? ", password-protected" : ""));
        caption("EXTRACT TO");
        float browseW = em * 6.2f;
        ImGui::SetCursorPosX(pad);
        ImGui::SetNextItemWidth(inner - browseW - em * 0.5f);
        bool enter = ImGui::InputText("##dest", g_ui.dest, sizeof g_ui.dest, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, em * 0.5f);
        if (secondaryButton("Browse...", ImVec2(browseW, ImGui::GetFrameHeight()))) {
            std::string f = browseFolder(g_ui.dest);
            if (!f.empty()) std::snprintf(g_ui.dest, sizeof g_ui.dest, "%s", f.c_str());
        }
        if (encrypted) {
            ImGui::Dummy(ImVec2(0, em * 0.2f));
            ImGui::SetCursorPosX(pad);
            caption("PASSWORD");
            float ib = ImGui::GetFrameHeight();
            if (g_ui.focusPw) {
                ImGui::SetKeyboardFocusHere();
                g_ui.focusPw = false;
            }
            ImGui::SetCursorPosX(pad);
            ImGui::SetNextItemWidth(inner - ib - em * 0.4f);
            enter |= ImGui::InputTextWithHint("##pw", "Password", g_ui.pw, sizeof g_ui.pw,
                                              ImGuiInputTextFlags_EnterReturnsTrue | (g_ui.showPw ? 0 : ImGuiInputTextFlags_Password));
            ImGui::SameLine(0, em * 0.4f);
            const ImVec4 on = col::AccentHover;
            if (iconButton("##show", Ico::Eye, ib, g_ui.showPw ? "Hide password" : "Show password", true, g_ui.showPw ? &on : nullptr))
                g_ui.showPw = !g_ui.showPw;
        }
        if (!g_ui.error.empty()) {
            ImGui::SetCursorPosX(pad);
            ImGui::TextColored(col::Error, "%s", fitText(g_ui.error, inner).c_str());
        }
        buttonRow();
        float pbw = em * 7.0f, cbw = em * 6.0f;
        alignEnd(pbw + cbw + em * 0.5f);
        if (secondaryButton("Cancel", ImVec2(cbw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) g_ui.quit = true;
        ImGui::SameLine(0, em * 0.5f);
        if ((primaryButton("Extract", Ico::Extract, ImVec2(pbw, bh)) || enter) && g_ui.dest[0]) startExtract();
    } else if (g_ui.screen == Screen::Working) {
        Job* j = g_ui.job.get();
        float frac = g_tr.size ? (float)((double)j->done.load() / (double)g_tr.size) : -1.0f;
        std::string dest = toUtf8(j->dest);
        std::string line = "to " + fitText(dest, textW - ImGui::CalcTextSize("to ").x, true);
        header(Ico::Extract, col::Accent, j->cancel.load() ? "Cancelling..." : "Extracting " + name, line);
        progressBar(frac, ImVec2(inner, em * 0.42f), col::Accent);
        ImGui::Dummy(ImVec2(0, em * 0.15f));
        double el = ImGui::GetTime() - g_ui.startedAt;
        std::string st = std::to_string((int)std::floor(std::max(frac, 0.0f) * 100.0f)) + "%";
        if (el > 0.5 && frac > 0) st += "  -  " + humanSize((uint64_t)((double)j->done.load() / el)) + "/s";
        ImGui::SetCursorPosX(pad);
        ImGui::TextColored(col::Dim, "%s", st.c_str());
        buttonRow();
        float cbw = em * 6.0f;
        alignEnd(cbw);
        if (secondaryButton("Cancel", ImVec2(cbw, bh), !j->cancel.load())) j->cancel = true;
        if (j->finished.load()) finishExtract();
    } else {
        header(Ico::Error, col::Error, "Couldn't extract " + name, "");
        ImGui::SetCursorPos(ImVec2(textX, top + ImGui::GetTextLineHeightWithSpacing() * 1.35f));
        ImGui::PushTextWrapPos(W - pad);
        ImGui::TextColored(mix(col::Error, col::Text, 0.55f), "%s", g_ui.error.c_str());
        ImGui::PopTextWrapPos();
        buttonRow();
        float tbw = em * 7.0f, cbw = em * 5.4f;
        alignEnd(tbw + cbw + em * 0.5f);
        if (secondaryButton("Try again", ImVec2(tbw, bh))) {
            g_ui.error.clear();
            g_ui.screen = Screen::Ask;
        }
        ImGui::SameLine(0, em * 0.5f);
        if (primaryButton("Close", Ico::Close, ImVec2(cbw, bh), false) || ImGui::IsKeyPressed(ImGuiKey_Escape)) g_ui.quit = true;
    }
    ImGui::End();
}

std::string firstExisting(std::initializer_list<const char*> candidates) {
    for (const char* c : candidates)
        if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) return c;
    return {};
}

// Segoe UI, as in the app; ImGui's own font if it is missing.
void applyScale(float xs) {
    if (xs <= 0) xs = 1;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F,
                                     0x2000, 0x206F, 0x2190, 0x21FF, 0};
    std::string ui = firstExisting({"C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"});
    std::string bold = firstExisting({"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf",
                                      "C:\\Windows\\Fonts\\arialbd.ttf"});
    if (bold.empty()) bold = ui;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    auto add = [&](const std::string& file, float size) -> ImFont* {
        ImFont* f = file.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, ranges);
        if (!f) {
            ImFontConfig d;
            d.SizePixels = size;
            f = io.Fonts->AddFontDefault(&d);
        }
        return f;
    };
    float px = std::round(15.0f * xs);
    Fonts& F = fonts();
    F.body = add(ui, px);
    F.bold = add(bold, px);
    F.caption = add(ui, std::round(px * 0.85f));
    F.h2 = add(bold, std::round(px * 1.3f));
    F.h1 = add(bold, std::round(px * 2.3f));
    F.mono = F.body;
    io.FontDefault = F.body;
    applyTheme(xs);
}

float g_pendingScale = 0;

int runWindow(bool broken, const std::wstring& password) {
    std::snprintf(g_ui.dest, sizeof g_ui.dest, "%s", toUtf8(defaultDest()).c_str());
    std::snprintf(g_ui.pw, sizeof g_ui.pw, "%s", toUtf8(password).c_str());
    g_ui.screen = broken ? Screen::Broken : Screen::Ask;
    g_ui.focusPw = !broken && (g_tr.flags & kFlagEncrypted);

    if (!glfwInit()) return 3;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);  // shown once placed
    const int winW = 500, winH = (g_tr.flags & kFlagEncrypted) ? 300 : 236;
    GLFWwindow* window = glfwCreateWindow(winW, winH, toUtf8(g_title).c_str(), nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 3;
    }
    g_ui.window = window;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    setWindowIcon(window);
    frame::install(window);
    float sx = 1, sy = 1;
    glfwGetWindowContentScale(window, &sx, &sy);
    frame::placeCentered((int)std::lround(winW * sx), (int)std::lround(winH * sx));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    applyScale(sx);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
    glfwSetWindowContentScaleCallback(window, [](GLFWwindow*, float x, float) { g_pendingScale = x; });

    g_render = [window] {
        if (g_pendingScale > 0) {
            applyScale(g_pendingScale);
            g_pendingScale = 0;
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        beginFrameUi();
        drawUi();
        frame::edgeCursor();
        ImGui::Render();
        int fw, fh;
        glfwGetFramebufferSize(window, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(col::Bg.x, col::Bg.y, col::Bg.z, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    };
    g_render();
    glfwShowWindow(window);

    // KZ_SFX_SNAPSHOT=out.ppm (debugging): draw for a moment, save the frame, and leave.
    const char* snap = std::getenv("KZ_SFX_SNAPSHOT");
    while (!glfwWindowShouldClose(window) && !g_ui.quit) {
        if (g_ui.screen == Screen::Working || animating() || snap) glfwWaitEventsTimeout(1.0 / 60.0);
        else glfwWaitEvents();
        g_render();
        if (snap && ImGui::GetTime() > 1.5) {
            int fw, fh;
            glfwGetFramebufferSize(window, &fw, &fh);
            std::vector<uint8_t> px((size_t)fw * fh * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* f = std::fopen(snap, "wb")) {
                std::fprintf(f, "P6\n%d %d\n255\n", fw, fh);
                for (int y = fh - 1; y >= 0; --y) std::fwrite(px.data() + (size_t)y * fw * 3, 1, (size_t)fw * 3, f);
                std::fclose(f);
            }
            break;
        }
    }
    if (g_ui.job) g_ui.job->cancel = true;
    if (g_ui.worker.joinable()) g_ui.worker.join();
    g_render = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t self[32768];
    DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    g_self.assign(self, n);
    size_t slash = g_self.find_last_of(L"\\/");
    g_title = g_self.substr(slash == std::wstring::npos ? 0 : slash + 1);
    bool broken = !readTrailer(g_self, g_tr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dest, password;
    bool quiet = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"-y" || a == L"/y" || a == L"/S") quiet = true;
        else if (a == L"-o" && i + 1 < argc) dest = argv[++i], quiet = true;
        else if (a.rfind(L"-o", 0) == 0 && a.size() > 2) dest = a.substr(2), quiet = true;
        else if (a.rfind(L"-p", 0) == 0) password = a.substr(2);
    }
    LocalFree(argv);

    if (broken && quiet) return 2;
    if (!quiet || broken) return runWindow(broken, password);

    Job job;
    job.dest = dest.empty() ? defaultDest() : dest;
    wchar_t full[MAX_PATH * 4];
    if (GetFullPathNameW(job.dest.c_str(), MAX_PATH * 4, full, nullptr)) job.dest = full;
    job.password = password;
    extract(job);
    if (!job.ok) {
        // A GUI program has no console of its own; report through the parent's, if there is one.
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::string m = toUtf8(g_title + L": " + job.error + L"\n");
            DWORD w = 0;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), m.data(), (DWORD)m.size(), &w, nullptr);
        }
        return 1;
    }
    return 0;
}
