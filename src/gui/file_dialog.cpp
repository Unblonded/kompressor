#include "file_dialog.hpp"

#include <GLFW/glfw3.h>
#include <nfd.h>
#include <nfd_glfw3.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>

#include "kz_common.hpp"

namespace fs = std::filesystem;

namespace kzgui {

namespace {

// Extension filter for a save dialog, e.g. "Archive.tar.gz" -> "tar.gz", "x.kmp" -> "kmp".
std::string saveExt(const std::string& name) {
    std::string n = name;
    for (auto& c : n) c = char(std::tolower((unsigned char)c));
    size_t t = n.rfind(".tar.");
    if (t != std::string::npos && n.find('.', t + 5) == std::string::npos) return name.substr(t + 1);
    size_t d = n.rfind('.');
    if (d == std::string::npos || d + 1 >= n.size()) return {};
    return name.substr(d + 1);
}

std::string existingDir(const std::string& p) {
    std::error_code ec;
    fs::path d = kz::u8path(p);
    while (!d.empty() && !fs::is_directory(d, ec)) {
        fs::path parent = d.parent_path();
        if (parent == d) break;
        d = parent;
    }
    return fs::is_directory(d, ec) ? kz::pathToUtf8(d) : std::string();
}

void collect(const nfdpathset_t* set, std::vector<std::string>& out) {
    nfdpathsetsize_t n = 0;
    if (NFD_PathSet_GetCount(set, &n) != NFD_OKAY) return;
    for (nfdpathsetsize_t i = 0; i < n; ++i) {
        nfdu8char_t* p = nullptr;
        if (NFD_PathSet_GetPathU8(set, i, &p) == NFD_OKAY && p) {
            out.emplace_back(p);
            NFD_PathSet_FreePathU8(p);
        }
    }
}

}  // namespace

FileDialog::FileDialog() { ready_ = NFD_Init() == NFD_OKAY; }

FileDialog::~FileDialog() {
    if (ready_) NFD_Quit();
}

std::string FileDialog::homeDir() {
#ifdef _WIN32
    if (const wchar_t* h = _wgetenv(L"USERPROFILE")) return kz::pathToUtf8(fs::path(h));
#else
    if (const char* h = std::getenv("HOME")) return h;
#endif
    std::error_code ec;
    return kz::pathToUtf8(fs::current_path(ec));
}

void FileDialog::open(const std::string& title, Mode mode, const std::string& startDir, Callback cb,
                      const std::string& defaultName) {
    title_ = title;
    mode_ = mode;
    startDir_ = startDir;
    defaultName_ = defaultName;
    cb_ = std::move(cb);
    pending_ = true;
}

std::string FileDialog::takeError() {
    std::string e;
    e.swap(error_);
    return e;
}

void FileDialog::draw() {
    if (!pending_) return;
    pending_ = false;
    if (!ready_) ready_ = NFD_Init() == NFD_OKAY;
    if (!ready_) {
        error_ = std::string("Could not open the system file dialog: ") + (NFD_GetError() ? NFD_GetError() : "unknown error");
        return;
    }

    nfdwindowhandle_t parent{};
    if (parent_) NFD_GetNativeWindowFromGLFWWindow(parent_, &parent);
    std::string dir = existingDir(startDir_.empty() ? homeDir() : startDir_);
    const nfdu8char_t* defPath = dir.empty() ? nullptr : dir.c_str();

    std::vector<std::string> out;
    nfdresult_t r = NFD_CANCEL;
    switch (mode_) {
        case Mode::OpenArchives:
        case Mode::AddFiles: {
            static const nfdu8filteritem_t archives[] = {
                {"Archives", "kmp,zip,7z,rar,tar,gz,tgz,bz2,tbz2,xz,txz,zst,tzst,lz4,lz,lzma,z,iso,cab,lha,lzh,xar,pkg,cpio,rpm,deb,ar,"
                             "jar,apk,ipa,war,xpi,nupkg,whl,zipx,001"},
            };
            nfdopendialogu8args_t args{};
            if (mode_ == Mode::OpenArchives) {
                args.filterList = archives;
                args.filterCount = 1;
            }
            args.defaultPath = defPath;
            args.parentWindow = parent;
            const nfdpathset_t* set = nullptr;
            r = NFD_OpenDialogMultipleU8_With(&set, &args);
            if (r == NFD_OKAY) {
                collect(set, out);
                NFD_PathSet_Free(set);
            }
            break;
        }
        case Mode::AddFolders: {
            nfdpickfolderu8args_t args{};
            args.defaultPath = defPath;
            args.parentWindow = parent;
            const nfdpathset_t* set = nullptr;
            r = NFD_PickFolderMultipleU8_With(&set, &args);
            if (r == NFD_OKAY) {
                collect(set, out);
                NFD_PathSet_Free(set);
            }
            break;
        }
        case Mode::PickFolder: {
            nfdpickfolderu8args_t args{};
            args.defaultPath = defPath;
            args.parentWindow = parent;
            nfdu8char_t* p = nullptr;
            r = NFD_PickFolderU8_With(&p, &args);
            if (r == NFD_OKAY && p) {
                out.emplace_back(p);
                NFD_FreePathU8(p);
            }
            break;
        }
        case Mode::SaveFile: {
            std::string ext = saveExt(defaultName_);
            nfdu8filteritem_t filter{"Archive", ext.c_str()};
            nfdsavedialogu8args_t args{};
            if (!ext.empty()) {
                args.filterList = &filter;
                args.filterCount = 1;
            }
            args.defaultPath = defPath;
            args.defaultName = defaultName_.empty() ? nullptr : defaultName_.c_str();
            args.parentWindow = parent;
            nfdu8char_t* p = nullptr;
            r = NFD_SaveDialogU8_With(&p, &args);
            if (r == NFD_OKAY && p) {
                out.emplace_back(p);
                NFD_FreePathU8(p);
            }
            break;
        }
    }
    if (r == NFD_ERROR) {
        error_ = std::string("File dialog error: ") + (NFD_GetError() ? NFD_GetError() : "unknown error");
        NFD_ClearError();
    }
    if (parent_) glfwPostEmptyEvent();  // redraw right away with the result
    if (r == NFD_OKAY && !out.empty() && cb_) {
        Callback cb = std::move(cb_);
        cb_ = nullptr;
        cb(out);
    }
}

}  // namespace kzgui
