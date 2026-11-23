// Kompressor - shared utilities used by the core, the CLI and the GUI.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kz {

namespace fs = std::filesystem;

// Thread-safe progress / cancellation shared between a worker and the UI.
class Progress {
public:
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> needPassword{false};

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }
    float fraction() const {
        uint64_t t = total.load(), d = done.load();
        if (t == 0) return 0.0f;
        double f = double(d) / double(t);
        return float(f > 1.0 ? 1.0 : f);
    }
    void setStatus(std::string s) {
        std::lock_guard<std::mutex> l(m_);
        status_ = std::move(s);
    }
    std::string status() const {
        std::lock_guard<std::mutex> l(m_);
        return status_;
    }
    void warn(std::string w) {
        std::lock_guard<std::mutex> l(m_);
        if (warnings_.size() < 1000) warnings_.push_back(std::move(w));
    }
    std::vector<std::string> warnings() const {
        std::lock_guard<std::mutex> l(m_);
        return warnings_;
    }

private:
    mutable std::mutex m_;
    std::string status_;
    std::vector<std::string> warnings_;
};

// An archive can live on disk or (for nested archives) in memory.
struct Source {
    std::string path;                                 // UTF-8 path on disk
    std::shared_ptr<const std::vector<uint8_t>> mem;  // set for in-memory archives
    std::string displayName;                          // shown in the UI / used for raw entry names

    bool inMemory() const { return mem != nullptr; }
    std::string name() const;
};

// --- paths (all strings in Kompressor are UTF-8) ---------------------------
fs::path u8path(const std::string& s);
std::string pathToUtf8(const fs::path& p);
std::string fileNameOf(const std::string& path);
std::string lowerExt(const std::string& name);      // "a/b.TAR.gz" -> "gz"
std::string stripArchiveExt(const std::string& name);  // "foo.tar.gz" -> "foo"
// Unicode NFC normalisation (macOS libarchive hands out decomposed names). No-op elsewhere.
std::string toNFC(const std::string& s);

// Normalises an archive member path ('\\' -> '/', removes "./", trailing '/').
// Returns false when the path is unsafe to extract (absolute, drive letter, "..").
bool sanitizeRelativePath(const std::string& in, std::string& out);

// --- file metadata ------------------------------------------------------------
struct FileStat {
    bool exists = false;
    bool isDir = false;
    bool isSymlink = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;
};
FileStat statPath(const fs::path& p, bool followSymlinks = false);
void setFileMTime(const fs::path& p, int64_t mtime);
void setFileMode(const fs::path& p, uint32_t mode);
std::string readSymlink(const fs::path& p);

// --- formatting ---------------------------------------------------------------
std::string humanSize(uint64_t bytes);
std::string formatTime(int64_t unixTime);
std::string formatDuration(double seconds);

unsigned hardwareThreads();

// Makes libarchive's narrow-string conversions UTF-8 aware. Call once at startup.
void initLocale();

// Command line arguments as UTF-8 on every platform (Windows re-reads the wide command line).
std::vector<std::string> utf8Args(int argc, char** argv);

}  // namespace kz
