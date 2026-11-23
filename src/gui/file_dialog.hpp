// Native OS file dialogs (Windows IFileDialog, macOS NSOpenPanel, Linux xdg-desktop-portal)
// via nativefiledialog-extended. Requests are queued with open() and shown from draw(), so the
// blocking system dialog never runs in the middle of a widget's click handler.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace kzgui {

class FileDialog {
public:
    enum class Mode {
        OpenArchives,  // pick one or more archives to open
        AddFiles,      // pick one or more files of any type
        AddFolders,    // pick one or more folders
        PickFolder,    // pick a single folder
        SaveFile,      // choose a file name to write
    };
    using Callback = std::function<void(const std::vector<std::string>&)>;

    FileDialog();
    ~FileDialog();
    void setParent(GLFWwindow* w) { parent_ = w; }
    void open(const std::string& title, Mode mode, const std::string& startDir, Callback cb,
              const std::string& defaultName = "");
    void draw();
    bool isOpen() const { return pending_; }
    // Non-empty after a dialog failed to open (shown in the activity log by the app).
    std::string takeError();

    static std::string homeDir();

private:
    GLFWwindow* parent_ = nullptr;
    bool pending_ = false;
    bool ready_ = false;
    std::string title_, startDir_, defaultName_, error_;
    Mode mode_ = Mode::OpenArchives;
    Callback cb_;
};

}  // namespace kzgui
