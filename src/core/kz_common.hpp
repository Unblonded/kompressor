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
    // Bytes already counted in `done` (read from the input) that a compressor still holds in its
    // queue: worker threads can lag far behind the reader, and the bar should show real work.
    std::atomic<int64_t> pending{0};
    // Decoding work (extract/test of big blocks): when set, the bar follows it rather than only the
    // bytes handed out, which stall while a block decodes.
    std::atomic<uint64_t> workTotal{0}, workDone{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> needPassword{false};

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }
    float fraction() const {
        uint64_t t = total.load();
        int64_t d = (int64_t)done.load() - pending.load();
        double f = t == 0 ? 0.0 : double(d < 0 ? 0 : d) / double(t);
        if (f > 1.0) f = 1.0;
        uint64_t wt = workTotal.load();
        if (wt) {
            double w = double(workDone.load()) / double(wt);
            f = 0.85 * (w > 1.0 ? 1.0 : w) + 0.15 * f;
        }
        return float(f);
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
// Physical memory currently available, in bytes (0 when unknown).
uint64_t availableMemory();
// Worker count for jobs that each need `bytesPerThread` of memory: every core, as long as the jobs
// fit in most of the free RAM. `requested` > 0 overrides.
unsigned threadsFor(uint64_t bytesPerThread, int requested = 0);

// --- multi-volume sets ----------------------------------------------------------
// Every file of the multi-volume set `path` belongs to, first volume first: "x.part1.rar"...,
// "x.7z.001"..., or old-style "x.rar" + "x.r00"... Any volume may be passed. Empty when `path` is
// not part of a set of two or more.
std::vector<std::string> volumeSet(const std::string& path);
// The first volume of the set `path` belongs to, or `path` itself.
std::string firstVolume(const std::string& path);
// True for names that look like a volume of a split archive (".001", ".part2.rar", ".r00").
bool looksLikeVolume(const std::string& name);
// Splits a finished file into "<path>.001", ".002"... of `volumeSize` bytes (the original becomes
// part 1). A file that already fits is left as it is. `parts` receives the resulting file names.
bool splitIntoVolumes(const std::string& path, uint64_t volumeSize, std::vector<std::string>& parts, std::string& err);

// Makes libarchive's narrow-string conversions UTF-8 aware. Call once at startup.
void initLocale();

// Command line arguments as UTF-8 on every platform (Windows re-reads the wide command line).
std::vector<std::string> utf8Args(int argc, char** argv);

}  // namespace kz
