#include "kz_zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>

#include "kz_pool.hpp"

namespace kz {

namespace {

constexpr size_t kChunk = 512 * 1024;   // unit of parallel work
constexpr size_t kWindow = 32 * 1024;   // deflate's window: each chunk is primed with this much history
constexpr uint32_t k32 = 0xFFFFFFFFu;
// A file at least this big (or of unknown size) gets ZIP64 sizes in its local header: they are
// patched in after the data, so the room must be there from the start.
constexpr uint64_t kZip64Local = 0xF0000000ull;

struct Entry {
    std::string name;
    uint32_t mode = 0;  // Unix mode, with type bits
    int64_t mtime = 0;
    uint16_t method = 8;
    uint32_t crc = 0;
    uint64_t offset = 0, csize = 0, usize = 0;
    bool zip64Local = false;
};

// One compressed piece of an entry, in archive order.
struct Piece {
    size_t entry = 0;
    bool first = false, last = false;
    bool stored = false;  // the entry uses method 0 (decided by its first piece)
    std::vector<uint8_t> data;
    uint32_t crc = 0;
    uint64_t raw = 0;
    std::string err;
};

Piece compressPiece(size_t entry, std::vector<uint8_t> in, std::vector<uint8_t> dict, bool first, bool last, int level) {
    Piece p;
    p.entry = entry;
    p.first = first;
    p.last = last;
    p.raw = in.size();
    p.crc = (uint32_t)crc32(0L, in.data(), (uInt)in.size());
    if (level == 0 || (first && last && in.empty())) {  // stored (and empty files are simply stored)
        p.stored = true;
        p.data = std::move(in);
        return p;
    }
    z_stream z{};
    if (deflateInit2(&z, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        p.err = "zip: cannot start deflate";
        return p;
    }
    if (!dict.empty()) deflateSetDictionary(&z, dict.data(), (uInt)dict.size());
    p.data.resize(deflateBound(&z, (uLong)in.size()) + 64);
    z.next_in = in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = p.data.data();
    z.avail_out = (uInt)p.data.size();
    // Every piece but the last ends on a byte boundary (sync flush), so the next one can follow it.
    int r = deflate(&z, last ? Z_FINISH : Z_SYNC_FLUSH);
    if ((last && r != Z_STREAM_END) || (!last && r != Z_OK)) p.err = "zip: deflate failed";
    p.data.resize(z.total_out);
    deflateEnd(&z);
    // A small file that doesn't shrink is stored instead (only possible when it is a single piece).
    if (first && last && p.err.empty() && p.data.size() >= in.size()) {
        p.stored = true;
        p.data = std::move(in);
    }
    return p;
}

void put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, v & 0xFFFF);
    put16(b, v >> 16);
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    put32(b, (uint32_t)v);
    put32(b, (uint32_t)(v >> 32));
}

void dosTime(int64_t t, uint16_t& time, uint16_t& date) {
    std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &tt) != 0) tm = std::tm{};
#else
    if (!localtime_r(&tt, &tm)) tm = std::tm{};
#endif
    if (tm.tm_year < 80) {  // DOS dates start in 1980
        time = 0;
        date = (1 << 5) | 1;
        return;
    }
    time = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    date = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

// "UT" extended timestamp: modification time in Unix seconds.
void putTimestamp(std::vector<uint8_t>& b, int64_t mtime) {
    put16(b, 0x5455);
    put16(b, 5);
    b.push_back(1);
    put32(b, (uint32_t)std::clamp<int64_t>(mtime, 0, 0xFFFFFFFFll));
}

}  // namespace

struct ZipWriter::Impl {
    std::string path;
    std::fstream out;
    uint64_t pos = 0;
    int level = 6;
    std::unique_ptr<ThreadPool> pool;
    size_t maxQueued = 8;
    std::deque<std::future<Piece>> q;
    std::vector<Entry> entries;
    Progress* prog = nullptr;
    std::string err;
    bool failed = false, opened = false;

