// Kompressor desktop application (Dear ImGui).
#pragma once

#include <imgui.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "file_dialog.hpp"
#include "kz_archive.hpp"

struct GLFWwindow;

namespace kzgui {

// Bytes per part for the "Split into parts" presets (0 = 100 MB, 1 = 700 MB, 2 = FAT32, 3 = custom MB).
uint64_t splitBytes(int preset, int customMb);

struct Job {
    int id = 0;
    std::string title;
    std::shared_ptr<kz::Progress> prog = std::make_shared<kz::Progress>();
    std::thread thread;
    std::atomic<bool> finished{false};
    bool ok = false;
    std::string message;
    std::function<void(Job&)> onDone;
    double started = 0.0;
    bool visible = true;  // listed in the activity panel
};

struct Node {
    std::string name, path;
    bool isDir = false, isLink = false, encrypted = false;
    int entry = -1, parent = -1;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t files = 0;  // files below a folder
    std::vector<int> children;
};

struct ExplorerTab {
    int id = 0;
    kz::Source src;
    std::string title, password;
    kz::ArchiveInfo info;
    bool loading = true;
    std::shared_ptr<kz::Progress> loadProg;
    std::string error;
    std::vector<Node> nodes;
    int cwd = 0;
    std::set<int> selection;
    int anchor = -1;
    char filter[256] = {};
    std::vector<int> rows;  // currently displayed rows (node ids)
    std::vector<int> back, forward;  // folder history (node ids) for Back / Forward
    int sortCol = 0;
    bool sortAsc = true;
    // preview
    int previewNode = -1;
    std::vector<uint8_t> preview;
    bool previewTruncated = false, previewLoading = false, previewIsText = false;
    std::vector<uint32_t> lineStarts;
    std::string previewError;
    std::shared_ptr<kz::Progress> previewProg;
    int previewMode = 0;  // 0 auto, 1 text, 2 hex
    float previewWidth = 0.0f;  // fraction of the explorer width
};

struct LogEntry {
    double time;  // wall-clock seconds (time_t)
    ImVec4 color;
    std::string text;
    std::string openPath;  // optional: "Open" button opens this archive
    std::string revealPath;  // optional: "Show" button reveals this folder/file
};

class App {
public:
    App(GLFWwindow* window, ImFont* mono, float scale);
    ~App();
    void frame();
    void onDrop(const std::vector<std::string>& paths);
    void openArchive(const std::string& path);
    void openCompressDialog(std::vector<std::string> inputs, bool super);
    // Extracts an archive straight to `destDir` (Explorer "Extract here" / "Extract to folder").
    void extractFile(const std::string& path, const std::string& destDir, const std::string& password = "");
    // Close the window once every extractFile() job has succeeded (launches from the Explorer menu).
    void quitWhenExtracted() { quitWhenExtracted_ = true; }
    void showSettings() { showSettings_ = true; }
    // Debug aid: place the Settings dialog beside the main window (tests detached viewports).
    void debugDetach() { debugDetach_ = true; }
    // Debug aid: open the Recompress dialog once the active archive has loaded.
    void debugRecompress() { debugRecompress_ = true; }
    bool busy() const;
    // Debug aid for --snapshot: navigate to and preview an entry once the active tab has loaded.
    void debugSelect(const std::string& entryPath) { debugSelect_ = entryPath; }

private:
    // jobs
    Job& startJob(const std::string& title, std::function<bool(kz::Progress&, std::string&)> work,
                  std::function<void(Job&)> onDone, bool visible = true);
    void pumpJobs();
    void log(const ImVec4& c, const std::string& text, const std::string& open = "", const std::string& reveal = "");
    void toast(const ImVec4& c, const std::string& text);

