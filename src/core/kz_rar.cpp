#include "kz_rar.hpp"

#include <lzma.h>  // lzma_crc32

#include <fstream>
#include <vector>

namespace kz {

namespace {

constexpr uint8_t kSignature[8] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00};  // "Rar!" 1A 07 01 00

enum : uint64_t {
    kTypeMain = 1,
    kTypeFile = 2,
    kTypeEnd = 5,
    kHflagData = 0x0002,      // header is followed by a data area
    kFflagDir = 0x0001,       // file flags: directory
    kFflagUnixTime = 0x0002,  //             mtime field present
    kFflagCrc = 0x0004,       //             data CRC32 present
    kHostWindows = 0,
    kHostUnix = 1,
    kRedirUnixSymlink = 1,
    kExtraRedirection = 5,
};

// Variable-length integer: 7 bits per byte, low bits first, top bit set on every byte but the last.
void vint(std::vector<uint8_t>& b, uint64_t v) {
    while (v >= 0x80) {
        b.push_back((uint8_t)(v | 0x80));
        v >>= 7;
    }
    b.push_back((uint8_t)v);
}
// The same, always 8 bytes long (values up to 2^56), so the field can be rewritten in place once the
// real value is known.
void vintFixed(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        b[at + i] = (uint8_t)((v & 0x7F) | (i < 7 ? 0x80 : 0));
        v >>= 7;
    }
}
void u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i)));
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) { return lzma_crc32(p, n, crc); }

// A complete block: CRC32 of everything after it, header size, then `body` (type, flags, fields).
std::vector<uint8_t> block(const std::vector<uint8_t>& body) {
    std::vector<uint8_t> h;
    vint(h, body.size());
    h.insert(h.end(), body.begin(), body.end());
    std::vector<uint8_t> out;
    u32(out, crc32(h.data(), h.size()));
    out.insert(out.end(), h.begin(), h.end());
    return out;
}

uint32_t unixTime(int64_t t) { return t <= 0 ? 0u : t >= 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)t; }

std::string rarName(std::string p) {
    for (auto& c : p)
        if (c == '\\') c = '/';
    while (!p.empty() && p.back() == '/') p.pop_back();
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return p;
}

}  // namespace

struct RarWriter::Impl {
    std::fstream out;
    std::string path, err;
    bool opened = false, failed = false, inFile = false;
    // The open file's header, kept so its size and CRC fields can be filled in afterwards.
    std::vector<uint8_t> hdr;
    std::streamoff hdrAt = 0;
    size_t dataSizeAt = 0, unpSizeAt = 0, crcAt = 0;
    uint64_t size = 0;
    uint32_t crc = 0;

    void fail(const std::string& e) {
        if (!failed) err = e;
        failed = true;
    }
    void put(const std::vector<uint8_t>& b) {
        out.write((const char*)b.data(), (std::streamsize)b.size());
        if (!out) fail("write error (disk full?)");
    }

    // File/directory header. Layout (RAR 5.0): type, header flags, [data size], file flags, unpacked
    // size, attributes, [mtime], [data CRC32], compression info, host OS, name length, name, [extra].
    std::vector<uint8_t> fileHeader(const std::string& name, bool dir, uint32_t mode, int64_t mtime,
                                    const std::vector<uint8_t>& extra, bool withData, size_t& dataSizeOff,
                                    size_t& unpSizeOff, size_t& crcOff) {
        std::vector<uint8_t> b;
        vint(b, kTypeFile);
        vint(b, (extra.empty() ? 0 : 0x0001) | (withData ? kHflagData : 0));
        if (!extra.empty()) vint(b, extra.size());
        dataSizeOff = unpSizeOff = crcOff = 0;
        if (withData) {
            dataSizeOff = b.size();
            b.resize(b.size() + 8);
            vintFixed(b, dataSizeOff, 0);
        }
        vint(b, (dir ? kFflagDir : 0) | kFflagUnixTime | (dir ? 0 : kFflagCrc));
        unpSizeOff = b.size();
        b.resize(b.size() + 8);
        vintFixed(b, unpSizeOff, 0);
#ifdef _WIN32
        vint(b, dir ? 0x10 : 0x20);  // FILE_ATTRIBUTE_DIRECTORY / _ARCHIVE
#else
        vint(b, mode ? mode : (dir ? 040755 : 0100644));
#endif
        u32(b, unixTime(mtime));
        if (!dir) {
            crcOff = b.size();
            u32(b, 0);
        }
        vint(b, 0);  // compression info: algorithm 0, not solid, method 0 (store)
#ifdef _WIN32
        vint(b, kHostWindows);
        (void)mode;
#else
        vint(b, kHostUnix);
#endif
        vint(b, name.size());
        b.insert(b.end(), name.begin(), name.end());
        b.insert(b.end(), extra.begin(), extra.end());
        return b;
    }

