#include "app.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <unordered_map>

#include "kmp.hpp"
#include "app_util.hpp"
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

bool isTextData(const std::vector<uint8_t>& d) {
    size_t n = std::min<size_t>(d.size(), 64 * 1024), ctrl = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = d[i];
        if (c == 0) return false;
        if (c < 9 || (c > 13 && c < 32 && c != 27)) ++ctrl;
    }
    return ctrl * 50 <= n;
}

}  // namespace

// ---------------------------------------------------------------- shared helpers (app_util.hpp)
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

void copyTo(char* dst, size_t n, const std::string& s) {
    std::strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = 0;
}

std::string parentDir(const std::string& p) { return kz::pathToUtf8(kz::u8path(p).parent_path()); }

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

int formatIndex(kz::OutFormat f) {
    auto& fs = kz::outputFormats();
    for (size_t i = 0; i < fs.size(); ++i)
        if (fs[i].id == f) return (int)i;
    return 0;
}


// ============================================================================ lifecycle
App::App(GLFWwindow* window, ImFont* mono, float scale) : window_(window), mono_(mono), scale_(scale) {
    lastDir_ = FileDialog::homeDir();
    fileDlg_.setParent(window);
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
    return !toasts_.empty() || animating();
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
    log_.push_back({(double)std::time(nullptr), c, text, open, rev});
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
    fileDlg_.open("Open Archive", FileDialog::Mode::OpenArchives, lastDir_, [this](const std::vector<std::string>& p) {
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

}  // namespace kzgui
