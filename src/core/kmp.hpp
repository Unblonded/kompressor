// KMP - the Kompressor super-compression container.
//
// Layout (all integers little endian):
//
//   [Header 16 B]  "KMPR\x1A\r\n\0" | u16 version | u16 flags | u32 reserved
//   [Block 0..N]   independently compressed solid blocks
//   [Index]        zstd-compressed table of blocks, chunks and entries
//   [Footer 40 B]  u64 indexOffset | u64 indexPacked | u64 indexRaw | u64 xxh64(index) | "KMPFOOT\0"
//
// How it squeezes harder than a plain archive:
//   * Content-defined chunking (FastCDC / gear hash) + XXH3-128 dedup: identical data that appears
//     anywhere - duplicate files, shared regions in VM images, repeated assets - is stored once.
//   * Content-aware streams: data is routed into separate solid blocks by kind (text, x86 code,
//     ARM64 code, already-compressed media, generic binary) so similar data shares a dictionary.
//   * Per-stream codecs: LZMA2 with x86/ARM64 branch converters for executables, text-tuned LZMA2
//     literal context for text, store/fast-zstd for media, and in Max mode every block is compressed
//     with several candidates and the smallest wins.
//   * Extreme mode replaces LZ with KCM, a context-mixing coder (see kz_cm.hpp): every bit is
//     predicted by a dozen models mixed by online-trained networks. Several times slower in both
//     directions, noticeably smaller on text, code and structured binaries.
//   * Ultimate mode runs a heavier KCM (bit-history states, more models, mixers and SSE, 1 GiB
//     tables) over blocks of up to 256 MiB, so the model learns from far more history. Built for
//     archiving: expect a few hundred KB/s per stream, and several GiB of RAM.
//   * Files are fed grouped by extension, then name, so related data lands in the same block.
//   * The index lives at the end, so listing/exploring is instant and single files can be read
//     with random access (only the blocks that hold them are decompressed).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kz_common.hpp"

namespace kz::kmp {

enum class Mode : int { Fast = 0, Balanced = 1, Ultra = 2, Max = 3, Extreme = 4, Ultimate = 5 };
const char* modeName(Mode m);

struct Options {
    Mode mode = Mode::Ultra;
    int threads = 0;          // 0 = auto (memory-aware)
    bool dedup = true;
    uint32_t blockSizeMB = 0; // 0 = auto for the mode
    uint64_t sizeHint = 0;    // expected input bytes; small inputs get smaller blocks so every core works
};

enum class EntryType : uint8_t { File = 0, Dir = 1, Symlink = 2 };

struct Entry {
    std::string path;
    EntryType type = EntryType::File;
    uint32_t mode = 0;
    int64_t mtime = 0;
    uint64_t size = 0;
    uint64_t hash = 0;  // XXH3-64 of the content
    std::string link;
    std::vector<uint32_t> chunks;
};

struct Stats {
    uint64_t rawBytes = 0;      // total input bytes
    uint64_t uniqueBytes = 0;   // after dedup
    uint64_t packedBytes = 0;   // compressed blocks
    uint64_t files = 0;
    uint64_t chunks = 0;
    uint64_t dupChunks = 0;
    uint64_t blocks = 0;
    std::string summary() const;
};

class Writer {
public:
    Writer();
    ~Writer();
    bool open(const fs::path& out, const Options& opt, std::string& err);
    void setProgress(Progress* p);
    void addDir(const std::string& path, uint32_t mode, int64_t mtime);
    void addSymlink(const std::string& path, const std::string& target, uint32_t mode, int64_t mtime);
    void beginFile(const std::string& path, uint32_t mode, int64_t mtime);
    bool write(const void* data, size_t n);
    bool endFile();
    bool finish(std::string& err);
    void abort();
    const Stats& stats() const;
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

class Reader {
public:
    Reader();
    ~Reader();
    bool open(const Source& src, std::string& err);
    const std::vector<Entry>& entries() const;
    const Stats& stats() const;
    Mode mode() const;
    uint64_t archiveSize() const;
    // Streams a file's content; returning false from the sink stops early.
    bool readFile(size_t index, const std::function<bool(const uint8_t*, size_t)>& sink, Progress* prog,
                  std::string& err);
    int findEntry(const std::string& path) const;
    // Call before reading files front to back (extract, test, convert): blocks are then decoded
    // ahead on all cores.
    // With `prog`, decoding progress is reported in its workDone/workTotal.
    void setSequential(Progress* prog = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

bool isKmp(const Source& src);

}  // namespace kz::kmp
