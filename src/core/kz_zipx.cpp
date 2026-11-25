#include "kz_zipx.hpp"

#include <lzma.h>
#include <zlib.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "kz_inflate.hpp"
#include "native/nat.hpp"

namespace kz::zipx {

using native::StreamPtr;

namespace {

struct Ent {
    std::string name;
    uint16_t flags = 0, method = 0;
    uint32_t crc = 0, dosTime = 0, dosDate = 0;
    uint64_t csize = 0, usize = 0, local = 0;
    bool dir = false;
};

StreamPtr openSource(const Source& src) {
    if (src.inMemory()) return native::memStream(src.mem);
    return native::fileStream(src.path);
}

// The central directory. False when this isn't a ZIP (or a damaged one).
bool readDirectory(const StreamPtr& s, std::vector<Ent>& out) {
    uint64_t size = s->size();
    if (size < 22) return false;
    size_t tail = (size_t)std::min<uint64_t>(size, 65536 + 22);
    std::vector<uint8_t> t = s->get(size - tail, tail);
    if (t.size() != tail) return false;
    size_t e = SIZE_MAX;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (t[i] == 'P' && t[i + 1] == 'K' && t[i + 2] == 5 && t[i + 3] == 6) {
            e = i;
            break;
        }
    if (e == SIZE_MAX) return false;
    native::Span sp(t);
    uint64_t eocdAt = size - tail + e;
    uint64_t count = sp.u16(e + 10), cdSize = sp.u32(e + 12), cdOff = sp.u32(e + 16);
    // ZIP64: the locator sits just before the end record.
    if (e >= 20 && sp.u32(e - 20) == 0x07064b50) {
        uint64_t z64 = sp.u64(e - 20 + 8);
        std::vector<uint8_t> z = s->get(z64, 56);
        native::Span zs(z);
        if (z.size() == 56 && zs.u32(0) == 0x06064b50) {
            count = zs.u64(32);
            cdSize = zs.u64(40);
            cdOff = zs.u64(48);
            eocdAt = z64;
        }
    }
    // A prefix (self-extractor) moves everything: the directory really ends where the end record starts.
    uint64_t shift = eocdAt >= cdSize && eocdAt - cdSize > cdOff ? eocdAt - cdSize - cdOff : 0;
    if (cdSize > (512u << 20)) return false;
    std::vector<uint8_t> cd = s->get(cdOff + shift, (size_t)cdSize);
    if (cd.size() != cdSize) return false;
    native::Span c(cd);
    size_t p = 0;
    for (uint64_t i = 0; i < count && c.u32(p) == 0x02014b50; ++i) {
        Ent x;
        x.flags = (uint16_t)c.u16(p + 8);
        x.method = (uint16_t)c.u16(p + 10);
        x.dosTime = c.u16(p + 12);
        x.dosDate = c.u16(p + 14);
        x.crc = c.u32(p + 16);
        x.csize = c.u32(p + 20);
        x.usize = c.u32(p + 24);
        size_t nl = c.u16(p + 28), el = c.u16(p + 30), cl = c.u16(p + 32);
        x.local = c.u32(p + 42);
        if (!c.has(p + 46, nl + el)) return false;
        std::string raw((const char*)cd.data() + p + 46, nl);
        // ZIP64 extra field: only the fields that are 0xFFFFFFFF, in this order.
        size_t ex = p + 46 + nl, exEnd = ex + el;
        while (ex + 4 <= exEnd) {
            uint32_t id = c.u16(ex), len = c.u16(ex + 2);
            size_t q = ex + 4;
            if (id == 1) {
                if (x.usize == 0xFFFFFFFF) x.usize = c.u64(q), q += 8;
                if (x.csize == 0xFFFFFFFF) x.csize = c.u64(q), q += 8;
                if (x.local == 0xFFFFFFFF) x.local = c.u64(q), q += 8;
            } else if (id == 0x7075 && len >= 5) {  // Info-ZIP Unicode path
                raw.assign((const char*)cd.data() + ex + 9, len - 5);
                x.flags |= 0x800;
            }
            ex += 4 + len;
        }
        x.name = (x.flags & 0x800) ? raw : native::latin1((const uint8_t*)raw.data(), raw.size());
        std::replace(x.name.begin(), x.name.end(), '\\', '/');
        x.dir = !x.name.empty() && x.name.back() == '/';
        while (!x.name.empty() && x.name.back() == '/') x.name.pop_back();
        x.local += shift;
        out.push_back(std::move(x));
        p += 46 + nl + el + cl;
    }
    return !out.empty() || count == 0;
}

// Reads one entry's data and decodes it into the sink, checking the CRC.
bool readEntry(const StreamPtr& s, const Ent& x, const native::Sink& sink, std::string& err) {
    std::vector<uint8_t> lh = s->get(x.local, 30);
    native::Span l(lh);
    if (lh.size() != 30 || l.u32(0) != 0x04034b50) {
        err = "The archive is damaged (local header of " + x.name + ")";
        return false;
    }
    uint64_t at = x.local + 30 + l.u16(26) + l.u16(28), left = x.csize;
    auto src = [&](uint8_t* d, size_t n) -> size_t {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n || !s->readAt(at, d, n)) return 0;
        at += n, left -= n;
        return n;
    };
    std::vector<uint8_t> buf(1 << 16), ib(1 << 16);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t produced = 0;
    bool stop = false;
    auto give = [&](const uint8_t* p, size_t n) {
        crc = crc32(crc, p, (uInt)n);
        produced += n;
        if (!sink(p, n)) stop = true;
        return !stop;
    };
    switch (x.method) {
        case 0:
            for (size_t n; (n = src(buf.data(), buf.size())) > 0;)
                if (!give(buf.data(), n)) return true;
            break;
        case 8:
        case 9: {
            Inflater inf(src, x.method == 9);
            for (size_t n; (n = inf.read(buf.data(), buf.size())) > 0;)
                if (!give(buf.data(), n)) return true;
            if (!inf.error().empty()) {
                err = x.name + ": " + inf.error();
                return false;
            }
            break;
        }
#ifdef KZ_HAVE_BZIP2
        case 12: {
            bz_stream z{};
            if (BZ2_bzDecompressInit(&z, 0, 0) != BZ_OK) return false;
            int r = BZ_OK;
            size_t got = 0;
            while (r == BZ_OK) {
                if (z.avail_in == 0) {
                    got = src(ib.data(), ib.size());
                    if (!got) break;
                    z.next_in = (char*)ib.data();
                    z.avail_in = (unsigned)got;
                }
                z.next_out = (char*)buf.data();
                z.avail_out = (unsigned)buf.size();
                r = BZ2_bzDecompress(&z);
                size_t n = buf.size() - z.avail_out;
                if (n && !give(buf.data(), n)) break;
            }
            BZ2_bzDecompressEnd(&z);
            if (stop) return true;
            break;
        }
#endif
        case 14: {  // LZMA: 2 bytes version, 2 bytes props size, the props, then raw LZMA1
            uint8_t h[9];
            if (src(h, 4) != 4 || h[2] != 5 || src(h + 4, 5) != 5) {
                err = x.name + ": damaged LZMA header";
                return false;
            }
            lzma_filter f[2] = {{LZMA_FILTER_LZMA1, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
            if (lzma_properties_decode(&f[0], nullptr, h + 4, 5) != LZMA_OK) return false;
            lzma_stream z = LZMA_STREAM_INIT;
            bool ok = lzma_raw_decoder(&z, f) == LZMA_OK;
            std::free(f[0].options);
            lzma_ret r = LZMA_OK;
            while (ok && r == LZMA_OK && produced < x.usize) {
                if (z.avail_in == 0) {
                    size_t got = src(ib.data(), ib.size());
                    z.next_in = ib.data();
                    z.avail_in = got;
                    if (!got) break;
                }
                z.next_out = buf.data();
                z.avail_out = (size_t)std::min<uint64_t>(buf.size(), x.usize - produced);
                r = lzma_code(&z, LZMA_RUN);
                size_t n = (size_t)(std::min<uint64_t>(buf.size(), x.usize - produced) - z.avail_out);
                if (n && !give(buf.data(), n)) break;
            }
            lzma_end(&z);
            if (stop) return true;
            break;
        }
        default:
            err = x.name + ": compression method " + std::to_string(x.method) + " isn't supported";
            return false;
    }
    if (produced != x.usize || (uint32_t)crc != x.crc) {
        err = x.name + ": CRC error (the archive is damaged)";
        return false;
    }
    return true;
}

}  // namespace

bool handles(const Source& src) {
    StreamPtr s = openSource(src);
    if (!s) return false;
    uint8_t h[4];
    // A ZIP starts with a local header, or (self-extractor) a program.
    if (!s->readAt(0, h, 4) || !((h[0] == 'P' && h[1] == 'K') || (h[0] == 'M' && h[1] == 'Z'))) return false;
    std::vector<Ent> ents;
    if (!readDirectory(s, ents)) return false;
    bool d64 = false;
    for (auto& e : ents) {
        if (e.flags & 1) return false;  // encrypted: libarchive's job
        d64 |= e.method == 9;
    }
    return d64;
}

bool open(const Source& src, native::Container& out, std::string& err) {
    out = native::Container();
    StreamPtr s = openSource(src);
    std::vector<Ent> ents;
    if (!s || !readDirectory(s, ents)) {
        err = "Can't read the ZIP's directory";
        return false;
    }
    out.format = "ZIP (Deflate64)";
    for (auto& e : ents) {
        if (e.name.empty()) continue;
        int64_t mt = native::fromDos(e.dosDate, e.dosTime);
        if (e.dir) {
            out.items.push_back(native::dirItem(e.name, mt));
            continue;
        }
        native::Item it;
        it.path = e.name;
        it.size = e.usize;
        it.mtime = mt;
        Ent copy = e;
        it.read = [s, copy](const native::Sink& sink, std::string& err) { return readEntry(s, copy, sink, err); };
        out.items.push_back(std::move(it));
    }
    return true;
}

}  // namespace kz::zipx
