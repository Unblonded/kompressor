// A self-contained Dear ImGui file browser (no GTK/Win32/Cocoa dialog dependencies),
// identical on Windows, macOS and Linux.
#pragma once

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace kzgui {

class FileDialog {
public:
    enum class Mode {
        OpenFiles,   // pick one or more files
        OpenAny,     // pick files and/or folders (multi)
        PickFolder,  // pick a single folder
        SaveFile,    // choose a file name to write
    };
    using Callback = std::function<void(const std::vector<std::string>&)>;

    void open(const std::string& title, Mode mode, const std::string& startDir, Callback cb,
              const std::string& defaultName = "");
    void draw();
    bool isOpen() const { return visible_ || openRequest_; }

    static std::string homeDir();

private:
    struct Item {
        std::string name;
        bool dir = false;
        uint64_t size = 0;
        int64_t mtime = 0;
    };
    void navigate(const std::filesystem::path& p);
    void refresh();
    void accept(std::vector<std::string> paths);
    std::vector<std::pair<std::string, std::string>> places() const;

    std::string title_;
    Mode mode_ = Mode::OpenFiles;
    Callback cb_;
    bool openRequest_ = false;
    bool visible_ = false;
    std::filesystem::path cwd_;
    std::vector<Item> items_;
    std::set<std::string> sel_;
    int anchor_ = -1;
    char pathBuf_[2048] = {};
    char nameBuf_[512] = {};
    char filterBuf_[128] = {};
    bool showHidden_ = false;
    std::string error_;
};

}  // namespace kzgui
