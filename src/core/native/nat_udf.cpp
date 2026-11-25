// UDF (DVDs, Blu-ray, some USB images): anchor -> volume descriptors (partitions, logical volume)
// -> file set -> root directory. Files are File Entries whose allocation descriptors place their data
// in a partition; UDF 2.50+ keeps the directory tree in a metadata partition (itself a file).
#include <algorithm>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

bool tagOk(const Span& T, size_t o, uint32_t id) {
    if (!T.has(o, 16) || T.u16(o) != id) return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < 16; ++i)
        if (i != 4) sum = (uint8_t)(sum + T.p[o + i]);
    return sum == T.p[o + 4];
}

int64_t udfTime(const Span& T, size_t o) {
    uint32_t tz = T.u16(o);
    int year = (int16_t)T.u16(o + 2), mon = T.u8(o + 4), day = T.u8(o + 5), h = T.u8(o + 6), mi = T.u8(o + 7), sec = T.u8(o + 8);
    if (year < 1970 || mon < 1 || mon > 12 || day < 1 || day > 31) return 0;
    // Days from the civil date (proleptic Gregorian).
    int y = year - (mon <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    int64_t t = days * 86400 + h * 3600 + mi * 60 + sec;
    if ((tz >> 12) == 1) {  // local time with an offset in minutes
        int off = (int)(tz & 0xFFF);
        if (off & 0x800) off -= 0x1000;
        if (off != -2047) t -= (int64_t)off * 60;
    }
    return t;
}

// OSTA compressed Unicode: a byte saying 8 (one byte per character) or 16 (two, big-endian).
std::string dstring(const uint8_t* p, size_t n) {
    if (!n) return "";
    std::string s;
    if (p[0] == 8 || p[0] == 254) {
        for (size_t i = 1; i < n; ++i) putUtf8(s, p[i]);
    } else if (p[0] == 16 || p[0] == 255) {
        s = utf16be(p + 1, (n - 1) / 2);
    }
    while (!s.empty() && s.back() == 0) s.pop_back();
    return s;
}

struct Udf {
    StreamPtr s;
    uint32_t bs = 2048;
    std::vector<StreamPtr> parts;  // by partition reference number
    std::string err;

    std::vector<uint8_t> block(uint32_t part, uint32_t lbn, size_t n = 0) {
        if (part >= parts.size() || !parts[part]) return {};
        return parts[part]->getUpTo((uint64_t)lbn * bs, n ? n : bs);
    }

    // A file's data, from its (extended) file entry. False when it isn't one.
    struct Entry {
        uint32_t type = 0;
        uint64_t size = 0;
        int64_t mtime = 0;
        StreamPtr data;
    };

    bool entry(uint32_t part, uint32_t lbn, Entry& e) {
        std::vector<uint8_t> fe = block(part, lbn);
        Span F(fe);
        bool ext;
        if (tagOk(F, 0, 261)) ext = false;
        else if (tagOk(F, 0, 266)) ext = true;
        else return false;
        e.type = F.u8(27);
        uint32_t adType = F.u16(34) & 7;
        e.size = F.u64(56);
        e.mtime = udfTime(F, ext ? 92 : 84);
        uint32_t lea = F.u32(ext ? 208 : 168), lad = F.u32(ext ? 212 : 172);
        size_t ad = (ext ? 216 : 176) + (size_t)lea;
        if (!F.has(ad, lad)) return false;
        if (adType == 3) {  // the data is right here
            std::vector<uint8_t> v(fe.begin() + (ptrdiff_t)ad, fe.begin() + (ptrdiff_t)(ad + lad));
            v.resize((size_t)std::min<uint64_t>(e.size, v.size()));
            e.data = memStream(std::move(v));
            return true;
        }
        if (adType > 1) return false;  // ext_ad: not used by UDF writers
        std::vector<ExtentStream::Extent> ext2;
        uint64_t at = 0;
        std::vector<uint8_t> cur(fe.begin() + (ptrdiff_t)ad, fe.begin() + (ptrdiff_t)(ad + lad));
        uint32_t curPart = part;
        for (int guard = 0; guard < 4096 && at < e.size;) {
            Span A(cur);
            size_t step = adType == 0 ? 8 : 16;
            bool next = false;
            for (size_t o = 0; o + step <= cur.size() && at < e.size; o += step) {
                uint32_t len = A.u32(o) & 0x3FFFFFFF, kind = A.u32(o) >> 30;
                uint32_t pos = A.u32(o + 4);
                uint32_t p = adType == 0 ? curPart : A.u16(o + 8);
                if (len == 0) break;
                if (kind == 3) {  // the descriptors carry on in an allocation extent descriptor
                    std::vector<uint8_t> aed = block(p, pos, len);
                    Span E(aed);
                    if (!tagOk(E, 0, 258)) return false;
                    uint32_t l = E.u32(20);
                    if (!E.has(24, l)) return false;
                    cur.assign(aed.begin() + 24, aed.begin() + 24 + l);
                    curPart = p;
                    next = true;
                    ++guard;
                    break;
                }
                uint64_t take = std::min<uint64_t>(len, e.size - at);
                if (kind == 0 && p < parts.size() && parts[p]) ext2.push_back({at, take, parts[p], (uint64_t)pos * bs, {}});
                at += take;
            }
            if (!next) break;
        }
        e.data = std::make_shared<ExtentStream>(e.size, std::move(ext2));
        return true;
    }
};

struct Walker {
    Ctx& ctx;
    Udf& u;
    std::set<std::pair<uint32_t, uint32_t>> seen;

    void dir(const Udf::Entry& d, const std::string& path, int depth) {
        if (depth > 64 || d.size > (256u << 20)) return;
        std::vector<uint8_t> v = d.data->get(0, (size_t)d.size);
        Span V(v);
        for (size_t o = 0; o + 38 <= v.size();) {
            if (!tagOk(V, o, 257)) break;
            uint32_t chars = V.u8(o + 18), lfi = V.u8(o + 19), liu = V.u16(o + 36);
            uint32_t len = V.u32(o + 20) & 0x3FFFFFFF, lbn = V.u32(o + 24), part = V.u16(o + 28);
            size_t total = (38 + liu + lfi + 3) & ~size_t(3);
            if (!V.has(o, total)) break;
            std::string name = dstring(v.data() + o + 38 + liu, lfi);
            o += total;
            if ((chars & 0x0C) || name.empty() || !len) continue;  // deleted, parent, or nameless
            name = component(name);
            std::string p = path.empty() ? name : path + "/" + name;
            if (!seen.insert({part, lbn}).second && (chars & 2)) continue;  // a directory loop
            Udf::Entry e;
            if (!u.entry(part, lbn, e)) {
                if (ctx.out.warning.empty()) ctx.out.warning = "Some entries are damaged and were skipped";
                continue;
            }
            if ((chars & 2) || e.type == 4) {
                ctx.out.items.push_back(dirItem(ctx.prefix + p, e.mtime));
                dir(e, p, depth + 1);
            } else if (e.type == 5 || e.type == 12 || e.type == 0) {
                ctx.out.items.push_back(streamItem(ctx.prefix + p, e.data, e.mtime));
            }
        }
    }
};

// Where the anchor is, giving the sector size: sector 256 holds it.
bool findAnchor(Stream& s, uint32_t& bs, std::vector<uint8_t>& avdp) {
    for (uint32_t b : {2048u, 512u, 4096u, 1024u}) {
        avdp = s.getUpTo((uint64_t)256 * b, 512);
        Span A(avdp);
        if (tagOk(A, 0, 2) && A.u32(12) == 256) {
            bs = b;
            return true;
        }
    }
    return false;
}

bool openUdf(Ctx& ctx, StreamPtr s) {
    Udf u;
    u.s = s;
    std::vector<uint8_t> avdp;
    if (!findAnchor(*s, u.bs, avdp)) return false;
    Span A(avdp);
    // The volume descriptors: partitions (by number) and the logical volume (partition maps, file set).
    std::vector<std::pair<uint32_t, uint32_t>> pds;  // number -> (start), with length below
    std::vector<uint64_t> pdLen;
    std::vector<uint32_t> pdNum;
    std::vector<uint8_t> lvd;
    for (int which = 0; which < 2 && lvd.empty(); ++which) {
        uint32_t len = A.u32(16 + which * 8), loc = A.u32(20 + which * 8);
        pds.clear(), pdLen.clear(), pdNum.clear();
        for (int guard = 0; guard < 256 && len >= u.bs; ++guard) {
            std::vector<uint8_t> d = s->getUpTo((uint64_t)loc * u.bs, u.bs);
            Span D(d);
            uint32_t id = D.u16(0);
            if (!tagOk(D, 0, id) || id == 8 || id == 0) break;
            if (id == 3) {  // a pointer to where the sequence continues
                len = D.u32(20), loc = D.u32(24);
                continue;
            }
            if (id == 5) {
                pdNum.push_back(D.u16(22));
                pds.push_back({D.u16(22), D.u32(188)});
                pdLen.push_back(D.u32(192));
            }
            if (id == 6 && lvd.empty()) lvd = d;
            ++loc;
            len -= u.bs;
        }
    }
    if (lvd.empty() || pds.empty()) {
        ctx.err = "Damaged UDF volume (no logical volume)";
        return false;
    }
    Span L(lvd);
    uint32_t lbs = L.u32(212);
    if (lbs >= 512 && lbs <= 65536) u.bs = lbs;
    uint32_t nmaps = L.u32(268);
    auto physical = [&](uint32_t num) -> StreamPtr {
        for (size_t i = 0; i < pds.size(); ++i)
            if (pds[i].first == num) return rangeStream(s, (uint64_t)pds[i].second * u.bs, pdLen[i] * u.bs);
        return nullptr;
    };
    struct Meta {
        size_t ref;
        uint32_t base, file, mirror;
    };
    std::vector<Meta> metas;
    size_t o = 440;
    for (uint32_t m = 0; m < nmaps && m < 64 && L.has(o, 2); ++m) {
        uint32_t type = L.u8(o), mlen = L.u8(o + 1);
        if (mlen < 2) break;
        StreamPtr ps;
        if (type == 1) {
            ps = physical(L.u16(o + 4));
        } else if (type == 2) {
            std::string ident((const char*)lvd.data() + o + 5, 23);
            uint32_t num = L.u16(o + 38);
            if (ident.compare(0, 23, "*UDF Metadata Partition") == 0) {
                metas.push_back({u.parts.size(), num, L.u32(o + 40), L.u32(o + 44)});
            } else if (ident.compare(0, 22, "*UDF Virtual Partition") == 0) {
                ctx.out.warning = "This disc was written in sessions (VAT), which isn't fully supported";
                ps = physical(num);
            } else {
                ps = physical(num);  // sparable: spared packets are rare and read as originally placed
            }
        }
        u.parts.push_back(ps);
        o += mlen;
    }
    // Metadata partitions: the data of the metadata file (or its mirror) in the physical partition.
    for (auto& mt : metas) {
        size_t phys = SIZE_MAX;
        for (size_t i = 0; i < u.parts.size(); ++i)
            if (u.parts[i] && i != mt.ref) {
                phys = i;
                break;
            }
        if (phys == SIZE_MAX) continue;
        Udf::Entry e;
        if (u.entry((uint32_t)phys, mt.file, e) || u.entry((uint32_t)phys, mt.mirror, e)) u.parts[mt.ref] = e.data;
    }
    // The file set, then the root directory.
    uint32_t fsLbn = L.u32(248 + 4), fsPart = L.u16(248 + 8);
    std::vector<uint8_t> fsd = u.block(fsPart, fsLbn);
    Span FS(fsd);
    if (!tagOk(FS, 0, 256)) {
        ctx.err = "Damaged UDF volume (no file set)";
        return false;
    }
    uint32_t rootLbn = FS.u32(400 + 4), rootPart = FS.u16(400 + 8);
    Udf::Entry root;
    if (!u.entry(rootPart, rootLbn, root)) {
        ctx.err = "Damaged UDF volume (no root directory)";
        return false;
    }
    ctx.formats.push_back("UDF");
    Walker w{ctx, u, {}};
    w.seen.insert({rootPart, rootLbn});
    w.dir(root, "", 0);
    return true;
}

}  // namespace

const Handler hUdf = {"UDF",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          // The volume recognition sequence names NSR02/NSR03 from 32 KiB on.
                          bool nsr = false;
                          for (size_t o = 32768; o + 6 <= h.size() && o < 32768 + 16 * 2048; o += 2048)
                              if (Span(h).eq(o + 1, "NSR02", 5) || Span(h).eq(o + 1, "NSR03", 5)) nsr = true;
                          if (!nsr) return false;
                          uint32_t bs;
                          std::vector<uint8_t> a;
                          return findAnchor(s, bs, a);
                      },
                      openUdf};

}  // namespace kz::native
