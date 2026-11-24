// Multithreaded compressors used in place of libarchive's single-threaded ones:
//   * StreamCompressor - the outer compression of .tar.xz/.xz, .tar.zst/.zst, .tar.gz/.gz and
//     .tar.bz2/.bz2, fed with the plain tar (or raw) stream libarchive produces;
//   * SevenZipWriter   - a 7z writer whose solid LZMA2 stream is cut into independent blocks that
//     are compressed on every core (the way 7-Zip itself multithreads LZMA2).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kz_common.hpp"
#include "kz_formats.hpp"

namespace kz {

class StreamCompressor {
public:
    virtual ~StreamCompressor() = default;
    virtual bool write(const void* data, size_t n) = 0;
    virtual bool finish() = 0;  // flushes everything and closes the file
    const std::string& error() const { return err_; }
    // Input still waiting in the compressor is reported as Progress::pending.
    void setProgress(Progress* p) { prog_ = p; }

protected:
    void pend(int64_t n) {
        if (prog_ && n) prog_->pending += n;
    }
    std::string err_;
    Progress* prog_ = nullptr;
};

// A parallel compressor writing `outPath`, or nullptr when the format has none here (libarchive's
// own filter is used then). `sizeHint` is the expected input size (0 = unknown) and sizes the work
// units so every core gets some.
std::unique_ptr<StreamCompressor> makeStreamCompressor(OutFormat f, int level, int threads, uint64_t sizeHint,
                                                       const std::string& outPath, std::string& err);

// An archive written entry by entry straight to disk (7z, RAR), bypassing libarchive.
class ArchiveWriter {
public:
    virtual ~ArchiveWriter() = default;
    virtual void addDir(const std::string& path, uint32_t mode, int64_t mtime) = 0;
    virtual bool addSymlink(const std::string& path, const std::string& target, int64_t mtime) = 0;
    // Optional: the size of the file the next beginFile() starts (lets ZIP skip ZIP64 headers).
    virtual void expectSize(uint64_t) {}
    virtual bool beginFile(const std::string& path, uint32_t mode, int64_t mtime) = 0;
    virtual bool write(const void* data, size_t n) = 0;
    virtual bool finish(std::string& err) = 0;
    virtual void abort() = 0;
    virtual const std::string& error() const = 0;
    virtual void setProgress(Progress* p) = 0;
};

class SevenZipWriter : public ArchiveWriter {
public:
    SevenZipWriter();
    ~SevenZipWriter() override;
    // level 0 stores, 1..9 are LZMA2 presets.
    bool open(const std::string& outPath, int level, int threads, uint64_t sizeHint, std::string& err);
    void addDir(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool addSymlink(const std::string& path, const std::string& target, int64_t mtime) override;
    bool beginFile(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool write(const void* data, size_t n) override;
    bool finish(std::string& err) override;
    void abort() override;
    const std::string& error() const override;
    void setProgress(Progress* p) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace kz
