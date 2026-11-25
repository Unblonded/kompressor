// Shared plumbing for the formats Kompressor reads itself (see kz_native.hpp): random-access streams
// that can be layered (a partition inside a virtual disk inside a file), bounds-checked readers, text
// helpers, and the table of format handlers.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../kz_native.hpp"

namespace kz::native {

using Sink = std::function<bool(const uint8_t*, size_t)>;

// ------------------------------------------------------------------------------------------- streams
class Stream {
public:
    virtual ~Stream() = default;
    virtual uint64_t size() const = 0;
    // Exactly n bytes at off, or false.
    virtual bool readAt(uint64_t off, void* dst, size_t n) = 0;

    std::vector<uint8_t> get(uint64_t off, size_t n) {
        std::vector<uint8_t> v(n);
        if (!readAt(off, v.data(), n)) v.clear();
        return v;
    }
    std::vector<uint8_t> getUpTo(uint64_t off, size_t n) {
        if (off >= size()) return {};
        return get(off, (size_t)std::min<uint64_t>(n, size() - off));
    }
};
using StreamPtr = std::shared_ptr<Stream>;

StreamPtr fileStream(const std::string& utf8Path);
StreamPtr memStream(std::shared_ptr<const std::vector<uint8_t>> data);
StreamPtr memStream(std::vector<uint8_t> data);
// Bytes [off, off + len) of another stream.
StreamPtr rangeStream(StreamPtr parent, uint64_t off, uint64_t len);

// A stream made of fixed-size blocks that are produced on demand (virtual disks, compressed images).
// The most recently used blocks are kept.
class BlockStream : public Stream {
public:
    BlockStream(uint64_t size, uint32_t blockSize) : size_(size), bs_(blockSize) {}
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t off, void* dst, size_t n) override;

protected:
    // Fills `dst` (blockSize bytes) with block `index`.
    virtual bool readBlock(uint64_t index, uint8_t* dst) = 0;
    uint64_t size_;
    uint32_t bs_;

private:
    struct Cached {
        uint64_t index = UINT64_MAX;
        std::vector<uint8_t> data;
        uint64_t used = 0;
    };
    Cached cache_[4];
    uint64_t tick_ = 0;
};

// A stream put together from pieces of other streams, zero-filled where nothing is mapped.
class ExtentStream : public Stream {
public:
    struct Extent {
        uint64_t at;      // where it starts in this stream
        uint64_t len;
        StreamPtr src;    // null = zeros
        uint64_t srcOff;
        std::vector<uint8_t> pattern;  // with src null: repeated fill (Android sparse images), empty = zeros
    };
    ExtentStream(uint64_t size, std::vector<Extent> ext);
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t off, void* dst, size_t n) override;

private:
    uint64_t size_;
    std::vector<Extent> ext_;  // sorted by `at`
};

// ------------------------------------------------------------------------------------------- reading
// Bounds-checked little/big-endian reads; out of range reads as 0.
struct Span {
    const uint8_t* p = nullptr;
    size_t n = 0;
    Span() = default;
    Span(const uint8_t* d, size_t len) : p(d), n(len) {}
    explicit Span(const std::vector<uint8_t>& v) : p(v.data()), n(v.size()) {}
    bool has(size_t o, size_t len) const { return o <= n && len <= n - o; }
    uint32_t u8(size_t o) const { return has(o, 1) ? p[o] : 0; }
    uint32_t u16(size_t o) const { return has(o, 2) ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 : 0; }
    uint32_t u24(size_t o) const { return has(o, 3) ? u16(o) | (uint32_t)p[o + 2] << 16 : 0; }
    uint32_t u32(size_t o) const { return has(o, 4) ? u16(o) | u16(o + 2) << 16 : 0; }
    uint64_t u64(size_t o) const { return (uint64_t)u32(o) | (uint64_t)u32(o + 4) << 32; }
    uint32_t b16(size_t o) const { return has(o, 2) ? (uint32_t)p[o] << 8 | p[o + 1] : 0; }
    uint32_t b24(size_t o) const { return has(o, 3) ? (uint32_t)p[o] << 16 | b16(o + 1) : 0; }
    uint32_t b32(size_t o) const { return has(o, 4) ? b16(o) << 16 | b16(o + 2) : 0; }
    uint64_t b64(size_t o) const { return (uint64_t)b32(o) << 32 | b32(o + 4); }
    bool eq(size_t o, const char* s, size_t len) const { return has(o, len) && std::memcmp(p + o, s, len) == 0; }
};

