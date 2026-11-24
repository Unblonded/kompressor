#include "kmp.hpp"

#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#define XXH_INLINE_ALL
#include "kz_cm.hpp"
#include "kz_pool.hpp"
#include "xxhash.h"

namespace kz::kmp {

namespace {

constexpr uint8_t kMagic[8] = {'K', 'M', 'P', 'R', 0x1A, '\r', '\n', 0};
constexpr uint8_t kFootMagic[8] = {'K', 'M', 'P', 'F', 'O', 'O', 'T', 0};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 16;
constexpr size_t kFooterSize = 40;
constexpr uint64_t MiB = 1024ull * 1024ull;

enum Codec : uint8_t { CodecStore = 0, CodecZstd = 1, CodecLzma2 = 2, CodecCm = 3 };
enum Filter : uint8_t { FilterNone = 0, FilterX86 = 1, FilterArm64 = 2 };
enum Category : uint8_t { CatGeneric = 0, CatText, CatX86, CatArm64, CatMedia, CatCount };

struct BlockRec {
    uint64_t offset = 0, packed = 0, raw = 0;
    uint8_t codec = 0, filter = 0;
    uint32_t param = 0;  // LZMA2 dictionary size; KCM profile | level << 8
    uint64_t hash = 0;   // XXH3-64 of the raw block
};

struct ChunkRec {
    uint32_t block = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// ---------------------------------------------------------------- serialization
struct OutBuf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16le(uint16_t v) { for (int i = 0; i < 2; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u32le(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u64le(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void var(uint64_t v) {
        while (v >= 0x80) {
            b.push_back(uint8_t(v | 0x80));
            v >>= 7;
        }
        b.push_back(uint8_t(v));
    }
    void svar(int64_t v) { var((uint64_t(v) << 1) ^ uint64_t(v >> 63)); }
    void str(const std::string& s) {
        var(s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    void raw(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
};

struct InBuf {
    const uint8_t* p;
    const uint8_t* e;
    void need(size_t n) {
        if (size_t(e - p) < n) throw std::runtime_error("index is truncated or corrupt");
    }
    uint8_t u8() { need(1); return *p++; }
    uint64_t u64le() {
        need(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
        p += 8;
        return v;
    }
    uint64_t var() {
        uint64_t v = 0;
        for (int s = 0; s < 64; s += 7) {
            uint8_t c = u8();
            v |= uint64_t(c & 0x7F) << s;
            if (!(c & 0x80)) return v;
        }
        throw std::runtime_error("bad varint in index");
    }
    int64_t svar() {
        uint64_t v = var();
        return int64_t(v >> 1) ^ -int64_t(v & 1);
    }
    std::string str() {
        uint64_t n = var();
        need(n);
        std::string s((const char*)p, (size_t)n);
        p += n;
        return s;
    }
};

uint64_t rdLE(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

// ---------------------------------------------------------------- content-defined chunking
struct GearTable {
    uint64_t t[256];
    GearTable() {
        uint64_t s = 0x4B4D5052u;  // "KMPR"
        for (auto& v : t) {        // splitmix64
            s += 0x9E3779B97F4A7C15ull;
            uint64_t z = s;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            v = z ^ (z >> 31);
        }
    }
};
const GearTable& gear() {
    static GearTable g;
    return g;
}

// FastCDC with normalised chunking: min 16 KiB, avg ~64 KiB, max 256 KiB.
class Chunker {
public:
    static constexpr size_t kMin = 16 * 1024;
    static constexpr size_t kAvg = 64 * 1024;
    static constexpr size_t kMax = 256 * 1024;
    // gear hashes shift left, so the top bits depend on the most bytes: mask the high bits.
    static constexpr uint64_t kMaskS = ~0ull << (64 - 18);  // harder to cut before avg
    static constexpr uint64_t kMaskL = ~0ull << (64 - 14);  // easier to cut after avg

    template <class F>
    void feed(const uint8_t* p, size_t n, F&& emit) {
        const uint64_t* g = gear().t;
        while (n) {
            size_t have = buf_.size();
            if (have < kMin) {
                size_t take = std::min(n, kMin - have);
                buf_.insert(buf_.end(), p, p + take);
                p += take;
                n -= take;
                have += take;
                if (have < kMin) break;
                if (!n) break;
            }
            size_t limit = std::min(n, kMax - have);
            size_t i = 0;
            bool cut = false;
            uint64_t h = h_;
            for (; i < limit; ++i) {
                h = (h << 1) + g[p[i]];
                size_t sz = have + i + 1;
                if (!(h & (sz < kAvg ? kMaskS : kMaskL))) {
                    ++i;
                    cut = true;
                    break;
                }
            }
            h_ = h;
            buf_.insert(buf_.end(), p, p + i);
            p += i;
            n -= i;
            if (cut || buf_.size() >= kMax) {
                emit(buf_.data(), buf_.size());
                buf_.clear();
                h_ = 0;
            }
        }
    }
    template <class F>
    void finish(F&& emit) {
        if (!buf_.empty()) emit(buf_.data(), buf_.size());
        buf_.clear();
        h_ = 0;
    }
    void reset() {
        buf_.clear();
        h_ = 0;
    }

private:
    std::vector<uint8_t> buf_;
    uint64_t h_ = 0;
};

// ---------------------------------------------------------------- classification
bool extIn(const std::string& e, std::initializer_list<const char*> list) {
    for (const char* x : list)
        if (e == x) return true;
    return false;
}

Category classify(const std::string& path, const uint8_t* p, size_t n) {
    std::string e = lowerExt(path);
    if (extIn(e, {"jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "avif", "jxl", "mp3", "aac", "ogg", "opus",
                  "flac", "m4a", "mp4", "m4v", "mkv", "webm", "mov", "avi", "wmv", "zip", "gz", "tgz", "bz2", "xz",
                  "7z", "rar", "zst", "lz4", "br", "jar", "apk", "ipa", "docx", "xlsx", "pptx", "odt", "ods", "epub",
                  "kmp", "dmg", "woff", "woff2", "cab", "msi", "pkg", "deb", "rpm", "lzma", "zipx", "cbz", "cbr"}))
        return CatMedia;

    if (n >= 64) {
        if (p[0] == 'M' && p[1] == 'Z') {
            uint32_t pe = (uint32_t)rdLE(p + 0x3C, 4);
            if ((uint64_t)pe + 6 <= n && std::memcmp(p + pe, "PE\0\0", 4) == 0) {
                uint32_t m = (uint32_t)rdLE(p + pe + 4, 2);
                if (m == 0xAA64) return CatArm64;
                if (m == 0x14C || m == 0x8664) return CatX86;
            }
            return CatX86;
        }
        if (p[0] == 0x7F && p[1] == 'E' && p[2] == 'L' && p[3] == 'F' && p[5] == 1) {
            uint32_t m = (uint32_t)rdLE(p + 18, 2);
            if (m == 183) return CatArm64;
            if (m == 3 || m == 62) return CatX86;
            return CatGeneric;
        }
        uint32_t magic = (uint32_t)rdLE(p, 4);
        if (magic == 0xFEEDFACF || magic == 0xFEEDFACE) {
            uint32_t cpu = (uint32_t)rdLE(p + 4, 4);
            if (cpu == 0x0100000C) return CatArm64;
            if (cpu == 0x01000007 || cpu == 7) return CatX86;
            return CatGeneric;
        }
    }
    // Text heuristic: no NULs and very few control characters in the first 8 KiB.
    size_t m = std::min<size_t>(n, 8192), ctrl = 0;
    if (m == 0) return CatGeneric;
    for (size_t i = 0; i < m; ++i) {
        uint8_t c = p[i];
        if (c == 0) return CatGeneric;
        if (c < 9 || (c > 13 && c < 32 && c != 27)) ++ctrl;
    }
    return (ctrl * 100 <= m) ? CatText : CatGeneric;
}

// ---------------------------------------------------------------- codecs
int ceilLog2(uint64_t v) {
    int r = 0;
    while ((1ull << r) < v && r < 62) ++r;
    return r;
}

bool zstdCompress(const uint8_t* in, size_t n, int level, bool longRange, std::vector<uint8_t>& out) {
    ZSTD_CCtx* c = ZSTD_createCCtx();
    if (!c) return false;
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 0);
    if (longRange) {
        ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, std::clamp(ceilLog2(n), 10, 27));
    }
    out.resize(ZSTD_compressBound(n));
    size_t r = ZSTD_compress2(c, out.data(), out.size(), in, n);
    ZSTD_freeCCtx(c);
    if (ZSTD_isError(r)) return false;
    out.resize(r);
    return true;
}

bool lzmaCompress(const uint8_t* in, size_t n, uint8_t filter, bool text, uint32_t& dict, std::vector<uint8_t>& out) {
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 9 | LZMA_PRESET_EXTREME)) return false;
    uint64_t d = 1ull << std::clamp(ceilLog2(n), 16, 30);
    opt.dict_size = (uint32_t)std::min<uint64_t>(d, 1024ull * MiB);
    if (text) {  // literal-heavy data: more literal context, no position bits
        opt.lc = 4;
        opt.lp = 0;
        opt.pb = 0;
    }
    lzma_filter f[3];
    int k = 0;
    if (filter == FilterX86) f[k++] = {LZMA_FILTER_X86, nullptr};
#ifdef LZMA_FILTER_ARM64
    if (filter == FilterArm64) f[k++] = {LZMA_FILTER_ARM64, nullptr};
#else
    if (filter == FilterArm64) return false;
#endif
    f[k++] = {LZMA_FILTER_LZMA2, &opt};
    f[k] = {LZMA_VLI_UNKNOWN, nullptr};
    out.resize(n + n / 4 + 64 * 1024);
    size_t pos = 0;
    lzma_ret r = lzma_raw_buffer_encode(f, nullptr, in, n, out.data(), &pos, out.size());
    if (r != LZMA_OK) return false;
    out.resize(pos);
    dict = opt.dict_size;
    return true;
}

bool decompressBlock(const BlockRec& b, const uint8_t* in, std::vector<uint8_t>& out, std::string& err,
                     const cm::Tick& tick = {}) {
    out.resize((size_t)b.raw);
    switch (b.codec) {
        case CodecStore:
            if (b.packed != b.raw) { err = "stored block size mismatch"; return false; }
            std::memcpy(out.data(), in, (size_t)b.raw);
            return true;
        case CodecZstd: {
            ZSTD_DCtx* d = ZSTD_createDCtx();
            ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, 31);
            size_t r = ZSTD_decompressDCtx(d, out.data(), out.size(), in, (size_t)b.packed);
            ZSTD_freeDCtx(d);
            if (ZSTD_isError(r) || r != b.raw) {
                err = std::string("zstd: ") + (ZSTD_isError(r) ? ZSTD_getErrorName(r) : "size mismatch");
                return false;
            }
            return true;
        }
        case CodecLzma2: {
            lzma_options_lzma opt;
            lzma_lzma_preset(&opt, 6);
            opt.dict_size = b.param ? b.param : (1u << 26);
            lzma_filter f[3];
            int k = 0;
            if (b.filter == FilterX86) f[k++] = {LZMA_FILTER_X86, nullptr};
#ifdef LZMA_FILTER_ARM64
            if (b.filter == FilterArm64) f[k++] = {LZMA_FILTER_ARM64, nullptr};
#else
            if (b.filter == FilterArm64) { err = "this build's liblzma lacks the ARM64 filter"; return false; }
#endif
            f[k++] = {LZMA_FILTER_LZMA2, &opt};
            f[k] = {LZMA_VLI_UNKNOWN, nullptr};
            size_t ip = 0, op = 0;
            lzma_ret r = lzma_raw_buffer_decode(f, nullptr, in, &ip, (size_t)b.packed, out.data(), &op, out.size());
            if ((r != LZMA_OK && r != LZMA_STREAM_END) || op != b.raw) {
                err = "lzma2: corrupt block (code " + std::to_string((int)r) + ")";
                return false;
            }
            return true;
        }
        case CodecCm:
            if (!cm::decompress(in, (size_t)b.packed, out.data(), (size_t)b.raw, (cm::Profile)(b.param & 255),
                                (int)(b.param >> 8), tick)) {
                err = "kcm: corrupt block";
                return false;
            }
            return true;
        default:
            err = "unknown codec " + std::to_string(b.codec) + " (made by a newer Kompressor?)";
            return false;
    }
}

struct PendingBlock {
    uint32_t id = 0;
    uint8_t cat = 0;
    std::vector<uint8_t> raw;
};

struct DoneBlock {
    uint32_t id = 0;
    BlockRec rec;
    std::vector<uint8_t> data;
};

DoneBlock compressJob(PendingBlock pb, Mode mode, Progress* prog) {
    DoneBlock d;
    d.id = pb.id;
    const uint8_t* in = pb.raw.data();
    size_t n = pb.raw.size();
    d.rec.raw = n;
    d.rec.hash = XXH3_64bits(in, n);
    // These bytes were counted as read; hand them over to "done" as the work actually happens.
    size_t reported = 0;
    auto tick = [&](size_t k) {
        k = std::min(k, n - reported);
        reported += k;
        if (prog) prog->pending -= (int64_t)k;
    };
    struct Settle {
        std::function<void()> f;
        ~Settle() { f(); }
    } settle{[&] { tick(n); }};

    bool have = false;
    auto consider = [&](std::vector<uint8_t>&& data, uint8_t codec, uint8_t filter, uint32_t param) {
        if (!have || data.size() < d.data.size()) {
            d.data = std::move(data);
            d.rec.codec = codec;
            d.rec.filter = filter;
            d.rec.param = param;
            have = true;
        }
    };
    auto tryZstd = [&](int level, bool ldm) {
        std::vector<uint8_t> o;
        if (zstdCompress(in, n, level, ldm, o)) consider(std::move(o), CodecZstd, FilterNone, 0);
    };
    auto tryLzma = [&](uint8_t filter, bool text) {
        std::vector<uint8_t> o;
        uint32_t dict = 0;
        if (lzmaCompress(in, n, filter, text, dict, o)) consider(std::move(o), CodecLzma2, filter, dict);
    };

    uint8_t exeFilter = pb.cat == CatX86 ? FilterX86 : (pb.cat == CatArm64 ? FilterArm64 : FilterNone);
    bool text = pb.cat == CatText;

    if (pb.cat == CatMedia) {
        // Already-compressed data: a quick pass catches the odd compressible file, otherwise store.
        tryZstd(mode == Mode::Fast ? 1 : 3, false);
        if (mode == Mode::Max || mode == Mode::Extreme || mode == Mode::Ultimate) tryLzma(FilterNone, false);
    } else if (mode == Mode::Extreme || mode == Mode::Ultimate) {
        // Context mixing. A cheap zstd probe first: data it cannot shrink at all (random, encrypted,
        // unrecognised compressed formats) is not worth minutes of modelling.
        std::vector<uint8_t> probe;
        size_t sample = std::min<size_t>(n, 4 * MiB);
        bool hopeless = zstdCompress(in, sample, 1, false, probe) && probe.size() > sample - sample / 64;
        if (hopeless) {
            tryZstd(3, false);
        } else {
            cm::Profile prof = text ? cm::Text : (pb.cat == CatX86 ? cm::X86 : cm::Generic);
            std::vector<uint8_t> o;
            int level = mode == Mode::Ultimate ? 1 : 0;
            if (cm::compress(in, n, prof, o, level, tick)) consider(std::move(o), CodecCm, FilterNone, (uint32_t)prof | ((uint32_t)level << 8));
            if (!have) tryLzma(exeFilter, text);
        }
    } else {
        switch (mode) {
            case Mode::Fast: tryZstd(3, false); break;
            case Mode::Balanced: tryZstd(19, true); break;
            case Mode::Ultra: tryLzma(exeFilter, text); break;
            case Mode::Max:
                tryLzma(exeFilter, text);
                if (exeFilter != FilterNone) tryLzma(FilterNone, false);
                if (text) tryLzma(FilterNone, false);
                tryZstd(22, true);
                break;
        }
    }
    // Never expand: fall back to storing (also covers codec failures).
    if (!have || d.data.size() >= n) {
        d.data = std::move(pb.raw);
        d.rec.codec = CodecStore;
        d.rec.filter = FilterNone;
        d.rec.param = 0;
    }
    d.rec.packed = d.data.size();
    return d;
}

struct H128 {
    uint64_t lo, hi;
    bool operator==(const H128& o) const { return lo == o.lo && hi == o.hi; }
};
struct H128Hash {
    size_t operator()(const H128& h) const { return (size_t)(h.lo ^ (h.hi * 0x9E3779B97F4A7C15ull)); }
};

}  // namespace

const char* modeName(Mode m) {
    switch (m) {
        case Mode::Fast: return "Fast";
        case Mode::Balanced: return "Balanced";
        case Mode::Ultra: return "Ultra";
        case Mode::Max: return "Max";
        case Mode::Extreme: return "Extreme";
        case Mode::Ultimate: return "Ultimate";
    }
    return "?";
}

std::string Stats::summary() const {
    char buf[512];
    double ratio = rawBytes ? 100.0 * double(packedBytes) / double(rawBytes) : 0.0;
    std::snprintf(buf, sizeof buf, "%llu files, %s -> %s (%.1f%%), dedup saved %s, %llu blocks",
                  (unsigned long long)files, humanSize(rawBytes).c_str(), humanSize(packedBytes).c_str(), ratio,
                  humanSize(rawBytes > uniqueBytes ? rawBytes - uniqueBytes : 0).c_str(), (unsigned long long)blocks);
    return buf;
}

// ============================================================================ Writer
struct Writer::Impl {
    Options opt;
    Progress* prog = nullptr;
    std::ofstream out;
    fs::path outPath;
    uint64_t pos = 0;
    bool opened = false, failed = false, finished = false;
    std::string err;
    size_t blockSize = 64 * MiB;
    unsigned threads = 1;
    size_t maxQueued = 2;  // blocks compressing or waiting to be written
    std::unique_ptr<ThreadPool> pool;

    std::vector<BlockRec> blocks;
    std::vector<ChunkRec> chunks;
    std::vector<Entry> entries;
    std::unordered_map<H128, uint32_t, H128Hash> seen;
    std::unique_ptr<PendingBlock> open[CatCount];
    std::deque<std::future<DoneBlock>> inflight;
    Stats st;

    bool inFile = false;
    Entry cur;
    Category curCat = CatGeneric;
    bool catKnown = false;
    XXH3_state_t* fh = nullptr;
    Chunker chunker;

    Impl() { fh = XXH3_createState(); }
    ~Impl() {
        for (auto& f : inflight)
            if (f.valid()) f.wait();
        if (fh) XXH3_freeState(fh);
    }

    void fail(const std::string& e) {
        if (!failed) err = e;
        failed = true;
    }

    void emitChunk(const uint8_t* p, size_t n) {
        if (!catKnown) {
            curCat = classify(cur.path, p, n);
            catKnown = true;
        }
        st.chunks++;
        H128 key{0, 0};
        if (opt.dedup) {
            XXH128_hash_t h = XXH3_128bits(p, n);
            key = {h.low64, h.high64};
            auto it = seen.find(key);
            if (it != seen.end() && chunks[it->second].size == n) {
                cur.chunks.push_back(it->second);
                st.dupChunks++;
                return;
            }
        }
        auto& blk = open[curCat];
        if (!blk) {
            blk = std::make_unique<PendingBlock>();
            blk->id = (uint32_t)blocks.size();
            blk->cat = curCat;
            blocks.emplace_back();
        }
        ChunkRec c;
        c.block = blk->id;
        c.offset = (uint32_t)blk->raw.size();
        c.size = (uint32_t)n;
        blk->raw.insert(blk->raw.end(), p, p + n);
        uint32_t id = (uint32_t)chunks.size();
        chunks.push_back(c);
        if (opt.dedup) seen.emplace(key, id);
        cur.chunks.push_back(id);
        st.uniqueBytes += n;
        if (prog) prog->pending += (int64_t)n;
        if (blk->raw.size() >= blockSize) flushBlock(curCat);
    }

    void flushBlock(int cat) {
        if (!open[cat] || open[cat]->raw.empty()) return;
        PendingBlock pb = std::move(*open[cat]);
        open[cat].reset();
        Mode m = opt.mode;
        Progress* pr = prog;
        inflight.push_back(pool->submit([pb = std::move(pb), m, pr]() mutable { return compressJob(std::move(pb), m, pr); }));
        // Write out whatever is already finished, and keep the queue (and its memory) bounded.
        while (!inflight.empty() && inflight.front().wait_for(std::chrono::seconds(0)) == std::future_status::ready) drainOne();
        while (inflight.size() >= maxQueued) drainOne();
    }

    void drainOne() {
        if (inflight.empty()) return;
        if (prog) prog->setStatus("Compressing blocks...");
        DoneBlock d;
        try {
            d = inflight.front().get();
        } catch (const std::exception& e) {
            inflight.pop_front();
            fail(std::string("block compression failed: ") + e.what());
            return;
        }
        inflight.pop_front();
        BlockRec& r = blocks[d.id];
        r = d.rec;
        r.offset = pos;
        out.write((const char*)d.data.data(), (std::streamsize)d.data.size());
        if (!out) fail("write error (disk full?)");
        pos += d.data.size();
        st.packedBytes += d.data.size();
        st.blocks++;
    }
};

Writer::Writer() : d(new Impl) {}
Writer::~Writer() {
    if (d->opened && !d->finished) abort();
}
void Writer::setProgress(Progress* p) { d->prog = p; }
const Stats& Writer::stats() const { return d->st; }
const std::string& Writer::error() const { return d->err; }

bool Writer::open(const fs::path& outPath, const Options& opt, std::string& err) {
    d->opt = opt;
    d->outPath = outPath;
    size_t mb = opt.blockSizeMB, minMb = 4;
    if (!mb) {
        switch (opt.mode) {
            case Mode::Fast: mb = 16; minMb = 4; break;
            case Mode::Balanced: mb = 64; minMb = 8; break;
            case Mode::Ultra: mb = 64; minMb = 16; break;
            case Mode::Max: mb = 128; minMb = 32; break;
            case Mode::Extreme: mb = 64; minMb = 2; break;  // context mixing parallelises by block only
            case Mode::Ultimate: mb = 256; minMb = 256; break;  // long history beats parallelism here
        }
        // Not enough data to give every core a full-size block: use smaller blocks (a small ratio
        // cost) so all of them work instead of a few.
        if (opt.sizeHint) {
            uint64_t perCore = (opt.sizeHint + hardwareThreads() - 1) / hardwareThreads();
            size_t want = (size_t)((perCore + MiB - 1) / MiB);
            mb = std::clamp<size_t>(want, minMb, mb);
        }
    }
    mb = std::clamp<size_t>(mb, 1, 1024);
    d->blockSize = mb * MiB;

    // Memory per worker: LZMA2 (BT4 match finder) needs ~11x its dictionary while encoding, plus the
    // block itself and its output. The dictionary is the block size rounded up to a power of two.
    uint64_t dict = 1;
    while (dict < d->blockSize) dict <<= 1;
    uint64_t perThread = d->blockSize * 3;
    if (opt.mode == Mode::Balanced) perThread = d->blockSize * 6;
    if (opt.mode == Mode::Ultra) perThread = dict * 12 + d->blockSize * 2;
    if (opt.mode == Mode::Max) perThread = dict * 14 + d->blockSize * 3;
    if (opt.mode == Mode::Extreme) perThread = cm::memoryFor(d->blockSize) + d->blockSize * 3;
    if (opt.mode == Mode::Ultimate) perThread = cm::memoryFor(d->blockSize, 1) + d->blockSize * 3;
    d->threads = threadsFor(perThread, opt.threads);
    d->pool = std::make_unique<ThreadPool>(d->threads);
    // A few extra finished blocks may wait for an earlier one to be written; workers never idle.
    d->maxQueued = d->threads + std::max(2u, d->threads / 2);

    d->out.open(outPath, std::ios::binary | std::ios::trunc);
    if (!d->out) {
        err = "cannot create " + pathToUtf8(outPath);
        return false;
    }
    OutBuf h;
    h.raw(kMagic, 8);
    h.u16le(kVersion);
    h.u16le(0);
    h.u32le(0);
    d->out.write((const char*)h.b.data(), (std::streamsize)h.b.size());
    d->pos = kHeaderSize;
    d->opened = true;
    return true;
}

void Writer::addDir(const std::string& path, uint32_t mode, int64_t mtime) {
    Entry e;
    e.path = path;
    e.type = EntryType::Dir;
    e.mode = mode;
    e.mtime = mtime;
    d->entries.push_back(std::move(e));
}

void Writer::addSymlink(const std::string& path, const std::string& target, uint32_t mode, int64_t mtime) {
    Entry e;
    e.path = path;
    e.type = EntryType::Symlink;
    e.mode = mode;
    e.mtime = mtime;
    e.link = target;
    d->entries.push_back(std::move(e));
}

void Writer::beginFile(const std::string& path, uint32_t mode, int64_t mtime) {
    if (d->inFile) endFile();
    d->cur = Entry();
    d->cur.path = path;
    d->cur.type = EntryType::File;
    d->cur.mode = mode;
    d->cur.mtime = mtime;
    d->catKnown = false;
    d->chunker.reset();
    XXH3_64bits_reset(d->fh);
    d->inFile = true;
}

bool Writer::write(const void* data, size_t n) {
    if (d->failed || !d->inFile) return false;
    const uint8_t* p = (const uint8_t*)data;
    XXH3_64bits_update(d->fh, p, n);
    d->cur.size += n;
    d->st.rawBytes += n;
    d->chunker.feed(p, n, [this](const uint8_t* c, size_t len) { d->emitChunk(c, len); });
    return !d->failed;
}

bool Writer::endFile() {
    if (!d->inFile) return !d->failed;
    d->chunker.finish([this](const uint8_t* c, size_t len) { d->emitChunk(c, len); });
    d->cur.hash = XXH3_64bits_digest(d->fh);
    d->entries.push_back(std::move(d->cur));
    d->cur = Entry();
    d->inFile = false;
    d->st.files++;
    return !d->failed;
}

bool Writer::finish(std::string& err) {
    if (!d->opened) {
        err = "archive not open";
        return false;
    }
    endFile();
    for (int c = 0; c < CatCount; ++c) d->flushBlock(c);
    while (!d->inflight.empty()) d->drainOne();
    if (d->prog) d->prog->setStatus("Writing index...");

    // Drop block slots that were opened but never received data (cannot happen, but be safe).
    OutBuf ix;
    ix.var((uint64_t)d->opt.mode);
    ix.var(d->blocks.size());
    for (auto& b : d->blocks) {
        ix.var(b.offset);
        ix.var(b.packed);
        ix.var(b.raw);
        ix.u8(b.codec);
        ix.u8(b.filter);
        ix.var(b.param);
        ix.u64le(b.hash);
    }
    ix.var(d->chunks.size());
    for (auto& c : d->chunks) {
        ix.var(c.block);
        ix.var(c.offset);
        ix.var(c.size);
    }
    ix.var(d->entries.size());
    for (auto& e : d->entries) {
        ix.str(e.path);
        ix.u8((uint8_t)e.type);
        ix.var(e.mode);
        ix.svar(e.mtime);
        ix.var(e.size);
        ix.u64le(e.hash);
        if (e.type == EntryType::Symlink) ix.str(e.link);
        ix.var(e.chunks.size());
        int64_t prev = -1;
        for (uint32_t c : e.chunks) {
            ix.svar((int64_t)c - prev);
            prev = c;
        }
    }
    ix.var(d->st.rawBytes);
    ix.var(d->st.uniqueBytes);
    ix.var(d->st.dupChunks);

    std::vector<uint8_t> packed;
    if (!zstdCompress(ix.b.data(), ix.b.size(), 19, false, packed)) {
        d->fail("index compression failed");
    } else {
        uint64_t ixOff = d->pos;
        d->out.write((const char*)packed.data(), (std::streamsize)packed.size());
        OutBuf f;
        f.u64le(ixOff);
        f.u64le(packed.size());
        f.u64le(ix.b.size());
        f.u64le(XXH3_64bits(ix.b.data(), ix.b.size()));
        f.raw(kFootMagic, 8);
        d->out.write((const char*)f.b.data(), (std::streamsize)f.b.size());
        d->pos += packed.size() + f.b.size();
        d->out.flush();
        if (!d->out) d->fail("write error (disk full?)");
    }
    d->out.close();
    d->finished = true;
    d->st.packedBytes = d->pos;  // report the true file size
    if (d->failed) {
        std::error_code ec;
        fs::remove(d->outPath, ec);
        err = d->err;
        return false;
    }
    return true;
}

void Writer::abort() {
    for (auto& f : d->inflight)
        if (f.valid()) f.wait();
    d->inflight.clear();
    if (d->out.is_open()) d->out.close();
    std::error_code ec;
    if (d->opened) fs::remove(d->outPath, ec);
    d->finished = true;
}

// ============================================================================ Reader
struct Reader::Impl {
    Source src;
    // The archive may be split into volumes (x.kmp.001, .002 ...): reads map onto the parts.
    struct Part {
        std::string path;
        uint64_t offset = 0, size = 0;
        std::ifstream f;
    };
    std::vector<Part> parts;
    std::mutex io;  // parts' streams are shared by the decode workers
    uint64_t size = 0;
    Mode mode = Mode::Ultra;
    std::vector<BlockRec> blocks;
    std::vector<ChunkRec> chunks;
    std::vector<Entry> entries;
    std::unordered_map<std::string, int> byPath;
    Stats st;

    // Decoded blocks. While reading sequentially (extract / test) the blocks after the current one
    // are decoded ahead on a worker pool, so every core decompresses at once.
    struct Decoded {
        std::shared_ptr<std::vector<uint8_t>> data;
        std::string err;
    };
    std::unordered_map<uint32_t, std::shared_future<Decoded>> cache;
    std::vector<uint32_t> order;  // blocks in the order a front-to-back read needs them
    std::vector<int> orderPos;    // block id -> index in `order`
    // For each block, how far into `order` a front-to-back read has got by the time it needs the block
    // for the last time; once the read is past that, the block can be dropped. (Blocks are shared by
    // files that are far apart, so "behind the current block" is not enough: re-decoding a
    // context-mixed block costs as much as making it.)
    std::vector<int> lastNeed;
    int frontier = -1;
    Progress* workProg = nullptr;  // decode progress while reading front to back
    bool sequential = false;
    std::unique_ptr<ThreadPool> pool;
    size_t window = 1;

    bool readAt(uint64_t off, void* dst, size_t n) {
        if (off + n > size) return false;
        if (src.inMemory()) {
            std::memcpy(dst, src.mem->data() + off, n);
            return true;
        }
        std::lock_guard<std::mutex> lk(io);
        uint8_t* out = (uint8_t*)dst;
        for (auto& p : parts) {
            if (n == 0) break;
            if (off >= p.offset + p.size) continue;
            uint64_t within = off - p.offset;
            size_t take = (size_t)std::min<uint64_t>(n, p.size - within);
            if (!p.f.is_open()) p.f.open(u8path(p.path), std::ios::binary);
            p.f.clear();
            p.f.seekg((std::streamoff)within);
            p.f.read((char*)out, (std::streamsize)take);
            if ((size_t)p.f.gcount() != take) return false;
            out += take;
            off += take;
            n -= take;
        }
        return n == 0;
    }

    Decoded decode(uint32_t id) {
        Decoded r;
        const BlockRec& b = blocks[id];
        std::vector<uint8_t> packed((size_t)b.packed);
        if (!readAt(b.offset, packed.data(), packed.size())) {
            r.err = "archive is truncated (block " + std::to_string(id) + ")";
            return r;
        }
        auto out = std::make_shared<std::vector<uint8_t>>();
        Progress* pr = workProg;
        cm::Tick tick;
        if (pr) tick = [pr](size_t k) { pr->workDone += k; };
        if (!decompressBlock(b, packed.data(), *out, r.err, tick)) return r;
        if (pr && b.codec != CodecCm) pr->workDone += b.raw;
        if (XXH3_64bits(out->data(), out->size()) != b.hash) {
            r.err = "block " + std::to_string(id) + " failed its checksum (archive damaged)";
            return r;
        }
        r.data = std::move(out);
        return r;
    }

    void startDecode(uint32_t id) {
        if (cache.count(id)) return;
        if (pool) {
            cache.emplace(id, pool->submit([this, id] { return decode(id); }).share());
        } else {
            std::promise<Decoded> p;
            p.set_value(decode(id));
            cache.emplace(id, p.get_future().share());
        }
    }

    void enableSequential() {
        if (sequential || blocks.empty()) return;
        sequential = true;
        uint64_t biggest = 1;
        for (auto& b : blocks)
            biggest = std::max<uint64_t>(biggest, (b.raw + b.packed) * 2 + (b.codec == CodecCm ? cm::memoryFor((size_t)b.raw, (int)(b.param >> 8)) : 0));
        unsigned t = threadsFor(biggest, 0);
        if (t > 1) {
            pool = std::make_unique<ThreadPool>(t);
            window = std::max<size_t>(t + t / 2, 2);
        }
    }

    std::shared_ptr<std::vector<uint8_t>> block(uint32_t id, std::string& err) {
        if (id >= blocks.size()) {
            err = "chunk references missing block";
            return nullptr;
        }
        startDecode(id);
        if (sequential && pool) {
            frontier = std::max(frontier, orderPos[id]);
            int pos = frontier;
            for (size_t k = 1; k <= window && pos + k < order.size(); ++k) startDecode(order[pos + k]);
            // Forget blocks no later read needs.
            for (auto it = cache.begin(); it != cache.end();) {
                if (it->first != id && lastNeed[it->first] < frontier &&
                    it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    it = cache.erase(it);
                else
                    ++it;
            }
        } else if (cache.size() > 4) {
            for (auto it = cache.begin(); it != cache.end();) {
                if (it->first != id) it = cache.erase(it);
                else ++it;
            }
        }
        const Decoded& d = cache.at(id).get();
        if (!d.data) {
            err = d.err;
            return nullptr;
        }
        return d.data;
    }
};

Reader::Reader() : d(new Impl) {}
Reader::~Reader() = default;
void Reader::setSequential(Progress* prog) {
    d->enableSequential();
    if (prog) {
        uint64_t t = 0;
        for (auto& b : d->blocks) t += b.raw;
        prog->workDone = 0;
        prog->workTotal = t;
        d->workProg = prog;
    }
}
const std::vector<Entry>& Reader::entries() const { return d->entries; }
const Stats& Reader::stats() const { return d->st; }
Mode Reader::mode() const { return d->mode; }
uint64_t Reader::archiveSize() const { return d->size; }

int Reader::findEntry(const std::string& path) const {
    auto it = d->byPath.find(path);
    return it == d->byPath.end() ? -1 : it->second;
}

bool Reader::open(const Source& src, std::string& err) {
    d->src = src;
    if (src.inMemory()) {
        d->size = src.mem->size();
    } else {
        std::vector<std::string> vols = volumeSet(src.path);
        if (vols.empty()) vols.push_back(src.path);
        d->parts.resize(vols.size());
        for (size_t i = 0; i < vols.size(); ++i) {
            std::error_code ec;
            uint64_t sz = fs::file_size(u8path(vols[i]), ec);
            if (ec) {
                err = "cannot open " + vols[i];
                return false;
            }
            d->parts[i].path = vols[i];
            d->parts[i].offset = d->size;
            d->parts[i].size = sz;
            d->size += sz;
        }
    }
    if (d->size < kHeaderSize + kFooterSize) {
        err = "file too small to be a KMP archive";
        return false;
    }
    uint8_t hdr[kHeaderSize], foot[kFooterSize];
    if (!d->readAt(0, hdr, kHeaderSize) || std::memcmp(hdr, kMagic, 8) != 0) {
        err = "not a KMP archive";
        return false;
    }
    uint16_t ver = (uint16_t)rdLE(hdr + 8, 2);
    if (ver > kVersion) {
        err = "KMP version " + std::to_string(ver) + " is newer than this Kompressor";
        return false;
    }
    if (!d->readAt(d->size - kFooterSize, foot, kFooterSize) || std::memcmp(foot + 32, kFootMagic, 8) != 0) {
        err = "KMP footer missing (archive incomplete or truncated)";
        return false;
    }
    uint64_t ixOff = rdLE(foot, 8), ixPacked = rdLE(foot + 8, 8), ixRaw = rdLE(foot + 16, 8), ixHash = rdLE(foot + 24, 8);
    if (ixOff + ixPacked > d->size - kFooterSize || ixRaw > (1ull << 34)) {
        err = "KMP index location is invalid";
        return false;
    }
    std::vector<uint8_t> packed((size_t)ixPacked), raw((size_t)ixRaw);
    if (!d->readAt(ixOff, packed.data(), packed.size())) {
        err = "cannot read KMP index";
        return false;
    }
    size_t r = ZSTD_decompress(raw.data(), raw.size(), packed.data(), packed.size());
    if (ZSTD_isError(r) || r != ixRaw || XXH3_64bits(raw.data(), raw.size()) != ixHash) {
        err = "KMP index is corrupt";
        return false;
    }
    try {
        InBuf in{raw.data(), raw.data() + raw.size()};
        d->mode = (Mode)std::min<uint64_t>(in.var(), 5);
        uint64_t nb = in.var();
        if (nb > raw.size()) throw std::runtime_error("bad block count");
        d->blocks.resize((size_t)nb);
        for (auto& b : d->blocks) {
            b.offset = in.var();
            b.packed = in.var();
            b.raw = in.var();
            b.codec = in.u8();
            b.filter = in.u8();
            b.param = (uint32_t)in.var();
            b.hash = in.u64le();
            if (b.offset + b.packed > d->size || b.raw > (4ull << 30)) throw std::runtime_error("bad block record");
        }
        uint64_t nc = in.var();
        if (nc > raw.size()) throw std::runtime_error("bad chunk count");
        d->chunks.resize((size_t)nc);
        for (auto& c : d->chunks) {
            c.block = (uint32_t)in.var();
            c.offset = (uint32_t)in.var();
            c.size = (uint32_t)in.var();
            if (c.block >= d->blocks.size() || (uint64_t)c.offset + c.size > d->blocks[c.block].raw)
                throw std::runtime_error("bad chunk record");
        }
        uint64_t ne = in.var();
        if (ne > raw.size()) throw std::runtime_error("bad entry count");
        d->entries.resize((size_t)ne);
        for (auto& e : d->entries) {
            e.path = in.str();
            e.type = (EntryType)in.u8();
            e.mode = (uint32_t)in.var();
            e.mtime = in.svar();
            e.size = in.var();
            e.hash = in.u64le();
            if (e.type == EntryType::Symlink) e.link = in.str();
            uint64_t n = in.var();
            if (n > nc) throw std::runtime_error("bad chunk list");
            e.chunks.resize((size_t)n);
            int64_t prev = -1;
            for (auto& c : e.chunks) {
                prev += in.svar();
                if (prev < 0 || (uint64_t)prev >= nc) throw std::runtime_error("bad chunk reference");
                c = (uint32_t)prev;
            }
        }
        d->st.rawBytes = in.var();
        d->st.uniqueBytes = in.var();
        d->st.dupChunks = in.var();
    } catch (const std::exception& ex) {
        err = std::string("KMP index: ") + ex.what();
        return false;
    }
    d->st.packedBytes = d->size;
    d->st.blocks = d->blocks.size();
    d->st.chunks = d->chunks.size() + d->st.dupChunks;
    for (size_t i = 0; i < d->entries.size(); ++i) {
        if (d->entries[i].type == EntryType::File) d->st.files++;
        d->byPath[d->entries[i].path] = (int)i;
    }
    d->orderPos.assign(d->blocks.size(), -1);
    d->lastNeed.assign(d->blocks.size(), -1);
    int front = -1;
    for (auto& e : d->entries)
        for (uint32_t cid : e.chunks) {
            uint32_t b = d->chunks[cid].block;
            if (d->orderPos[b] < 0) {
                d->orderPos[b] = (int)d->order.size();
                d->order.push_back(b);
            }
            front = std::max(front, d->orderPos[b]);
            d->lastNeed[b] = front;
        }
    for (uint32_t b = 0; b < d->blocks.size(); ++b)
        if (d->orderPos[b] < 0) {
            d->orderPos[b] = (int)d->order.size();
            d->order.push_back(b);
        }
    return true;
}

bool Reader::readFile(size_t index, const std::function<bool(const uint8_t*, size_t)>& sink, Progress* prog,
                      std::string& err) {
    if (index >= d->entries.size()) {
        err = "no such entry";
        return false;
    }
    const Entry& e = d->entries[index];
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    uint64_t total = 0;
    bool stopped = false;
    for (uint32_t cid : e.chunks) {
        if (prog && prog->cancelled()) {
            XXH3_freeState(st);
            err = "cancelled";
            return false;
        }
        const ChunkRec& c = d->chunks[cid];
        auto blk = d->block(c.block, err);
        if (!blk) {
            XXH3_freeState(st);
            return false;
        }
        const uint8_t* p = blk->data() + c.offset;
        XXH3_64bits_update(st, p, c.size);
        total += c.size;
        if (!sink(p, c.size)) {
            stopped = true;
            break;
        }
    }
    uint64_t h = XXH3_64bits_digest(st);
    XXH3_freeState(st);
    if (stopped) return true;
    if (total != e.size || h != e.hash) {
        err = "checksum mismatch in " + e.path;
        return false;
    }
    return true;
}

bool isKmp(const Source& src) {
    uint8_t m[8];
    if (src.inMemory()) {
        if (src.mem->size() < 8) return false;
        std::memcpy(m, src.mem->data(), 8);
    } else {
        std::ifstream f(u8path(firstVolume(src.path)), std::ios::binary);
        if (!f.read((char*)m, 8)) return false;
    }
    return std::memcmp(m, kMagic, 8) == 0;
}

}  // namespace kz::kmp