    // The file being fed.
    bool inFile = false;
    size_t cur = 0;
    bool firstPiece = true;
    std::vector<uint8_t> buf, dict;
    uint64_t expected = UINT64_MAX;

    bool fail(const std::string& e) {
        if (!failed) err = e;
        failed = true;
        return false;
    }
    bool put(const std::vector<uint8_t>& b) { return put(b.data(), b.size()); }
    bool put(const void* p, size_t n) {
        out.write((const char*)p, (std::streamsize)n);
        pos += n;
        return out ? true : fail("write error (disk full?)");
    }

    size_t addEntry(const std::string& name, uint32_t mode, int64_t mtime, uint32_t type) {
        Entry e;
        e.name = name;
        e.mode = (mode & 0170000) ? mode : (mode | type);
        e.mtime = mtime;
        entries.push_back(std::move(e));
        return entries.size() - 1;
    }

    // Queues one piece; finished pieces are written in order as they come back.
    bool submit(size_t entry, std::vector<uint8_t> data, bool first, bool last) {
        std::vector<uint8_t> d0;
        if (!first) d0 = dict;
        if (!last && data.size() >= kWindow) dict.assign(data.end() - kWindow, data.end());
        int lv = level;
        Progress* pr = prog;
        size_t n = data.size();
        q.push_back(pool->submit([entry, data = std::move(data), d0 = std::move(d0), first, last, lv, pr, n]() mutable {
            Piece p = compressPiece(entry, std::move(data), std::move(d0), first, last, lv);
            if (pr) pr->pending -= (int64_t)n;
            return p;
        }));
        while (!q.empty() && q.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            if (!drain()) return false;
        while (q.size() >= maxQueued)
            if (!drain()) return false;
        return !failed;
    }

    // A piece that needs no compressing (a folder, a symlink target), kept in order with the rest.
    bool submitReady(size_t entry, std::vector<uint8_t> data) {
        Piece p;
        p.entry = entry;
        p.first = p.last = p.stored = true;
        p.raw = data.size();
        p.crc = (uint32_t)crc32(0L, data.data(), (uInt)data.size());
        p.data = std::move(data);
        std::promise<Piece> pr;
        pr.set_value(std::move(p));
        q.push_back(pr.get_future());
        while (q.size() >= maxQueued)
            if (!drain()) return false;
        return !failed;
    }

    bool drain() {
        Piece p = q.front().get();
        q.pop_front();
        if (!p.err.empty()) return fail(p.err);
        Entry& e = entries[p.entry];
        if (p.first) {
            e.method = p.stored ? 0 : 8;
            e.offset = pos;
            if (!writeLocalHeader(e)) return false;
        }
        e.crc = p.first ? p.crc : (uint32_t)crc32_combine(e.crc, p.crc, (z_off_t)p.raw);
        e.usize += p.raw;
        e.csize += p.data.size();
        if (!put(p.data)) return false;
        if (p.last) return patchLocalHeader(e);
        return true;
    }

    bool writeLocalHeader(const Entry& e) {
        uint16_t t, dt;
        dosTime(e.mtime, t, dt);
        std::vector<uint8_t> extra;
        if (e.zip64Local) {
            put16(extra, 0x0001);
            put16(extra, 16);
            put64(extra, 0);  // uncompressed size, patched later
            put64(extra, 0);  // compressed size, patched later
        }
        putTimestamp(extra, e.mtime);
        std::vector<uint8_t> h;
        put32(h, 0x04034b50);
        put16(h, e.zip64Local ? 45 : (e.method == 8 ? 20 : 10));
        put16(h, 0x0800);  // names are UTF-8
        put16(h, e.method);
        put16(h, t);
        put16(h, dt);
        put32(h, 0);  // crc, patched later
        put32(h, e.zip64Local ? k32 : 0);
        put32(h, e.zip64Local ? k32 : 0);
        put16(h, (uint32_t)e.name.size());
        put16(h, (uint32_t)extra.size());
        h.insert(h.end(), e.name.begin(), e.name.end());
        h.insert(h.end(), extra.begin(), extra.end());
        return put(h);
    }

    // Sizes and CRC are known only after the data: go back and fill them in.
    bool patchLocalHeader(const Entry& e) {
        if (!e.zip64Local && (e.csize >= k32 || e.usize >= k32))
            return fail("zip: " + e.name + " grew past 4 GB beyond its expected size");
        std::vector<uint8_t> b;
        put32(b, e.crc);
        if (!e.zip64Local) {
            put32(b, (uint32_t)e.csize);
            put32(b, (uint32_t)e.usize);
        }
        out.seekp((std::streamoff)(e.offset + 14));
        out.write((const char*)b.data(), (std::streamsize)b.size());
        if (e.zip64Local) {
            std::vector<uint8_t> z;
            put64(z, e.usize);
            put64(z, e.csize);
            out.seekp((std::streamoff)(e.offset + 30 + e.name.size() + 4));
            out.write((const char*)z.data(), (std::streamsize)z.size());
        }
        out.seekp((std::streamoff)pos);
        return out ? true : fail("write error (disk full?)");
    }

    bool endFile() {
        if (!inFile) return true;
        inFile = false;
        std::vector<uint8_t> last;
        last.swap(buf);
        return submit(cur, std::move(last), firstPiece, true);
    }

    bool writeCentralDirectory() {
        uint64_t cdStart = pos;
        std::vector<uint8_t> cd;
        for (auto& e : entries) {
            uint16_t t, dt;
            dosTime(e.mtime, t, dt);
            bool bigU = e.usize >= k32, bigC = e.csize >= k32, bigO = e.offset >= k32;
            std::vector<uint8_t> extra;
            if (bigU || bigC || bigO) {
                put16(extra, 0x0001);
                put16(extra, (uint32_t)(8 * (bigU + bigC + bigO)));
                if (bigU) put64(extra, e.usize);
                if (bigC) put64(extra, e.csize);
                if (bigO) put64(extra, e.offset);
            }
            putTimestamp(extra, e.mtime);
            bool z64 = bigU || bigC || bigO || e.zip64Local;
            bool dir = (e.mode & 0170000) == 0040000;
            put32(cd, 0x02014b50);
            put16(cd, (3 << 8) | 45);  // made by: Unix (so the external attributes carry the mode), spec 4.5
            put16(cd, z64 ? 45 : (e.method == 8 ? 20 : 10));
            put16(cd, 0x0800);
            put16(cd, e.method);
            put16(cd, t);
            put16(cd, dt);
            put32(cd, e.crc);
            put32(cd, bigC ? k32 : (uint32_t)e.csize);
            put32(cd, bigU ? k32 : (uint32_t)e.usize);
            put16(cd, (uint32_t)e.name.size());
            put16(cd, (uint32_t)extra.size());
            put16(cd, 0);  // comment
            put16(cd, 0);  // disk
            put16(cd, 0);  // internal attributes
            put32(cd, (e.mode << 16) | (dir ? 0x10 : 0));
            put32(cd, bigO ? k32 : (uint32_t)e.offset);
            cd.insert(cd.end(), e.name.begin(), e.name.end());
            cd.insert(cd.end(), extra.begin(), extra.end());
            if (cd.size() > (1u << 20)) {
                if (!put(cd)) return false;
                cd.clear();
            }
        }
        if (!put(cd)) return false;
        uint64_t cdSize = pos - cdStart, n = entries.size();
        std::vector<uint8_t> end;
        if (n >= 0xFFFF || cdSize >= k32 || cdStart >= k32) {
            uint64_t z64At = pos;
            put32(end, 0x06064b50);  // ZIP64 end of central directory record
            put64(end, 44);
            put16(end, (3 << 8) | 45);
            put16(end, 45);
            put32(end, 0);
            put32(end, 0);
            put64(end, n);
            put64(end, n);
            put64(end, cdSize);
            put64(end, cdStart);
            put32(end, 0x07064b50);  // and its locator
            put32(end, 0);
            put64(end, z64At);
            put32(end, 1);
        }
        put32(end, 0x06054b50);
        put16(end, 0);
        put16(end, 0);
        put16(end, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
        put16(end, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
        put32(end, (uint32_t)std::min<uint64_t>(cdSize, k32));
        put32(end, (uint32_t)std::min<uint64_t>(cdStart, k32));
        put16(end, 0);
        return put(end);
    }
};

ZipWriter::ZipWriter() : d(std::make_unique<Impl>()) {}
ZipWriter::~ZipWriter() {
    if (d->opened) abort();
}

const std::string& ZipWriter::error() const { return d->err; }
void ZipWriter::setProgress(Progress* p) { d->prog = p; }
void ZipWriter::expectSize(uint64_t bytes) { d->expected = bytes; }

bool ZipWriter::open(const std::string& outPath, int level, int threads, std::string& err) {
    d->path = outPath;
    d->level = std::clamp(level, 0, 9);
    d->out.open(u8path(outPath), std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!d->out) {
        err = "cannot create " + outPath;
        return false;
    }
    unsigned t = threadsFor(kChunk * 4, threads);
    d->pool = std::make_unique<ThreadPool>(t);
    d->maxQueued = (size_t)t * 3 + 2;  // enough pieces in flight that no core waits
    d->opened = true;
    return true;
}

void ZipWriter::addDir(const std::string& path, uint32_t mode, int64_t mtime) {
    if (!d->endFile()) return;
    std::string name = path;
    if (name.empty() || name.back() != '/') name += '/';
    size_t i = d->addEntry(name, mode ? mode : 0755, mtime, 0040000);
    d->submitReady(i, {});
}

bool ZipWriter::addSymlink(const std::string& path, const std::string& target, int64_t mtime) {
    if (!d->endFile()) return false;
    size_t i = d->addEntry(path, 0120777, mtime, 0120000);
    return d->submitReady(i, std::vector<uint8_t>(target.begin(), target.end()));
}

bool ZipWriter::beginFile(const std::string& path, uint32_t mode, int64_t mtime) {
    if (!d->endFile()) return false;
    d->cur = d->addEntry(path, mode ? mode : 0644, mtime, 0100000);
    d->entries[d->cur].zip64Local = d->expected == UINT64_MAX || d->expected >= kZip64Local;
    d->expected = UINT64_MAX;
    d->inFile = true;
    d->firstPiece = true;
    d->buf.clear();
    d->buf.reserve(kChunk);
    d->dict.clear();
    return !d->failed;
}

bool ZipWriter::write(const void* data, size_t n) {
    if (!d->inFile) return d->fail("zip: data outside a file");
    const uint8_t* p = (const uint8_t*)data;
    if (d->prog) d->prog->pending += (int64_t)n;
    while (n) {
        size_t take = std::min(n, kChunk - d->buf.size());
        d->buf.insert(d->buf.end(), p, p + take);
        p += take;
        n -= take;
        if (d->buf.size() == kChunk) {
            std::vector<uint8_t> piece;
            piece.swap(d->buf);
            d->buf.reserve(kChunk);
            bool first = d->firstPiece;
            d->firstPiece = false;
            if (!d->submit(d->cur, std::move(piece), first, false)) return false;
        }
    }
    return !d->failed;
}

bool ZipWriter::finish(std::string& err) {
    bool ok = d->endFile();
    while (ok && !d->q.empty()) ok = d->drain();
    if (ok) ok = d->writeCentralDirectory();
    d->out.close();
    d->opened = false;
    if (!ok || d->failed) {
        err = d->err.empty() ? "zip: write failed" : d->err;
        std::error_code ec;
        std::filesystem::remove(u8path(d->path), ec);
        return false;
    }
    return true;
}

void ZipWriter::abort() {
    for (auto& f : d->q)
        if (f.valid()) f.wait();
    d->q.clear();
    d->out.close();
    d->opened = false;
    std::error_code ec;
    std::filesystem::remove(u8path(d->path), ec);
}

}  // namespace kz
