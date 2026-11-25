// Kompressor desktop application (Dear ImGui).
#pragma once

#include <imgui.h>

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "file_dialog.hpp"
#include "kz_hash.hpp"
#include "kz_search.hpp"
#include "kz_ramdrive.hpp"
#include "kz_advisor.hpp"
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
    bool controls = true;  // with pause and cancel buttons
    // extractFile() / quickCompress() jobs, which the quick-task window shows: the file(s) worked on
    // and where the result goes.
    bool direct = false;
    std::string subject, dest;
    // Pausing (the flag the worker waits on is prog->paused). Elapsed time leaves paused stretches out.
    double pausedSince = 0.0, pausedTotal = 0.0;
    bool paused() const { return prog->paused.load(); }
    void setPaused(bool on, double now) {
        if (on == paused()) return;
        if (on) pausedSince = now;
        else pausedTotal += now - pausedSince;
        prog->paused = on;
    }
    double elapsed(double now) const { return (paused() ? pausedSince : now) - started - pausedTotal; }

    // Speed of the work, in bytes of input per second: measured once a second and smoothed over
    // roughly the last ten, so a stretch of slow blocks shows up without the number jumping about.
    double rate = 0.0;
    int rateSamples = 0;
    double rateT = -1.0, rateBytes = 0.0;
    void sampleRate(double now) {
        if (paused() || prog->total.load() == 0) {
            rateT = -1.0;  // start a fresh measurement on resume
            return;
        }
        double bytes = (double)prog->fraction() * (double)prog->total.load();
        if (rateT < 0) {
            rateT = now;
            rateBytes = bytes;
            return;
        }
        double dt = now - rateT;
        if (dt < 1.0) return;
        double r = bytes > rateBytes ? (bytes - rateBytes) / dt : 0.0;
        rate = rateSamples == 0 ? r : rate * 0.9 + r * 0.1;
        rateSamples++;
        rateT = now;
        rateBytes = bytes;
    }
    // Seconds left at the current speed, or -1 while there is no fair estimate yet.
    double secondsLeft() const {
        uint64_t t = prog->total.load();
        if (t == 0 || rateSamples < 3 || rate <= 1.0) return -1.0;
        double left = (1.0 - (double)prog->fraction()) * (double)t / rate;
        return left < 0 ? 0 : left;
    }
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
    // A spot to scroll the preview to and mark once it has loaded (a search hit): byte offset, or none.
    uint64_t previewMark = UINT64_MAX;
    int previewJump = 0;  // frames left to keep scrolling there (the first frame may not know the height)
    float previewWidth = 0.0f;  // fraction of the explorer width
    // editing (adding / deleting entries)
    bool editable = false;      // Kompressor can write this archive back
    std::string editWhy;        // why not, when it can't
    bool editing = false;       // an update is running
    std::string restoreDir;     // after a reload: folder to return to...
    std::vector<std::string> restoreSelect;  // ...and entries to select
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
    // Work that must run outside a frame (dragging files out of the window). Called after each frame.
    void afterFrame();
    // Files dropped onto the window at screen position `at`.
    void onDrop(const std::vector<std::string>& paths, ImVec2 at);
    void openArchive(const std::string& path);
    void openCompressDialog(std::vector<std::string> inputs, bool super);
    // Extracts an archive straight to `destDir` (Explorer "Extract here" / "Extract to folder").
    void extractFile(const std::string& path, const std::string& destDir, const std::string& password = "");
    // Compresses straight to "<name>.7z/.zip/.kmp" next to the inputs with default settings (Explorer "Add to").
    void quickCompress(const std::vector<std::string>& inputs, kz::OutFormat format);
    // Opens an archive and shows its Extract dialog / runs its integrity test (Explorer menu).
    void openExtractDialog(const std::string& path);
    void testArchiveFile(const std::string& path);
    // Checksums window for files and folders on disk (File menu, Explorer's "Checksums").
    void hashFiles(const std::vector<std::string>& paths);
    // Close the window once every extractFile() / quickCompress() job has succeeded (Explorer menu launches).
    void quitWhenDone() { quitWhenDone_ = true; }
    // Explorer menu launches that need no dialog: a small window with only the task's progress in place
    // of the full app (the window is already sized for it). `verb` heads it ("Extracting", "Compressing").
    void enterQuickMode(const std::string& verb);
    bool quickMode() const { return quick_; }
    void showSettings() { showSettings_ = true; }
    // Debug aid: place the Settings dialog beside the main window (tests detached viewports).
    void debugDetach() { debugDetach_ = true; }
    // Debug aid: open the Recompress dialog once the active archive has loaded.
    void debugRecompress() { debugRecompress_ = true; }
    // Debug aid: open the Checksums window once the active archive has loaded (and check `list`, if given).
    void debugHash(const std::string& list) { debugHash_ = true, debugHashList_ = list; }
    // Debug aid: open Find in Files once the active archive has loaded and search it for `text`.
    // (then shows the first file's last listed hit, as a click on it would).
    void debugSearch(const std::string& text) { debugSearch_ = text, debugSearchJump_ = true; }
    void debugRam(bool run);
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
    bool nestedArchive(const ExplorerTab& t, int node) const;
    std::vector<std::string> selectedPaths(ExplorerTab& t, bool& any);
    // Opens folder `dir` in the tab; `record` adds the folder being left to the Back history.
    void navigateTo(ExplorerTab& t, int dir, int select = -1, bool record = true);
    void goBack(ExplorerTab& t);
    void goForward(ExplorerTab& t);

    // files in and out of archives (app_files.cpp)
    void openEntry(ExplorerTab& t, int node);       // extract to a temp folder, open in its default app
    void copySelection(ExplorerTab& t);             // extract to a temp folder, put on the clipboard
    void pasteFiles(ExplorerTab& t);                // add the clipboard's files to the open folder
    void askAddFiles(ExplorerTab& t, std::vector<std::string> paths, bool offerOpen = false);
    void askDeleteSelection(ExplorerTab& t);
    void pickFilesToAdd(ExplorerTab& t, bool folders);
    void runEdit(int tabId, kz::ArchiveEdit edit, std::vector<std::string> selectAfter);
    void dragSelectionOut(int tabId);
    void askConfirm(const std::string& title, const std::string& text, const std::string& okLabel, int icon,
                    const ImVec4& color, std::function<void()> onOk, const std::string& altLabel = "",
                    std::function<void()> onAlt = nullptr);
    void drawConfirmPopup();
    int pendingDrag_ = -1;   // tab whose selection starts dragging out after this frame
    bool dragging_ = false;  // a drag out is running
    // A drag out shows in the activity panel once the files are released over a target and their
    // extraction starts (on a worker thread, so the UI thread makes the row visible).
    Job* dropJob_ = nullptr;
    std::atomic<bool> dropStarted_{false};
    void syncDropJob();
    int dropTab_ = -1;       // tab whose file area was drawn last frame...
    ImVec2 dropMin_, dropMax_;  // ...and where (screen coordinates)

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
    // Rebuilds a damaged ZIP from its intact entries, or extracts every intact file of anything else.
    void runRepair(int tabId);
    // Checksums window (app_hash.cpp): every file, or the selected ones.
    void actionHash(ExplorerTab& t, bool selectionOnly);
    void runHash(unsigned which);
    void recheckHashes();
    void pickHashList();
    void loadHashList(const std::string& path);
    void saveHashList(kz::HashKind k);
    void drawHashWindow();
    // Find in Files (app_search.cpp): text or bytes inside every file, or the selected ones.
    void actionSearch(ExplorerTab& t, bool selectionOnly);
    void runSearch();
    void showSearchHit(const kz::FileHits& f, const kz::SearchHit* h);
    void drawSearchWindow();
    // RAM Drive window (app_ramdrive.cpp): a scratch drive that's deleted when it stops.
    void openRamDrive();
    void drawRamDriveWindow();
    void ramRun(const char* busyText, std::function<std::string()> work);
    // Opens the tab's archive in Windows File Explorer, or shows it there when Explorer can't browse it.
    void openInExplorer(ExplorerTab& t);
    void actionConvert(ExplorerTab& t, bool super = false);  // recompress into another format
    void askPassword(const std::string& prompt, std::function<void(const std::string&)> onOk,
                     std::function<void()> onCancel = nullptr);
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
    bool debugDetach_ = false, debugRecompress_ = false, debugHash_ = false;
    std::string debugHashList_, debugSearch_;
    bool debugSearchJump_ = false;
    std::string lastDir_;
    bool shellMenu_ = false;  // Explorer right-click menu installed
    bool maxPerf_ = false;    // Settings: maximum performance while compressing / extracting
    bool useGpu_ = false;     // Settings: let the graphics card help (ZIP)
    void updateBoost();
    int directJobs_ = 0;  // extractFile() / quickCompress() jobs still running or waiting for a password
    bool directFailed_ = false, quitWhenDone_ = false;

    // Quick-task window (Explorer "Extract here", "Add to .7z" ...): drawQuick() replaces the whole frame.
    struct QuickResult {
        bool ok = false, cancelled = false;
        std::string subject, message, reveal;
    };
    bool quick_ = false;
    std::string quickVerb_;
    std::vector<QuickResult> quickResults_;  // one per finished task
    double quickDoneAt_ = -1.0;  // when the last task finished (the window closes shortly after)
    void quickResult(bool ok, bool cancelled, const std::string& subject, const std::string& message,
                     const std::string& reveal);
    void drawQuick();
    void drawQuickTitleBar();
    void leaveQuickMode();  // "Open in Kompressor": grow into the full window

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
        bool sfx = false;  // Windows: a self-extracting .exe
        // recommendation
        int goal = 0;                                   // kz::Goal
        std::string adviceKey;                          // the inputs the profile below belongs to
        std::shared_ptr<kz::ContentProfile> profile;    // null while measuring
        std::shared_ptr<kz::Progress> adviceProg;
        bool focusPassword = false;
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
        int goal = 0;
        kz::ContentProfile profile;
        bool focusPassword = false;
    } conv_;
    struct HashState {
        bool open = false, focus = false;
        std::vector<std::string> files;  // files and folders on disk; empty = the files of an archive (`src`)
        std::string base;                // on disk: the folder names are relative to
        int tabId = -1;
        kz::Source src;
        std::string title, password, scope;
        std::vector<std::string> only;
        bool algo[4] = {false, false, false, true};  // CRC32, MD5, SHA-1, SHA-256
        unsigned computed = 0;                        // kinds in `results`
        bool running = false;
        int runId = 0;
        std::shared_ptr<kz::Progress> prog;
        std::vector<kz::EntryHash> results;
        std::string error;
        // checking against a list
        std::string listPath;
        std::vector<kz::ListedHash> listed;
        std::vector<kz::HashCheck> checks;
        std::vector<std::string> notListed;
        std::unordered_map<std::string, int> checkOf;  // entry path -> index in checks
        char filter[128] = {};
        bool problemsOnly = false;
    } hash_;
    struct SearchState {
        bool open = false, focus = false, focusInput = false;
        int tabId = -1;
        kz::Source src;
        std::string title, password, scope;
        std::vector<std::string> only;
        char query[256] = {};
        char names[128] = {};
        int mode = 0;  // 0 text, 1 hex
        bool matchCase = false;
        bool running = false;
        int runId = 0;
        std::shared_ptr<kz::Progress> prog;
        // Files with hits arrive from the worker thread while it runs.
        struct Feed {
            std::mutex mu;
            std::vector<kz::FileHits> files;
        };
        std::shared_ptr<Feed> feed;
        std::vector<kz::FileHits> files;
        uint64_t total = 0;          // matches in `files`
        std::string error, searched;  // `searched`: the query the results are for
        bool done = false;
        std::set<std::string> collapsed;
    } search_;
    struct RamState {
        bool open = false, focus = false;
        int size = 2;            // index into the size choices
        int customMb = 4096;
        int letter = 0;          // index into the free drive letters (Windows)
        std::unique_ptr<kz::RamDrive> drive;
        // Starting and stopping wait on other programs (and on Windows an administrator prompt), so
        // they run here; the window only reads `drive` while nothing is running.
        std::thread worker;
        std::atomic<bool> busy{false};
        std::string busyText;
        std::mutex mu;           // guards `error`
        std::string error;
        ~RamState() {
            if (worker.joinable()) worker.join();
        }
    } ram_;
    void drawSplitOption(bool* on, int* preset, int* customMb, float inner);
    // "Recommended for you": goal picker, the suggested format with its reasons and estimates, and a button
    // that applies it. Returns true when the user clicked "Use this" (`out` holds the recommendation).
    bool drawAdvice(const kz::ContentProfile* profile, bool measuring, int* goal, int curFormat, int curLevel, int curMode,
                    unsigned threads, float inner, kz::Recommendation& out);

    struct ConfirmState {
        bool request = false;
        std::string title, text, ok, alt;
        int icon = 0;
        ImVec4 color;
        std::function<void()> onOk, onAlt;
    } confirm_;

    struct PasswordState {
        bool request = false;
        std::string prompt;
        char buf[256] = {};
        bool show = false;
        std::function<void(const std::string&)> onOk;
        std::function<void()> onCancel;
    } pwd_;
};

}  // namespace kzgui
