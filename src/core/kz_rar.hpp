// RAR5 archive writer (stored, no compression).
//
// Written from RARLAB's published RAR 5.0 archive format description: signature, main archive
// header, one file header per entry (UTF-8 names with '/' separators, Unix mtime, CRC32), and the
// end-of-archive header. RAR's compression algorithm is proprietary and not part of that document,
// so file data is stored as is (method 0); every RAR reader (WinRAR, unrar, 7-Zip, libarchive)
// opens these archives.
#pragma once

#include <memory>
#include <string>

#include "kz_parallel.hpp"

namespace kz {

class RarWriter : public ArchiveWriter {
public:
    RarWriter();
    ~RarWriter() override;
    bool open(const std::string& outPath, std::string& err);
    void addDir(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool addSymlink(const std::string& path, const std::string& target, int64_t mtime) override;
    bool beginFile(const std::string& path, uint32_t mode, int64_t mtime) override;
    bool write(const void* data, size_t n) override;
    bool finish(std::string& err) override;
    void abort() override;
    const std::string& error() const override;
    void setProgress(Progress*) override {}

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace kz
