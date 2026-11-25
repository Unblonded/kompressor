#include "kz_repair.hpp"

#include <lzma.h>
#include <zlib.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

#include "kz_archive.hpp"
#include "kz_inflate.hpp"
#include "native/nat.hpp"

namespace kz {

namespace {

using native::StreamPtr;

constexpr uint64_t kNone = UINT64_MAX;

// ------------------------------------------------------------------------------------- ZIP rebuild
struct ZEnt {
    std::string name;            // raw bytes, as stored
    uint16_t ver = 20, flags = 0, method = 0, time = 0, date = 0;
    uint32_t crc = 0;
    uint64_t csize = 0, usize = 0, dataAt = 0;
    std::vector<uint8_t> extra;  // the local extra fields, less ZIP64's (rebuilt when needed)
    bool dir = false;
};

struct Scanner {
    StreamPtr s;
    uint64_t size;
    std::vector<uint8_t> buf;
    uint64_t bufAt = kNone;

    // Next "PK" + a, b at or after `from`, or kNone.
    uint64_t find(uint64_t from, uint8_t a, uint8_t b, uint64_t limit = kNone) {
        const size_t chunk = 4 << 20;
        limit = std::min(limit, size);
        for (uint64_t at = from; at + 4 <= limit;) {
            size_t n = (size_t)std::min<uint64_t>(chunk, limit - at);
            if (bufAt != at || buf.size() < n) {
                buf = s->getUpTo(at, n);
                bufAt = at;
            }
            if (buf.size() < 4) return kNone;
            for (size_t i = 0; i + 4 <= buf.size(); ++i)
                if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == a && buf[i + 3] == b) return at + i;
            if (buf.size() < n || at + n >= limit) return kNone;
            at += n - 3;
        }
        return kNone;
    }
};

// Decodes [at, at + len) (or, with len kNone, a Deflate stream to its end) and works out its CRC and
// size. `measured` gets the compressed length when len was kNone. False when it can't be decoded.
bool decodeCheck(const StreamPtr& s, uint64_t at, uint64_t len, uint16_t method, uint64_t expectSize, uint32_t& crc,
                 uint64_t& size, uint64_t& measured) {
    uint64_t left = len == kNone ? s->size() - at : len, pos = at;
    auto src = [&](uint8_t* d, size_t n) -> size_t {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n || !s->readAt(pos, d, n)) return 0;
        pos += n, left -= n;
        return n;
    };
    std::vector<uint8_t> buf(1 << 16), ib(1 << 16);
    uLong c = crc32(0, nullptr, 0);
    size = 0;
    auto take = [&](const uint8_t* p, size_t n) {
        c = crc32(c, p, (uInt)n);
        size += n;
    };
    bool ok = false;
    if (method == 0) {
        if (len == kNone) return false;
        for (size_t n; (n = src(buf.data(), buf.size())) > 0;) take(buf.data(), n);
        ok = pos == at + len;
    } else if (method == 8 || method == 9) {
        Inflater inf(src, method == 9);
        for (size_t n; (n = inf.read(buf.data(), buf.size())) > 0;) take(buf.data(), n);
        ok = inf.error().empty() && inf.done();
        measured = inf.consumed();
        if (len != kNone && measured != len) ok = false;
#ifdef KZ_HAVE_BZIP2
    } else if (method == 12) {
        if (len == kNone) return false;
        bz_stream z{};
        if (BZ2_bzDecompressInit(&z, 0, 0) != BZ_OK) return false;
        int r = BZ_OK;
        while (r == BZ_OK) {
            if (z.avail_in == 0) {
                size_t got = src(ib.data(), ib.size());
                if (!got) break;
                z.next_in = (char*)ib.data();
                z.avail_in = (unsigned)got;
            }
            z.next_out = (char*)buf.data();
            z.avail_out = (unsigned)buf.size();
            r = BZ2_bzDecompress(&z);
            take(buf.data(), buf.size() - z.avail_out);
        }
        BZ2_bzDecompressEnd(&z);
        ok = r == BZ_STREAM_END;
#endif
    } else if (method == 14) {
        if (len == kNone || expectSize == kNone) return false;
        uint8_t h[9];
        if (src(h, 4) != 4 || h[2] != 5 || src(h + 4, 5) != 5) return false;
        lzma_filter f[2] = {{LZMA_FILTER_LZMA1, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
        if (lzma_properties_decode(&f[0], nullptr, h + 4, 5) != LZMA_OK) return false;
        lzma_stream z = LZMA_STREAM_INIT;
        bool started = lzma_raw_decoder(&z, f) == LZMA_OK;
        std::free(f[0].options);
        lzma_ret r = LZMA_OK;
        while (started && (r == LZMA_OK) && size < expectSize) {
            if (z.avail_in == 0) {
                size_t got = src(ib.data(), ib.size());
                if (!got) break;
                z.next_in = ib.data();
                z.avail_in = got;
            }
            size_t want = (size_t)std::min<uint64_t>(buf.size(), expectSize - size);
            z.next_out = buf.data();
            z.avail_out = want;
            r = lzma_code(&z, LZMA_RUN);
            take(buf.data(), want - z.avail_out);
        }
        lzma_end(&z);
        ok = started && size == expectSize;
    } else {
        return false;
    }
    crc = (uint32_t)c;
    return ok;
}

bool decodable(uint16_t m) {
    return m == 0 || m == 8 || m == 9 || m == 14
#ifdef KZ_HAVE_BZIP2
           || m == 12
#endif
        ;
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)x);
    v.push_back((uint8_t)(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, (uint32_t)x);
    put32(v, (uint32_t)(x >> 32));
}

bool rebuildZip(const StreamPtr& s, const std::string& outPath, Progress& prog, RepairResult& res, std::string& err) {
    Scanner sc{s, s->size()};
    prog.total = sc.size;
    std::vector<ZEnt> ents;
    std::map<std::string, size_t> byName;
    std::vector<std::string> lost;
    uint64_t pos = 0;
    while (!prog.cancelled()) {
        uint64_t o = sc.find(pos, 3, 4);
        if (o == kNone) break;
        prog.done = o;
        pos = o + 4;
        std::vector<uint8_t> lh = s->get(o, 30);
        native::Span l(lh);
        if (lh.size() != 30) break;
        ZEnt e;
        e.ver = (uint16_t)l.u16(4);
        e.flags = (uint16_t)l.u16(6);
        e.method = (uint16_t)l.u16(8);
        e.time = (uint16_t)l.u16(10);
        e.date = (uint16_t)l.u16(12);
        e.crc = l.u32(14);
        uint64_t csize = l.u32(18), usize = l.u32(22);
        size_t nl = l.u16(26), el = l.u16(28);
        static const uint16_t known[] = {0, 1, 6, 8, 9, 12, 14, 19, 93, 95, 96, 97, 98, 99};
        if (nl == 0 || nl > 4096 || std::find(std::begin(known), std::end(known), e.method) == std::end(known) ||
            o + 30 + nl + el > sc.size)
            continue;
        std::vector<uint8_t> ne = s->get(o + 30, nl + el);
        e.name.assign((const char*)ne.data(), nl);
        native::Span x(ne);
        bool zip64 = false;
        for (size_t q = nl; q + 4 <= ne.size();) {
            uint32_t id = x.u16(q), len = x.u16(q + 2);
            if (q + 4 + len > ne.size()) break;
            if (id == 1) {
                size_t r = q + 4;
                zip64 = true;
                if (usize == 0xFFFFFFFF && len >= r - q - 4 + 8) usize = x.u64(r), r += 8;
                if (csize == 0xFFFFFFFF && len >= r - q - 4 + 8) csize = x.u64(r), r += 8;
            } else {
                e.extra.insert(e.extra.end(), ne.begin() + q, ne.begin() + q + 4 + len);
            }
            q += 4 + len;
        }
        e.dataAt = o + 30 + nl + el;
        e.dir = e.name.back() == '/';
        bool enc = (e.flags & 1) || e.method == 99;
        bool desc = (e.flags & 8) != 0;
        uint64_t len = kNone, descLen = 0;
        uint32_t gotCrc = 0;
        uint64_t gotSize = 0, measured = 0;
        bool checked = false;

        if (!desc || (csize != 0 && csize != 0xFFFFFFFF)) {
            if (e.dataAt + csize <= sc.size) len = csize;
        } else if (!enc && (e.method == 8 || e.method == 9)) {
            // Sizes only after the data: decoding the stream finds where it ends.
            if (decodeCheck(s, e.dataAt, kNone, e.method, kNone, gotCrc, gotSize, measured)) {
                len = measured;
                checked = true;
            }
        } else {
            // Stored / unknown with a trailing descriptor: look for one whose size fits.
            for (uint64_t q = e.dataAt; (q = sc.find(q, 7, 8)) != kNone; ++q) {
                std::vector<uint8_t> d = s->get(q, 24);
                native::Span ds(d);
                if (d.size() >= 16 && ds.u32(8) == q - e.dataAt) {
                    len = q - e.dataAt;
                    break;
                }
                if (d.size() >= 24 && ds.u64(8) == q - e.dataAt) {
                    len = q - e.dataAt;
                    break;
                }
            }
        }
        if (len == kNone) {
            if (!e.dir) lost.push_back(e.name);
            continue;
        }
        if (desc) {
            std::vector<uint8_t> d = s->get(e.dataAt + len, 24);
            native::Span ds(d);
            size_t b = d.size() >= 4 && ds.u32(0) == 0x08074b50 ? 4 : 0;
            e.crc = ds.u32(b);
            if (zip64) {
                csize = ds.u64(b + 4), usize = ds.u64(b + 12);
                descLen = b + 20;
            } else {
                csize = ds.u32(b + 4), usize = ds.u32(b + 8);
                descLen = b + 12;
            }
            if (csize != len) csize = len;
        }
        e.csize = len;
        e.usize = usize;
        bool good;
        if (enc || !decodable(e.method)) {
            good = true;  // can't be checked: kept as found
            if (!e.dir) res.unchecked++;
        } else {
            if (!checked) checked = decodeCheck(s, e.dataAt, len, e.method, usize, gotCrc, gotSize, measured);
            good = checked && gotCrc == e.crc && (desc || gotSize == usize);
            if (good) e.usize = gotSize;
        }
        if (!good) {
            if (!e.dir) lost.push_back(e.name);
            continue;
        }
        e.flags &= (uint16_t)~8;  // sizes go in the header now
        pos = e.dataAt + len + descLen;
        auto it = byName.find(e.name);
        if (it != byName.end()) ents[it->second] = std::move(e);  // a later copy (an update) wins
        else {
            byName[e.name] = ents.size();
            ents.push_back(std::move(e));
        }
    }
    if (prog.cancelled()) {
        err = "Cancelled";
        return false;
    }
    // Lost names that turned up intact elsewhere aren't lost.
    for (auto& n : lost)
        if (!byName.count(n) && std::find(res.lostNames.begin(), res.lostNames.end(), n) == res.lostNames.end())
            res.lostNames.push_back(n);
    if (ents.empty()) {
        err = "Nothing in this file could be recovered: no intact ZIP entries were found.";
        return false;
    }

    std::string tmp = outPath + ".kztmp";
    std::ofstream out(u8path(tmp), std::ios::binary | std::ios::trunc);
    if (!out) {
        err = "Can't write " + outPath;
        return false;
    }
    std::vector<uint8_t> central;
    uint64_t at = 0;
    std::vector<uint8_t> buf(1 << 20);
    prog.setStatus("Writing the repaired archive");
    for (auto& e : ents) {
        bool big = e.csize >= 0xFFFFFFFF || e.usize >= 0xFFFFFFFF;
        std::vector<uint8_t> h;
        put32(h, 0x04034b50);
        put16(h, big ? 45 : std::max<uint16_t>(e.ver, 20));
        put16(h, e.flags);
        put16(h, e.method);
        put16(h, e.time);
        put16(h, e.date);
        put32(h, e.crc);
        put32(h, big ? 0xFFFFFFFF : (uint32_t)e.csize);
        put32(h, big ? 0xFFFFFFFF : (uint32_t)e.usize);
        put16(h, (uint32_t)e.name.size());
        std::vector<uint8_t> ex = e.extra;
        if (big) {
            put16(ex, 1);
            put16(ex, 16);
            put64(ex, e.usize);
            put64(ex, e.csize);
        }
        put16(h, (uint32_t)ex.size());
        h.insert(h.end(), e.name.begin(), e.name.end());
        h.insert(h.end(), ex.begin(), ex.end());
        out.write((const char*)h.data(), (std::streamsize)h.size());
        for (uint64_t done = 0; done < e.csize;) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), e.csize - done);
            if (!s->readAt(e.dataAt + done, buf.data(), n)) {
                err = "Read error while copying " + e.name;
                return false;
            }
            out.write((const char*)buf.data(), (std::streamsize)n);
            done += n;
        }
        // Central record.
        bool farOff = at >= 0xFFFFFFFF;
        std::vector<uint8_t> cx = e.extra;
        if (big || farOff) {
            put16(cx, 1);
            put16(cx, (uint32_t)((big ? 16 : 0) + (farOff ? 8 : 0)));
            if (big) put64(cx, e.usize), put64(cx, e.csize);
            if (farOff) put64(cx, at);
        }
        put32(central, 0x02014b50);
        put16(central, 63);  // made by: 6.3, MS-DOS attributes
        put16(central, (big || farOff) ? 45 : std::max<uint16_t>(e.ver, 20));
        put16(central, e.flags);
        put16(central, e.method);
        put16(central, e.time);
        put16(central, e.date);
        put32(central, e.crc);
        put32(central, big ? 0xFFFFFFFF : (uint32_t)e.csize);
        put32(central, big ? 0xFFFFFFFF : (uint32_t)e.usize);
        put16(central, (uint32_t)e.name.size());
        put16(central, (uint32_t)cx.size());
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, e.dir ? 0x10 : 0x20);
        put32(central, farOff ? 0xFFFFFFFF : (uint32_t)at);
        central.insert(central.end(), e.name.begin(), e.name.end());
        central.insert(central.end(), cx.begin(), cx.end());
        at += h.size() + e.csize;
        if (!e.dir) res.recovered++;
    }
    uint64_t cdAt = at, cdSize = central.size(), n = ents.size();
    out.write((const char*)central.data(), (std::streamsize)central.size());
    std::vector<uint8_t> end;
    if (n >= 0xFFFF || cdAt >= 0xFFFFFFFF || cdSize >= 0xFFFFFFFF) {
        uint64_t z64At = cdAt + cdSize;
        put32(end, 0x06064b50);
        put64(end, 44);
        put16(end, 45);
        put16(end, 45);
        put32(end, 0);
        put32(end, 0);
        put64(end, n);
        put64(end, n);
        put64(end, cdSize);
        put64(end, cdAt);
        put32(end, 0x07064b50);
        put32(end, 0);
        put64(end, z64At);
        put32(end, 1);
    }
    put32(end, 0x06054b50);
    put16(end, 0);
    put16(end, 0);
    put16(end, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
    put16(end, (uint32_t)std::min<uint64_t>(n, 0xFFFF));
    put32(end, (uint32_t)std::min<uint64_t>(cdSize, 0xFFFFFFFF));
    put32(end, (uint32_t)std::min<uint64_t>(cdAt, 0xFFFFFFFF));
    put16(end, 0);
    out.write((const char*)end.data(), (std::streamsize)end.size());
    out.close();
    std::error_code ec;
    if (!out) {
        fs::remove(u8path(tmp), ec);
        err = "Writing the repaired archive failed (disk full?)";
        return false;
    }
    fs::remove(u8path(outPath), ec);
    fs::rename(u8path(tmp), u8path(outPath), ec);
    if (ec) {
        err = "Can't move the repaired archive into place: " + ec.message();
        return false;
    }
    res.lost = res.lostNames.size();
    res.rebuilt = true;
    res.output = outPath;
    return true;
}

