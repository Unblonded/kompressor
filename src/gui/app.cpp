#include "app.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "kmp.hpp"
#include "theme.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace kzgui {

namespace {

constexpr uint64_t kPreviewMax = 4ull << 20;        // 4 MiB shown in the preview pane
constexpr uint64_t kNestedMax = 2048ull << 20;      // nested archives are opened in memory

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void copyTo(char* dst, size_t n, const std::string& s) {
    std::strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = 0;
}

std::string parentDir(const std::string& p) { return kz::pathToUtf8(kz::u8path(p).parent_path()); }

void launch(const std::vector<std::string>& args) {
#ifdef _WIN32
    (void)args;
#else
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) == 0)
        std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
#endif
}

// Shows a file or folder in Finder / Explorer / the Linux file manager.
void reveal(const std::string& path) {
    std::error_code ec;
    bool dir = std::filesystem::is_directory(kz::u8path(path), ec);
#ifdef _WIN32
    std::wstring w = kz::u8path(path).wstring();
    if (dir) {
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        std::wstring arg = L"/select,\"" + w + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
#elif defined(__APPLE__)
    if (dir) launch({"open", path});
    else launch({"open", "-R", path});
#else
    launch({"xdg-open", dir ? path : parentDir(path)});
#endif
}

enum class Icon { Folder, File, Archive, Image, Code, Exec, Text, Link };

Icon iconFor(const Node& n) {
    if (n.isDir) return Icon::Folder;
    if (n.isLink) return Icon::Link;
    std::string e = kz::lowerExt(n.name);
    if (kz::looksLikeArchive(n.name)) return Icon::Archive;
    static const char* img[] = {"png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico", "tif", "tiff", "heic", "psd", "mp4", "mov", "mkv", "mp3", "wav", "flac", "ogg"};
    static const char* code[] = {"c", "h", "cpp", "hpp", "cc", "cs", "java", "py", "js", "ts", "rs", "go", "swift", "m", "mm", "kt", "lua", "rb", "php", "sh", "json", "xml", "html", "css", "yml", "yaml", "toml", "cmake"};
    static const char* exe[] = {"exe", "dll", "so", "dylib", "bin", "app", "msi", "sys", "o", "a", "lib"};
    static const char* txt[] = {"txt", "md", "log", "ini", "cfg", "csv", "rtf", "pdf", "doc", "readme"};
    for (auto x : img) if (e == x) return Icon::Image;
    for (auto x : code) if (e == x) return Icon::Code;
    for (auto x : exe) if (e == x) return Icon::Exec;
    for (auto x : txt) if (e == x) return Icon::Text;
    return Icon::File;
}

ImU32 u32(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

// Draws a small vector icon at the cursor and advances past it.
void drawIcon(Icon ic, float s) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ic == Icon::Folder) {
        dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.16f), ImVec2(p.x + s * 0.48f, p.y + s * 0.34f), u32(col::FolderCol, 0.85f), s * 0.08f);
        dl->AddRectFilled(ImVec2(p.x + s * 0.05f, p.y + s * 0.26f), ImVec2(p.x + s * 0.95f, p.y + s * 0.86f), u32(col::FolderCol), s * 0.1f);
    } else {
        ImVec4 c = col::FileCol;
        switch (ic) {
            case Icon::Archive: c = col::ArchiveCol; break;
            case Icon::Image: c = col::ImageCol; break;
            case Icon::Code: c = col::CodeCol; break;
            case Icon::Exec: c = col::ExecCol; break;
            case Icon::Link: c = col::Teal; break;
            default: break;
        }
        ImVec2 a(p.x + s * 0.18f, p.y + s * 0.06f), b(p.x + s * 0.82f, p.y + s * 0.94f);
        dl->AddRectFilled(a, b, u32(c, 0.18f), s * 0.1f);
        dl->AddRect(a, b, u32(c), s * 0.1f, 0, std::max(1.0f, s * 0.07f));
        if (ic == Icon::Archive) {  // zipper
            for (int i = 0; i < 4; ++i) {
                float y = p.y + s * (0.2f + i * 0.16f);
                float x = p.x + s * (i % 2 ? 0.5f : 0.42f);
                dl->AddRectFilled(ImVec2(x, y), ImVec2(x + s * 0.09f, y + s * 0.08f), u32(c));
            }
        } else if (ic == Icon::Link) {
            dl->AddLine(ImVec2(p.x + s * 0.35f, p.y + s * 0.65f), ImVec2(p.x + s * 0.65f, p.y + s * 0.35f), u32(c), s * 0.08f);
        } else {
            for (int i = 0; i < 3; ++i) {
                float y = p.y + s * (0.32f + i * 0.17f);
                dl->AddLine(ImVec2(p.x + s * 0.32f, y), ImVec2(p.x + s * (i == 2 ? 0.55f : 0.68f), y), u32(c, 0.8f), std::max(1.0f, s * 0.06f));
            }
        }
    }
    ImGui::Dummy(ImVec2(s, s));
}

void spinner(float radius, const ImVec4& c) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ctr(p.x + radius, p.y + radius);
    float t = (float)ImGui::GetTime() * 5.0f;
    dl->PathArcTo(ctr, radius * 0.8f, t, t + 4.2f, 24);
    dl->PathStroke(u32(c), 0, radius * 0.25f);
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
}

bool isTextData(const std::vector<uint8_t>& d) {
    size_t n = std::min<size_t>(d.size(), 64 * 1024), ctrl = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = d[i];
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32 && c != 27)) ++ctrl;
    }
    return ctrl * 50 <= n;
}

void centerText(const char* t, const ImVec4* c = nullptr) {
    float w = ImGui::CalcTextSize(t).x;
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - w) * 0.5f + ImGui::GetCursorPosX()));
    if (c) ImGui::TextColored(*c, "%s", t);
    else ImGui::TextUnformatted(t);
}

int formatIndex(kz::OutFormat f) {
    auto& fs = kz::outputFormats();
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].id == f) return (int)i;
    return 0;
}

}  // namespace

// ============================================================================ lifecycle
App::App(GLFWwindow* window, ImFont* mono, float scale) : window_(window), mono_(mono), scale_(scale) {
    lastDir_ = FileDialog::homeDir();
    log(col::Dim, std::string("Kompressor ") + KZ_VERSION + " ready. Drop files or archives onto the window.");
}

App::~App() {
    for (auto& j : jobs_) j->prog->cancel = true;
    for (auto& j : jobs_)
        if (j->thread.joinable()) j->thread.join();
}

bool App::busy() const {
    for (auto& j : jobs_)
        if (!j->finished) return true;
    return !toasts_.empty();
}

// ============================================================================ jobs
Job& App::startJob(const std::string& title, std::function<bool(kz::Progress&, std::string&)> work,
                   std::function<void(Job&)> onDone, bool visible) {
    auto j = std::make_unique<Job>();
    Job* jp = j.get();
    jp->id = nextJobId_++;
    jp->title = title;
    jp->onDone = std::move(onDone);
    jp->started = ImGui::GetTime();
    jp->visible = visible;
    jp->thread = std::thread([jp, work = std::move(work)] {
        std::string msg;
        bool ok = false;
        try {
            ok = work(*jp->prog, msg);
        } catch (const std::exception& e) {
            msg = std::string("Internal error: ") + e.what();
        }
        jp->ok = ok;
        jp->message = msg;
        jp->finished = true;
        glfwPostEmptyEvent();  // wake the UI
    });
    jobs_.push_back(std::move(j));
    return *jp;
}

void App::pumpJobs() {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        Job& j = **it;
        if (!j.finished) {
            ++it;
            continue;
        }
        if (j.thread.joinable()) j.thread.join();
        if (j.onDone) j.onDone(j);
        it = jobs_.erase(it);
    }
}

void App::log(const ImVec4& c, const std::string& text, const std::string& open, const std::string& rev) {
    log_.push_back({ImGui::GetTime(), c, text, open, rev});
    if (log_.size() > 500) log_.erase(log_.begin());
}

