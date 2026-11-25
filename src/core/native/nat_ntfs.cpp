// NTFS: the MFT is read record by record; folders come from each name's parent reference. Handles
// sparse, compressed (LZNT1) and Windows "compact" (WOF: XPRESS/LZX) files and alternate data
// streams ("file:stream").
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "nat.hpp"

namespace kz::native {

namespace {

struct Run {
    uint64_t vcn, lcn, len;  // lcn == kHole: sparse
};

struct DataAttr {
    std::string name;             // "" = the file's contents
    bool resident = false;
    std::vector<uint8_t> value;   // resident data
    std::vector<Run> runs;        // non-resident, possibly from several records (sorted by vcn)
    uint64_t size = 0, initialized = 0;
    uint32_t compUnit = 0;        // clusters per compression unit (0 = not compressed)
    bool encrypted = false;
};

struct Name {
    uint64_t parent;
    std::string name;
};

struct Rec {
    bool used = false, dir = false;
    int64_t mtime = 0;
    std::vector<Name> names;
    std::vector<DataAttr> data;
    uint32_t reparseTag = 0;
    std::vector<uint8_t> reparse;
};

struct Ntfs {
    StreamPtr s;
    uint64_t cluster = 4096;
    uint32_t recSize = 1024;
};

// Undoes the "update sequence" fix-up NTFS applies to each 512-byte stride of a record.
bool fixup(std::vector<uint8_t>& r, uint32_t bps) {
    Span R(r);
    uint32_t usaOff = R.u16(4), usaCount = R.u16(6);
    if (usaCount == 0 || !R.has(usaOff, usaCount * 2)) return false;
    uint32_t stride = bps ? bps : 512;
    for (uint32_t i = 1; i < usaCount; ++i) {
        size_t end = (size_t)i * stride - 2;
        if (end + 2 > r.size()) break;
        if (r[end] != r[usaOff] || r[end + 1] != r[usaOff + 1]) return false;
        r[end] = r[usaOff + i * 2];
        r[end + 1] = r[usaOff + i * 2 + 1];
    }
    return true;
}

std::vector<Run> decodeRuns(const uint8_t* p, size_t n, uint64_t vcn) {
    std::vector<Run> runs;
    size_t i = 0;
    int64_t lcn = 0;
    while (i < n && p[i]) {
        uint32_t h = p[i++], lb = h & 15, ob = h >> 4;
        if (lb == 0 || lb > 8 || ob > 8 || i + lb + ob > n) break;
        uint64_t len = 0;
        for (uint32_t k = 0; k < lb; ++k) len |= (uint64_t)p[i + k] << (8 * k);
        i += lb;
        if (ob == 0) {
            runs.push_back({vcn, kHole, len});
        } else {
            int64_t d = 0;
            for (uint32_t k = 0; k < ob; ++k) d |= (int64_t)p[i + k] << (8 * k);
            if (p[i + ob - 1] & 0x80) d |= ~((int64_t)0) << (8 * ob);  // sign-extend
            i += ob;
            lcn += d;
            runs.push_back({vcn, (uint64_t)lcn, len});
        }
        vcn += len;
    }
    return runs;
}

void parseRecord(const std::vector<uint8_t>& r, Rec& rec, uint64_t& base) {
    Span R(r);
    uint32_t flags = R.u16(22);
    base = R.u64(32) & 0xFFFFFFFFFFFFull;
    rec.used = flags & 1;
    if (flags & 2) rec.dir = true;
    size_t at = R.u16(20);
    while (R.has(at, 16)) {
        uint32_t type = R.u32(at), len = R.u32(at + 4);
        if (type == 0xFFFFFFFF || len < 16 || !R.has(at, len)) break;
        bool nonres = R.u8(at + 8);
        uint32_t nameLen = R.u8(at + 9), nameOff = R.u16(at + 10), aflags = R.u16(at + 12);
        std::string aname = R.has(at + nameOff, nameLen * 2) ? utf16le(r.data() + at + nameOff, nameLen) : "";
        Span V;
        if (!nonres) {
            uint32_t vlen = R.u32(at + 16), voff = R.u16(at + 20);
            if (R.has(at + voff, vlen)) V = Span(r.data() + at + voff, vlen);
        }
        if (type == 0x10 && !nonres) {  // $STANDARD_INFORMATION
            rec.mtime = fromFiletime(V.u64(8));
        } else if (type == 0x30 && !nonres && V.n >= 66) {  // $FILE_NAME
            uint32_t nlen = V.u8(64), ns = V.u8(65);
            if (ns != 2 && V.has(66, nlen * 2))  // not the DOS 8.3 alias
                rec.names.push_back({V.u64(0) & 0xFFFFFFFFFFFFull, utf16le(V.p + 66, nlen)});
            if (!rec.mtime) rec.mtime = fromFiletime(V.u64(16));
        } else if (type == 0x80) {  // $DATA
            DataAttr* d = nullptr;
            for (auto& x : rec.data)
                if (x.name == aname) d = &x;
            if (!d) {
                rec.data.push_back({});
                d = &rec.data.back();
                d->name = aname;
            }
            d->encrypted |= (aflags & 0x4000) != 0;
            if (!nonres) {
                d->resident = true;
                d->value.assign(V.p, V.p + V.n);
                d->size = d->initialized = V.n;
            } else {
                uint64_t start = R.u64(at + 16);
                uint32_t roff = R.u16(at + 32), cu = R.u16(at + 34);
                if (start == 0) {
                    d->size = R.u64(at + 48);
                    d->initialized = R.u64(at + 56);
                    if ((aflags & 1) && cu) d->compUnit = 1u << cu;
                }
                if (R.has(at + roff, len - roff)) {
                    std::vector<Run> rr = decodeRuns(r.data() + at + roff, len - roff, start);
                    d->runs.insert(d->runs.end(), rr.begin(), rr.end());
                }
            }
        } else if (type == 0xC0) {  // $REPARSE_POINT
            rec.reparseTag = nonres ? 0 : V.u32(0);
            if (!nonres) rec.reparse.assign(V.p, V.p + V.n);
        }
        at += len;
    }
}

// Byte runs for a non-compressed non-resident stream.
std::vector<std::pair<uint64_t, uint64_t>> byteRuns(const DataAttr& d, uint64_t cluster) {
    std::vector<std::pair<uint64_t, uint64_t>> out;
    uint64_t want = std::min(d.initialized ? d.initialized : d.size, d.size), got = 0;
    for (auto& r : d.runs) {
        if (got >= want) break;
        uint64_t len = std::min(r.len * cluster, want - got);
        addRun(out, r.lcn == kHole ? kHole : r.lcn * cluster, len);
        got += len;
    }
    return out;
}

// Reads `n` clusters of a stream starting at `vcn` (sparse parts are zeros).
bool readVcns(Stream& s, const DataAttr& d, uint64_t cluster, uint64_t vcn, uint64_t n, std::vector<uint8_t>& out, bool& allSparse,
              bool& allData) {
    out.assign(n * cluster, 0);
    allSparse = allData = true;
    for (auto& r : d.runs) {
        uint64_t a = std::max(vcn, r.vcn), b = std::min(vcn + n, r.vcn + r.len);
        if (a >= b) continue;
        if (r.lcn == kHole) {
            allData = false;
            continue;
        }
        allSparse = false;
        if (!s.readAt((r.lcn + (a - r.vcn)) * cluster, out.data() + (a - vcn) * cluster, (size_t)((b - a) * cluster))) return false;
    }
    // Clusters no run covers count as sparse.
    uint64_t covered = 0;
    for (auto& r : d.runs) {
        uint64_t a = std::max(vcn, r.vcn), b = std::min(vcn + n, r.vcn + r.len);
        if (a < b) covered += b - a;
    }
    if (covered < n) allData = false;
    return true;
}

// The contents of a non-resident stream through `sink`, decompressing LZNT1 units.
bool readCompressed(StreamPtr s, const DataAttr& d, uint64_t cluster, const Sink& sink, std::string& err) {
    uint64_t unit = d.compUnit, left = d.size;
    std::vector<uint8_t> raw, out;
    for (uint64_t vcn = 0; left; vcn += unit) {
        bool sparse, full;
        if (!readVcns(*s, d, cluster, vcn, unit, raw, sparse, full)) {
            err = "The data can't be read";
            return false;
        }
        const std::vector<uint8_t>* piece = &raw;
        if (sparse) {
            std::fill(raw.begin(), raw.end(), 0);
        } else if (!full) {  // compressed unit: the allocated clusters hold LZNT1 data
            if (!unLznt1(raw.data(), raw.size(), out, unit * cluster)) {
                err = "Damaged compressed data";
                return false;
            }
            out.resize(unit * cluster, 0);
            piece = &out;
        }
        size_t take = (size_t)std::min<uint64_t>(left, piece->size());
        if (!sink(piece->data(), take)) return true;
        left -= take;
    }
    return true;
}

// Windows "compact" files: the real contents are an XPRESS/LZX-compressed alternate stream.
bool readWof(StreamPtr s, const DataAttr& wof, uint64_t cluster, uint64_t size, uint32_t algo, const Sink& sink, std::string& err) {
    std::vector<uint8_t> packed;
    if (wof.resident) {
        packed = wof.value;
    } else {
        std::vector<std::pair<uint64_t, uint64_t>> runs = byteRuns(wof, cluster);
        for (auto& [off, len] : runs) {
            if (off == kHole) packed.insert(packed.end(), (size_t)len, 0);
            else {
                std::vector<uint8_t> v = s->getUpTo(off, (size_t)len);
                packed.insert(packed.end(), v.begin(), v.end());
            }
        }
    }
    uint32_t chunk = algo == 1 ? 32768 : algo == 2 ? 8192 : algo == 3 ? 16384 : 4096;
    uint64_t chunks = (size + chunk - 1) / chunk;
    size_t entry = size > 0xFFFFFFFFull ? 8 : 4;
    size_t table = (size_t)(chunks ? (chunks - 1) * entry : 0);
    if (packed.size() < table) {
        err = "Damaged compact-file data";
        return false;
    }
    Span P(packed);
    std::unique_ptr<LzxDecoder> lzx;
    if (algo == 1) {
        lzx = std::make_unique<LzxDecoder>(15);
        lzx->setE8(true, 12000000);
        lzx->setWim(true);
    }
    std::vector<uint8_t> out(chunk);
    for (uint64_t i = 0; i < chunks; ++i) {
        uint64_t a = i == 0 ? 0 : (entry == 8 ? P.u64((size_t)(i - 1) * 8) : P.u32((size_t)(i - 1) * 4));
        uint64_t b = i + 1 == chunks ? packed.size() - table : (entry == 8 ? P.u64((size_t)i * 8) : P.u32((size_t)i * 4));
        if (b < a || table + b > packed.size()) {
            err = "Damaged compact-file data";
            return false;
        }
        size_t want = (size_t)std::min<uint64_t>(chunk, size - i * chunk);
        const uint8_t* in = packed.data() + table + a;
        size_t inLen = (size_t)(b - a);
        if (inLen == want) {  // stored
            if (!sink(in, want)) return true;
            continue;
        }
        bool ok;
        if (algo == 1) {
            lzx->reset();
            ok = lzx->decode(in, inLen, out.data(), want);
        } else {
            std::vector<uint8_t> v;
            ok = unXpressHuff(in, inLen, v, want) && v.size() >= want;
            if (ok) std::memcpy(out.data(), v.data(), want);
        }
        if (!ok) {
            err = "Damaged compact-file data";
            return false;
        }
        if (!sink(out.data(), want)) return true;
    }
    return true;
}

bool openNtfs(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    Span H(h);
    uint32_t bps = H.u16(11), spcRaw = H.u8(13);
    uint64_t spc = spcRaw <= 0x80 ? spcRaw : 1ull << (256 - spcRaw);
    if (!bps || !spc) return false;
    Ntfs v;
    v.s = s;
    v.cluster = bps * spc;
    int8_t cpr = (int8_t)H.u8(64);
    v.recSize = cpr > 0 ? (uint32_t)(cpr * v.cluster) : 1u << (-cpr);
    if (v.recSize < 256 || v.recSize > 65536) return false;
    uint64_t mftAt = H.u64(48) * v.cluster;

    // Record 0 is the MFT itself: its data runs say where the rest of the MFT is.
    std::vector<uint8_t> r0 = s->getUpTo(mftAt, v.recSize);
    if (r0.size() < v.recSize || !Span(r0).eq(0, "FILE", 4) || !fixup(r0, bps)) {
        ctx.err = "Damaged NTFS (the MFT can't be read)";
        return false;
    }
    Rec mft;
    uint64_t base;
    parseRecord(r0, mft, base);
    const DataAttr* md = nullptr;
    for (auto& d : mft.data)
        if (d.name.empty()) md = &d;
    if (!md || md->resident) return false;
    std::vector<ExtentStream::Extent> ext;
    for (auto& r : md->runs)
        if (r.lcn != kHole) ext.push_back({r.vcn * v.cluster, r.len * v.cluster, s, r.lcn * v.cluster, {}});
    StreamPtr mftStream = std::make_shared<ExtentStream>(md->size, std::move(ext));
    uint64_t count = md->size / v.recSize;

    std::unordered_map<uint64_t, Rec> recs;
    std::vector<uint8_t> buf;
    const uint64_t batch = std::max<uint64_t>(1, (4u << 20) / v.recSize);
    for (uint64_t i = 0; i < count; i += batch) {
        uint64_t n = std::min(batch, count - i);
        buf = mftStream->getUpTo(i * v.recSize, (size_t)(n * v.recSize));
        for (uint64_t k = 0; k < n && (k + 1) * v.recSize <= buf.size(); ++k) {
            std::vector<uint8_t> r(buf.begin() + k * v.recSize, buf.begin() + (k + 1) * v.recSize);
            if (!Span(r).eq(0, "FILE", 4) || !fixup(r, bps)) continue;
            if (!(Span(r).u16(22) & 1)) continue;  // not in use
            Rec one;
            uint64_t b;
            parseRecord(r, one, b);
            uint64_t id = b ? b : i + k;  // extension records add to their base record
            Rec& dst = recs[id];
            if (!b) {
                dst.used = true;
                dst.dir |= one.dir;
                dst.mtime = one.mtime ? one.mtime : dst.mtime;
                dst.reparseTag = one.reparseTag;
                dst.reparse = std::move(one.reparse);
            }
            dst.names.insert(dst.names.end(), one.names.begin(), one.names.end());
            for (auto& d : one.data) {
                DataAttr* t = nullptr;
                for (auto& x : dst.data)
                    if (x.name == d.name) t = &x;
                if (!t) {
                    dst.data.push_back(std::move(d));
                    continue;
                }
                t->runs.insert(t->runs.end(), d.runs.begin(), d.runs.end());
                if (d.size) t->size = d.size, t->initialized = d.initialized, t->compUnit = d.compUnit;
                if (d.resident) t->resident = true, t->value = std::move(d.value);
            }
        }
    }
    for (auto& [id, r] : recs)
        for (auto& d : r.data) std::sort(d.runs.begin(), d.runs.end(), [](const Run& a, const Run& b) { return a.vcn < b.vcn; });

    // Paths, from each name's parent (record 5 is the root).
    std::unordered_map<uint64_t, std::string> paths;
    std::function<std::string(uint64_t, int)> pathOf = [&](uint64_t id, int depth) -> std::string {
        if (id == 5) return "";
        auto c = paths.find(id);
        if (c != paths.end()) return c->second;
        auto it = recs.find(id);
        if (it == recs.end() || it->second.names.empty() || depth > 256) return "[ORPHAN]/" + std::to_string(id) + "/";
        const Name& n = it->second.names.front();
        std::string p = pathOf(n.parent, depth + 1) + component(n.name) + "/";
        paths[id] = p;
        return p;
    };

    ctx.formats.push_back("NTFS");
    uint64_t cluster = v.cluster;
    for (auto& [id, r] : recs) {
        if (!r.used || id == 5 || r.names.empty()) continue;
        if (id < 16) continue;  // $MFT, $LogFile, $Bitmap and the other metadata files
        std::set<std::pair<uint64_t, std::string>> done;
        for (auto& n : r.names) {
            if (!done.insert({n.parent, n.name}).second) continue;  // a hard link listed twice
            std::string parent = pathOf(n.parent, 0);
            if (n.parent == 5 && !n.name.empty() && n.name[0] == '$') continue;  // $Extend and friends
            std::string full = parent + component(n.name);
            if (r.dir) {
                ctx.out.items.push_back(dirItem(ctx.prefix + full, r.mtime));
                continue;
            }
            const DataAttr* wof = nullptr;
            uint32_t wofAlgo = 0;
            if (r.reparseTag == 0x80000017 && r.reparse.size() >= 24) {  // WOF-compressed ("compact")
                Span W(r.reparse);
                if (W.u32(12) == 2) wofAlgo = W.u32(20);  // file provider: XPRESS4K / LZX / XPRESS8K / XPRESS16K
                for (auto& d : r.data)
                    if (d.name == "WofCompressedData") wof = &d;
            }
            for (auto& d : r.data) {
                if (&d == wof) continue;
                std::string path = ctx.prefix + full + (d.name.empty() ? "" : ":" + component(d.name));
                StreamPtr src = s;
                if (d.encrypted) {
                    if (ctx.err.empty()) ctx.err = "Some files are encrypted (EFS) and can't be read";
                }
                if (d.name.empty() && wof && wofAlgo <= 3) {
                    Item it;
                    it.path = path;
                    it.size = d.size;
                    it.mtime = r.mtime;
                    DataAttr w = *wof;
                    uint64_t sz = d.size;
                    it.read = [src, w, cluster, sz, wofAlgo](const Sink& sink, std::string& err) {
                        return readWof(src, w, cluster, sz, wofAlgo, sink, err);
                    };
                    ctx.out.items.push_back(std::move(it));
                } else if (d.resident) {
                    ctx.out.items.push_back(builtItem(path, d.value, r.mtime));
                } else if (d.compUnit) {
                    Item it;
                    it.path = path;
                    it.size = d.size;
                    it.mtime = r.mtime;
                    DataAttr copy = d;
                    it.read = [src, copy, cluster](const Sink& sink, std::string& err) {
                        return readCompressed(src, copy, cluster, sink, err);
                    };
                    ctx.out.items.push_back(std::move(it));
                } else {
                    ctx.out.items.push_back(runsItem(path, src, byteRuns(d, cluster), d.size, r.mtime));
                }
            }
            if (r.data.empty()) ctx.out.items.push_back(builtItem(ctx.prefix + full, {}, r.mtime));
        }
    }
    return true;
}

}  // namespace

const Handler hNtfs = {"NTFS",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                           return Span(h).eq(3, "NTFS    ", 8) && Span(h).u16(510) == 0xAA55;
                       },
                       openNtfs};

}  // namespace kz::native