// ------------------------------------------------------------------------------------- salvage
bool safeRel(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos) return false;
    size_t s = 0;
    while (s <= p.size()) {
        size_t e = p.find('/', s);
        if (e == std::string::npos) e = p.size();
        if (p.compare(s, e - s, "..") == 0 && e - s == 2) return false;
        s = e + 1;
    }
    return true;
}

bool salvage(const Source& src, const std::string& outDir, const std::string& password, Progress& prog,
             RepairResult& res, std::string& err) {
    fs::path dest = u8path(outDir);
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::ofstream cur;
    fs::path curPath, partPath;
    std::string curName;
    EntryVisitor v;
    v.begin = [&](const std::string& path, uint64_t) {
        if (!safeRel(path)) return false;
        curName = path;
        curPath = dest / u8path(path);
        partPath = curPath;
        partPath += ".kzpart";
        fs::create_directories(curPath.parent_path(), ec);
        cur.open(partPath, std::ios::binary | std::ios::trunc);
        return (bool)cur;
    };
    v.data = [&](const uint8_t* p, size_t n) {
        cur.write((const char*)p, (std::streamsize)n);
        return (bool)cur;
    };
    v.end = [&](bool ok, const std::string&) {
        cur.close();
        if (ok && cur) {
            fs::remove(curPath, ec);
            fs::rename(partPath, curPath, ec);
            res.recovered++;
        } else {
            fs::remove(partPath, ec);
            res.lostNames.push_back(curName);
        }
    };
    std::string verr;
    bool ok = visitEntries(src, password, v, prog, verr);
    if (cur.is_open()) {  // stopped inside a file
        cur.close();
        fs::remove(partPath, ec);
        if (!curName.empty()) res.lostNames.push_back(curName);
    }
    if (prog.cancelled()) {
        err = "Cancelled";
        return false;
    }
    res.lost = res.lostNames.size();
    res.output = outDir;
    if (res.recovered == 0) {
        fs::remove(dest, ec);  // only if empty
        err = "Nothing could be recovered" + (verr.empty() ? std::string(".") : ": " + verr);
        if (prog.needPassword) err = verr;
        return false;
    }
    if (!ok && !verr.empty()) res.summary = "the rest couldn't be read (" + verr + ")";
    return true;
}