    // tabs / explorer
    ExplorerTab* tabById(int id);
    void openSource(const kz::Source& src, const std::string& title, const std::string& password);
    void loadTab(int tabId);
    void buildTree(ExplorerTab& t);
    void computeRows(ExplorerTab& t);
    void requestPreview(ExplorerTab& t, int node);
    void openNested(ExplorerTab& t, int node);
    std::vector<std::string> selectedPaths(ExplorerTab& t, bool& any);
    // Opens folder `dir` in the tab; `record` adds the folder being left to the Back history.
    void navigateTo(ExplorerTab& t, int dir, int select = -1, bool record = true);
    void goBack(ExplorerTab& t);
    void goForward(ExplorerTab& t);

    // drawing
    void drawMenuBar();
    void drawToolbar();
    void drawWelcome();
    void drawExplorer(ExplorerTab& t);
    void drawTable(ExplorerTab& t, float width);
    void drawPreview(ExplorerTab& t);
    void drawActivity();
    void drawCompressWindow();
    void drawExtractWindow();
    void drawConvertWindow();
    void drawPasswordPopup();
    void drawAbout();
    void drawSettings();
    void drawToasts();
    void drawStatusBar();

    // actions
    void actionOpen();
    void actionExtract(ExplorerTab& t, bool selectionOnly);
    void actionTest(ExplorerTab& t);
    void actionConvert(ExplorerTab& t, bool super = false);  // recompress into another format
    void askPassword(const std::string& prompt, std::function<void(const std::string&)> onOk);
    void runExtract(int tabId, kz::ExtractOptions o, bool reveal);
    void runTest(int tabId);
    void runConvert(int tabId, std::string out, kz::CompressOptions o, bool openWhenDone);
    void runCompress();
    void closeTab(int id);
    void drawFormats();
    int activeTab_ = -1;
    bool showFormats_ = false;
    std::string debugSelect_;

    GLFWwindow* window_;
    ImFont* mono_;
    float scale_;
    FileDialog fileDlg_;
    std::list<std::unique_ptr<Job>> jobs_;
    int nextJobId_ = 1;
    std::vector<std::unique_ptr<ExplorerTab>> tabs_;
    int nextTabId_ = 1;
    int focusTab_ = -1;
    std::vector<LogEntry> log_;
    struct Toast { double time; ImVec4 color; std::string text; };
    std::vector<Toast> toasts_;
    bool showActivity_ = true;
    bool showAbout_ = false;
    bool showSettings_ = false;
    bool debugDetach_ = false, debugRecompress_ = false;
    std::string lastDir_;
    bool shellMenu_ = false;  // Explorer right-click menu installed
    int directExtracts_ = 0;  // extractFile() jobs still running or waiting for a password
    bool directFailed_ = false, quitWhenExtracted_ = false;

    struct CompressState {
        bool open = false, focus = false;
        std::vector<std::string> inputs;
        char out[2048] = {};
        int format = 0;  // index into kz::outputFormats()
        int level = 0;
        int kmpMode = 2;
        char pw[256] = {}, pw2[256] = {};
        int threads = 0;
        bool dedup = true;
        bool openWhenDone = true;
        bool split = false;
        int splitPreset = 0, splitCustomMb = 1024;
    } cmp_;

    struct ExtractState {
        bool open = false, focus = false;
        int tabId = -1;
        bool selectionOnly = false;
        std::vector<std::string> only;
        std::string stripPrefix;
        char dest[2048] = {};
        bool subfolder = true;
        bool skipExisting = false;
        bool reveal = true;
    } ext_;

    struct ConvertState {
        bool open = false, focus = false;
        int tabId = -1;
        char out[2048] = {};
        int format = 0;  // index into kz::outputFormats()
        int level = 0;
        int mode = 2;    // KMP mode
        bool dedup = true;
        bool openWhenDone = true;
        bool split = false;
        int splitPreset = 0, splitCustomMb = 1024;
        char pw[256] = {}, pw2[256] = {};
    } conv_;
    void drawSplitOption(bool* on, int* preset, int* customMb, float inner);

    struct PasswordState {
        bool request = false;
        std::string prompt;
        char buf[256] = {};
        bool show = false;
        std::function<void(const std::string&)> onOk;
    } pwd_;
};

}  // namespace kzgui
