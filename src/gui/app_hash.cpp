// The Checksums window: CRC32 / MD5 / SHA-1 / SHA-256 of the files in an archive, or of files and
// folders on disk, saved or copied as a hash list, and checked against a .sha256 / .md5 / .sha1 / .sfv list.
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "app.hpp"
#include "app_util.hpp"
#include "theme.hpp"

namespace kzgui {

namespace {

float emPx() { return ImGui::GetFontSize(); }

const kz::HashKind kKinds[4] = {kz::kHashCrc32, kz::kHashMd5, kz::kHashSha1, kz::kHashSha256};

unsigned maskOf(const bool algo[4]) {
    unsigned m = 0;
    for (int i = 0; i < 4; ++i)
        if (algo[i]) m |= kKinds[i];
    return m;
}

bool containsNoCase(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(hay.begin(), hay.end(), needle, needle + std::strlen(needle),
                          [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != hay.end();
}

}  // namespace

void App::actionHash(ExplorerTab& t, bool selectionOnly) {
    if (hash_.prog) hash_.prog->cancel = true;
    bool algo[4];
    std::copy(hash_.algo, hash_.algo + 4, algo);
    hash_ = HashState();
    std::copy(algo, algo + 4, hash_.algo);
    hash_.open = hash_.focus = true;
    hash_.tabId = t.id;
    hash_.src = t.src;
    hash_.title = t.title;
    hash_.password = t.password;
    if (selectionOnly) {
        bool any = false;
        hash_.only = selectedPaths(t, any);
        if (hash_.only.size() == 1) hash_.scope = hash_.only[0];
        else if (any) hash_.scope = std::to_string(hash_.only.size()) + " selected items";
    }
    if (hash_.scope.empty()) hash_.scope = "all files";
    if (!maskOf(hash_.algo)) hash_.algo[3] = true;
    runHash(maskOf(hash_.algo));
}

void App::hashFiles(const std::vector<std::string>& paths) {
    if (paths.empty()) return;
    if (hash_.prog) hash_.prog->cancel = true;
    bool algo[4];
    std::copy(hash_.algo, hash_.algo + 4, algo);
    hash_ = HashState();
    std::copy(algo, algo + 4, hash_.algo);
    hash_.open = hash_.focus = true;
    hash_.files = paths;
    hash_.title = paths.size() == 1 ? kz::fileNameOf(paths[0]) : std::to_string(paths.size()) + " items";
    std::error_code ec;
    bool folder = paths.size() == 1 && std::filesystem::is_directory(kz::u8path(paths[0]), ec);
    hash_.scope = folder ? "every file in the folder" : paths.size() == 1 ? "file on disk" : "files on disk";
    if (!maskOf(hash_.algo)) hash_.algo[3] = true;
    runHash(maskOf(hash_.algo));
}

void App::runHash(unsigned which) {
    if (hash_.prog) hash_.prog->cancel = true;
    int run = ++hash_.runId;
    hash_.running = true;
    hash_.error.clear();
    auto out = std::make_shared<std::vector<kz::EntryHash>>();
    kz::Source src = hash_.src;
    std::vector<std::string> only = hash_.only;
    std::string pw = hash_.password, title = hash_.title;
    std::vector<std::string> files = hash_.files;
    auto base = std::make_shared<std::string>();
    Job& j = startJob("Checksums for " + title,
        [=](kz::Progress& p, std::string& err) {
            return files.empty() ? kz::hashArchive(src, only, pw, which, *out, p, err) : kz::hashFiles(files, which, *out, *base, p, err);
        },
        [this, run, which, out, base, title](Job& j) {
            if (run != hash_.runId) return;  // a newer run replaced this one
            hash_.running = false;
            hash_.prog.reset();
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                hash_.results = std::move(*out);
                if (!hash_.files.empty()) hash_.base = *base;
                hash_.computed = which;
                size_t bad = 0;
                for (auto& h : hash_.results) bad += !h.error.empty();
                log(bad ? col::Warn : col::Ok, title + ": checksums of " + std::to_string(hash_.results.size()) + " file(s)" +
                                                   (bad ? ", " + std::to_string(bad) + " could not be read" : ""));
                if (!hash_.listed.empty()) recheckHashes();
            } else if (j.prog->needPassword) {
                askPassword("Password needed to read \"" + title + "\":", [this, which](const std::string& pw) {
                    hash_.password = pw;
                    if (ExplorerTab* t = tabById(hash_.tabId)) t->password = pw;
                    runHash(which);
                });
            } else if (!j.prog->cancel) {
                hash_.error = j.message;
                log(col::Error, title + ": checksums failed - " + j.message);
            }
        });
    hash_.prog = j.prog;
}

void App::recheckHashes() {
    hash_.notListed.clear();
    hash_.checks = kz::checkHashes(hash_.results, hash_.listed, &hash_.notListed);
    hash_.checkOf.clear();
    for (size_t i = 0; i < hash_.checks.size(); ++i)
        if (!hash_.checks[i].entry.empty()) hash_.checkOf[hash_.checks[i].entry] = (int)i;
    size_t ok = 0;
    for (auto& c : hash_.checks) ok += c.state == kz::HashCheck::Ok;
    bool good = ok == hash_.checks.size();
    std::string name = kz::fileNameOf(hash_.listPath);
    log(good ? col::Ok : col::Error, hash_.title + " checked against " + name + ": " + std::to_string(ok) + " of " +
                                         std::to_string(hash_.checks.size()) + " match");
    toast(good ? col::Ok : col::Error, good ? "Every listed file matches" : "Some files don't match the list");
}

void App::pickHashList() {
    fileDlg_.open("Check Against a Checksum List", FileDialog::Mode::AddFiles, lastDir_,
                  [this](const std::vector<std::string>& p) { loadHashList(p[0]); });
}

void App::loadHashList(const std::string& path) {
    std::ifstream f(kz::u8path(path), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto listed = kz::parseHashList(text);
    if (listed.empty()) {
        toast(col::Error, "No checksums found in " + kz::fileNameOf(path));
        return;
    }
    lastDir_ = parentDir(path);
    hash_.listPath = path;
    hash_.listed = std::move(listed);
    unsigned need = kz::hashKindsOfList(hash_.listed);
    for (int i = 0; i < 4; ++i)
        if (need & kKinds[i]) hash_.algo[i] = true;
    if ((need & ~hash_.computed) || hash_.results.empty()) {
        hash_.checks.clear();
        hash_.checkOf.clear();
        runHash(hash_.computed | need | maskOf(hash_.algo));
    } else {
        recheckHashes();
    }
}

void App::saveHashList(kz::HashKind k) {
    // On disk the list goes in the folder its names are relative to, so sha256sum -c works there.
    bool onDisk = !hash_.files.empty();
    std::string stem = onDisk ? (hash_.files.size() == 1 ? hash_.title : kz::fileNameOf(hash_.base)) : kz::stripArchiveExt(hash_.title);
    if (stem.empty()) stem = "checksums";
    std::string name = stem + kz::hashListExt(k);
    std::string dir = onDisk ? hash_.base : hash_.src.inMemory() ? lastDir_ : parentDir(hash_.src.path);
    fileDlg_.open("Save Checksum List", FileDialog::Mode::SaveFile, dir, [this, k](const std::vector<std::string>& p) {
        std::string text = kz::formatHashList(hash_.results, k);
        std::ofstream f(kz::u8path(p[0]), std::ios::binary);
        f.write(text.data(), (std::streamsize)text.size());
        if (!f) {
            toast(col::Error, "Could not write " + kz::fileNameOf(p[0]));
            return;
        }
        lastDir_ = parentDir(p[0]);
        log(col::Ok, std::string(kz::hashName(k)) + " list saved: " + p[0], "", p[0]);
    }, name);
}

void App::drawHashWindow() {
    if (!hash_.open) {
        if (hash_.prog) {  // closed while running
            hash_.prog->cancel = true;
            hash_.prog.reset();
            hash_.running = false;
        }
        return;
    }
    const float em = emPx();
    if (hash_.focus) {
        ImGui::SetNextWindowFocus();
        hash_.focus = false;
    }
    std::string sub = hash_.title + "  -  " + hash_.scope;
    float W = em * 62;
    if (!beginDialog("###hashes", "Checksums", sub.c_str(), Ico::Hash, &hash_.open, W, col::Accent2)) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;
    const unsigned shown = hash_.computed & maskOf(hash_.algo);

    // Algorithms
    caption("ALGORITHMS");
    static const char* names[4] = {"CRC32", "MD5", "SHA-1", "SHA-256"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, em * 0.4f);
        ImGui::PushID(i);
        if (pill(names[i], hash_.algo[i], em * 5.5f)) hash_.algo[i] = !hash_.algo[i];
        ImGui::PopID();
    }
    unsigned want = maskOf(hash_.algo);
    bool needRun = (want & ~hash_.computed) != 0 || (hash_.results.empty() && hash_.error.empty() && !hash_.running);
    if (needRun && want && !hash_.running) {
        ImGui::SameLine(0, em * 0.9f);
        if (primaryButton("Calculate", Ico::Hash, ImVec2(em * 8, 0))) runHash(want | hash_.computed);
    }

    // Status
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    if (hash_.running && hash_.prog) {
        float f = hash_.prog->total.load() ? hash_.prog->fraction() : -1.0f;
        progressBar(f, ImVec2(inner, em * 0.36f), col::Accent);
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "Reading %s", hash_.prog->status().c_str());
        ImGui::PopFont();
    } else if (!hash_.error.empty()) {
        icon(Ico::Error, em, col::Error);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::TextColored(col::Error, "%s", hash_.error.c_str());
    } else if (!hash_.checks.empty()) {
        size_t n[4] = {};
        for (auto& c : hash_.checks) n[c.state]++;
        bool good = n[0] == hash_.checks.size();
        icon(good ? Ico::Check : Ico::Warn, em, good ? col::Ok : col::Error);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::PushFont(fonts().bold);
        ImGui::TextColored(good ? col::Ok : col::Error, good ? "Every listed file matches" : "Some files don't match");
        ImGui::PopFont();
        ImGui::SameLine(0, em * 0.6f);
        std::string s = std::to_string(n[0]) + " OK";
        if (n[1]) s += "  -  " + std::to_string(n[1]) + " different";
        if (n[2]) s += "  -  " + std::to_string(n[2]) + " missing";
        if (n[3]) s += "  -  " + std::to_string(n[3]) + " unreadable";
        if (!hash_.notListed.empty()) s += "  -  " + std::to_string(hash_.notListed.size()) + " not in the list";
        s += "   (" + kz::fileNameOf(hash_.listPath) + ")";
        ImGui::TextColored(col::Dim, "%s", s.c_str());
    } else {
        uint64_t total = 0;
        for (auto& h : hash_.results) total += h.size;
        ImGui::TextColored(col::Dim, "%zu file%s, %s", hash_.results.size(), hash_.results.size() == 1 ? "" : "s",
                           kz::humanSize(total).c_str());
    }