void App::toast(const ImVec4& c, const std::string& text) { toasts_.push_back({ImGui::GetTime(), c, text}); }

void App::askPassword(const std::string& prompt, std::function<void(const std::string&)> onOk) {
    pwd_.request = true;
    pwd_.prompt = prompt;
    pwd_.buf[0] = 0;
    pwd_.onOk = std::move(onOk);
}

// ============================================================================ tabs
ExplorerTab* App::tabById(int id) {
    for (auto& t : tabs_)
        if (t->id == id) return t.get();
    return nullptr;
}

void App::openArchive(const std::string& path) {
    for (auto& t : tabs_)
        if (!t->src.inMemory() && t->src.path == path) {
            focusTab_ = t->id;
            return;
        }
    lastDir_ = parentDir(path);
    kz::Source s;
    s.path = path;
    openSource(s, kz::fileNameOf(path), "");
}

void App::openSource(const kz::Source& src, const std::string& title, const std::string& password) {
    auto t = std::make_unique<ExplorerTab>();
    t->id = nextTabId_++;
    t->src = src;
    t->title = title;
    t->password = password;
    int id = t->id;
    tabs_.push_back(std::move(t));
    focusTab_ = id;
    loadTab(id);
}

void App::closeTab(int id) {
    for (auto it = tabs_.begin(); it != tabs_.end(); ++it)
        if ((*it)->id == id) {
            if ((*it)->previewProg) (*it)->previewProg->cancel = true;
            if ((*it)->loadProg) (*it)->loadProg->cancel = true;
            tabs_.erase(it);
            return;
        }
}

void App::loadTab(int tabId) {
    ExplorerTab* t = tabById(tabId);
    if (!t) return;
    t->loading = true;
    t->error.clear();
    auto info = std::make_shared<kz::ArchiveInfo>();
    kz::Source src = t->src;
    std::string pw = t->password, title = t->title;
    Job& j = startJob("Reading " + title,
        [src, pw, info](kz::Progress& p, std::string& err) { return kz::listArchive(src, *info, pw, p, err); },
        [this, tabId, info, title](Job& j) {
            ExplorerTab* t = tabById(tabId);
            if (!t) return;
            t->loading = false;
            if (j.ok) {
                t->info = std::move(*info);
                buildTree(*t);
                computeRows(*t);
                if (!t->info.extra.empty() && !t->info.isKmp) log(col::Warn, title + ": " + t->info.extra);
                log(col::Dim, "Opened " + title + " (" + t->info.format + ", " + std::to_string(t->info.entries.size()) + " entries)");
            } else if (j.prog->needPassword && j.message != "Cancelled") {
                t->error = "This archive is encrypted.";
                askPassword("\"" + title + "\" is encrypted. Enter its password:", [this, tabId](const std::string& pw) {
                    if (ExplorerTab* t = tabById(tabId)) {
                        t->password = pw;
                        loadTab(tabId);
                    }
                });
            } else {
                t->error = j.message;
                log(col::Error, "Could not open " + title + ": " + j.message);
            }
        });
    t->loadProg = j.prog;
}

void App::buildTree(ExplorerTab& t) {
    t.nodes.clear();
    t.selection.clear();
    t.cwd = 0;
    t.previewNode = -1;
    Node root;
    root.name = t.title;
    root.isDir = true;
    t.nodes.push_back(root);
    std::unordered_map<std::string, int> index;
    index[""] = 0;

    std::function<int(const std::string&)> ensureDir = [&](const std::string& path) -> int {
        auto it = index.find(path);
        if (it != index.end()) return it->second;
        size_t s = path.find_last_of('/');
        int parent = ensureDir(s == std::string::npos ? std::string() : path.substr(0, s));
        Node n;
        n.name = s == std::string::npos ? path : path.substr(s + 1);
        n.path = path;
        n.isDir = true;
        n.parent = parent;
        int id = (int)t.nodes.size();
        t.nodes.push_back(n);
        t.nodes[parent].children.push_back(id);
        index[path] = id;
        return id;
    };

    for (size_t i = 0; i < t.info.entries.size(); ++i) {
        const kz::Entry& e = t.info.entries[i];
        if (e.path.empty()) continue;
        if (e.isDir) {
            int id = ensureDir(e.path);
            t.nodes[id].entry = (int)i;
            t.nodes[id].mtime = e.mtime;
            continue;
        }
        auto it = index.find(e.path);
        int id;
        if (it != index.end()) {
            id = it->second;  // duplicate member (e.g. appended tar): newest wins
        } else {
            size_t s = e.path.find_last_of('/');
            int parent = ensureDir(s == std::string::npos ? std::string() : e.path.substr(0, s));
            Node n;
            n.name = s == std::string::npos ? e.path : e.path.substr(s + 1);
            n.path = e.path;
            n.parent = parent;
            id = (int)t.nodes.size();
            t.nodes.push_back(n);
            t.nodes[parent].children.push_back(id);
            index[e.path] = id;
        }
        Node& n = t.nodes[id];
        n.entry = (int)i;
        n.size = e.size;
        n.mtime = e.mtime;
        n.isLink = e.isSymlink;
        n.encrypted = e.encrypted;
    }
    // Children always have higher ids than their parents: aggregate bottom-up.
    for (int i = (int)t.nodes.size() - 1; i > 0; --i) {
        Node& n = t.nodes[i];
        Node& p = t.nodes[n.parent];
        p.size += n.size;
        p.files += n.isDir ? n.files : 1;
    }
    // A single top-level folder is the common case: start inside it? Keep root, it is clearer.
}

void App::computeRows(ExplorerTab& t) {
    t.rows.clear();
    if (t.nodes.empty()) return;
    std::string f = lower(t.filter);
    if (f.empty()) {
        t.rows = t.nodes[t.cwd].children;
    } else {
        std::vector<int> stack = t.nodes[t.cwd].children;
        while (!stack.empty()) {
            int id = stack.back();
            stack.pop_back();
            if (lower(t.nodes[id].name).find(f) != std::string::npos) t.rows.push_back(id);
            for (int c : t.nodes[id].children) stack.push_back(c);
        }
    }
    auto& N = t.nodes;
    int col = t.sortCol;
    bool asc = t.sortAsc;
    std::sort(t.rows.begin(), t.rows.end(), [&](int a, int b) {
        const Node &x = N[a], &y = N[b];
        if (x.isDir != y.isDir) return x.isDir;
        int c = 0;
        switch (col) {
            case 1: c = x.size < y.size ? -1 : (x.size > y.size ? 1 : 0); break;
            case 2: c = x.mtime < y.mtime ? -1 : (x.mtime > y.mtime ? 1 : 0); break;
            case 3: {
                std::string ex = kz::lowerExt(x.name), ey = kz::lowerExt(y.name);
                c = ex < ey ? -1 : (ex > ey ? 1 : 0);
                break;
            }
            default: break;
        }
        if (c == 0) {
            std::string lx = lower(x.name), ly = lower(y.name);
            c = lx < ly ? -1 : (lx > ly ? 1 : 0);
        }
        return asc ? c < 0 : c > 0;
    });
}

std::vector<std::string> App::selectedPaths(ExplorerTab& t, bool& any) {
    std::vector<std::string> out;
    for (int id : t.selection)
        if (id > 0 && id < (int)t.nodes.size()) out.push_back(t.nodes[id].path);
    any = !out.empty();
    return out;
}

