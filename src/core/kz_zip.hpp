// Multithreaded ZIP writer (deflate or store, no encryption).
//
// libarchive deflates ZIP entries on one thread. This writer cuts each file into 512 KiB chunks and
// deflates them on every core the way pigz does: each chunk is primed with the 32 KiB before it as
// its dictionary and ends on a byte boundary (sync flush), so the pieces join into one ordinary
// deflate stream that any unzip tool reads. Small files are whole chunks, so many of them compress
// at once too. Entries carry UTF-8 names, Unix modes, extended timestamps and ZIP64 records where
// sizes or offsets need them. Encrypted ZIPs keep going through libarchive (AES-256).
//
// With "use the graphics card too" on, the card's thread takes batches of the newest pieces whenever
// it will finish them before the CPU threads would reach them (kz_gpu.hpp), so it only adds speed.
#pragma once

#include <memory>
#include <string>

#include "kz_parallel.hpp"

namespace kz {

class ZipWriter : public ArchiveWriter {
public:
    ZipWriter();
    ~ZipWriter() override;
    // level 0 stores, 1..9 are deflate levels.
    bool open(const std::string& outPath, int level, int threads, std::string& err);
    void expectSize(uint64_t bytes) override;
    void addDir(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool addSymlink(const std::string& path, const std::string& target, int64_t mtime) override;
    bool beginFile(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool write(const void* data, size_t n) override;
    bool finish(std::string& err) override;
    void abort() override;
    const std::string& error() const override;
    void setProgress(Progress* p) override;

private:
    void stopWorkers();
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace kz