void put16(std::vector<uint8_t>& v, uint32_t x);
void put32(std::vector<uint8_t>& v, uint32_t x);
void putUtf8(std::string& s, uint32_t c);
std::string utf16le(const uint8_t* p, size_t chars);   // stops at nothing: give the exact length
std::string utf16be(const uint8_t* p, size_t chars);
std::string latin1(const uint8_t* p, size_t n);       // stops at the first NUL
std::string cstr(const uint8_t* p, size_t max);        // UTF-8/ASCII up to the first NUL
// A name usable as one path component.
std::string component(std::string s);
// An extension (".png", ".zip"...) for data by what it starts with; ".bin" when unknown.
std::string extFor(const uint8_t* p, size_t n);
std::string hex(uint64_t v, int digits = 0);
// A GUID from its text form, as the 16 bytes stored on disk (first three fields little-endian).
std::vector<uint8_t> guidBytes(const char* text);
std::string guidText(const uint8_t* p);
int64_t fromFiletime(uint64_t ft);
int64_t fromDos(uint32_t date, uint32_t time);  // DOS date/time (local, taken as UTC)
// UTF-8 text with a byte-order mark, so every Windows viewer reads it as UTF-8.
std::vector<uint8_t> textFile(const std::string& s);

// --------------------------------------------------------------------------------------------- items
void addRun(std::vector<std::pair<uint64_t, uint64_t>>& runs, uint64_t off, uint64_t len);
Item rangeItem(std::string path, StreamPtr s, uint64_t off, uint64_t len, int64_t mtime);
// Pieces of `s` in order; a piece with offset UINT64_MAX reads as zeros (a hole).
Item runsItem(std::string path, StreamPtr s, std::vector<std::pair<uint64_t, uint64_t>> runs, uint64_t size,
              int64_t mtime);
Item builtItem(std::string path, std::vector<uint8_t> data, int64_t mtime);
Item streamItem(std::string path, StreamPtr s, int64_t mtime);
Item dirItem(std::string path, int64_t mtime);
constexpr uint64_t kHole = UINT64_MAX;

// ------------------------------------------------------------------------------------------ handlers
// What a handler is given: the stream to read, where its files go in the listing, and the list.
struct Ctx {
    Container& out;
    std::string prefix;   // "" or "2.ntfs/"
    std::string name;     // the file's name, for formats only known by their extension
    int depth = 0;        // nesting (a disk in a disk...), capped
    std::string err;
    std::vector<std::string> formats;  // "VHDX", "GPT", "NTFS"... for the status line
    std::string srcPath;  // the file on disk, when this is it (differencing disks find their parents by it)
};

// A virtual disk image (VHD, VHDX, VDI, VMDK, QCOW2) as the disk it holds, or null when `s` isn't one.
// `path` (may be empty) locates parent/backing images and split extents; `fmt` gets the format's name.
StreamPtr openDiskImage(StreamPtr s, const std::string& path, std::string& fmt, std::string& err, int depth = 0);

struct Handler {
    const char* name;
    // Cheap check on the first bytes (`head` holds up to 72 KiB) and the stream itself.
    bool (*probe)(Stream& s, const std::vector<uint8_t>& head, const std::string& name);
    // Lists what's inside into ctx.out (paths start with ctx.prefix). False with ctx.err when damaged.
    bool (*open)(Ctx& ctx, StreamPtr s);
};