void App::requestPreview(ExplorerTab& t, int node) {
    if (t.previewProg) t.previewProg->cancel = true;
    t.previewProg.reset();
    t.previewNode = node;
    t.preview.clear();
    t.lineStarts.clear();
    t.previewError.clear();
    t.previewLoading = false;
    if (node <= 0 || t.nodes[node].isDir || t.nodes[node].isLink) return;
    t.previewLoading = true;
    auto data = std::make_shared<std::vector<uint8_t>>();
    auto trunc = std::make_shared<bool>(false);
    kz::Source src = t.src;
    std::string path = t.nodes[node].path, pw = t.password;
    int tabId = t.id;
    Job& j = startJob("Preview " + t.nodes[node].name,
        [=](kz::Progress& p, std::string& err) { return kz::readEntry(src, path, pw, kPreviewMax, *data, *trunc, p, err); },
        [this, tabId, node, data, trunc](Job& j) {
            ExplorerTab* t = tabById(tabId);
            if (!t || t->previewNode != node || t->previewProg != j.prog) return;
            t->previewLoading = false;
            t->previewProg.reset();
            if (!j.ok) {
                t->previewError = j.message;
                if (j.prog->needPassword) t->previewError = "Encrypted - a password is needed to preview this file.";
                return;
            }
            t->preview = std::move(*data);
            t->previewTruncated = *trunc;
            t->previewIsText = isTextData(t->preview);
            t->lineStarts.clear();
            t->lineStarts.push_back(0);
            for (size_t i = 0; i < t->preview.size(); ++i)
                if (t->preview[i] == '\n') t->lineStarts.push_back((uint32_t)i + 1);
        },
        false);
    t.previewProg = j.prog;
}

void App::openNested(ExplorerTab& t, int node) {
    if (node <= 0 || t.nodes[node].isDir) return;
    auto data = std::make_shared<std::vector<uint8_t>>();
    auto trunc = std::make_shared<bool>(false);
    kz::Source src = t.src;
    std::string path = t.nodes[node].path, pw = t.password, name = t.nodes[node].name;
    std::string title = t.title + " › " + name;
    startJob("Opening nested " + name,
        [=](kz::Progress& p, std::string& err) {
            p.setStatus("Decompressing " + name);
            return kz::readEntry(src, path, pw, kNestedMax, *data, *trunc, p, err);
        },
        [this, data, trunc, name, title, pw](Job& j) {
            if (!j.ok) {
                log(col::Error, "Could not open " + name + ": " + j.message);
                return;
            }
            if (*trunc) {
                log(col::Error, name + " is larger than 2 GB - extract it first to explore it.");
                return;
            }
            kz::Source s;
            s.mem = std::make_shared<const std::vector<uint8_t>>(std::move(*data));
            s.displayName = name;
            openSource(s, title, pw);
        });
}

// ============================================================================ actions
void App::actionOpen() {
    fileDlg_.open("Open Archive", FileDialog::Mode::OpenFiles, lastDir_, [this](const std::vector<std::string>& p) {
        for (auto& f : p) openArchive(f);
    });
}

void App::onDrop(const std::vector<std::string>& paths) {
    if (cmp_.open) {
        for (auto& p : paths) cmp_.inputs.push_back(p);
        return;
    }
    bool allArchives = true;
    for (auto& p : paths) {
        std::error_code ec;
        bool isFile = std::filesystem::is_regular_file(kz::u8path(p), ec);
        kz::Source s;
        s.path = p;
        if (!isFile || !(kz::looksLikeArchive(p) || kz::kmp::isKmp(s))) allArchives = false;
    }
    if (allArchives) {
        for (auto& p : paths) openArchive(p);
    } else {
        openCompressDialog(paths, false);
    }
}

void App::openCompressDialog(std::vector<std::string> inputs, bool super) {
    cmp_.open = true;
    cmp_.focus = true;
    cmp_.inputs = std::move(inputs);
    cmp_.format = formatIndex(super ? kz::OutFormat::Kmp : kz::OutFormat::Zip);
    const auto& fd = kz::outputFormats()[cmp_.format];
    cmp_.level = fd.defLevel;
    cmp_.pw[0] = cmp_.pw2[0] = 0;
    cmp_.out[0] = 0;
    if (!cmp_.inputs.empty()) {
        std::string first = cmp_.inputs[0];
        while (first.size() > 1 && (first.back() == '/' || first.back() == '\\')) first.pop_back();
        std::string base = cmp_.inputs.size() == 1 ? kz::fileNameOf(first) : "Archive";
        std::error_code ec;
        if (cmp_.inputs.size() == 1 && std::filesystem::is_regular_file(kz::u8path(first), ec) &&
            !fd.singleFile) {
            std::string e = kz::lowerExt(base);
            if (!e.empty()) base = base.substr(0, base.size() - e.size() - 1);
        }
        copyTo(cmp_.out, sizeof cmp_.out,
               kz::pathToUtf8(kz::u8path(parentDir(first)) / kz::u8path(base + fd.ext)));
    }
}

void App::actionExtract(ExplorerTab& t, bool selectionOnly) {
    ext_ = ExtractState();
    ext_.open = true;
    ext_.focus = true;
    ext_.tabId = t.id;
    ext_.selectionOnly = selectionOnly;
    if (selectionOnly) {
        bool any;
        ext_.only = selectedPaths(t, any);
        if (!any) ext_.selectionOnly = false;
        else ext_.stripPrefix = t.nodes[t.cwd].path;
        ext_.subfolder = false;
    }
    std::string base = t.src.inMemory() ? lastDir_ : parentDir(t.src.path);
    copyTo(ext_.dest, sizeof ext_.dest, base);
}

void App::runExtract(int tabId, kz::ExtractOptions o, bool doReveal) {
    ExplorerTab* t = tabById(tabId);
    if (!t) return;
    kz::Source src = t->src;
    std::string title = t->title;
    startJob("Extracting " + title,
        [src, o](kz::Progress& p, std::string& err) { return kz::extractArchive(src, o, p, err); },
        [this, tabId, o, title, doReveal](Job& j) {
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                log(col::Ok, "Extracted " + title + " to " + o.destDir, "", o.destDir);
                toast(col::Ok, "Extracted " + title);
                if (doReveal) reveal(o.destDir);
            } else if (j.prog->needPassword) {
                askPassword(o.password.empty() ? "\"" + title + "\" is encrypted. Enter its password:"
                                               : "Wrong password for \"" + title + "\". Try again:",
                            [this, tabId, o, doReveal](const std::string& pw) {
                                kz::ExtractOptions o2 = o;
                                o2.password = pw;
                                if (ExplorerTab* t = tabById(tabId)) t->password = pw;
                                runExtract(tabId, o2, doReveal);
                            });
            } else {
                log(col::Error, "Extract failed (" + title + "): " + j.message, "", o.destDir);
                toast(col::Error, "Extraction failed: " + j.message);
            }
        });
}

void App::actionTest(ExplorerTab& t) { runTest(t.id); }

void App::runTest(int tabId) {
    ExplorerTab* t = tabById(tabId);
    if (!t) return;
    kz::Source src = t->src;
    std::string title = t->title, pw = t->password;
    startJob("Testing " + title,
        [src, pw](kz::Progress& p, std::string& err) { return kz::testArchive(src, pw, p, err); },
        [this, tabId, title](Job& j) {
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                log(col::Ok, title + ": integrity test passed - every byte decompressed and verified.");
                toast(col::Ok, title + " is intact");
            } else if (j.prog->needPassword) {
                askPassword("Password needed to test \"" + title + "\":", [this, tabId](const std::string& pw) {
                    if (ExplorerTab* t = tabById(tabId)) {
                        t->password = pw;
                        runTest(tabId);
                    }
                });
            } else {
                log(col::Error, title + ": TEST FAILED - " + j.message);
                toast(col::Error, title + ": test failed");
            }
        });
}

void App::actionConvert(ExplorerTab& t) {
    conv_ = ConvertState();
    conv_.open = true;
    conv_.focus = true;
    conv_.tabId = t.id;
    std::string base = t.src.inMemory() ? lastDir_ : parentDir(t.src.path);
    std::string stem = kz::stripArchiveExt(t.src.name());
    if (t.info.isKmp) stem += "-repacked";
    copyTo(conv_.out, sizeof conv_.out, kz::pathToUtf8(kz::u8path(base) / kz::u8path(stem + ".kmp")));
}