    // Wraps a body into a block and remembers where its patchable fields ended up.
    void startBlock(const std::vector<uint8_t>& body, size_t dataSizeOff, size_t unpSizeOff, size_t crcOff) {
        std::vector<uint8_t> sz;
        vint(sz, body.size());
        size_t shift = 4 + sz.size();  // fields move by the CRC and the size vint in front of them
        hdr = block(body);
        dataSizeAt = dataSizeOff ? dataSizeOff + shift : 0;
        unpSizeAt = unpSizeOff + shift;
        crcAt = crcOff ? crcOff + shift : 0;
        hdrAt = out.tellp();
        put(hdr);
    }

    void closeFile() {
        if (!inFile) return;
        inFile = false;
        if (dataSizeAt) vintFixed(hdr, dataSizeAt, size);
        vintFixed(hdr, unpSizeAt, size);
        if (crcAt)
            for (int i = 0; i < 4; ++i) hdr[crcAt + i] = (uint8_t)(crc >> (8 * i));
        uint32_t hc = crc32(hdr.data() + 4, hdr.size() - 4);
        for (int i = 0; i < 4; ++i) hdr[i] = (uint8_t)(hc >> (8 * i));
        std::streamoff end = out.tellp();
        out.seekp(hdrAt);
        put(hdr);
        out.seekp(end);
        if (!out) fail("write error (disk full?)");
    }
};

RarWriter::RarWriter() : d(new Impl) {}
RarWriter::~RarWriter() {
    if (d->opened) abort();
}
const std::string& RarWriter::error() const { return d->err; }

bool RarWriter::open(const std::string& outPath, std::string& err) {
    d->path = outPath;
    d->out.open(u8path(outPath), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!d->out) {
        err = "cannot create " + outPath;
        return false;
    }
    d->opened = true;
    d->put(std::vector<uint8_t>(kSignature, kSignature + 8));
    std::vector<uint8_t> main;
    vint(main, kTypeMain);
    vint(main, 0);  // header flags
    vint(main, 0);  // archive flags: not a volume, not solid
    d->put(block(main));
    return !d->failed;
}

void RarWriter::addDir(const std::string& path, uint32_t mode, int64_t mtime) {
    d->closeFile();
    std::string name = rarName(path);
    if (name.empty() || d->failed) return;
    size_t a, b, c;
    std::vector<uint8_t> body = d->fileHeader(name, true, mode, mtime, {}, false, a, b, c);
    d->put(block(body));
}

bool RarWriter::addSymlink(const std::string& path, const std::string& target, int64_t mtime) {
    d->closeFile();
    std::string name = rarName(path);
    if (name.empty() || d->failed) return !d->failed;
    // Extra record "file system redirection": size, type, redirection type, flags, target.
    std::vector<uint8_t> rec, extra;
    vint(rec, kExtraRedirection);
    vint(rec, kRedirUnixSymlink);
    vint(rec, 0);
    vint(rec, target.size());
    rec.insert(rec.end(), target.begin(), target.end());
    vint(extra, rec.size());
    extra.insert(extra.end(), rec.begin(), rec.end());
    size_t a, b, c;
    std::vector<uint8_t> body = d->fileHeader(name, false, 0120777, mtime, extra, false, a, b, c);
    d->put(block(body));  // unpacked size and CRC stay 0: a link has no data
    return !d->failed;
}

bool RarWriter::beginFile(const std::string& path, uint32_t mode, int64_t mtime) {
    d->closeFile();
    if (d->failed) return false;
    std::string name = rarName(path);
    if (name.empty()) {
        d->fail("empty file name");
        return false;
    }
    size_t ds, us, cs;
    std::vector<uint8_t> body = d->fileHeader(name, false, mode, mtime, {}, true, ds, us, cs);
    d->size = 0;
    d->crc = 0;
    d->startBlock(body, ds, us, cs);
    d->inFile = true;
    return !d->failed;
}

bool RarWriter::write(const void* data, size_t n) {
    if (d->failed || !d->inFile) return false;
    d->crc = crc32((const uint8_t*)data, n, d->crc);
    d->size += n;
    d->out.write((const char*)data, (std::streamsize)n);
    if (!d->out) d->fail("write error (disk full?)");
    return !d->failed;
}

bool RarWriter::finish(std::string& err) {
    d->closeFile();
    std::vector<uint8_t> end;
    vint(end, kTypeEnd);
    vint(end, 0);  // header flags
    vint(end, 0);  // end of archive flags: last volume
    d->put(block(end));
    d->out.close();
    d->opened = false;
    if (d->failed) {
        err = d->err;
        std::error_code ec;
        fs::remove(u8path(d->path), ec);
        return false;
    }
    return true;
}

void RarWriter::abort() {
    d->out.close();
    d->opened = false;
    std::error_code ec;
    fs::remove(u8path(d->path), ec);
}

}  // namespace kz