// Looks at a stream and lists what's inside by whichever handler recognises it. False when none does.
// Disk images and partition maps call it again on what they contain.
bool explore(Ctx& ctx, StreamPtr s);
// Like explore, but for a partition or other piece inside a container: when nothing recognises it,
// it's listed as one file `rawName` instead. Returns the format found ("" when none).
std::string explorePart(Ctx& ctx, StreamPtr s, const std::string& folder, const std::string& rawName);

// Each format's handler (defined in its own file).
#define KZ_HANDLER(id) extern const Handler id;
KZ_HANDLER(hPe)
KZ_HANDLER(hTe)
KZ_HANDLER(hElf)
KZ_HANDLER(hMachO)
KZ_HANDLER(hMub)
KZ_HANDLER(hCoff)
KZ_HANDLER(hCfb)
KZ_HANDLER(hVhd)
KZ_HANDLER(hVhdx)
KZ_HANDLER(hVdi)
KZ_HANDLER(hVmdk)
KZ_HANDLER(hQcow)
KZ_HANDLER(hSparse)
KZ_HANDLER(hGpt)
KZ_HANDLER(hApm)
KZ_HANDLER(hMbr)
KZ_HANDLER(hNtfs)
KZ_HANDLER(hExfat)
KZ_HANDLER(hFat)
KZ_HANDLER(hExt)
KZ_HANDLER(hSquash)
KZ_HANDLER(hCramfs)
KZ_HANDLER(hHfs)
KZ_HANDLER(hUdf)
KZ_HANDLER(hApfs)
KZ_HANDLER(hNsis)
KZ_HANDLER(hChm)
KZ_HANDLER(hWim)
KZ_HANDLER(hArj)
KZ_HANDLER(hDmg)
KZ_HANDLER(hMslz)
KZ_HANDLER(hSwf)
KZ_HANDLER(hFlv)
KZ_HANDLER(hPpmd)
KZ_HANDLER(hBase64)
KZ_HANDLER(hIhex)
KZ_HANDLER(hUefi)
KZ_HANDLER(hLp)
KZ_HANDLER(hLvm)
KZ_HANDLER(hXar)
#undef KZ_HANDLER

// ------------------------------------------------------------------------------------------- codecs
// Each returns false on damaged input. `out` is resized to what was produced.
bool inflateZlib(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);  // zlib wrapper
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);   // bare deflate
bool unBzip2(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unXz(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);         // .xz or .lzma
bool unLzmaRaw(const uint8_t* props5, const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unZstd(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unLz4Block(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unLzo1x(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unLznt1(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unAdc(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unLzvn(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unLzfse(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
bool unXpressHuff(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
// LZX as used by WIM (one 32 KiB chunk, its own E8 translation) and CHM (a running decoder).
class LzxDecoder {
public:
    explicit LzxDecoder(int windowBits);
    ~LzxDecoder();
    void reset();
    // Decodes the next `outLen` bytes from `in` (CHM: frames continue the previous state).
    bool decode(const uint8_t* in, size_t n, uint8_t* out, size_t outLen);
    void setE8(bool on, uint32_t translationSize) { e8_ = on, e8Size_ = translationSize; }
    // WIM / Windows compact files: no E8 header bit, a different block size field, chunks decoded alone.
    void setWim(bool on) { wim_ = on; }

private:
    struct State;
    State* st_;
    bool e8_ = false;
    bool wim_ = false;
    uint32_t e8Size_ = 12000000;
};
bool unLzms(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect);
// The LZ77 + static Huffman coding of ARJ (np 17, pbit 5) and EFI/Tiano firmware compression
// (np 14 / pbit 4, and np 20 / pbit 5).
bool unLzhStatic(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t size, int np, int pbit);
// A transparently compressed Mac file (HFS+/APFS): its "com.apple.decmpfs" attribute and resource fork.
bool decmpfs(const std::vector<uint8_t>& attr, StreamPtr rsrc, const Sink& sink, std::string& err);

}  // namespace kz::native
