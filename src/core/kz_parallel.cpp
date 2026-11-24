#include "kz_parallel.hpp"

#include <lzma.h>
#include <zlib.h>
#define ZSTD_STATIC_LINKING_ONLY  // ZSTD_getFrameProgression
#include <zstd.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <vector>

#include "kz_common.hpp"
#include "kz_pool.hpp"

namespace kz {
namespace {

constexpr uint64_t MiB = 1024ull * 1024ull;

// Effective level for a format (the same clamp as the libarchive path).
int levelFor(OutFormat f, int level) {
    const FormatDesc& fd = formatDesc(f);
    return level < 0 ? fd.defLevel : std::clamp(level, fd.minLevel, fd.maxLevel);
}

uint64_t perCore(uint64_t sizeHint, unsigned threads, uint64_t lo, uint64_t hi, uint64_t unknown) {
    if (!sizeHint) return unknown;
    return std::clamp<uint64_t>((sizeHint + threads - 1) / std::max(1u, threads), lo, hi);
}

class FileSink {
public:
    bool open(const std::string& path, std::string& err) {
        f_.open(u8path(path), std::ios::binary | std::ios::trunc);
        if (!f_) err = "cannot create " + path;
        return (bool)f_;
    }
    bool put(const void* p, size_t n) {
        f_.write((const char*)p, (std::streamsize)n);
        return (bool)f_;
    }
    bool close() {
        f_.flush();
        bool ok = (bool)f_;
        f_.close();
        return ok;
    }

private:
    std::ofstream f_;
};

// ---------------------------------------------------------------- xz (liblzma's threaded encoder)
class XzCompressor : public StreamCompressor {
public:
    bool init(int level, int threads, uint64_t sizeHint, const std::string& path, std::string& err) {
        if (!out_.open(path, err)) return false;
        lzma_options_lzma o;
        lzma_lzma_preset(&o, (uint32_t)level);
        unsigned t = threads > 0 ? (unsigned)threads : hardwareThreads();
        lzma_mt mt{};
        mt.preset = (uint32_t)level;
        mt.check = LZMA_CHECK_CRC64;
        // xz's own default is 3 x dictionary per block, which leaves big inputs with a handful of
        // blocks; cap it so every thread gets work.
        mt.block_size = perCore(sizeHint, t, 1 * MiB, 3ull * o.dict_size, 3ull * o.dict_size);
        uint64_t budget = availableMemory() / 4 * 3;
        for (mt.threads = t; mt.threads > 1; --mt.threads)
            if (!budget || lzma_stream_encoder_mt_memusage(&mt) <= budget) break;
        if (lzma_stream_encoder_mt(&s_, &mt) != LZMA_OK) {
            err = "cannot start the xz encoder";
            return false;
        }
        buf_.resize(4 * MiB);
        return true;
    }
    ~XzCompressor() override { lzma_end(&s_); }
    bool write(const void* p, size_t n) override {
        in_ += n;
        bool ok = run(p, n, LZMA_RUN);
        track();
        return ok;
    }
    bool finish() override {
        bool ok = run(nullptr, 0, LZMA_FINISH);
        pend(-held_);
        held_ = 0;
        return out_.close() && ok;
    }

private:
    bool run(const void* p, size_t n, lzma_action action) {
        s_.next_in = (const uint8_t*)p;
        s_.avail_in = n;
        for (;;) {
            s_.next_out = buf_.data();
            s_.avail_out = buf_.size();
            lzma_ret r = lzma_code(&s_, action);
            size_t got = buf_.size() - s_.avail_out;
            if (got && !out_.put(buf_.data(), got)) {
                err_ = "write error (disk full?)";
                return false;
            }
            if (r == LZMA_STREAM_END) return true;
            if (r != LZMA_OK) {
                err_ = "xz encoder error " + std::to_string((int)r);
                return false;
            }
            if (action == LZMA_RUN && s_.avail_in == 0) return true;
        }
    }
    void track() {
        uint64_t pin = 0, pout = 0;
        lzma_get_progress(&s_, &pin, &pout);
        int64_t now = (int64_t)(in_ - std::min(in_, pin));
        pend(now - held_);
        held_ = now;
    }
    lzma_stream s_ = LZMA_STREAM_INIT;
    FileSink out_;
    std::vector<uint8_t> buf_;
    uint64_t in_ = 0;
    int64_t held_ = 0;
};

// ---------------------------------------------------------------- zstd (built-in workers)
class ZstdCompressor : public StreamCompressor {
public:
    bool init(int level, int threads, uint64_t sizeHint, const std::string& path, std::string& err) {
        if (!out_.open(path, err)) return false;
        c_ = ZSTD_createCCtx();
        unsigned t = threads > 0 ? (unsigned)threads : hardwareThreads();
        size_t job = (size_t)perCore(sizeHint, t, 1 * MiB, 256 * MiB, 0);
        // Rough per-worker memory: the window/tables grow with the level (~200 MB at 19+).
        uint64_t perWorker = (level >= 19 ? 200 * MiB : level >= 10 ? 64 * MiB : 16 * MiB) + 2 * std::max<size_t>(job, 8 * MiB);
        t = threadsFor(perWorker, threads);
        ZSTD_CCtx_setParameter(c_, ZSTD_c_compressionLevel, level);
        ZSTD_CCtx_setParameter(c_, ZSTD_c_checksumFlag, 1);
        if (ZSTD_isError(ZSTD_CCtx_setParameter(c_, ZSTD_c_nbWorkers, (int)t))) t = 0;  // single-threaded zstd build
        if (t && job) ZSTD_CCtx_setParameter(c_, ZSTD_c_jobSize, (int)job);
        buf_.resize(ZSTD_CStreamOutSize() * 4);
        return true;
    }
    ~ZstdCompressor() override { ZSTD_freeCCtx(c_); }
    bool write(const void* p, size_t n) override {
        ZSTD_inBuffer in{p, n, 0};
        while (in.pos < in.size)
            if (!step(in, ZSTD_e_continue)) return false;
        ZSTD_frameProgression fp = ZSTD_getFrameProgression(c_);
        int64_t now = (int64_t)(fp.ingested - std::min(fp.ingested, fp.consumed));
        pend(now - held_);
        held_ = now;
        return true;
    }
    bool finish() override {
        ZSTD_inBuffer in{nullptr, 0, 0};
        bool ok = step(in, ZSTD_e_end);
        pend(-held_);
        held_ = 0;
        return out_.close() && ok;
    }

private:
    bool step(ZSTD_inBuffer& in, ZSTD_EndDirective mode) {
        for (;;) {
            ZSTD_outBuffer o{buf_.data(), buf_.size(), 0};
            size_t r = ZSTD_compressStream2(c_, &o, &in, mode);
            if (ZSTD_isError(r)) {
                err_ = std::string("zstd: ") + ZSTD_getErrorName(r);
                return false;
            }
            if (o.pos && !out_.put(buf_.data(), o.pos)) {
                err_ = "write error (disk full?)";
                return false;
            }
            if (mode == ZSTD_e_end ? r == 0 : (in.pos == in.size || o.pos < o.size)) return true;
        }
    }
    ZSTD_CCtx* c_ = nullptr;
    FileSink out_;
    std::vector<uint8_t> buf_;
    int64_t held_ = 0;
};

// ---------------------------------------------------------------- independent blocks, in order
// Cuts the input into blocks, compresses them on a pool and writes the results in order. Used for
// gzip (one gzip member per block, like pigz -i) and bzip2 (one stream per block); every decoder
// reads concatenated members/streams as one file.
class BlockCompressor : public StreamCompressor {
public:
    using Job = std::vector<uint8_t> (*)(const std::vector<uint8_t>& in, int level, std::string& err);
    bool init(Job job, int level, size_t blockSize, unsigned threads, const std::string& path, std::string& err) {
        if (!out_.open(path, err)) return false;
        job_ = job;
        level_ = level;
        block_ = blockSize;
        pool_ = std::make_unique<ThreadPool>(threads);
        maxQueued_ = threads * 2;
        cur_.reserve(block_);
        return true;
    }
    ~BlockCompressor() override {
        for (auto& f : q_)
            if (f.valid()) f.wait();
    }
    bool write(const void* p, size_t n) override {
        const uint8_t* b = (const uint8_t*)p;
        while (n) {
            size_t take = std::min(n, block_ - cur_.size());
            cur_.insert(cur_.end(), b, b + take);
            pend((int64_t)take);
            b += take;
            n -= take;
            if (cur_.size() == block_ && !submit()) return false;
        }
        return err_.empty();
    }
    bool finish() override {
        if ((!cur_.empty() || !wroteAny_) && !submit()) return false;
        while (!q_.empty())
            if (!drain()) return false;
        if (!out_.close()) {
            err_ = "write error (disk full?)";
            return false;
        }
        return true;
    }

private:
    struct Result {
        std::vector<uint8_t> data;
        std::string err;
    };
    bool submit() {
        wroteAny_ = true;
        std::vector<uint8_t> block;
        block.swap(cur_);
        cur_.reserve(block_);
        Job job = job_;
        int level = level_;
        Progress* pr = prog_;
        q_.push_back(pool_->submit([job, level, block = std::move(block), pr]() {
            Result r;
            r.data = job(block, level, r.err);
            if (pr) pr->pending -= (int64_t)block.size();
            return r;
        }));
        while (!q_.empty() && q_.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            if (!drain()) return false;
        while (q_.size() >= maxQueued_)
            if (!drain()) return false;
        return true;
    }
    bool drain() {
        Result r = q_.front().get();
        q_.pop_front();
        if (!r.err.empty()) {
            err_ = r.err;
            return false;
        }
        if (!out_.put(r.data.data(), r.data.size())) {
            err_ = "write error (disk full?)";
            return false;
        }
        return true;
    }
    Job job_ = nullptr;
    int level_ = 6;
    size_t block_ = 8 * MiB;
    size_t maxQueued_ = 2;
    bool wroteAny_ = false;
    std::vector<uint8_t> cur_;
    std::deque<std::future<Result>> q_;
    std::unique_ptr<ThreadPool> pool_;
    FileSink out_;
};

std::vector<uint8_t> gzipMember(const std::vector<uint8_t>& in, int level, std::string& err) {
    z_stream z{};
    std::vector<uint8_t> out;
    if (deflateInit2(&z, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        err = "gzip: cannot start deflate";
        return out;
    }
    out.resize(deflateBound(&z, (uLong)in.size()) + 64);
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = (uInt)in.size();
    z.next_out = out.data();
    z.avail_out = (uInt)out.size();
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) err = "gzip: deflate failed";
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

#ifdef KZ_HAVE_BZIP2
std::vector<uint8_t> bzip2Stream(const std::vector<uint8_t>& in, int level, std::string& err) {
    std::vector<uint8_t> out(in.size() + in.size() / 100 + 1024);
    unsigned int len = (unsigned int)out.size();
    int r = BZ2_bzBuffToBuffCompress((char*)out.data(), &len, (char*)const_cast<uint8_t*>(in.data()),
                                     (unsigned int)in.size(), level, 0, 0);
    if (r != BZ_OK) err = "bzip2: compression failed (" + std::to_string(r) + ")";
    out.resize(len);
    return out;
}
#endif

}  // namespace

std::unique_ptr<StreamCompressor> makeStreamCompressor(OutFormat f, int level, int threads, uint64_t sizeHint,
                                                       const std::string& outPath, std::string& err) {
    int lv = levelFor(f, level);
    switch (f) {
        case OutFormat::TarXz:
        case OutFormat::Xz: {
            auto c = std::make_unique<XzCompressor>();
            if (!c->init(lv, threads, sizeHint, outPath, err)) return nullptr;
            return c;
        }
        case OutFormat::TarZst:
        case OutFormat::Zst: {
            auto c = std::make_unique<ZstdCompressor>();
            if (!c->init(lv, threads, sizeHint, outPath, err)) return nullptr;
            return c;
        }
        case OutFormat::TarGz:
        case OutFormat::Gz: {
            unsigned t = threadsFor(64 * MiB, threads);
            size_t block = (size_t)perCore(sizeHint, t, 1 * MiB, 16 * MiB, 8 * MiB);
            auto c = std::make_unique<BlockCompressor>();
            if (!c->init(gzipMember, lv, block, t, outPath, err)) return nullptr;
            return c;
        }
#ifdef KZ_HAVE_BZIP2
        case OutFormat::TarBz2:
        case OutFormat::Bz2: {
            unsigned t = threadsFor(64 * MiB, threads);
            size_t block = (size_t)lv * 100000 * 8;  // eight bzip2 blocks per stream
            auto c = std::make_unique<BlockCompressor>();
            if (!c->init(bzip2Stream, lv, block, t, outPath, err)) return nullptr;
            return c;
        }
#endif
        default:
            return nullptr;
    }
}

// ============================================================================ 7z writer
namespace {

enum : uint8_t {
    k7End = 0x00, k7Header = 0x01, k7MainStreamsInfo = 0x04, k7FilesInfo = 0x05, k7PackInfo = 0x06,
    k7UnpackInfo = 0x07, k7SubStreamsInfo = 0x08, k7Size = 0x09, k7CRC = 0x0A, k7Folder = 0x0B,
    k7CodersUnpackSize = 0x0C, k7NumUnpackStream = 0x0D, k7EmptyStream = 0x0E, k7EmptyFile = 0x0F,
    k7Name = 0x11, k7MTime = 0x14, k7WinAttributes = 0x15,
};

struct Buf7 {
    std::vector<uint8_t> b;
    void byte(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8 * i)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) b.push_back((uint8_t)(v >> (8 * i)));
    }
    // 7z variable-length number: the leading 1-bits of the first byte count the extra bytes.
    void num(uint64_t v) {
        uint8_t first = 0, mask = 0x80;
        int i = 0;
        for (; i < 8; ++i) {
            if (v < (1ull << (7 * (i + 1)))) {
                first |= (uint8_t)(v >> (8 * i));
                break;
            }
            first |= mask;
            mask >>= 1;
        }
        b.push_back(first);
        for (; i > 0; --i) {
            b.push_back((uint8_t)v);
            v >>= 8;
        }
    }
    void bits(const std::vector<bool>& v) {
        uint8_t cur = 0, mask = 0x80;
        for (bool x : v) {
            if (x) cur |= mask;
            mask >>= 1;
            if (!mask) {
                b.push_back(cur);
                cur = 0;
                mask = 0x80;
            }
        }
        if (mask != 0x80) b.push_back(cur);
    }
};

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (uint8_t)s[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c = extra == 3 ? c & 0x07 : extra == 2 ? c & 0x0F : extra == 1 ? c & 0x1F : c;
        for (int k = 1; k <= extra && i + k < s.size(); ++k) c = (c << 6) | ((uint8_t)s[i + k] & 0x3F);
        i += 1 + extra;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back((char16_t)(0xD800 + (c >> 10)));
            out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back((char16_t)c);
        }
    }
    return out;
}

