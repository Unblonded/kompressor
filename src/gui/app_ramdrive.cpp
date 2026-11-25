// The RAM Drive window: a scratch drive that's deleted when it stops - a local drive held in memory on
// Windows, a RAM disk on macOS, a folder in /dev/shm on Linux (see kz_ramdrive.hpp).
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdio>
#include <string>
#include <vector>

#include "app.hpp"
#include "app_util.hpp"
#include "theme.hpp"

namespace kzgui {

namespace {

float emPx() { return ImGui::GetFontSize(); }

struct SizeChoice {
    const char* label;
    uint64_t mb;  // 0 = custom
};
const SizeChoice kSizes[] = {{"512 MB", 512}, {"1 GB", 1024}, {"2 GB", 2048}, {"4 GB", 4096}, {"8 GB", 8192}, {"Custom", 0}};
const int kSizeCount = (int)(sizeof kSizes / sizeof kSizes[0]);

uint64_t physicalMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

// Drive letters not in use, D to Z ("R" first when free, as the drive's usual letter).
std::vector<char> freeLetters() {
    std::vector<char> out;
#ifdef _WIN32
    DWORD used = GetLogicalDrives();
    for (char c = 'D'; c <= 'Z'; ++c)
        if (!(used & (1u << (c - 'A')))) out.push_back(c);
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] == 'R') {
            out.erase(out.begin() + (ptrdiff_t)i);
            out.insert(out.begin(), 'R');
            break;
        }
#endif
    return out;
}

}  // namespace

void App::openRamDrive() { ram_.open = ram_.focus = true; }

// Runs `work` off the UI thread; it returns an error ("" = fine).
void App::ramRun(const char* text, std::function<std::string()> work) {
    if (ram_.worker.joinable()) ram_.worker.join();
    {
        std::lock_guard<std::mutex> l(ram_.mu);
        ram_.error.clear();
    }
    ram_.busyText = text;
    ram_.busy = true;
    ram_.worker = std::thread([this, work] {
        std::string e = work();
        {
            std::lock_guard<std::mutex> l(ram_.mu);
            ram_.error = e;
        }
        ram_.busy = false;
    });
}

void App::debugRam(bool run) {
    openRamDrive();
    if (!run) return;
    ramRun("Starting the drive...", [this]() -> std::string {
        auto d = std::make_unique<kz::RamDrive>();
        std::string err;
        char letter = freeLetters().empty() ? 'R' : freeLetters()[0];
        if (!d->start(1ull << 30, letter, err)) return err;
        ram_.drive = std::move(d);
        return {};
    });
    ram_.worker.join();
}

