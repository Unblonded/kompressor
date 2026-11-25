// WIM and ESD (Windows imaging): a table of content-addressed blobs (by SHA-1), one metadata blob per
// image holding its directory tree, and chunked compression (XPRESS, LZX or LZMS). ESD files keep many
// blobs together in "solid" resources. Split images (.swm) are read across their parts.
#include <algorithm>
#include <map>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

enum : uint8_t { kResFree = 1, kResMetadata = 2, kResCompressed = 4, kResSpanned = 8, kResSolid = 0x10 };
enum Algo { kNone = 0, kXpress = 1, kLzx = 2, kLzms = 3 };

struct Res {
    uint64_t size = 0;  // stored size
    uint8_t flags = 0;
    uint64_t off = 0, orig = 0;
};

Res readRes(const Span& S, size_t o) {
    Res r;
    r.size = S.u64(o) & 0x00FFFFFFFFFFFFFFull;
    r.flags = (uint8_t)S.u8(o + 7);
    r.off = S.u64(o + 8);
    r.orig = S.u64(o + 16);
    return r;
}

bool decodeChunk(int algo, const uint8_t* in, size_t n, uint8_t* out, size_t outLen, uint32_t chunkSize,
                 std::unique_ptr<LzxDecoder>& lzx) {
    if (algo == kXpress) {
        std::vector<uint8_t> v;
        if (!unXpressHuff(in, n, v, outLen) || v.size() < outLen) return false;
        std::memcpy(out, v.data(), outLen);
        return true;
    }
    if (algo == kLzx) {
        if (!lzx) {
            int bits = 15;
            while (bits < 21 && (1u << bits) < chunkSize) ++bits;
            lzx = std::make_unique<LzxDecoder>(bits);
            lzx->setWim(true);
            lzx->setE8(true, 12000000);
        }
        lzx->reset();
        return lzx->decode(in, n, out, outLen);
    }
    if (algo == kLzms) {
        std::vector<uint8_t> v;
        if (!unLzms(in, n, v, outLen) || v.size() < outLen) return false;
        std::memcpy(out, v.data(), outLen);
        return true;
    }
    return false;
}

// A compressed resource, decompressed a chunk at a time. `starts` has every chunk's compressed offset
// (relative to `data`) plus the end.
class ChunkedStream : public BlockStream {
public:
    ChunkedStream(StreamPtr src, uint64_t data, std::vector<uint64_t> starts, uint64_t size, uint32_t chunk, int algo)
        : BlockStream(size, chunk), src_(std::move(src)), data_(data), starts_(std::move(starts)), algo_(algo) {}

protected:
    bool readBlock(uint64_t i, uint8_t* dst) override {
        if (i + 1 >= starts_.size()) return false;
        uint64_t a = starts_[i], b = starts_[i + 1];
        size_t outLen = (size_t)std::min<uint64_t>(bs_, size_ - i * bs_);
        if (b < a || b - a > (uint64_t)bs_ + 4096) return false;
        std::vector<uint8_t> in((size_t)(b - a));
        if (!src_->readAt(data_ + a, in.data(), in.size())) return false;
        if (in.size() == outLen) {  // stored as is: compressing it didn't help
            std::memcpy(dst, in.data(), outLen);
            return true;
        }
        in.resize(in.size() + 16, 0);
        return decodeChunk(algo_, in.data(), in.size() - 16, dst, outLen, bs_, lzx_);
    }

private:
    StreamPtr src_;
    uint64_t data_;
    std::vector<uint64_t> starts_;
    int algo_;
    std::unique_ptr<LzxDecoder> lzx_;
};

// A plain (non-solid) resource as its uncompressed bytes.
StreamPtr openPlain(StreamPtr s, const Res& r, int algo, uint32_t chunk) {
    if (r.off > s->size() || r.size > s->size() - r.off) return nullptr;
    if (!(r.flags & kResCompressed) || algo == kNone) return rangeStream(s, r.off, r.size);
    if (!chunk || r.orig == 0) return memStream(std::vector<uint8_t>{});
    uint64_t chunks = (r.orig + chunk - 1) / chunk;
    size_t entry = r.orig > 0xFFFFFFFFull ? 8 : 4;
    uint64_t tableLen = (chunks - 1) * entry;
    if (chunks > (1ull << 26) || tableLen > r.size) return nullptr;
    std::vector<uint8_t> t = s->get(r.off, (size_t)tableLen);
    if (t.size() != tableLen) return nullptr;
    Span T(t);
    std::vector<uint64_t> starts{0};
    for (uint64_t i = 0; i + 1 < chunks; ++i) starts.push_back(entry == 8 ? T.u64((size_t)i * 8) : T.u32((size_t)i * 4));
    starts.push_back(r.size - tableLen);
    return std::make_shared<ChunkedStream>(s, r.off + tableLen, std::move(starts), r.orig, chunk, algo);
}