    // Filter
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    float toggleW = hash_.checks.empty() ? 0.0f : em * 11;
    searchInput("##hfilter", "Filter by name or checksum", hash_.filter, sizeof hash_.filter, inner - toggleW);
    if (!hash_.checks.empty()) {
        ImGui::SameLine(0, em * 0.8f);
        toggle("Problems only", &hash_.problemsOnly);
    }

    // Rows: the files hashed, then listed names that weren't found.
    struct Row {
        const kz::EntryHash* h;
        const kz::HashCheck* c;
        bool notListed;
    };
    std::vector<Row> rows;
    rows.reserve(hash_.results.size() + 8);
    bool checking = !hash_.checks.empty();
    auto keep = [&](const std::string& path, const kz::EntryHash* h, bool problem) {
        if (hash_.problemsOnly && !problem) return false;
        if (!hash_.filter[0] || containsNoCase(path, hash_.filter)) return true;
        if (h)
            for (kz::HashKind k : kKinds)
                if (containsNoCase(h->get(k), hash_.filter)) return true;
        return false;
    };
    for (auto& h : hash_.results) {
        const kz::HashCheck* c = nullptr;
        if (checking) {
            auto it = hash_.checkOf.find(h.path);
            if (it != hash_.checkOf.end()) c = &hash_.checks[it->second];
        }
        bool problem = !h.error.empty() || (checking && (!c || c->state != kz::HashCheck::Ok));
        if (keep(h.path, &h, problem)) rows.push_back({&h, c, checking && !c});
    }
    for (auto& c : hash_.checks)
        if (c.state == kz::HashCheck::Missing && keep(c.name, nullptr, true)) rows.push_back({nullptr, &c, false});