void App::runConvert(int tabId, std::string out, int mode) {
    ExplorerTab* t = tabById(tabId);
    if (!t) return;
    kz::Source src = t->src;
    std::string title = t->title, pw = t->password;
    uint64_t before = t->info.archiveSize;
    auto res = std::make_shared<kz::CompressResult>();
    kz::CompressOptions o;
    o.format = kz::OutFormat::Kmp;
    o.level = mode;
    startJob("Super-compressing " + title,
        [=](kz::Progress& p, std::string& err) { return kz::convertToKmp(src, out, o, pw, p, *res, err); },
        [this, res, out, title, before](Job& j) {
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                double saved = before ? 100.0 * (1.0 - double(res->outputBytes) / double(before)) : 0.0;
                char buf[160];
                std::snprintf(buf, sizeof buf, " (%s -> %s, %.1f%% smaller)", kz::humanSize(before).c_str(),
                              kz::humanSize(res->outputBytes).c_str(), saved);
                log(col::Ok, "Super-compressed " + title + buf, out, out);
                toast(col::Ok, "Created " + kz::fileNameOf(out) + buf);
            } else {
                log(col::Error, "Conversion failed: " + j.message);
                toast(col::Error, "Conversion failed");
            }
        });
}

void App::runCompress() {
    const auto& fd = kz::outputFormats()[cmp_.format];
    kz::CompressOptions o;
    o.format = fd.id;
    o.level = fd.id == kz::OutFormat::Kmp ? cmp_.kmpMode : cmp_.level;
    o.password = fd.password ? cmp_.pw : "";
    o.threads = cmp_.threads;
    o.dedup = cmp_.dedup;
    std::vector<std::string> inputs = cmp_.inputs;
    std::string out = cmp_.out;
    bool openAfter = cmp_.openWhenDone;
    lastDir_ = parentDir(out);
    auto res = std::make_shared<kz::CompressResult>();
    startJob(std::string(fd.id == kz::OutFormat::Kmp ? "Super-compressing " : "Compressing ") + kz::fileNameOf(out),
        [=](kz::Progress& p, std::string& err) { return kz::createArchive(inputs, out, o, p, *res, err); },
        [this, res, out, openAfter](Job& j) {
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                log(col::Ok, "Created " + kz::fileNameOf(out) + ": " + res->summary, out, out);
                toast(col::Ok, "Created " + kz::fileNameOf(out) + " - " + kz::humanSize(res->outputBytes));
                if (openAfter) openArchive(out);
            } else {
                log(col::Error, "Compression failed: " + j.message);
                toast(col::Error, "Compression failed: " + j.message);
            }
        });
    cmp_.open = false;
}

// ============================================================================ frame
void App::frame() {
    pumpJobs();
    const float em = ImGui::GetFontSize();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);

    drawMenuBar();
    drawToolbar();

    float activityH = showActivity_ ? em * 9.5f : 0.0f;
    float statusH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##content", ImVec2(0, -(activityH + statusH + ImGui::GetStyle().ItemSpacing.y)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    if (tabs_.empty()) {
        activeTab_ = -1;
        drawWelcome();
    } else if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                                                ImGuiTabBarFlags_FittingPolicyScroll)) {
        int toClose = -1;
        for (auto& tp : tabs_) {
            ExplorerTab& t = *tp;
            bool open = true;
            ImGuiTabItemFlags fl = focusTab_ == t.id ? ImGuiTabItemFlags_SetSelected : 0;
            std::string label = t.title + "###tab" + std::to_string(t.id);
            if (ImGui::BeginTabItem(label.c_str(), &open, fl)) {
                activeTab_ = t.id;
                drawExplorer(t);
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered() && !t.src.inMemory()) ImGui::SetItemTooltip("%s", t.src.path.c_str());
            if (!open) toClose = t.id;
        }
        focusTab_ = -1;
        ImGui::EndTabBar();
        if (toClose >= 0) closeTab(toClose);
    }
    ImGui::EndChild();

    if (showActivity_) drawActivity();

    // status bar
    int running = 0;
    for (auto& j : jobs_)
        if (j->visible && !j->finished) ++running;
    ExplorerTab* at = tabById(activeTab_);
    if (running) ImGui::TextColored(col::Teal, "%d task%s running", running, running > 1 ? "s" : "");
    else ImGui::TextDisabled("Ready");
    if (at && !at->loading && !at->nodes.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("  |  %s  |  %zu entries  |  %s unpacked  |  %d selected", at->info.format.c_str(),
                            at->info.entries.size(), kz::humanSize(at->info.totalSize).c_str(), (int)at->selection.size());
    }
    ImGui::End();

    // Global shortcuts (Ctrl on Windows/Linux, Cmd on macOS).
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) actionOpen();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_N, ImGuiInputFlags_RouteGlobal)) openCompressDialog({}, false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N, ImGuiInputFlags_RouteGlobal)) openCompressDialog({}, true);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal) && activeTab_ >= 0) closeTab(activeTab_);

    drawCompressWindow();
    drawExtractWindow();
    drawConvertWindow();
    drawFormats();
    drawAbout();
    drawPasswordPopup();
    fileDlg_.draw();
    drawToasts();
    if (showDemo_) ImGui::ShowDemoWindow(&showDemo_);
}

void App::drawMenuBar() {
    if (!ImGui::BeginMenuBar()) return;
#ifdef __APPLE__
    const char* mod = "Cmd";
#else
    const char* mod = "Ctrl";
#endif
    std::string kO = std::string(mod) + "+O", kN = std::string(mod) + "+N", kSN = std::string(mod) + "+Shift+N",
                kW = std::string(mod) + "+W";
    ExplorerTab* at = tabById(activeTab_);
    bool ready = at && !at->loading && !at->nodes.empty();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open Archive...", kO.c_str())) actionOpen();
        if (ImGui::MenuItem("New Archive...", kN.c_str())) openCompressDialog({}, false);
        if (ImGui::MenuItem("Super Compress...", kSN.c_str())) openCompressDialog({}, true);
        ImGui::Separator();
        if (ImGui::MenuItem("Close Tab", kW.c_str(), false, at != nullptr)) closeTab(activeTab_);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) glfwSetWindowShouldClose(window_, 1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Archive", ready)) {
        if (ImGui::MenuItem("Extract All...")) actionExtract(*at, false);
        if (ImGui::MenuItem("Extract Selected...", nullptr, false, !at->selection.empty())) actionExtract(*at, true);
        if (ImGui::MenuItem("Test Integrity")) actionTest(*at);
        ImGui::Separator();
        if (ImGui::MenuItem("Super Compress to .kmp...")) actionConvert(*at);
        if (ImGui::MenuItem("Reload")) loadTab(at->id);
        if (!at->src.inMemory() && ImGui::MenuItem("Show in File Manager")) reveal(at->src.path);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Activity Panel", nullptr, &showActivity_);
        ImGui::MenuItem("ImGui Demo", nullptr, &showDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Supported Formats")) showFormats_ = true;
        if (ImGui::MenuItem("About Kompressor")) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void App::drawToolbar() {
    const float em = ImGui::GetFontSize();
    ImVec2 bs(0, em * 1.9f);
    if (ImGui::Button(" Open ", bs)) actionOpen();
    ImGui::SetItemTooltip("Open and explore an archive without extracting it");
    ImGui::SameLine();
    if (ImGui::Button(" Compress ", bs)) openCompressDialog({}, false);
    ImGui::SetItemTooltip("Create a ZIP, 7Z, TAR.XZ, TAR.ZST... archive");
    ImGui::SameLine();
    if (accentButton(" Super Compress ", bs)) openCompressDialog({}, true);
    ImGui::SetItemTooltip("Kompressor's own .kmp format: dedup + content-aware LZMA2/Zstd");

    ExplorerTab* at = tabById(activeTab_);
    bool ready = at && !at->loading && !at->nodes.empty();
    if (ready) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::Button(" Extract All ", bs)) actionExtract(*at, false);
        ImGui::SameLine();
        ImGui::BeginDisabled(at->selection.empty());
        if (ImGui::Button(" Extract Selected ", bs)) actionExtract(*at, true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(" Test ", bs)) actionTest(*at);
        ImGui::SetItemTooltip("Decompress everything in memory and verify checksums");
        ImGui::SameLine();
        if (ImGui::Button(" To .kmp ", bs)) actionConvert(*at);
        ImGui::SetItemTooltip("Re-pack this archive into Kompressor's super format (no temp files)");
    }
    ImGui::Spacing();
}