uint8_t lzma2DictProp(uint32_t dict) {
    for (uint8_t p = 0; p < 40; ++p)
        if (((uint64_t)(2 | (p & 1)) << (p / 2 + 11)) >= dict) return p;
    return 40;
}

// One block of the solid stream as LZMA2 chunks, starting with a dictionary reset and without the
// end marker, so blocks can simply be concatenated.
std::vector<uint8_t> lzma2Block(const std::vector<uint8_t>& in, int level, uint32_t dict, std::string& err) {
    lzma_options_lzma o;
    lzma_lzma_preset(&o, (uint32_t)level);
    o.dict_size = dict;
    lzma_filter f[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
    std::vector<uint8_t> out(in.size() + in.size() / 512 + 4096);
    size_t pos = 0;
    if (lzma_raw_buffer_encode(f, nullptr, in.data(), in.size(), out.data(), &pos, out.size()) != LZMA_OK) {
        err = "7z: LZMA2 encoding failed";
        return {};
    }
    if (pos && out[pos - 1] == 0x00) --pos;  // drop the end marker
    out.resize(pos);
    return out;
}

}  // namespace

struct SevenZipWriter::Impl {
    struct Item {
        std::u16string name;
        uint64_t size = 0;
        uint32_t crc = 0;
        uint32_t attrib = 0;
        uint64_t mtime = 0;  // FILETIME
        bool dir = false;
    };
    struct Result {
        std::vector<uint8_t> data;
        std::string err;
    };