    // Table
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    int hashCols = 0;
    for (kz::HashKind k : kKinds) hashCols += (shown & k) != 0;
    int cols = 2 + hashCols + (checking ? 1 : 0);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(em * 0.5f, em * 0.25f));
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
                         ImGuiTableFlags_BordersInnerV;
    if (ImGui::BeginTable("##hashes", cols, tf, ImVec2(inner, em * 24))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        if (checking) ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, em * 6);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, em * 16);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 5);
        static const int digits[4] = {8, 32, 40, 64};
        ImGui::PushFont(mono_);
        for (int i = 0; i < 4; ++i)
            if (shown & kKinds[i])
                ImGui::TableSetupColumn(names[i], ImGuiTableColumnFlags_WidthFixed,
                                        std::max(ImGui::CalcTextSize(std::string(digits[i], '0').c_str()).x, em * 4.5f));
        ImGui::PopFont();
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const Row& row = rows[r];
                const std::string& path = row.h ? row.h->path : row.c->name;
                ImGui::TableNextRow();
                ImGui::PushID(r);
                int ci = 0;
                if (checking) {
                    ImGui::TableSetColumnIndex(ci++);
                    if (row.notListed) chip("Not listed", col::Dim);
                    else if (row.c) {
                        static const char* word[] = {"OK", "Different", "Missing", "Unreadable"};
                        const ImVec4 colr[] = {col::Ok, col::Error, col::Warn, col::Warn};
                        chip(word[row.c->state], colr[row.c->state], row.c->state == kz::HashCheck::Mismatch);
                        if (row.c->state == kz::HashCheck::Mismatch && ImGui::IsItemHovered())
                            ImGui::SetTooltip("List:     %s\nActual:   %s", row.c->expected.c_str(), row.c->actual.c_str());
                        if (row.c->state == kz::HashCheck::Missing && ImGui::IsItemHovered())
                            ImGui::SetTooltip(hash_.files.empty() ? "The list names this file, but the archive doesn't have it"
                                                                           : "The list names this file, but it isn't there");
                    }
                }
                ImGui::TableSetColumnIndex(ci++);
                ImGui::Selectable("##row", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
                if (ImGui::BeginPopupContextItem("##hctx")) {
                    if (row.h)
                        for (kz::HashKind k : kKinds) {
                            const std::string& v = row.h->get(k);
                            if (v.empty()) continue;
                            std::string label = std::string("Copy ") + kz::hashName(k);
                            if (ImGui::MenuItem(label.c_str())) ImGui::SetClipboardText(v.c_str());
                        }
                    if (ImGui::MenuItem("Copy name")) ImGui::SetClipboardText(path.c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(path.c_str());
                if (ImGui::IsItemHovered() && ImGui::CalcTextSize(path.c_str()).x > ImGui::GetColumnWidth()) ImGui::SetTooltip("%s", path.c_str());
                ImGui::TableSetColumnIndex(ci++);
                if (row.h) ImGui::TextColored(col::Dim, "%s", kz::humanSize(row.h->size).c_str());
                for (int i = 0; i < 4; ++i) {
                    if (!(shown & kKinds[i])) continue;
                    ImGui::TableSetColumnIndex(ci++);
                    if (!row.h) continue;
                    if (!row.h->error.empty()) {
                        ImGui::TextColored(col::Error, "%s", row.h->error.c_str());
                        continue;
                    }
                    const std::string& v = row.h->get(kKinds[i]);
                    bool bad = row.c && row.c->kind == kKinds[i] && row.c->state == kz::HashCheck::Mismatch;
                    ImGui::PushFont(mono_);
                    ImGui::TextColored(bad ? col::Error : col::Text, "%s", v.c_str());
                    ImGui::PopFont();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    if (rows.empty() && !hash_.running && !hash_.results.empty()) {
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Faint, "Nothing matches the filter");
        ImGui::PopFont();
    }

    // Footer
    dialogFooter();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - em * 1.5f, p.y), ImVec2(p.x + inner + em * 1.5f, p.y), u32(col::Line));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
    }
    bool have = !hash_.results.empty() && !hash_.running;
    float bh = ImGui::GetFrameHeight() * 1.15f;
    if (secondaryButton("Check against list...", ImVec2(em * 11, bh), !hash_.running)) pickHashList();
    ImGui::SetItemTooltip("Compare with a .sha256, .md5, .sha1 or .sfv list (sha256sum, BSD or SFV style)");
    float cw = em * 7, sw = em * 9;
    ImGui::SameLine();
    alignRight(cw + sw + em * 0.6f);
    if (secondaryButton("Copy", ImVec2(cw, bh), have && shown)) ImGui::OpenPopup("##hcopy");
    ImGui::SameLine(0, em * 0.6f);
    ImGui::BeginDisabled(!have || !shown);
    if (primaryButton("Save list...", Ico::Hash, ImVec2(sw, bh))) {
        int n = 0;
        kz::HashKind only = kz::kHashSha256;
        for (kz::HashKind k : kKinds)
            if (shown & k) ++n, only = k;
        if (n == 1) saveHashList(only);
        else ImGui::OpenPopup("##hsave");
    }
    ImGui::EndDisabled();
    for (int which = 0; which < 2; ++which) {
        if (!ImGui::BeginPopup(which ? "##hsave" : "##hcopy")) continue;
        for (kz::HashKind k : kKinds) {
            if (!(shown & k)) continue;
            std::string label = std::string(kz::hashName(k)) + " list (" + kz::hashListExt(k) + ")";
            if (!ImGui::MenuItem(label.c_str())) continue;
            if (which) saveHashList(k);
            else {
                ImGui::SetClipboardText(kz::formatHashList(hash_.results, k).c_str());
                toast(col::Ok, std::string(kz::hashName(k)) + " list copied");
            }
        }
        ImGui::EndPopup();
    }
    endDialog();
}

}  // namespace kzgui