void App::drawRamDriveWindow() {
    if (!ram_.open) return;
    if (!ram_.busy && ram_.worker.joinable()) ram_.worker.join();

    const float em = emPx();
    if (ram_.focus) {
        ImGui::SetNextWindowFocus();
        ram_.focus = false;
    }
    bool running = !ram_.busy && ram_.drive && ram_.drive->running();
    const char* sub = running ? "Running - everything on it is deleted when it stops" : "A fast scratch drive that cleans itself up";
    if (!beginDialog("###ramdrive", "RAM Drive", sub, Ico::Bolt, &ram_.open, em * 34, col::Accent2)) {
        endDialog();
        return;
    }
    float inner = ImGui::GetContentRegionAvail().x;
    float bh = ImGui::GetFrameHeight();
    std::string error;
    {
        std::lock_guard<std::mutex> l(ram_.mu);
        error = ram_.error;
    }
    auto wrapped = [&](const ImVec4& c, const std::string& text, bool small = false) {
        if (small) ImGui::PushFont(fonts().caption);
        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + inner);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        if (small) ImGui::PopFont();
    };

    if (ram_.busy) {
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        spinner(em * 0.5f, col::Accent);
        ImGui::SameLine(0, em * 0.6f);
        ImGui::TextColored(col::Dim, "%s", ram_.busyText.c_str());
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        endDialog();
        return;
    }

    if (!running) {
        // ------------------------------------------------------------ setup
#ifdef __linux__
        const bool pickSize = false;  // /dev/shm has its own limit
#else
        const bool pickSize = true;
#endif
        uint64_t cap = 0;
        if (pickSize) {
            caption("SIZE");
            ImGui::Dummy(ImVec2(0, em * 0.1f));
            for (int i = 0; i < kSizeCount; ++i) {
                if (i) ImGui::SameLine(0, em * 0.35f);
                if (pill(kSizes[i].label, ram_.size == i, em * 3.6f)) ram_.size = i;
            }
            if (kSizes[ram_.size].mb == 0) {
                ImGui::Dummy(ImVec2(0, em * 0.1f));
                ImGui::SetNextItemWidth(em * 8);
                ImGui::InputInt("MB##ramcustom", &ram_.customMb, 256, 1024);
                if (ram_.customMb < 64) ram_.customMb = 64;
            }
            uint64_t mb = kSizes[ram_.size].mb ? kSizes[ram_.size].mb : (uint64_t)ram_.customMb;
            cap = mb << 20;
        }
#ifdef __APPLE__
        uint64_t phys = physicalMemory();
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        if (phys) wrapped(col::Faint, "This Mac has " + kz::humanSize(phys) + " of memory. The disk takes memory as files are written to it.", true);
        if (phys && cap > phys / 2) wrapped(col::Warn, "That's more than half of this Mac's memory: filling it can slow everything down.");
#endif

#ifdef _WIN32
        std::vector<char> letters = freeLetters();
        if (ram_.letter >= (int)letters.size()) ram_.letter = 0;
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        caption("DRIVE LETTER");
        ImGui::Dummy(ImVec2(0, em * 0.1f));
        std::string cur = letters.empty() ? std::string("none free") : std::string(1, letters[ram_.letter]) + ":";
        ImGui::SetNextItemWidth(em * 6);
        if (ImGui::BeginCombo("##ramletter", cur.c_str())) {
            for (int i = 0; i < (int)letters.size(); ++i) {
                std::string l = std::string(1, letters[i]) + ":";
                if (ImGui::Selectable(l.c_str(), i == ram_.letter)) ram_.letter = i;
            }
            ImGui::EndCombo();
        }
        char letter = letters.empty() ? 0 : letters[ram_.letter];
#else
        char letter = 'R';
#endif
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        wrapped(col::Faint,
#ifdef _WIN32
                "A local drive in File Explorer whose files are held in RAM: memory is used as you copy files in and "
                "freed as you delete them. Making it asks for administrator approval. It's removed, and everything "
                "on it gone, when it stops or Kompressor closes."
#elif defined(__APPLE__)
                "A RAM disk in Finder. It's ejected, and everything on it deleted, when it stops or Kompressor closes."
#else
                "A folder in /dev/shm, which is kept in memory (up to half of it, the system's own limit). It's "
                "deleted when it stops or Kompressor closes."
#endif
                ,
                true);
        if (!error.empty()) {
            ImGui::Dummy(ImVec2(0, em * 0.2f));
            wrapped(col::Error, error);
        }

        dialogFooter();
        float bw = em * 9;
        alignRight(bw);
        ImGui::BeginDisabled(letter == 0);
        if (primaryButton("Start drive", Ico::Bolt, ImVec2(bw, bh))) {
            ramRun(kz::RamDrive::needsAdmin() ? "Waiting for administrator approval, then making the drive..." : "Making the drive...",
                   [this, cap, letter]() -> std::string {
                       auto d = std::make_unique<kz::RamDrive>();
                       std::string err;
                       if (!d->start(cap, letter, err)) return err;
                       std::string where = d->where();
                       ram_.drive = std::move(d);  // the window doesn't read it while busy
                       reveal(where);
                       return {};
                   });
        }
        ImGui::EndDisabled();
        endDialog();
        return;
    }

    // ------------------------------------------------------------ running
    kz::RamDrive& d = *ram_.drive;
    uint64_t used = d.used(), cap = d.capacity();
    ImGui::Dummy(ImVec2(0, em * 0.2f));
    ImGui::PushFont(fonts().bold);
    ImGui::TextColored(col::Text, "%s used of %s", kz::humanSize(used).c_str(), kz::humanSize(cap).c_str());
    ImGui::PopFont();
    progressBar(cap ? (float)((double)used / (double)cap) : 0.0f, ImVec2(inner, em * 0.36f), used * 10 > cap * 9 ? col::Warn : col::Accent);
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    kvRow("Location", d.where().c_str(), em * 6);
    if (!error.empty()) {
        ImGui::Dummy(ImVec2(0, em * 0.2f));
        wrapped(col::Error, error);
    }

    dialogFooter();
    float bw = em * 7.5f, gap = em * 0.5f;
    alignRight(bw * 2 + gap);
    if (secondaryButton("Open", ImVec2(bw, bh))) reveal(d.where());
    ImGui::SameLine(0, gap);
    if (primaryButton("Stop", Ico::Close, ImVec2(bw, bh))) {
        auto stop = [this] {
            ramRun("Stopping the drive and deleting its files...", [this]() -> std::string {
                ram_.drive.reset();
                return {};
            });
        };
        if (d.hasFiles())
            askConfirm("Stop the RAM drive?",
                       "Everything on " + d.where() + " (" + kz::humanSize(used) +
                           " used) will be deleted. Copy anything you want to keep first.",
                       "Stop and delete", (int)Ico::Trash, col::Error, stop);
        else
            stop();
    }
    endDialog();
}

}  // namespace kzgui