    std::ofstream out;
    std::string path, err;
    bool opened = false, failed = false, inFile = false;
    int level = 9;
    uint32_t dict = 1 << 20;
    size_t blockSize = 8 * MiB;
    std::vector<Item> items;
    std::vector<uint8_t> cur;
    uint64_t packSize = 0, unpackSize = 0;
    std::unique_ptr<ThreadPool> pool;
    std::deque<std::future<Result>> q;
    size_t maxQueued = 2;
    Progress* prog = nullptr;

    static uint64_t fileTime(int64_t unix) { return (uint64_t)(unix + 11644473600ll) * 10000000ull; }

    void fail(const std::string& e) {
        if (!failed) err = e;
        failed = true;
    }
    bool put(const std::vector<uint8_t>& v) {
        out.write((const char*)v.data(), (std::streamsize)v.size());
        if (!out) fail("write error (disk full?)");
        packSize += v.size();
        return !failed;
    }
    bool drain() {
        Result r = q.front().get();
        q.pop_front();
        if (!r.err.empty()) fail(r.err);
        return !failed && put(r.data);
    }
    void flushBlock() {
        if (cur.empty() || failed) return;
        std::vector<uint8_t> block;
        block.swap(cur);
        cur.reserve(blockSize);
        if (level == 0) {  // Copy coder: stored as is
            if (prog) prog->pending -= (int64_t)block.size();
            put(block);
            return;
        }
        int lv = level;
        uint32_t dc = dict;
        Progress* pr = prog;
        q.push_back(pool->submit([block = std::move(block), lv, dc, pr]() {
            Result r;
            r.data = lzma2Block(block, lv, dc, r.err);
            if (pr) pr->pending -= (int64_t)block.size();
            return r;
        }));
        while (!q.empty() && q.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            if (!drain()) return;
        while (q.size() >= maxQueued)
            if (!drain()) return;
    }
    void endFile() { inFile = false; }

    std::vector<uint8_t> header() {
        Buf7 h;
        h.byte(k7Header);
        std::vector<const Item*> streams;
        for (auto& it : items)
            if (!it.dir && it.size) streams.push_back(&it);
        if (!streams.empty()) {
            h.byte(k7MainStreamsInfo);
            h.byte(k7PackInfo);
            h.num(0);  // pack position
            h.num(1);  // one pack stream
            h.byte(k7Size);
            h.num(packSize);
            h.byte(k7End);
            h.byte(k7UnpackInfo);
            h.byte(k7Folder);
            h.num(1);   // one folder
            h.byte(0);  // not external
            h.num(1);   // one coder
            if (level == 0) {
                h.byte(0x01);  // id size 1, simple, no properties
                h.byte(0x00);  // Copy
            } else {
                h.byte(0x21);  // id size 1, simple, has properties
                h.byte(0x21);  // LZMA2
                h.num(1);
                h.byte(lzma2DictProp(dict));
            }
            h.byte(k7CodersUnpackSize);
            h.num(unpackSize);
            h.byte(k7End);
            h.byte(k7SubStreamsInfo);
            h.byte(k7NumUnpackStream);
            h.num(streams.size());
            if (streams.size() > 1) {
                h.byte(k7Size);
                for (size_t i = 0; i + 1 < streams.size(); ++i) h.num(streams[i]->size);
            }
            h.byte(k7CRC);
            h.byte(1);  // all defined
            for (auto* s : streams) h.u32(s->crc);
            h.byte(k7End);
            h.byte(k7End);
        }
        h.byte(k7FilesInfo);
        h.num(items.size());
        std::vector<bool> emptyStream, emptyFile;
        bool anyEmpty = false, anyEmptyFile = false;
        for (auto& it : items) {
            bool e = it.dir || it.size == 0;
            emptyStream.push_back(e);
            anyEmpty |= e;
            if (e) {
                emptyFile.push_back(!it.dir);
                anyEmptyFile |= !it.dir;
            }
        }
        if (anyEmpty) {
            Buf7 v;
            v.bits(emptyStream);
            h.byte(k7EmptyStream);
            h.num(v.b.size());
            h.b.insert(h.b.end(), v.b.begin(), v.b.end());
            if (anyEmptyFile) {
                Buf7 w;
                w.bits(emptyFile);
                h.byte(k7EmptyFile);
                h.num(w.b.size());
                h.b.insert(h.b.end(), w.b.begin(), w.b.end());
            }
        }
        {
            Buf7 n;
            n.byte(0);  // not external
            for (auto& it : items) {
                for (char16_t c : it.name) {
                    n.byte((uint8_t)c);
                    n.byte((uint8_t)(c >> 8));
                }
                n.byte(0);
                n.byte(0);
            }
            h.byte(k7Name);
            h.num(n.b.size());
            h.b.insert(h.b.end(), n.b.begin(), n.b.end());
        }
        h.byte(k7MTime);
        h.num(2 + 8 * items.size());
        h.byte(1);  // all defined
        h.byte(0);  // not external
        for (auto& it : items) h.u64(it.mtime);
        h.byte(k7WinAttributes);
        h.num(2 + 4 * items.size());
        h.byte(1);
        h.byte(0);
        for (auto& it : items) h.u32(it.attrib);
        h.byte(k7End);
        h.byte(k7End);
        return h.b;
    }
};

SevenZipWriter::SevenZipWriter() : d(new Impl) {}
SevenZipWriter::~SevenZipWriter() {
    if (d->opened) abort();
}
const std::string& SevenZipWriter::error() const { return d->err; }

bool SevenZipWriter::open(const std::string& outPath, int level, int threads, uint64_t sizeHint, std::string& err) {
    d->path = outPath;
    d->level = std::clamp(level < 0 ? 9 : level, 0, 9);
    d->out.open(u8path(outPath), std::ios::binary | std::ios::trunc);
    if (!d->out) {
        err = "cannot create " + outPath;
        return false;
    }
    std::vector<uint8_t> zero(32, 0);
    d->out.write((const char*)zero.data(), 32);  // signature header, filled in by finish()
    d->opened = true;
    if (d->level == 0) {
        d->blockSize = 4 * MiB;
        return true;
    }
    lzma_options_lzma o;
    lzma_lzma_preset(&o, (uint32_t)d->level);
    unsigned hw = threads > 0 ? (unsigned)threads : hardwareThreads();
    // Blocks of up to 4x the preset dictionary (7-Zip's choice), smaller when that leaves cores idle.
    d->blockSize = (size_t)perCore(sizeHint, hw, 2 * MiB, 4ull * o.dict_size, 4ull * o.dict_size);
    uint64_t dict = 1 << 16;
    while (dict < d->blockSize && dict < o.dict_size) dict <<= 1;
    d->dict = (uint32_t)std::min<uint64_t>(dict, o.dict_size);
    unsigned t = threadsFor((uint64_t)d->dict * 12 + d->blockSize * 3, threads);
    d->pool = std::make_unique<ThreadPool>(t);
    d->maxQueued = t + std::max(2u, t / 2);
    d->cur.reserve(d->blockSize);
    return true;
}

void SevenZipWriter::addDir(const std::string& path, uint32_t mode, int64_t mtime) {
    d->endFile();
    Impl::Item it;
    it.name = utf16(path);
    it.dir = true;
    it.mtime = Impl::fileTime(mtime);
    it.attrib = 0x10 | 0x8000 | ((0040000u | (mode ? (mode & 07777) : 0755)) << 16);
    d->items.push_back(std::move(it));
}

bool SevenZipWriter::beginFile(const std::string& path, uint32_t mode, int64_t mtime) {
    d->endFile();
    Impl::Item it;
    it.name = utf16(path);
    it.mtime = Impl::fileTime(mtime);
    it.attrib = 0x20 | 0x8000 | ((0100000u | (mode ? (mode & 07777) : 0644)) << 16);
    d->items.push_back(std::move(it));
    d->inFile = true;
    return !d->failed;
}

bool SevenZipWriter::addSymlink(const std::string& path, const std::string& target, int64_t mtime) {
    // 7z stores a link as a file whose content is the target, marked by the Unix mode bits.
    if (!beginFile(path, 0777, mtime)) return false;
    d->items.back().attrib = 0x20 | 0x8000 | ((0120000u | 0777u) << 16);
    bool ok = write(target.data(), target.size());
    d->endFile();
    return ok;
}

void SevenZipWriter::setProgress(Progress* p) { d->prog = p; }

bool SevenZipWriter::write(const void* data, size_t n) {
    if (d->failed || !d->inFile) return false;
    Impl::Item& it = d->items.back();
    it.crc = lzma_crc32((const uint8_t*)data, n, it.crc);
    it.size += n;
    d->unpackSize += n;
    if (d->prog) d->prog->pending += (int64_t)n;
    const uint8_t* p = (const uint8_t*)data;
    while (n) {
        size_t take = std::min(n, d->blockSize - d->cur.size());
        d->cur.insert(d->cur.end(), p, p + take);
        p += take;
        n -= take;
        if (d->cur.size() >= d->blockSize) d->flushBlock();
    }
    return !d->failed;
}

bool SevenZipWriter::finish(std::string& err) {
    d->endFile();
    d->flushBlock();
    while (!d->q.empty() && !d->failed) d->drain();
    if (!d->failed && d->level > 0 && d->unpackSize) d->put({0x00});  // LZMA2 end marker
    if (!d->failed) {
        std::vector<uint8_t> hdr = d->header();
        uint64_t nextOff = d->packSize;  // header follows the packed stream
        d->out.write((const char*)hdr.data(), (std::streamsize)hdr.size());
        Buf7 start;
        start.u64(nextOff);
        start.u64(hdr.size());
        start.u32(lzma_crc32(hdr.data(), hdr.size(), 0));
        Buf7 sig;
        const uint8_t magic[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};
        sig.b.assign(magic, magic + 6);
        sig.byte(0);  // version 0.4
        sig.byte(4);
        sig.u32(lzma_crc32(start.b.data(), start.b.size(), 0));
        sig.b.insert(sig.b.end(), start.b.begin(), start.b.end());
        d->out.seekp(0);
        d->out.write((const char*)sig.b.data(), (std::streamsize)sig.b.size());
        d->out.flush();
        if (!d->out) d->fail("write error (disk full?)");
    }
    d->out.close();
    d->opened = false;
    if (d->failed) {
        std::error_code ec;
        fs::remove(u8path(d->path), ec);
        err = d->err;
        return false;
    }
    return true;
}

void SevenZipWriter::abort() {
    for (auto& f : d->q)
        if (f.valid()) f.wait();
    d->q.clear();
    if (d->out.is_open()) d->out.close();
    std::error_code ec;
    if (d->opened) fs::remove(u8path(d->path), ec);
    d->opened = false;
}

}  // namespace kz