void App::drawWelcome() {
    const float em = ImGui::GetFontSize();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = std::min(avail.x, em * 40);
    ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f, std::max(em, avail.y * 0.12f)));
    ImGui::BeginGroup();
    ImGui::SetWindowFontScale(2.4f);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Accent);
    float tw = ImGui::CalcTextSize("KOMPRESSOR").x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) * 0.5f);
    ImGui::TextUnformatted("KOMPRESSOR");
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    const char* tag = "Extract anything. Compress everything. Super-compress the rest.";
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - ImGui::CalcTextSize(tag).x) * 0.5f);
    ImGui::TextDisabled("%s", tag);
    ImGui::Dummy(ImVec2(0, em * 1.5f));

    float bw = (w - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    ImVec2 bsz(bw, em * 4.5f);
    if (ImGui::Button("Open Archive\n\nexplore without extracting", bsz)) actionOpen();
    ImGui::SameLine();
    if (ImGui::Button("Compress Files\n\nZIP / 7Z / TAR.* / ISO", bsz)) openCompressDialog({}, false);
    ImGui::SameLine();
    if (accentButton("Super Compress\n\n.kmp - smallest output", bsz)) openCompressDialog({}, true);

    ImGui::Dummy(ImVec2(0, em));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + w, p.y + em * 4);
    dl->AddRect(p, q, u32(col::Accent, 0.6f), em * 0.6f, 0, 2.0f);
    const char* drop = "Drop files, folders or archives anywhere on this window";
    ImVec2 ts = ImGui::CalcTextSize(drop);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (em * 4 - ts.y) * 0.5f), u32(col::Dim), drop);
    ImGui::Dummy(ImVec2(w, em * 4));
    ImGui::Dummy(ImVec2(0, em));
    ImGui::TextDisabled("Reads natively: ZIP, 7Z, RAR/RAR5, TAR, GZ, BZ2, XZ, ZST, LZ4, ISO, CAB, LHA, XAR, CPIO, RPM, DEB, AR, KMP ...");
    if (ImGui::SmallButton("See all supported formats")) showFormats_ = true;
    ImGui::EndGroup();
}

// ============================================================================ explorer
void App::drawExplorer(ExplorerTab& t) {
    const float em = ImGui::GetFontSize();
    if (!debugSelect_.empty() && !t.loading && !t.nodes.empty()) {
        for (int i = 1; i < (int)t.nodes.size(); ++i)
            if (t.nodes[i].path == debugSelect_) {
                t.cwd = std::max(0, t.nodes[i].parent);
                computeRows(t);
                t.selection = {i};
                requestPreview(t, i);
            }
        debugSelect_.clear();
    }
    if (t.loading) {
        ImGui::Dummy(ImVec2(0, em * 4));
        float w = std::min(ImGui::GetContentRegionAvail().x, em * 26);
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - em * 1.6f) * 0.5f);
        spinner(em * 0.8f, col::Accent);
        centerText(("Reading " + t.title + "...").c_str());
        if (t.loadProg) {
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - w) * 0.5f);
            ImGui::ProgressBar(t.loadProg->fraction(), ImVec2(w, 0));
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - em * 6) * 0.5f);
            if (ImGui::Button("Cancel", ImVec2(em * 6, 0))) t.loadProg->cancel = true;
        }
        return;
    }
    if (t.nodes.empty()) {
        ImGui::Dummy(ImVec2(0, em * 3));
        centerText(t.error.empty() ? "Empty archive" : t.error.c_str(), &col::Error);
        ImGui::Dummy(ImVec2(0, em));
        ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - em * 14) * 0.5f);
        if (ImGui::Button("Retry", ImVec2(em * 6.5f, 0))) loadTab(t.id);
        ImGui::SameLine();
        int id = t.id;
        if (ImGui::Button("Password...", ImVec2(em * 7, 0)))
            askPassword("Password for \"" + t.title + "\":", [this, id](const std::string& pw) {
                if (ExplorerTab* t = tabById(id)) {
                    t->password = pw;
                    loadTab(id);
                }
            });
        return;
    }

    // --- navigation row: up, breadcrumb, filter
    ImGui::BeginDisabled(t.cwd == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up) || (t.cwd != 0 && !ImGui::GetIO().WantTextInput &&
                                                    ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        int from = t.cwd;
        t.cwd = std::max(0, t.nodes[t.cwd].parent);
        t.selection = {from};
        computeRows(t);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::vector<int> crumbs;
    for (int n = t.cwd; n >= 0; n = t.nodes[n].parent) crumbs.push_back(n);
    std::reverse(crumbs.begin(), crumbs.end());
    float filterW = em * 14;
    for (size_t i = 0; i < crumbs.size(); ++i) {
        if (i) {
            ImGui::SameLine(0, em * 0.2f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0, em * 0.2f);
        }
        ImGui::PushID((int)i);
        bool last = i + 1 == crumbs.size();
        if (last) ImGui::PushStyleColor(ImGuiCol_Text, col::Accent);
        if (ImGui::SmallButton(t.nodes[crumbs[i]].name.c_str()) && !last) {
            t.cwd = crumbs[i];
            t.selection.clear();
            computeRows(t);
        }
        if (last) ImGui::PopStyleColor();
        ImGui::PopID();
        if (i + 1 < crumbs.size()) ImGui::SameLine();
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - filterW);
    ImGui::SetNextItemWidth(filterW);
    if (ImGui::InputTextWithHint("##filter", "Search in this folder", t.filter, sizeof t.filter)) computeRows(t);

    // --- archive summary line
    {
        const auto& in = t.info;
        double ratio = in.totalSize ? 100.0 * double(in.archiveSize) / double(in.totalSize) : 0.0;
        ImGui::TextDisabled("%s%s%s  |  %s packed  |  %s unpacked  |  %.1f%%", in.format.c_str(),
                            (in.filter.empty() || in.filter == "none") ? "" : " + ", (in.filter == "none") ? "" : in.filter.c_str(),
                            kz::humanSize(in.archiveSize).c_str(), kz::humanSize(in.totalSize).c_str(), ratio);
        if (in.hasEncrypted) {
            ImGui::SameLine();
            ImGui::TextColored(col::Warn, " [encrypted entries]");
        }
    }

    // --- table | splitter | preview
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (t.previewWidth <= 0) t.previewWidth = std::floor(avail.x * 0.36f);
    t.previewWidth = std::clamp(t.previewWidth, em * 12, std::max(em * 12, avail.x - em * 20));
    float split = em * 0.4f;
    float tableW = avail.x - t.previewWidth - split - ImGui::GetStyle().ItemSpacing.x * 2;
    drawTable(t, tableW);
    ImGui::SameLine();
    ImGui::InvisibleButton("##split", ImVec2(split, avail.y));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) t.previewWidth -= ImGui::GetIO().MouseDelta.x;
    {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        float x = (a.x + b.x) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(x, a.y + em), ImVec2(x, b.y - em),
                                            u32(ImGui::IsItemActive() ? col::Accent : col::Dim, 0.35f), 2.0f);
    }
    ImGui::SameLine();
    drawPreview(t);
}

