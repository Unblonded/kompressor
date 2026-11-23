#include "file_dialog.hpp"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "kz_common.hpp"
#include "theme.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace kzgui {

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

static void copyTo(char* dst, size_t n, const std::string& s) {
    std::strncpy(dst, s.c_str(), n - 1);
    dst[n - 1] = 0;
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

std::vector<std::pair<std::string, std::string>> FileDialog::places() const {
    std::vector<std::pair<std::string, std::string>> p;
    std::string home = homeDir();
    p.push_back({"Home", home});
    for (const char* sub : {"Desktop", "Documents", "Downloads"}) {
        std::error_code ec;
        fs::path d = kz::u8path(home) / sub;
        if (fs::is_directory(d, ec)) p.push_back({sub, kz::pathToUtf8(d)});
    }
#ifdef _WIN32
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) {
            std::string d = std::string(1, char('A' + i)) + ":\\";
            p.push_back({d, d});
        }
#elif defined(__APPLE__)
    p.push_back({"Computer", "/"});
    p.push_back({"Volumes", "/Volumes"});
#else
    p.push_back({"Computer", "/"});
    std::error_code ec;
    if (fs::is_directory("/media", ec)) p.push_back({"Media", "/media"});
    if (fs::is_directory("/mnt", ec)) p.push_back({"Mounts", "/mnt"});
#endif
    return p;
}

void FileDialog::open(const std::string& title, Mode mode, const std::string& startDir, Callback cb,
                      const std::string& defaultName) {
    title_ = title;
    mode_ = mode;
    cb_ = std::move(cb);
    openRequest_ = true;
    copyTo(nameBuf_, sizeof nameBuf_, defaultName);
    filterBuf_[0] = 0;
    std::error_code ec;
    fs::path start = startDir.empty() ? kz::u8path(homeDir()) : kz::u8path(startDir);
    if (!fs::is_directory(start, ec)) start = start.parent_path();
    if (start.empty() || !fs::is_directory(start, ec)) start = kz::u8path(homeDir());
    navigate(start);
}

void FileDialog::navigate(const fs::path& p) {
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    cwd_ = abs.lexically_normal();
    copyTo(pathBuf_, sizeof pathBuf_, kz::pathToUtf8(cwd_));
    sel_.clear();
    anchor_ = -1;
    refresh();
}

void FileDialog::refresh() {
    items_.clear();
    error_.clear();
    std::error_code ec;
    fs::directory_iterator it(cwd_, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        error_ = ec.message();
        return;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        Item i;
        i.name = kz::pathToUtf8(it->path().filename());
        if (!showHidden_ && !i.name.empty() && i.name[0] == '.') continue;
        std::error_code e2;
        i.dir = it->is_directory(e2);
        if (mode_ == Mode::PickFolder && !i.dir) continue;
        kz::FileStat st = kz::statPath(it->path(), true);
        i.size = st.size;
        i.mtime = st.mtime;
        items_.push_back(std::move(i));
    }
    std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.dir != b.dir) return a.dir;
        return lower(a.name) < lower(b.name);
    });
}

void FileDialog::accept(std::vector<std::string> paths) {
    visible_ = false;
    ImGui::CloseCurrentPopup();
    if (cb_ && !paths.empty()) {
        auto cb = cb_;
        cb(paths);
    }
}

