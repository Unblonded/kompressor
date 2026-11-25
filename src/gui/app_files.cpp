// Moving files between archives and the desktop: opening entries in their default app, copying and
// dragging them out, and adding / deleting entries (the archive is rewritten in place).
#include <GLFW/glfw3.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include "app.hpp"
#include "app_util.hpp"
#include "native_files.hpp"
#include "theme.hpp"

namespace kzgui {

namespace fs = std::filesystem;

namespace {

float emPx() { return ImGui::GetFontSize(); }

// %TEMP%/Kompressor: entries opened, copied or dragged out are extracted below it.
fs::path tempRoot() {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    if (ec) p = fs::current_path(ec);
    return p / "Kompressor";
}

std::string makeTempDir(const char* kind) {
    static std::atomic<unsigned> seq{0};
    auto stamp = (unsigned long long)std::chrono::system_clock::now().time_since_epoch().count();
    std::error_code ec;
    for (int i = 0; i < 100; ++i) {
        fs::path d = tempRoot() / (std::string(kind) + "-" + std::to_string(stamp) + "-" + std::to_string(++seq));
        if (fs::create_directories(d, ec)) return kz::pathToUtf8(d);
    }
    return {};
}

// Leftovers from earlier sessions. Files handed to other apps (or still being pasted) need to outlive
// us, so only folders untouched for a day go.
void cleanOldTemp() {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (auto it = fs::directory_iterator(tempRoot(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        auto t = fs::last_write_time(it->path(), e2);
        if (!e2 && now - t > std::chrono::hours(24)) fs::remove_all(it->path(), e2);
    }
}

struct Group {
    std::string parent;              // archive folder the entries sit in ("" = top level)
    std::vector<std::string> paths;  // entries (folders include everything below them)
};

// The given nodes grouped by folder (search results can come from several), so each group extracts
// with its folder stripped and lands directly in the target directory.
std::vector<Group> groupNodes(const ExplorerTab& t, const std::set<int>& ids) {
    std::vector<Group> out;
    for (int id : ids) {
        if (id <= 0 || id >= (int)t.nodes.size()) continue;
        const Node& n = t.nodes[id];
        std::string parent = n.parent > 0 ? t.nodes[n.parent].path : "";
        auto g = std::find_if(out.begin(), out.end(), [&](const Group& x) { return x.parent == parent; });
        if (g == out.end()) {
            out.push_back({parent, {}});
            g = out.end() - 1;
        }
        g->paths.push_back(n.path);
    }
    return out;
}

bool extractGroups(const kz::Source& src, const std::string& pw, const std::vector<Group>& groups, const std::string& dest,
                   kz::Progress& prog, std::string& err) {
    for (auto& g : groups) {
        kz::ExtractOptions o;
        o.destDir = dest;
        o.only = g.paths;
        o.stripPrefix = g.parent;
        o.password = pw;
        if (!kz::extractArchive(src, o, prog, err)) return false;
    }
    return true;
}

// What extraction actually produced (names can be adjusted for the file system).
std::vector<std::string> topLevel(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(kz::u8path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        out.push_back(kz::pathToUtf8(it->path()));
    std::sort(out.begin(), out.end());
    return out;
}

std::string countText(size_t n, const char* what) {
    return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
}

}  // namespace

void startTempCleanup() { std::thread(cleanOldTemp).detach(); }

// ---------------------------------------------------------------------------------------------- open
void App::openEntry(ExplorerTab& t, int node) {
    if (node <= 0 || node >= (int)t.nodes.size() || t.nodes[node].isDir) return;
    std::string dir = makeTempDir("open");
    if (dir.empty()) {
        toast(col::Error, "Could not create a temporary folder");
        return;
    }
    auto groups = groupNodes(t, {node});
    kz::Source src = t.src;
    std::string pw = t.password, name = t.nodes[node].name;
    int tabId = t.id;
    startJob("Opening " + name,
        [=](kz::Progress& p, std::string& err) {
            p.setStatus("Decompressing " + name);
            return extractGroups(src, pw, groups, dir, p, err);
        },
        [this, dir, name, tabId, node](Job& j) {
            if (j.ok) {
                auto files = topLevel(dir);
                if (files.empty()) {
                    toast(col::Error, "Could not open " + name);
                    return;
                }
                native::openWithDefaultApp(files.front());
                log(col::Dim, "Opened " + name + " (a temporary copy; changes are not saved into the archive)", "", files.front());
            } else if (j.prog->needPassword) {
                std::error_code ec;
                fs::remove_all(kz::u8path(dir), ec);
                askPassword("\"" + name + "\" is encrypted. Enter the archive's password:", [this, tabId, node](const std::string& pw) {
                    if (ExplorerTab* t = tabById(tabId)) {
                        t->password = pw;
                        openEntry(*t, node);
                    }
                });
            } else {
                log(col::Error, "Could not open " + name + ": " + j.message);
                toast(col::Error, "Could not open " + name);
            }
        });
}

// ---------------------------------------------------------------------------------------------- copy out
void App::copySelection(ExplorerTab& t) {
    if (t.selection.empty()) return;
    if (!native::fileTransferSupported()) {
        toast(col::Warn, "Copying files to the clipboard isn't supported here - use Extract");
        return;
    }
    std::string dir = makeTempDir("copy");
    if (dir.empty()) {
        toast(col::Error, "Could not create a temporary folder");
        return;
    }
    auto groups = groupNodes(t, t.selection);
    kz::Source src = t.src;
    std::string pw = t.password, title = t.title;
    int tabId = t.id;
    size_t n = t.selection.size();
    startJob("Copying from " + title,
        [=](kz::Progress& p, std::string& err) { return extractGroups(src, pw, groups, dir, p, err); },
        [this, dir, n, tabId, title](Job& j) {
            if (j.ok) {
                auto files = topLevel(dir);
                if (!files.empty() && native::setClipboardFiles(window_, files)) {
                    toast(col::Ok, "Copied " + countText(n, "item") + " - paste them anywhere");
                    log(col::Ok, "Copied " + countText(n, "item") + " from " + title + " to the clipboard");
                } else {
                    toast(col::Error, "Could not put the files on the clipboard");
                }
            } else if (j.prog->needPassword) {
                askPassword("\"" + title + "\" is encrypted. Enter its password:", [this, tabId](const std::string& pw) {
                    if (ExplorerTab* t = tabById(tabId)) {
                        t->password = pw;
                        copySelection(*t);
                    }
                });
            } else {
                log(col::Error, "Copy failed (" + title + "): " + j.message);
                toast(col::Error, "Copy failed: " + j.message);
            }
        });
}

// ---------------------------------------------------------------------------------------------- drag out
void App::syncDropJob() {
    if (dropJob_ && !dropJob_->visible && dropStarted_.load()) {
        dropJob_->visible = true;
        dropJob_->started = ImGui::GetTime();
    }
}

void App::afterFrame() {
    if (pendingDrag_ < 0 || dragging_) return;
    int tabId = pendingDrag_;
    pendingDrag_ = -1;
    dragSelectionOut(tabId);
    pendingDrag_ = -1;  // a frame drawn during the drag may have asked again
}

void App::dragSelectionOut(int tabId) {
    ExplorerTab* t = tabById(tabId);
    if (!t || t->selection.empty() || !native::fileTransferSupported()) return;
    std::string dir = makeTempDir("drag");
    if (dir.empty()) return;
    auto groups = groupNodes(*t, t->selection);
    std::vector<std::string> predicted;
    for (int id : t->selection)
        if (id > 0 && id < (int)t->nodes.size()) predicted.push_back(kz::pathToUtf8(kz::u8path(dir) / kz::u8path(t->nodes[id].name)));
    kz::Source src = t->src;
    std::string pw = t->password, title = t->title, err;
    size_t n = predicted.size();
    bool needPw = false;

    // The drop's extraction is listed with the other jobs (hidden until the files are released over a
    // target). No pause or cancel buttons: the drag still holds the mouse while it runs, so Esc cancels.
    auto job = std::make_unique<Job>();
    Job* dj = job.get();
    dj->id = nextJobId_++;
    dj->title = "Dropping " + countText(n, "item") + " out of " + title + " (Esc cancels)";
    dj->visible = false;
    dj->controls = false;
    jobs_.push_back(std::move(job));
    dropJob_ = dj;
    dropStarted_ = false;
    std::shared_ptr<kz::Progress> prog = dj->prog;

    dragging_ = true;
    bool dropped = native::dragFilesOut(window_, predicted,
        [&]() -> std::vector<std::string> {
            prog->setStatus("Extracting...");
            dropStarted_ = true;
            glfwPostEmptyEvent();
            if (!extractGroups(src, pw, groups, dir, *prog, err)) {
                needPw = prog->needPassword;
                return {};
            }
            return topLevel(dir);
        },
        [prog] { prog->cancel = true; });
    dragging_ = false;
    dropJob_ = nullptr;
    dj->ok = dropped;
    dj->finished = true;  // the next frame takes it off the list
    glfwPostEmptyEvent();

    if (dropped) {
        log(col::Ok, "Copied " + countText(n, "item") + " out of " + title);
        return;
    }
    std::error_code ec;
    fs::remove_all(kz::u8path(dir), ec);
    if (prog->cancel) {
        log(col::Dim, "Drop cancelled");
    } else if (needPw) {
        askPassword("\"" + title + "\" is encrypted. Enter its password, then drag the files again:", [this, tabId](const std::string& pw) {
            if (ExplorerTab* t = tabById(tabId)) t->password = pw;
        });
    } else if (!err.empty()) {
        log(col::Error, "Could not extract for the drop: " + err);
        toast(col::Error, "Drop failed: " + err);
    }
}

// ---------------------------------------------------------------------------------------------- add / delete
void App::askAddFiles(ExplorerTab& t, std::vector<std::string> paths, bool offerOpen) {
    if (paths.empty() || t.loading || t.nodes.empty()) return;
    if (!t.editable) {
        toast(col::Warn, t.editWhy.empty() ? "This archive can't be changed" : t.editWhy);
        return;
    }
    if (t.editing) {
        toast(col::Warn, "\"" + t.title + "\" is still being updated");
        return;
    }
    for (auto& p : paths)
        while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back();
    std::string under = t.cwd > 0 ? t.nodes[t.cwd].path : "";
    int replaced = 0;
    std::vector<std::string> select;
    for (auto& p : paths) {
        std::string name = kz::fileNameOf(p);
        select.push_back(under.empty() ? name : under + "/" + name);
        for (int c : t.nodes[t.cwd].children)
            if (t.nodes[c].name == name) ++replaced;
    }
    std::string text = paths.size() == 1 ? "Add \"" + kz::fileNameOf(paths[0]) + "\"" : "Add " + countText(paths.size(), "item");
    text += " to \"" + t.title + "\"" + (under.empty() ? "" : ", in " + under) + "?";
    if (replaced) text += "\n\n" + countText((size_t)replaced, "item") + " with the same name will be replaced.";
    kz::OutFormat f;
    if (kz::formatFromFileName(lower(kz::fileNameOf(t.src.path)), f) && f == kz::OutFormat::Rar)
        text += "\n\nRAR's compression is proprietary, so the updated archive is stored without compression.";
    text += "\n\nThe archive is rewritten with the change, which takes a while for big archives.";
    int tabId = t.id;
    std::function<void()> openInstead;
    if (offerOpen)
        openInstead = [this, paths] {
            for (auto& p : paths) openArchive(p);
        };
    askConfirm("Add to archive", text, "Add", (int)Ico::Plus, col::Accent,
               [this, tabId, paths, under, select] {
                   kz::ArchiveEdit e;
                   e.add = paths;
                   e.addUnder = under;
                   runEdit(tabId, e, select);
               },
               offerOpen ? "Open instead" : "", openInstead);
}

void App::askDeleteSelection(ExplorerTab& t) {
    if (t.selection.empty() || t.loading) return;
    if (!t.editable) {
        toast(col::Warn, t.editWhy.empty() ? "This archive can't be changed" : t.editWhy);
        return;
    }
    if (t.editing) {
        toast(col::Warn, "\"" + t.title + "\" is still being updated");
        return;
    }
    bool any;
    std::vector<std::string> paths = selectedPaths(t, any);
    if (!any) return;
    std::string text;
    if (paths.size() == 1) {
        const Node& n = t.nodes[*t.selection.begin()];
        text = "Delete \"" + n.name + "\"" + (n.isDir ? " and everything in it" : "") + " from \"" + t.title + "\"?";
    } else {
        text = "Delete " + countText(paths.size(), "item") + " from \"" + t.title + "\"?";
    }
    text += "\n\nThis can't be undone.";
    int tabId = t.id;
    askConfirm("Delete from archive", text, "Delete", (int)Ico::Trash, col::Error, [this, tabId, paths] {
        kz::ArchiveEdit e;
        e.remove = paths;
        runEdit(tabId, e, {});
    });
}

void App::pasteFiles(ExplorerTab& t) {
    std::vector<std::string> files = native::clipboardFiles();
    if (files.empty()) {
        toast(col::Dim, "There are no copied files to paste");
        return;
    }
    askAddFiles(t, files);
}

void App::pickFilesToAdd(ExplorerTab& t, bool folders) {
    int tabId = t.id;
    fileDlg_.open(folders ? "Add Folders" : "Add Files", folders ? FileDialog::Mode::AddFolders : FileDialog::Mode::AddFiles, lastDir_,
                  [this, tabId](const std::vector<std::string>& p) {
                      if (!p.empty()) lastDir_ = parentDir(p.front());
                      if (ExplorerTab* t = tabById(tabId)) askAddFiles(*t, p);
                  });
}

void App::runEdit(int tabId, kz::ArchiveEdit edit, std::vector<std::string> selectAfter) {
    ExplorerTab* t = tabById(tabId);
    if (!t) return;
    t->editing = true;
    if (t->previewProg) t->previewProg->cancel = true;
    std::string restore = t->cwd > 0 && t->cwd < (int)t->nodes.size() ? t->nodes[t->cwd].path : "";
    kz::Source src = t->src;
    std::string pw = t->info.hasEncrypted ? t->password : "", title = t->title;
    bool adding = !edit.add.empty();
    size_t count = adding ? edit.add.size() : edit.remove.size();
    startJob((adding ? "Adding to " : "Deleting from ") + title,
        [src, edit, pw](kz::Progress& p, std::string& err) {
            kz::CompressResult r;
            return kz::editArchive(src, edit, pw, p, r, err);
        },
        [this, tabId, edit, selectAfter, restore, title, adding, count](Job& j) {
            ExplorerTab* t = tabById(tabId);
            if (t) t->editing = false;
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                std::string what = (adding ? "Added " : "Deleted ") + countText(count, "item");
                log(col::Ok, what + (adding ? " to " : " from ") + title);
                toast(col::Ok, what);
                if (t) {
                    t->restoreDir = restore;
                    t->restoreSelect = selectAfter;
                    loadTab(tabId);
                }
            } else if (j.prog->needPassword && j.message != "Cancelled") {
                askPassword("\"" + title + "\" is encrypted. Enter its password to change it:",
                            [this, tabId, edit, selectAfter](const std::string& pw) {
                                if (ExplorerTab* t = tabById(tabId)) {
                                    t->password = pw;
                                    t->info.hasEncrypted = true;
                                    runEdit(tabId, edit, selectAfter);
                                }
                            });
            } else {
                log(col::Error, "Could not update " + title + ": " + j.message);
                toast(col::Error, "Update failed: " + j.message);
            }
        });
}

// ---------------------------------------------------------------------------------------------- confirm
void App::askConfirm(const std::string& title, const std::string& text, const std::string& okLabel, int icon,
                     const ImVec4& color, std::function<void()> onOk, const std::string& altLabel, std::function<void()> onAlt) {
    confirm_.request = true;
    confirm_.title = title;
    confirm_.text = text;
    confirm_.ok = okLabel;
    confirm_.alt = altLabel;
    confirm_.icon = icon;
    confirm_.color = color;
    confirm_.onOk = std::move(onOk);
    confirm_.onAlt = std::move(onAlt);
}

void App::drawConfirmPopup() {
    if (confirm_.request && !fileDlg_.isOpen() && !ImGui::IsPopupOpen("Password Required")) {
        ImGui::OpenPopup("##confirm");
        confirm_.request = false;
    }
    const float em = emPx();
    float W = em * 32;
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(W, 0), ImVec2(W, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.5f, em * 1.3f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.075f, 0.078f, 0.102f, 1.0f));
    bool open = ImGui::BeginPopupModal("##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    if (ImGui::GetWindowViewport() == ImGui::GetMainViewport()) {
        dl->PushClipRectFullScreen();
        softShadow(dl, wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::GetStyle().WindowRounding, em * 1.6f, 0.55f);
        dl->PopClipRect();
    }
    float inner = W - em * 3.0f;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
    dialogHeader(confirm_.title.c_str(), nullptr, (Ico)confirm_.icon, confirm_.color, W, nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, col::Dim);
    ImGui::TextWrapped("%s", confirm_.text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, em * 0.7f));
    float obw = std::max(em * 7.5f, ImGui::CalcTextSize(confirm_.ok.c_str()).x + em * 3.5f), cbw = em * 6.5f;
    float abw = confirm_.alt.empty() ? 0.0f : ImGui::CalcTextSize(confirm_.alt.c_str()).x + em * 2.4f;
    float bh = ImGui::GetFrameHeight() * 1.15f;
    alignRight(obw + cbw + em * 0.6f + (abw > 0 ? abw + em * 0.6f : 0.0f));
    bool alt = false;
    if (abw > 0) {
        alt = secondaryButton(confirm_.alt.c_str(), ImVec2(abw, bh));
        ImGui::SameLine(0, em * 0.6f);
    }
    bool cancel = secondaryButton("Cancel", ImVec2(cbw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    ImGui::SameLine(0, em * 0.6f);
    bool ok = primaryButton(confirm_.ok.c_str(), (Ico)confirm_.icon, ImVec2(obw, bh)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
              ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (ok || cancel || alt) {
        auto cb = std::move(ok ? confirm_.onOk : confirm_.onAlt);
        confirm_.onOk = nullptr;
        confirm_.onAlt = nullptr;
        ImGui::CloseCurrentPopup();
        if ((ok || alt) && cb) cb();
    }
    ImGui::EndPopup();
}

}  // namespace kzgui