// A solid resource: its own header (size, chunk size, compression) and a table of chunk sizes.
StreamPtr openSolid(StreamPtr s, const Res& r) {
    std::vector<uint8_t> h = s->get(r.off, 16);
    if (h.size() != 16) return nullptr;
    Span H(h);
    uint64_t usize = H.u64(0);
    uint32_t chunk = H.u32(8), algo = H.u32(12);
    if (!chunk || chunk > (1u << 30) || algo > kLzms) return nullptr;
    uint64_t chunks = (usize + chunk - 1) / chunk;
    if (chunks > (1ull << 26) || 16 + chunks * 4 > r.size) return nullptr;
    std::vector<uint8_t> t = s->get(r.off + 16, (size_t)chunks * 4);
    if (t.size() != chunks * 4) return nullptr;
    Span T(t);
    std::vector<uint64_t> starts{0};
    for (uint64_t i = 0; i < chunks; ++i) starts.push_back(starts.back() + T.u32((size_t)i * 4));
    return std::make_shared<ChunkedStream>(s, r.off + 16 + chunks * 4, std::move(starts), usize, chunk, (int)algo);
}

struct Blob {
    StreamPtr data;
    uint64_t size = 0;
};

struct Part {
    StreamPtr s;
    int algo = kNone;
    uint32_t chunk = 32768;
    uint32_t images = 0;
    uint16_t number = 1, total = 1;
    Res table, xml;
};

bool readHeader(StreamPtr s, Part& p) {
    std::vector<uint8_t> h = s->getUpTo(0, 208);
    Span H(h);
    if (!H.eq(0, "MSWIM\0\0\0", 8) || h.size() < 148) return false;
    uint32_t flags = H.u32(16);
    p.s = s;
    p.chunk = H.u32(20);
    if (flags & 0x2) {
        if (flags & 0x20000) p.algo = kXpress;
        else if (flags & 0x40000) p.algo = kLzx;
        else if (flags & 0x80000) p.algo = kLzms;
        else if (flags & 0x200000) p.algo = kXpress;
    }
    if (!p.chunk) p.chunk = 32768;
    p.number = (uint16_t)H.u16(40);
    p.total = (uint16_t)H.u16(42);
    p.images = H.u32(44);
    p.table = readRes(H, 48);
    p.xml = readRes(H, 72);
    return true;
}

// Adds a part's blobs to `blobs` and its metadata resources (one per image, in order) to `meta`.
bool readTable(const Part& p, std::map<std::string, Blob>& blobs, std::vector<Blob>& meta, bool& solidSeen,
               std::string& err) {
    StreamPtr ts = openPlain(p.s, p.table, p.algo, p.chunk);
    if (!ts || ts->size() > (512u << 20)) {
        err = "Damaged Windows image (blob table)";
        return false;
    }
    std::vector<uint8_t> tv = ts->get(0, (size_t)ts->size());
    if (tv.size() != ts->size()) {
        err = "Damaged Windows image (blob table)";
        return false;
    }
    Span T(tv);
    std::vector<StreamPtr> solid;  // the run of solid resources the following blobs are cut from
    StreamPtr solidAll;
    bool lastWasHeader = false;
    for (size_t o = 0; o + 50 <= tv.size(); o += 50) {
        Res r = readRes(T, o);
        uint32_t part = T.u16(o + 24);
        std::string hash((const char*)tv.data() + o + 30, 20);
        if (r.flags & kResSolid) {
            if (r.orig == 0x100000000ull) {
                if (!lastWasHeader) solid.clear(), solidAll.reset();
                StreamPtr ss = openSolid(p.s, r);
                if (!ss) {
                    err = "Damaged Windows image (solid resource)";
                    return false;
                }
                solid.push_back(ss);
                solidSeen = true;
                lastWasHeader = true;
                continue;
            }
            lastWasHeader = false;
            if (!solidAll) {
                if (solid.size() == 1) {
                    solidAll = solid[0];
                } else {
                    std::vector<ExtentStream::Extent> ext;
                    uint64_t at = 0;
                    for (auto& x : solid) {
                        ext.push_back({at, x->size(), x, 0, {}});
                        at += x->size();
                    }
                    solidAll = std::make_shared<ExtentStream>(at, std::move(ext));
                }
            }
            if (!solidAll || part != p.number) continue;
            uint64_t len = r.orig ? r.orig : r.size;
            if (r.off > solidAll->size() || len > solidAll->size() - r.off) continue;
            Blob b{rangeStream(solidAll, r.off, len), len};
            if (r.flags & kResMetadata) meta.push_back(b);
            else blobs[hash] = b;
            continue;
        }
        lastWasHeader = false;
        if (part != p.number || (r.flags & kResFree)) continue;
        StreamPtr bs = openPlain(p.s, r, p.algo, p.chunk);
        if (!bs) continue;
        Blob b{bs, bs->size()};
        if (r.flags & kResMetadata) meta.push_back(b);
        else blobs[hash] = b;
    }
    return true;
}

struct Walker {
    Ctx& ctx;
    const std::vector<uint8_t>& m;
    std::map<std::string, Blob>& blobs;
    std::string base;
    std::set<uint64_t> seen;