void App::drawTable(ExplorerTab& t, float width) {
    const float em = ImGui::GetFontSize();
    const float rowH = em + 6.0f * scale_;
    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_Hideable;
    int openDir = -1, openFile = -1;
    if (!ImGui::BeginTable("##entries", 4, flags, ImVec2(width, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 6);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
        if (ss->SpecsDirty && ss->SpecsCount > 0) {
            t.sortCol = ss->Specs[0].ColumnIndex;
            t.sortAsc = ss->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            computeRows(t);
            ss->SpecsDirty = false;
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    bool searching = t.filter[0] != 0;
    ImGuiListClipper clip;
    clip.Begin((int)t.rows.size(), rowH);
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            int id = t.rows[r];
            Node& n = t.nodes[id];
            ImGui::TableNextRow(0, rowH);
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(id);
            bool sel = t.selection.count(id) > 0;
            ImVec2 cp = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                                    ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) {
                if (io.KeyShift && t.anchor >= 0 && t.anchor < (int)t.rows.size()) {
                    t.selection.clear();
                    for (int k = std::min(t.anchor, r); k <= std::max(t.anchor, r); ++k) t.selection.insert(t.rows[k]);
                } else if (io.KeyCtrl || io.KeySuper) {
                    if (sel) t.selection.erase(id);
                    else t.selection.insert(id);
                    t.anchor = r;
                } else {
                    t.selection = {id};
                    t.anchor = r;
                }
                if (t.selection.size() == 1 && !n.isDir && t.previewNode != id) requestPreview(t, id);
                if (t.selection.size() != 1) requestPreview(t, -1);
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (n.isDir) openDir = id;
                    else openFile = id;
                }
            }
            if (ImGui::BeginPopupContextItem("##ctx")) {
                if (!sel) t.selection = {id};
                if (ImGui::MenuItem("Extract...")) actionExtract(t, true);
                if (!n.isDir && ImGui::MenuItem("Preview")) requestPreview(t, id);
                if (!n.isDir && ImGui::MenuItem("Open as archive", nullptr, false, true)) openNested(t, id);
                if (n.isDir && ImGui::MenuItem("Open folder")) openDir = id;
                ImGui::Separator();
                if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(n.path.c_str());
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos(ImVec2(cp.x, cp.y + (rowH - em) * 0.5f));
            drawIcon(iconFor(n), em);
            ImGui::SameLine(0, em * 0.45f);
            if (n.isDir) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.92f, 0.78f, 1.0f));
            ImGui::TextUnformatted(searching ? n.path.c_str() : n.name.c_str());
            if (n.isDir) ImGui::PopStyleColor();
            if (n.encrypted) {
                ImGui::SameLine();
                ImGui::TextColored(col::Warn, "[locked]");
            }
            ImGui::TableSetColumnIndex(1);
            if (n.isDir) ImGui::TextDisabled("%u items", n.files);
            else ImGui::TextUnformatted(kz::humanSize(n.size).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", kz::formatTime(n.mtime).c_str());
            ImGui::TableSetColumnIndex(3);
            if (n.isDir) ImGui::TextDisabled("Folder");
            else if (n.isLink) ImGui::TextDisabled("Link");
            else {
                std::string e = kz::lowerExt(n.name);
                for (auto& c : e) c = char(std::toupper((unsigned char)c));
                ImGui::TextDisabled("%s", e.empty() ? "File" : e.c_str());
            }
            ImGui::PopID();
        }
    }
    if (t.rows.empty()) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled(searching ? "No matches" : "Empty folder");
    }
    ImGui::EndTable();

    // keyboard: Enter opens, Ctrl+A selects all
    if (!io.WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && t.selection.size() == 1) {
            int id = *t.selection.begin();
            if (t.nodes[id].isDir) openDir = id;
            else openFile = id;
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, ImGuiInputFlags_RouteGlobal))
            t.selection = std::set<int>(t.rows.begin(), t.rows.end());
    }
    if (openDir >= 0) {
        t.cwd = openDir;
        t.selection.clear();
        t.filter[0] = 0;
        requestPreview(t, -1);
        computeRows(t);
    } else if (openFile >= 0) {
        if (kz::looksLikeArchive(t.nodes[openFile].name)) openNested(t, openFile);
        else requestPreview(t, openFile);
    }
}

