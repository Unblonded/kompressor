// Handing files to the rest of the desktop: opening a file in its default app, the clipboard's list
// of copied files, and dragging files out of the window into the file manager.
#pragma once

#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace kzgui::native {

// Once, on the UI thread, before any drag or clipboard call (sets up OLE on Windows).
void init();
void shutdown();

// The UI's "draw one frame". While a drag out or a move of the window is under way Windows runs its
// own message loop and ours stands still, so the window would stop drawing (it looked frozen); those
// loops call this about 60 times a second instead. Never called from inside a frame.
void setRedraw(std::function<void()> redraw);
void redraw();

void openWithDefaultApp(const std::string& path);

// Whether files can be dragged out of the window and put on the clipboard on this platform.
bool fileTransferSupported();

// Files the user copied in the file manager, or none.
std::vector<std::string> clipboardFiles();
// Puts `paths` on the clipboard as copied files, ready to paste in the file manager.
bool setClipboardFiles(GLFWwindow* window, const std::vector<std::string>& paths);

// Runs a modal drag of files out of `window`. The files need not exist while dragging: `prepare` runs
// when the mouse is released over a target that accepts files (never over our own windows), creates
// them and returns their final paths (empty cancels the drop). `prepare` runs on a worker thread while
// the UI thread keeps handling window messages; Esc meanwhile calls `cancel`, which should make
// `prepare` stop early. Returns true when the files were dropped.
bool dragFilesOut(GLFWwindow* window, const std::vector<std::string>& paths,
                  const std::function<std::vector<std::string>()>& prepare, const std::function<void()>& cancel);

}  // namespace kzgui::native