bool looksLikeZip(const StreamPtr& s, const std::string& name) {
    uint8_t h[4] = {};
    if (s->readAt(0, h, 4) && h[0] == 'P' && h[1] == 'K' && h[2] == 3 && h[3] == 4) return true;
    std::string e = lowerExt(name);
    static const char* zipLike[] = {"zip", "zipx", "jar", "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub", "apk", "cbz"};
    for (auto z : zipLike)
        if (e == z) return true;
    return false;
}

}  // namespace

bool repairArchive(const Source& src, const std::string& outPath, const std::string& password, Progress& prog,
                   RepairResult& res, std::string& err) {
    res = RepairResult();
    StreamPtr s = src.inMemory() ? native::memStream(src.mem) : native::fileStream(src.path);
    if (!s) {
        err = "Can't open " + src.name();
        return false;
    }
    std::string dir = src.inMemory() ? pathToUtf8(fs::temp_directory_path()) : pathToUtf8(u8path(src.path).parent_path());
    std::string base = stripArchiveExt(fileNameOf(src.name()));
    bool zip = looksLikeZip(s, src.name());
    bool ok;
    if (zip) {
        std::string ext = lowerExt(src.name());
        if (ext.empty() || ext == "001") ext = "zip";
        std::string out = outPath.empty() ? pathToUtf8(u8path(dir) / u8path(base + " (repaired)." + ext)) : outPath;
        prog.setStatus("Scanning for intact entries");
        ok = rebuildZip(s, out, prog, res, err);
    } else {
        // A 7z keeps its whole directory (names, methods, sizes) at the end: cut that off and nothing
        // in it can be decoded, so say that rather than libarchive's "Seek error".
        uint8_t sh[32];
        if (s->readAt(0, sh, 32) && std::memcmp(sh, "7z\xBC\xAF\x27\x1C", 6) == 0) {
            native::Span sp(sh, 32);
            if (32 + sp.u64(12) + sp.u64(20) > s->size()) {
                err = "This 7z is cut short (it is " + humanSize(s->size()) + " of " + humanSize(32 + sp.u64(12) + sp.u64(20)) +
                      "), and a 7z keeps the list of its files at the very end, so nothing in it can be recovered. "
                      "Download or copy it again.";
                return false;
            }
        }
        std::string out = outPath.empty() ? pathToUtf8(u8path(dir) / u8path(base + " (recovered)")) : outPath;
        prog.setStatus("Extracting everything that reads back intact");
        ok = salvage(src, out, password, prog, res, err);
    }
    if (!ok) return false;
    std::string s1 = std::to_string(res.recovered) + " file" + (res.recovered == 1 ? "" : "s") + " recovered";
    if (res.lost) s1 += ", " + std::to_string(res.lost) + " lost";
    if (res.unchecked) s1 += " (" + std::to_string(res.unchecked) + " couldn't be checked: encrypted or an unusual method)";
    if (!res.summary.empty()) s1 += "; " + res.summary;
    res.summary = s1;
    return true;
}

}  // namespace kz