void App::drawPreview(ExplorerTab& t) {
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##preview", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_Borders);
    int nid = t.previewNode;
    if (nid <= 0 || nid >= (int)t.nodes.size()) {
        // Overview card for the archive or the current selection.
        uint64_t sz = 0;
        uint32_t files = 0;
        for (int id : t.selection) {
            sz += t.nodes[id].size;
            files += t.nodes[id].isDir ? t.nodes[id].files : 1;
        }
        if (!t.selection.empty()) {
            ImGui::SeparatorText("Selection");
            ImGui::Text("%d items, %u files", (int)t.selection.size(), files);
            ImGui::Text("%s uncompressed", kz::humanSize(sz).c_str());
            ImGui::Spacing();
            if (accentButton("Extract selection...")) actionExtract(t, true);
            ImGui::Spacing();
        }
        ImGui::SeparatorText("Archive");
        const auto& in = t.info;
        ImGui::TextWrapped("%s", t.title.c_str());
        ImGui::TextDisabled("Format");
        ImGui::SameLine(em * 7);
        ImGui::TextWrapped("%s", in.format.c_str());
        ImGui::TextDisabled("Filter");
        ImGui::SameLine(em * 7);
        ImGui::TextUnformatted(in.filter.c_str());
        ImGui::TextDisabled("Entries");
        ImGui::SameLine(em * 7);
        ImGui::Text("%zu", in.entries.size());
        ImGui::TextDisabled("Unpacked");
        ImGui::SameLine(em * 7);
        ImGui::TextUnformatted(kz::humanSize(in.totalSize).c_str());
        ImGui::TextDisabled("Packed");
        ImGui::SameLine(em * 7);
        ImGui::TextUnformatted(kz::humanSize(in.archiveSize).c_str());
        if (in.totalSize) {
            float frac = float(double(in.archiveSize) / double(in.totalSize));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, frac < 0.5f ? col::Ok : col::Accent);
            char ov[64];
            std::snprintf(ov, sizeof ov, "%.1f%% of original", frac * 100.0f);
            ImGui::ProgressBar(std::min(frac, 1.0f), ImVec2(-1, 0), ov);
            ImGui::PopStyleColor();
        }
        if (!in.extra.empty()) ImGui::TextColored(col::Teal, "%s", in.extra.c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("Select a file to preview it. Double-click a nested archive to explore it.");
        ImGui::EndChild();
        return;
    }

    Node& n = t.nodes[nid];
    drawIcon(iconFor(n), em * 1.6f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextWrapped("%s", n.name.c_str());
    ImGui::TextDisabled("%s  |  %s", kz::humanSize(n.size).c_str(), kz::formatTime(n.mtime).c_str());
    ImGui::EndGroup();
    if (ImGui::SmallButton("Extract...")) {
        t.selection = {nid};
        actionExtract(t, true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Open as archive")) openNested(t, nid);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 5.5f);
    const char* modes[] = {"Auto", "Text", "Hex"};
    ImGui::Combo("##mode", &t.previewMode, modes, 3);
    ImGui::Separator();

    if (t.previewLoading) {
        spinner(em * 0.6f, col::Accent);
        ImGui::SameLine();
        ImGui::TextDisabled("Decompressing...");
        if (t.previewProg) {
            ImGui::ProgressBar(t.previewProg->fraction(), ImVec2(-1, 0));
            if (ImGui::SmallButton("Cancel")) {
                t.previewProg->cancel = true;
            }
        }
        ImGui::EndChild();
        return;
    }
    if (!t.previewError.empty()) {
        ImGui::TextColored(col::Error, "%s", t.previewError.c_str());
        int id = t.id;
        if (ImGui::Button("Enter password..."))
            askPassword("Password for \"" + t.title + "\":", [this, id, nid](const std::string& pw) {
                if (ExplorerTab* t = tabById(id)) {
                    t->password = pw;
                    requestPreview(*t, nid);
                }
            });
        ImGui::EndChild();
        return;
    }
    if (t.preview.empty()) {
        ImGui::TextDisabled("(empty file)");
        ImGui::EndChild();
        return;
    }
    if (t.previewTruncated) ImGui::TextColored(col::Warn, "Showing the first %s", kz::humanSize(t.preview.size()).c_str());

    bool asText = t.previewMode == 1 || (t.previewMode == 0 && t.previewIsText);
    if (mono_) ImGui::PushFont(mono_);
    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const char* base = (const char*)t.preview.data();
    if (asText) {
        ImGuiListClipper clip;
        clip.Begin((int)t.lineStarts.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                size_t b = t.lineStarts[i];
                size_t e = i + 1 < (int)t.lineStarts.size() ? t.lineStarts[i + 1] - 1 : t.preview.size();
                if (e > b && base[e - 1] == '\r') --e;
                if (e - b > 4000) e = b + 4000;
                ImGui::TextUnformatted(base + b, base + e);
            }
    } else {
        int rows = (int)((t.preview.size() + 15) / 16);
        ImGuiListClipper clip;
        clip.Begin(rows);
        char line[128];
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                size_t off = (size_t)r * 16;
                int k = std::snprintf(line, sizeof line, "%08zX  ", off);
                for (int c = 0; c < 16; ++c) {
                    if (off + c < t.preview.size()) k += std::snprintf(line + k, sizeof line - k, "%02X ", t.preview[off + c]);
                    else k += std::snprintf(line + k, sizeof line - k, "   ");
                    if (c == 7) line[k++] = ' ';
                }
                line[k++] = ' ';
                for (int c = 0; c < 16 && off + c < t.preview.size(); ++c) {
                    uint8_t ch = t.preview[off + c];
                    line[k++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
                }
                line[k] = 0;
                ImGui::TextUnformatted(line);
            }
    }
    ImGui::EndChild();
    if (mono_) ImGui::PopFont();
    ImGui::EndChild();
}

// ============================================================================ panels & windows
void App::drawActivity() {
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##activity", ImVec2(0, em * 9.5f), ImGuiChildFlags_Borders);
    // Running jobs
    bool anyJob = false;
    for (auto& jp : jobs_) {
        Job& j = *jp;
        if (!j.visible || j.finished) continue;
        anyJob = true;
        ImGui::PushID(j.id);
        float f = j.prog->fraction();
        std::string st = j.prog->status();
        double el = ImGui::GetTime() - j.started;
        char ov[256];
        if (j.prog->total.load())
            std::snprintf(ov, sizeof ov, "%.0f%%  -  %s", f * 100.0f, kz::formatDuration(el).c_str());
        else
            std::snprintf(ov, sizeof ov, "%s", kz::formatDuration(el).c_str());
        ImGui::TextUnformatted(j.title.c_str());
        ImGui::SameLine(em * 18);
        ImGui::ProgressBar(j.prog->total.load() ? f : -1.0f * (float)ImGui::GetTime(), ImVec2(em * 16, 0), ov);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) j.prog->cancel = true;
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.c_str());
        ImGui::PopID();
    }
    if (anyJob) ImGui::Separator();
    // Log
    ImGui::BeginChild("##log");
    for (size_t i = 0; i < log_.size(); ++i) {
        auto& l = log_[i];
        ImGui::PushID((int)i);
        ImGui::TextColored(l.color, "%s", l.text.c_str());
        if (!l.openPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open")) openArchive(l.openPath);
        }
        if (!l.revealPath.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Show")) reveal(l.revealPath);
        }
        ImGui::PopID();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndChild();
}

void App::drawCompressWindow() {
    if (!cmp_.open) return;
    const float em = ImGui::GetFontSize();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(em * 40, em * 38), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (cmp_.focus) {
        ImGui::SetNextWindowFocus();
        cmp_.focus = false;
    }
    const auto& formats = kz::outputFormats();
    const auto& fd = formats[cmp_.format];
    bool isKmp = fd.id == kz::OutFormat::Kmp;
    if (!ImGui::Begin(isKmp ? "Super Compress###compress" : "Create Archive###compress", &cmp_.open,
                      ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Files & folders");
    ImGui::BeginChild("##inputs", ImVec2(0, em * 7), ImGuiChildFlags_Borders);
    if (cmp_.inputs.empty()) ImGui::TextDisabled("Add files or folders, or drop them onto the window.");
    int remove = -1;
    for (size_t i = 0; i < cmp_.inputs.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::SmallButton("x")) remove = (int)i;
        ImGui::SameLine();
        ImGui::TextUnformatted(cmp_.inputs[i].c_str());
        ImGui::PopID();
    }
    if (remove >= 0) cmp_.inputs.erase(cmp_.inputs.begin() + remove);
    ImGui::EndChild();
    if (ImGui::Button("Add files / folders...")) {
        std::string start = cmp_.inputs.empty() ? lastDir_ : parentDir(cmp_.inputs.back());
        fileDlg_.open("Add to Archive", FileDialog::Mode::OpenAny, start, [this](const std::vector<std::string>& p) {
            bool wasEmpty = cmp_.inputs.empty() && cmp_.out[0] == 0;
            for (auto& x : p) cmp_.inputs.push_back(x);
            if (wasEmpty) {
                int f = cmp_.format, k = cmp_.kmpMode;
                openCompressDialog(cmp_.inputs, false);
                cmp_.format = f;
                cmp_.kmpMode = k;
                copyTo(cmp_.out, sizeof cmp_.out, kz::withFormatExtension(cmp_.out, kz::outputFormats()[f].id));
            }
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) cmp_.inputs.clear();

    ImGui::SeparatorText("Format");
    ImGui::SetNextItemWidth(em * 14);
    if (ImGui::BeginCombo("##fmt", fd.name)) {
        for (size_t i = 0; i < formats.size(); ++i) {
            bool s = (int)i == cmp_.format;
            if (ImGui::Selectable(formats[i].name, s)) {
                cmp_.format = (int)i;
                cmp_.level = formats[i].defLevel;
                if (cmp_.out[0])
                    copyTo(cmp_.out, sizeof cmp_.out, kz::withFormatExtension(cmp_.out, formats[i].id));
            }
            ImGui::SameLine(em * 8);
            ImGui::TextDisabled("%s", formats[i].description);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", fd.description);

    if (isKmp) {
        struct M { const char* name; const char* desc; };
        static const M modes[] = {
            {"Fast", "Zstd + dedup. Faster than ZIP, usually smaller."},
            {"Balanced", "Zstd-19 with long-range matching. Fast to extract."},
            {"Ultra", "LZMA2 + x86/ARM64 code filters + text tuning. Recommended."},
            {"Max", "Tries every codec on every block and keeps the smallest. Slow."},
        };
        for (int i = 0; i < 4; ++i) {
            ImGui::RadioButton(modes[i].name, &cmp_.kmpMode, i);
            ImGui::SameLine(em * 7);
            ImGui::TextDisabled("%s", modes[i].desc);
        }
        ImGui::Checkbox("Deduplicate identical data (content-defined chunking)", &cmp_.dedup);
    } else if (fd.minLevel < fd.maxLevel) {
        ImGui::SetNextItemWidth(em * 14);
        ImGui::SliderInt("Level", &cmp_.level, fd.minLevel, fd.maxLevel);
        ImGui::SameLine();
        ImGui::TextDisabled(cmp_.level <= fd.minLevel + (fd.maxLevel - fd.minLevel) / 3 ? "faster" : (cmp_.level >= fd.maxLevel - (fd.maxLevel - fd.minLevel) / 4 ? "smallest" : "balanced"));
    }
    ImGui::SetNextItemWidth(em * 14);
    ImGui::SliderInt("Threads", &cmp_.threads, 0, (int)kz::hardwareThreads());
    ImGui::SameLine();
    ImGui::TextDisabled(cmp_.threads == 0 ? "(auto)" : "");

    bool pwMismatch = false;
    if (fd.password) {
        ImGui::SeparatorText("Encryption (AES-256)");
        ImGui::SetNextItemWidth(em * 14);
        ImGui::InputTextWithHint("##pw", "Password (optional)", cmp_.pw, sizeof cmp_.pw, ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(em * 14);
        ImGui::InputTextWithHint("##pw2", "Confirm password", cmp_.pw2, sizeof cmp_.pw2, ImGuiInputTextFlags_Password);
        pwMismatch = std::strcmp(cmp_.pw, cmp_.pw2) != 0;
        if (pwMismatch && cmp_.pw2[0]) ImGui::TextColored(col::Error, "Passwords do not match");
    }

    ImGui::SeparatorText("Output");
    ImGui::SetNextItemWidth(-em * 6);
    ImGui::InputText("##out", cmp_.out, sizeof cmp_.out);
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
        std::string cur = cmp_.out;
        fileDlg_.open("Save Archive As", FileDialog::Mode::SaveFile, cur.empty() ? lastDir_ : parentDir(cur),
                      [this](const std::vector<std::string>& p) {
                          std::string o = p[0];
                          kz::OutFormat f;
                          if (kz::formatFromFileName(o, f)) cmp_.format = formatIndex(f);
                          else o = kz::withFormatExtension(o, kz::outputFormats()[cmp_.format].id);
                          copyTo(cmp_.out, sizeof cmp_.out, o);
                      },
                      cur.empty() ? std::string("Archive") + fd.ext : kz::fileNameOf(cur));
    }
    ImGui::Checkbox("Open the archive when finished", &cmp_.openWhenDone);

    ImGui::Spacing();
    bool can = !cmp_.inputs.empty() && cmp_.out[0] && !(fd.password && pwMismatch);
    ImGui::BeginDisabled(!can);
    if (accentButton(isKmp ? "Super Compress" : "Create Archive", ImVec2(em * 12, em * 2))) runCompress();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 6, em * 2))) cmp_.open = false;
    ImGui::End();
}

void App::drawExtractWindow() {
    if (!ext_.open) return;
    ExplorerTab* t = tabById(ext_.tabId);
    if (!t) {
        ext_.open = false;
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 34, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ext_.focus) {
        ImGui::SetNextWindowFocus();
        ext_.focus = false;
    }
    if (!ImGui::Begin("Extract###extract", &ext_.open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("%s", t->title.c_str());
    if (ext_.selectionOnly) ImGui::TextDisabled("%zu selected item(s)", ext_.only.size());
    else ImGui::TextDisabled("All %zu entries", t->info.entries.size());
    ImGui::SeparatorText("Destination");
    ImGui::SetNextItemWidth(em * 25);
    ImGui::InputText("##dest", ext_.dest, sizeof ext_.dest);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        fileDlg_.open("Extract To", FileDialog::Mode::PickFolder, ext_.dest,
                      [this](const std::vector<std::string>& p) { copyTo(ext_.dest, sizeof ext_.dest, p[0]); });
    }
    std::string stem = kz::stripArchiveExt(t->src.name());
    std::string sub = "Create subfolder \"" + stem + "\"";
    ImGui::Checkbox(sub.c_str(), &ext_.subfolder);
    ImGui::Checkbox("Skip files that already exist", &ext_.skipExisting);
    ImGui::Checkbox("Show the folder when done", &ext_.reveal);
    std::string dest = ext_.dest;
    if (ext_.subfolder) dest = kz::pathToUtf8(kz::u8path(dest) / kz::u8path(stem));
    ImGui::TextDisabled("-> %s", dest.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(ext_.dest[0] == 0);
    if (accentButton("Extract", ImVec2(em * 9, em * 1.9f))) {
        kz::ExtractOptions o;
        o.destDir = dest;
        o.only = ext_.only;
        o.stripPrefix = ext_.stripPrefix;
        o.password = t->password;
        o.skipExisting = ext_.skipExisting;
        lastDir_ = ext_.dest;
        runExtract(t->id, o, ext_.reveal);
        ext_.open = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 6, em * 1.9f))) ext_.open = false;
    ImGui::End();
}

void App::drawConvertWindow() {
    if (!conv_.open) return;
    ExplorerTab* t = tabById(conv_.tabId);
    if (!t) {
        conv_.open = false;
        return;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (conv_.focus) {
        ImGui::SetNextWindowFocus();
        conv_.focus = false;
    }
    if (!ImGui::Begin("Super Compress Archive###convert", &conv_.open,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Re-pack \"%s\" into .kmp. Entries stream straight from the source - nothing is extracted to disk.",
                       t->title.c_str());
    ImGui::SeparatorText("Mode");
    const char* names[] = {"Fast", "Balanced", "Ultra (recommended)", "Max"};
    for (int i = 0; i < 4; ++i) {
        ImGui::RadioButton(names[i], &conv_.mode, i);
        if (i < 3) ImGui::SameLine();
    }
    ImGui::SeparatorText("Output");
    ImGui::SetNextItemWidth(em * 26);
    ImGui::InputText("##cout", conv_.out, sizeof conv_.out);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string cur = conv_.out;
        fileDlg_.open("Save .kmp As", FileDialog::Mode::SaveFile, parentDir(cur),
                      [this](const std::vector<std::string>& p) {
                          copyTo(conv_.out, sizeof conv_.out, kz::withFormatExtension(p[0], kz::OutFormat::Kmp));
                      },
                      kz::fileNameOf(cur));
    }
    ImGui::Spacing();
    if (accentButton("Super Compress", ImVec2(em * 11, em * 1.9f))) {
        runConvert(t->id, conv_.out, conv_.mode);
        conv_.open = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 6, em * 1.9f))) conv_.open = false;
    ImGui::End();
}

void App::drawPasswordPopup() {
    if (pwd_.request && !fileDlg_.isOpen()) {
        ImGui::OpenPopup("Password Required");
        pwd_.request = false;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Password Required", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(em * 24);
        ImGui::TextUnformatted(pwd_.prompt.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(em * 18);
        bool enter = ImGui::InputText("##pw", pwd_.buf, sizeof pwd_.buf,
                                      ImGuiInputTextFlags_EnterReturnsTrue | (pwd_.show ? 0 : ImGuiInputTextFlags_Password));
        ImGui::SameLine();
        ImGui::Checkbox("Show", &pwd_.show);
        if (accentButton("OK", ImVec2(em * 6, 0)) || enter) {
            std::string pw = pwd_.buf;
            std::memset(pwd_.buf, 0, sizeof pwd_.buf);
            auto cb = std::move(pwd_.onOk);
            pwd_.onOk = nullptr;
            ImGui::CloseCurrentPopup();
            if (cb) cb(pw);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 6, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            std::memset(pwd_.buf, 0, sizeof pwd_.buf);
            pwd_.onOk = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::drawFormats() {
    if (!showFormats_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 44, em * 30), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("Supported Formats", &showFormats_)) {
        ImGui::SeparatorText("Open, explore & extract");
        if (ImGui::BeginTable("##rf", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            for (auto& f : kz::readableFormats()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(f.first.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", f.second.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Create");
        if (ImGui::BeginTable("##wf", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            for (auto& f : kz::outputFormats()) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(f.name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextDisabled("%s", f.ext);
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", f.description);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("RAR can be read but not written (the RAR format is proprietary).");
    }
    ImGui::End();
}

void App::drawAbout() {
    if (!showAbout_) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("About Kompressor", &showAbout_, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(col::Accent, "Kompressor %s", KZ_VERSION);
        ImGui::TextUnformatted("Universal archive extractor, compressor and super compressor.");
        ImGui::Spacing();
        ImGui::PushTextWrapPos(em * 34);
        ImGui::TextDisabled("Engines: %s", kz::libraryVersions().c_str());
        ImGui::TextDisabled("KMP format: FastCDC dedup, XXH3, LZMA2 (+BCJ x86/ARM64), Zstandard.");
        ImGui::TextDisabled("UI: Dear ImGui %s, GLFW %s", IMGUI_VERSION, glfwGetVersionString());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void App::drawToasts() {
    const float em = ImGui::GetFontSize();
    double now = ImGui::GetTime();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [&](const Toast& t) { return now - t.time > 5.0; }),
                  toasts_.end());
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float y = vp->WorkPos.y + vp->WorkSize.y - em * 3;
    int i = 0;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it, ++i) {
        float age = float(now - it->time);
        float alpha = age > 4.0f ? 5.0f - age : std::min(1.0f, age * 6.0f);
        ImGui::SetNextWindowBgAlpha(0.95f * alpha);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - em, y), ImGuiCond_Always, ImVec2(1, 1));
        std::string name = "##toast" + std::to_string(i);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::Begin(name.c_str(), nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushTextWrapPos(em * 26);
        ImGui::TextColored(it->color, "%s", it->text.c_str());
        ImGui::PopTextWrapPos();
        y -= ImGui::GetWindowHeight() + em * 0.5f;
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

}  // namespace kzgui