    void dir(uint64_t off, const std::string& path, int depth) {
        Span M(m);
        while (depth < 256 && off + 8 <= m.size() && seen.insert(off).second) {
            uint64_t len = M.u64((size_t)off);
            if (len == 0) return;  // end of this directory
            if (len < 102 || len > m.size() - off) return;
            size_t o = (size_t)off;
            uint32_t attr = M.u32(o + 8);
            uint64_t sub = M.u64(o + 16);
            int64_t mtime = fromFiletime(M.u64(o + 56));
            std::string hash((const char*)m.data() + o + 64, 20);
            uint32_t extra = M.u16(o + 96), nameLen = M.u16(o + 100);
            std::string name = nameLen && M.has(o + 102, nameLen) ? component(utf16le(m.data() + o + 102, nameLen / 2)) : "";
            uint64_t next = off + ((len + 7) & ~7ull);
            // Extra streams: the unnamed one (if listed here) is the file's data; named ones are skipped.
            for (uint32_t k = 0; k < extra && next + 38 <= m.size(); ++k) {
                uint64_t el = M.u64((size_t)next);
                if (el < 38) break;
                if (M.u16((size_t)next + 36) == 0) hash.assign((const char*)m.data() + next + 16, 20);
                next += (el + 7) & ~7ull;
            }
            std::string p = path.empty() ? name : name.empty() ? path : path + "/" + name;
            if (attr & 0x10) {
                if (!p.empty()) ctx.out.items.push_back(dirItem(base + p, mtime));
                if (sub) dir(sub, p, depth + 1);
            } else if (!p.empty()) {
                file(p, hash, attr, mtime);
            }
            off = next;
        }
    }

    void file(const std::string& p, const std::string& hash, uint32_t attr, int64_t mtime) {
        static const std::string zero(20, '\0');
        if (hash == zero || (attr & 0x400)) {  // empty, or a reparse point (its data is the link itself)
            ctx.out.items.push_back(builtItem(base + p, {}, mtime));
            return;
        }
        auto it = blobs.find(hash);
        if (it == blobs.end()) {
            Item i;
            i.path = base + p;
            i.mtime = mtime;
            i.read = [](const Sink&, std::string& err) {
                err = "This file's data is in another part of the split image";
                return false;
            };
            ctx.out.items.push_back(std::move(i));
            if (ctx.out.warning.empty()) ctx.out.warning = "Some files are in parts of the split image that weren't found";
            return;
        }
        ctx.out.items.push_back(streamItem(base + p, it->second.data, mtime));
    }
};

bool openWim(Ctx& ctx, StreamPtr s) {
    Part first;
    if (!readHeader(s, first)) return false;
    std::vector<Part> parts{first};
    // A split image: the other parts are named like the first with a number ("install2.swm").
    if (first.total > 1 && !ctx.srcPath.empty()) {
        std::string path = ctx.srcPath;
        size_t dot = path.find_last_of('.'), slash = path.find_last_of("/\\");
        std::string stem = dot != std::string::npos && (slash == std::string::npos || dot > slash) ? path.substr(0, dot) : path;
        std::string ext = stem.size() < path.size() ? path.substr(stem.size()) : "";
        // Strip a trailing number so starting from any part finds the rest.
        while (!stem.empty() && stem.back() >= '0' && stem.back() <= '9') stem.pop_back();
        for (int n = 1; n <= first.total; ++n) {
            if (n == first.number) continue;
            StreamPtr ps = fileStream(n == 1 ? stem + ext : stem + std::to_string(n) + ext);
            Part p;
            if (ps && readHeader(ps, p) && p.number == n) parts.push_back(p);
        }
    }
    std::map<std::string, Blob> blobs;
    std::vector<Blob> meta;
    bool solidSeen = false;
    for (auto& p : parts) {
        std::string err;
        if (!readTable(p, blobs, meta, solidSeen, err) && p.number == first.number) {
            ctx.err = err;
            return false;
        }
    }
    ctx.formats.push_back(first.algo == kLzms || solidSeen ? "ESD" : "WIM");
    for (size_t i = 0; i < meta.size(); ++i) {
        if (meta[i].size > (1ull << 31)) continue;
        std::vector<uint8_t> m = meta[i].data->get(0, (size_t)meta[i].size);
        if (m.size() != meta[i].size || m.size() < 8) {
            ctx.out.warning = "Damaged image metadata";
            continue;
        }
        Span M(m);
        uint64_t sec = M.u32(0);
        uint64_t root = std::max<uint64_t>(8, (sec + 7) & ~7ull);
        std::string base = ctx.prefix + (meta.size() > 1 ? std::to_string(i + 1) + "/" : "");
        if (meta.size() > 1) ctx.out.items.push_back(dirItem(ctx.prefix + std::to_string(i + 1), 0));
        Walker w{ctx, m, blobs, base, {}};
        // The root's own entry: its children are the image's top level.
        if (root + 102 <= m.size()) {
            uint64_t sub = M.u64((size_t)root + 16);
            if (sub) w.dir(sub, "", 0);
        }
    }
    return true;
}

}  // namespace

const Handler hWim = {"WIM",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(0, "MSWIM\0\0\0", 8); },
                      openWim};

}  // namespace kz::native
