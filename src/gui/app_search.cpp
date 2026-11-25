// The Find in Files window: text (UTF-8 or UTF-16) or hex bytes inside the files of an archive, streamed
// without extracting anything, with results listed per file as they're found. Clicking a hit shows the
// file in the explorer and scrolls its preview to the spot.
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.hpp"
#include "app_util.hpp"
#include "theme.hpp"

namespace kzgui {

namespace {

float emPx() { return ImGui::GetFontSize(); }

// Entry paths as the explorer's tree stores them: forward slashes, no leading "./" or "/".
std::string normPath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.compare(0, 2, "./") == 0) p.erase(0, 2);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    while (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string plural(uint64_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

}  // namespace

void App::actionSearch(ExplorerTab& t, bool selectionOnly) {
    if (search_.prog) search_.prog->cancel = true;
    // Keep what was typed and the options; the rest belongs to the archive being searched.
    SearchState keep;
    std::memcpy(keep.query, search_.query, sizeof keep.query);
    std::memcpy(keep.names, search_.names, sizeof keep.names);
    keep.mode = search_.mode;
    keep.matchCase = search_.matchCase;
    keep.runId = search_.runId;
    search_ = std::move(keep);
    search_.open = search_.focus = search_.focusInput = true;
    search_.tabId = t.id;
    search_.src = t.src;
    search_.title = t.title;
    search_.password = t.password;
    if (selectionOnly) {
        bool any = false;
        search_.only = selectedPaths(t, any);
        if (search_.only.size() == 1) search_.scope = search_.only[0];
        else if (any) search_.scope = std::to_string(search_.only.size()) + " selected items";
    }
    if (search_.scope.empty()) search_.scope = "all files";
}

void App::runSearch() {
    if (search_.prog) search_.prog->cancel = true;
    search_.prog.reset();
    search_.files.clear();
    search_.collapsed.clear();
    search_.total = 0;
    search_.error.clear();
    search_.done = false;
    search_.running = false;
    kz::SearchOptions so;
    so.text = search_.query;
    so.hex = search_.mode == 1;
    so.matchCase = search_.matchCase;
    so.names = search_.names;
    std::vector<uint8_t> bytes;
    if (!kz::searchPattern(so, bytes, search_.error)) return;
    search_.searched = so.text;
    int run = ++search_.runId;
    search_.running = true;
    auto feed = std::make_shared<SearchState::Feed>();
    search_.feed = feed;
    kz::Source src = search_.src;
    std::vector<std::string> only = search_.only;
    std::string pw = search_.password, title = search_.title;
    Job& j = startJob("Searching " + title,
        [=](kz::Progress& p, std::string& err) {
            return kz::searchArchive(src, only, pw, so, [feed](kz::FileHits&& f) {
                std::lock_guard<std::mutex> l(feed->mu);
                feed->files.push_back(std::move(f));
            }, p, err);
        },
        [this, run, title](Job& j) {
            if (run != search_.runId) return;  // a newer search replaced this one
            search_.running = false;
            search_.prog.reset();
            for (auto& w : j.prog->warnings()) log(col::Warn, w);
            if (j.ok) {
                search_.done = true;
            } else if (j.prog->needPassword) {
                askPassword("Password needed to search \"" + title + "\":", [this](const std::string& pw) {
                    search_.password = pw;
                    if (ExplorerTab* t = tabById(search_.tabId)) t->password = pw;
                    runSearch();
                });
            } else if (!j.prog->cancel) {
                search_.error = j.message;
                log(col::Error, title + ": search failed - " + j.message);
            }
        });
    search_.prog = j.prog;
}

void App::showSearchHit(const kz::FileHits& f, const kz::SearchHit* h) {
    ExplorerTab* t = tabById(search_.tabId);
    if (!t || t->loading) {
        toast(col::Warn, "The archive's tab was closed");
        return;
    }
    std::string want = normPath(f.path);
    int node = -1;
    for (int i = 1; i < (int)t->nodes.size() && node < 0; ++i)
        if (t->nodes[i].path == want || normPath(t->nodes[i].path) == want) node = i;
    if (node < 0) {
        toast(col::Warn, "Couldn't find " + kz::fileNameOf(want) + " in the archive's tree");
        return;
    }
    focusTab_ = t->id;
    navigateTo(*t, std::max(0, t->nodes[node].parent), node);
    t->selection = {node};
    t->anchor = node;
    if (t->previewNode != node || (!t->previewLoading && t->preview.empty())) requestPreview(*t, node);
    if (h) {
        t->previewMark = h->offset;
        t->previewJump = 3;
    }
}

void App::drawSearchWindow() {
    if (!search_.open) {
        if (search_.prog) {  // closed while running
            search_.prog->cancel = true;
            search_.prog.reset();
            search_.running = false;
        }
        return;
    }
    // Take what the worker found since the last frame.
    if (search_.feed) {
        std::lock_guard<std::mutex> l(search_.feed->mu);
        for (auto& f : search_.feed->files) {
            search_.total += f.count;
            search_.files.push_back(std::move(f));
        }
        search_.feed->files.clear();
    }
    if (debugSearchJump_ && search_.done) {
        debugSearchJump_ = false;
        if (!search_.files.empty() && !search_.files[0].hits.empty()) showSearchHit(search_.files[0], &search_.files[0].hits.back());
    }

    const float em = emPx();
    if (search_.focus) {
        ImGui::SetNextWindowFocus();
        search_.focus = false;
    }
    std::string sub = search_.title + "  -  " + search_.scope;
    float W = em * 62;
    if (!beginDialog("###search", "Find in Files", sub.c_str(), Ico::Search, &search_.open, W, col::Accent2)) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;
    float bh = ImGui::GetFrameHeight();

    // Query and the Search / Stop button
    bool go = false;
    float bw = em * 7.5f;
    if (search_.focusInput) {
        ImGui::SetKeyboardFocusHere();
        search_.focusInput = false;
    }
    ImGuiID qid = ImGui::GetID("##sq");
    searchInput("##sq", search_.mode == 1 ? "Bytes in hex, e.g. 4D 5A 90 00" : "Text to find in the files", search_.query,
                sizeof search_.query, inner - bw - em * 0.6f);
    if (ImGui::GetActiveID() == qid && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        go = true;
    ImGui::SameLine(0, em * 0.6f);
    if (search_.running) {
        if (secondaryButton("Stop", ImVec2(bw, bh)) && search_.prog) search_.prog->cancel = true;
    } else {
        ImGui::BeginDisabled(!search_.query[0]);
        if (primaryButton("Search", Ico::Search, ImVec2(bw, bh))) go = true;
        ImGui::EndDisabled();
    }

    // Options
    ImGui::Dummy(ImVec2(0, em * 0.15f));
    float rowX = ImGui::GetCursorPosX();
    static const char* modes[2] = {"Text", "Hex bytes"};
    segmented("##smode", &search_.mode, modes, 2);
    ImGui::SetItemTooltip("Text is found as UTF-8 and as UTF-16; hex finds exact bytes");
    if (search_.mode == 0) {
        ImGui::SameLine(0, em * 1.0f);
        toggle("Match case", &search_.matchCase);
    }
    ImGui::SameLine(0, em * 1.2f);
    float nw = std::max(em * 10, inner - (ImGui::GetCursorPosX() - rowX));
    ImGuiID nid = ImGui::GetID("##snames");
    searchInput("##snames", "Only files named, e.g. *.txt;*.ini", search_.names, sizeof search_.names, nw);
    if (ImGui::GetActiveID() == nid && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
        go = true;
    if (go && search_.query[0]) runSearch();

    // Status
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    std::string found = plural(search_.total, "match", "matches") + " in " + plural(search_.files.size(), "file", "files");
    if (search_.running && search_.prog) {
        float f = search_.prog->total.load() ? search_.prog->fraction() : -1.0f;
        progressBar(f, ImVec2(inner, em * 0.36f), col::Accent);
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Dim, "%s so far  -  reading %s", found.c_str(), search_.prog->status().c_str());
        ImGui::PopFont();
    } else if (!search_.error.empty()) {
        icon(Ico::Error, em, col::Error);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::TextColored(col::Error, "%s", search_.error.c_str());
    } else if (search_.done) {
        bool any = search_.total > 0;
        icon(any ? Ico::Check : Ico::Info, em, any ? col::Ok : col::Dim);
        ImGui::SameLine(0, em * 0.4f);
        ImGui::PushFont(fonts().bold);
        if (any) ImGui::TextColored(col::Text, "%s", found.c_str());
        else ImGui::TextColored(col::Dim, "No matches for \"%s\"", search_.searched.c_str());
        ImGui::PopFont();
        size_t errors = 0;
        for (auto& f : search_.files) errors += !f.error.empty();
        if (errors) {
            ImGui::SameLine(0, em * 0.6f);
            ImGui::TextColored(col::Warn, "%s", (plural(errors, "file", "files") + " could not be read completely").c_str());
        }
    } else {
        ImGui::PushFont(fonts().caption);
        ImGui::TextColored(col::Faint, "Press Enter to search. Nothing is extracted to disk.");
        ImGui::PopFont();
    }

    // Results: a header per file, then its hits.
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    struct Row {
        int file, hit;  // hit: -1 the file's header, -2 "N more"
    };
    std::vector<Row> rows;
    for (int i = 0; i < (int)search_.files.size(); ++i) {
        const auto& f = search_.files[i];
        rows.push_back({i, -1});
        if (search_.collapsed.count(f.path)) continue;
        for (int h = 0; h < (int)f.hits.size(); ++h) rows.push_back({i, h});
        if (f.count > f.hits.size()) rows.push_back({i, -2});
    }
    ImVec2 area(inner, em * 24);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + area.x, a.y + area.y), u32(col::Bg), em * 0.5f);
    ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + area.x, a.y + area.y), u32(col::Line), em * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.6f, em * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.5f, em * 0.18f));
    ImGui::BeginChild("##sres", area, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    if (rows.empty() && search_.done && !search_.total) {
        ImGui::Dummy(ImVec2(0, em * 2));
        const char* msg = "Nothing found";
        ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(msg).x) * 0.5f));
        ImGui::TextColored(col::Faint, "%s", msg);
    }
    const float numW = em * 4.2f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiListClipper clip;
    clip.Begin((int)rows.size());
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const Row& row = rows[r];
            const kz::FileHits& f = search_.files[row.file];
            ImGui::PushID(r);
            if (row.hit == -1) {
                bool collapsed = search_.collapsed.count(f.path) != 0;
                if (ImGui::Selectable("##f", false, ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_AllowOverlap |
                                                        ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) showSearchHit(f, nullptr);
                    else if (collapsed) search_.collapsed.erase(f.path);
                    else search_.collapsed.insert(f.path);
                }
                ImGui::SetItemTooltip("Click to fold, double-click to show the file");
                if (ImGui::BeginPopupContextItem("##fctx")) {
                    if (ImGui::MenuItem("Show in archive")) showSearchHit(f, nullptr);
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(f.path.c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0, 0);
                float s = ImGui::GetTextLineHeight();
                ImVec2 p = ImGui::GetCursorScreenPos();
                // A small triangle: right when folded, down when open.
                ImVec2 c(p.x + s * 0.5f, p.y + s * 0.5f);
                float k = s * 0.22f;
                if (collapsed) dl->AddTriangleFilled(ImVec2(c.x - k * 0.6f, c.y - k), ImVec2(c.x - k * 0.6f, c.y + k), ImVec2(c.x + k, c.y), u32(col::Dim));
                else dl->AddTriangleFilled(ImVec2(c.x - k, c.y - k * 0.6f), ImVec2(c.x + k, c.y - k * 0.6f), ImVec2(c.x, c.y + k), u32(col::Dim));
                ImGui::Dummy(ImVec2(s, s));
                ImGui::SameLine(0, em * 0.2f);
                ImGui::PushFont(fonts().bold);
                ImGui::TextUnformatted(f.path.c_str());
                ImGui::PopFont();
                ImGui::SameLine(0, em * 0.6f);
                if (f.count) ImGui::TextColored(col::Dim, "%s", plural(f.count, "match", "matches").c_str());
                if (f.binary) {
                    ImGui::SameLine(0, em * 0.5f);
                    chip("binary", col::Dim);
                }
                if (!f.error.empty()) {
                    ImGui::SameLine(0, em * 0.5f);
                    ImGui::TextColored(col::Error, "%s", f.error.c_str());
                }
            } else if (row.hit == -2) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + em * 1.4f + numW);
                ImGui::PushFont(fonts().caption);
                ImGui::TextColored(col::Faint, "... and %s (the first %zu are listed)",
                                   plural(f.count - f.hits.size(), "more", "more").c_str(), f.hits.size());
                ImGui::PopFont();
            } else {
                const kz::SearchHit& h = f.hits[row.hit];
                if (ImGui::Selectable("##h", false, ImGuiSelectableFlags_SpanAvailWidth | ImGuiSelectableFlags_AllowOverlap))
                    showSearchHit(f, &h);
                if (ImGui::BeginPopupContextItem("##hctx")) {
                    if (ImGui::MenuItem("Show in archive")) showSearchHit(f, &h);
                    if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(h.snippet.c_str());
                    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(f.path.c_str());
                    ImGui::EndPopup();
                }
                ImGui::SameLine(0, 0);
                float x = ImGui::GetCursorPosX() + em * 1.4f;
                char num[32];
                if (h.line) std::snprintf(num, sizeof num, "%u", h.line);
                else std::snprintf(num, sizeof num, "0x%llX", (unsigned long long)h.offset);
                ImGui::PushFont(mono_);
                ImGui::SetCursorPosX(x + std::max(0.0f, numW - em * 0.8f - ImGui::CalcTextSize(num).x));
                ImGui::TextColored(col::Faint, "%s", num);
                ImGui::SameLine(x + numW);
                const char* s = h.snippet.c_str();
                size_t n = h.snippet.size();
                size_t m0 = std::min<size_t>(h.matchAt, n), m1 = std::min<size_t>(m0 + h.matchLen, n);
                ImGui::TextUnformatted(s, s + m0);
                ImGui::SameLine(0, 0);
                ImVec2 p = ImGui::GetCursorScreenPos();
                ImVec2 sz = ImGui::CalcTextSize(s + m0, s + m1);
                dl->AddRectFilled(ImVec2(p.x - 1, p.y), ImVec2(p.x + sz.x + 1, p.y + sz.y), u32(col::Accent, 0.3f), em * 0.15f);
                ImGui::TextColored(col::AccentHover, "%.*s", (int)(m1 - m0), s + m0);
                ImGui::SameLine(0, 0);
                ImGui::TextUnformatted(s + m1, s + n);
                ImGui::PopFont();
                if (h.utf16) {
                    ImGui::SameLine(0, em * 0.5f);
                    chip("UTF-16", col::Dim);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    // Footer
    dialogFooter();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - em * 1.5f, p.y), ImVec2(p.x + inner + em * 1.5f, p.y), u32(col::Line));
        ImGui::Dummy(ImVec2(0, em * 0.5f));
    }
    float fbh = ImGui::GetFrameHeight() * 1.15f;
    bool have = !search_.files.empty();
    if (secondaryButton("Fold all", ImVec2(em * 6.5f, fbh), have)) {
        for (auto& f : search_.files) search_.collapsed.insert(f.path);
    }
    ImGui::SameLine(0, em * 0.5f);
    if (secondaryButton("Unfold all", ImVec2(em * 6.5f, fbh), have)) search_.collapsed.clear();
    ImGui::SameLine();
    float cw = em * 9;
    alignRight(cw);
    if (secondaryButton("Copy results", ImVec2(cw, fbh), have && !search_.running)) {
        std::string text;
        for (auto& f : search_.files) {
            for (auto& h : f.hits) {
                char pos[48];
                if (h.line) std::snprintf(pos, sizeof pos, ":%u: ", h.line);
                else std::snprintf(pos, sizeof pos, ":@0x%llx: ", (unsigned long long)h.offset);
                text += f.path + pos + h.snippet + "\n";
            }
            if (f.count > f.hits.size()) text += f.path + ": ... " + std::to_string(f.count - f.hits.size()) + " more\n";
        }
        ImGui::SetClipboardText(text.c_str());
        toast(col::Ok, "Results copied");
    }
    ImGui::SetItemTooltip("As grep would print them: path:line: text");
    endDialog();
}

}  // namespace kzgui