void FileDialog::draw() {
    if (openRequest_) {
        ImGui::OpenPopup(title_.c_str());
        openRequest_ = false;
        visible_ = true;
    }
    if (!visible_) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.8f, vp->WorkSize.y * 0.8f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool keepOpen = true;
    if (!ImGui::BeginPopupModal(title_.c_str(), &keepOpen, ImGuiWindowFlags_NoSavedSettings)) {
        visible_ = false;
        return;
    }
    if (!keepOpen) {
        visible_ = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    const float em = ImGui::GetFontSize();
    // --- top bar: up / path / hidden toggle
    if (ImGui::Button("Up") && cwd_.has_parent_path() && cwd_.parent_path() != cwd_) navigate(cwd_.parent_path());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-em * 16);
    if (ImGui::InputText("##path", pathBuf_, sizeof pathBuf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::error_code ec;
        fs::path p = kz::u8path(pathBuf_);
        if (fs::is_directory(p, ec)) navigate(p);
        else if (fs::exists(p, ec) && mode_ != Mode::PickFolder) accept({pathBuf_});
        else error_ = "No such folder";
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 9);
    ImGui::InputTextWithHint("##filter", "Filter", filterBuf_, sizeof filterBuf_);
    ImGui::SameLine();
    if (ImGui::Checkbox("Hidden", &showHidden_)) refresh();

    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    // --- sidebar with places
    ImGui::BeginChild("##places", ImVec2(em * 10, -footer), ImGuiChildFlags_Borders);
    for (auto& pl : places()) {
        bool active = kz::pathToUtf8(cwd_) == pl.second;
        if (ImGui::Selectable(pl.first.c_str(), active)) navigate(kz::u8path(pl.second));
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- listing
    ImGui::BeginChild("##list", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (!error_.empty()) ImGui::TextColored(col::Error, "%s", error_.c_str());
    std::string flt = lower(filterBuf_);
    std::vector<int> visible;
    for (int i = 0; i < (int)items_.size(); ++i)
        if (flt.empty() || lower(items_[i].name).find(flt) != std::string::npos) visible.push_back(i);

    const bool multi = mode_ == Mode::OpenFiles || mode_ == Mode::OpenAny;
    std::string acceptNow;
    if (ImGui::BeginTable("##files", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 6);
        ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, em * 9);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)visible.size());
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const Item& it = items_[visible[r]];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool isSel = sel_.count(it.name) > 0;
                ImGui::PushID(visible[r]);
                ImGui::PushStyleColor(ImGuiCol_Text, it.dir ? col::FolderCol : ImGui::GetStyleColorVec4(ImGuiCol_Text));
                std::string label = (it.dir ? "[+] " : "    ") + it.name;
                if (ImGui::Selectable(label.c_str(), isSel,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    ImGuiIO& io = ImGui::GetIO();
                    bool selectable = !(mode_ == Mode::OpenFiles && it.dir);
                    if (multi && io.KeyShift && anchor_ >= 0) {
                        sel_.clear();
                        int a = std::min(anchor_, r), b = std::max(anchor_, r);
                        for (int k = a; k <= b && k < (int)visible.size(); ++k)
                            if (!(mode_ == Mode::OpenFiles && items_[visible[k]].dir)) sel_.insert(items_[visible[k]].name);
                    } else if (multi && (io.KeyCtrl || io.KeySuper)) {
                        if (isSel) sel_.erase(it.name);
                        else if (selectable) sel_.insert(it.name);
                        anchor_ = r;
                    } else {
                        sel_.clear();
                        if (selectable || mode_ == Mode::PickFolder) sel_.insert(it.name);
                        anchor_ = r;
                    }
                    if (!it.dir && mode_ == Mode::SaveFile) copyTo(nameBuf_, sizeof nameBuf_, it.name);
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        if (it.dir) acceptNow = "\x01" + it.name;  // navigate after the loop
                        else acceptNow = it.name;
                    }
                }
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                if (!it.dir) ImGui::TextDisabled("%s", kz::humanSize(it.size).c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::TextDisabled("%s", kz::formatTime(it.mtime).c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (!acceptNow.empty()) {
        if (acceptNow[0] == '\x01') {
            navigate(cwd_ / kz::u8path(acceptNow.substr(1)));
        } else {
            accept({kz::pathToUtf8(cwd_ / kz::u8path(acceptNow))});
            ImGui::EndPopup();
            return;
        }
    }

    // --- footer
    if (mode_ == Mode::SaveFile) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        bool enter = ImGui::InputText("##name", nameBuf_, sizeof nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
        if (enter && nameBuf_[0]) {
            accept({kz::pathToUtf8(cwd_ / kz::u8path(nameBuf_))});
            ImGui::EndPopup();
            return;
        }
        std::error_code ec;
        if (nameBuf_[0] && fs::exists(cwd_ / kz::u8path(nameBuf_), ec))
            ImGui::TextColored(col::Warn, "A file with this name exists and will be replaced.");
        else
            ImGui::TextDisabled("%s", kz::pathToUtf8(cwd_).c_str());
    } else {
        if (sel_.empty())
            ImGui::TextDisabled(mode_ == Mode::PickFolder ? "No folder selected - the current folder will be used"
                                                          : "Nothing selected");
        else
            ImGui::Text("%d selected", (int)sel_.size());
        ImGui::Spacing();
    }

    float bw = em * 7;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Cancel", ImVec2(bw, 0))) {
        visible_ = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    const char* okLabel = mode_ == Mode::SaveFile ? "Save" : (mode_ == Mode::PickFolder ? "Select" : "Open");
    bool canOk = mode_ == Mode::PickFolder || (mode_ == Mode::SaveFile ? nameBuf_[0] != 0 : !sel_.empty());
    ImGui::BeginDisabled(!canOk);
    if (accentButton(okLabel, ImVec2(bw, 0))) {
        std::vector<std::string> out;
        if (mode_ == Mode::SaveFile) {
            out.push_back(kz::pathToUtf8(cwd_ / kz::u8path(nameBuf_)));
        } else if (mode_ == Mode::PickFolder) {
            out.push_back(kz::pathToUtf8(sel_.empty() ? cwd_ : cwd_ / kz::u8path(*sel_.begin())));
        } else {
            for (auto& s : sel_) out.push_back(kz::pathToUtf8(cwd_ / kz::u8path(s)));
        }
        accept(out);
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        visible_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace kzgui
