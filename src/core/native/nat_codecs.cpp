// Decompressors for the native formats: zlib/deflate, bzip2, xz/LZMA and Zstandard through the
// libraries Kompressor already links, and small ones written here: LZ4 blocks (SquashFS), LZO1X
// (SquashFS), LZNT1 (NTFS) and ADC (DMG).
#include <zlib.h>
#define LZMA_API_STATIC
#include <lzma.h>
#include <zstd.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include "nat.hpp"

namespace kz::native {

static bool inflateWith(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect, int bits) {
    z_stream z{};
    if (inflateInit2(&z, bits) != Z_OK) return false;
    out.resize(expect ? expect : std::max<size_t>(n * 4, 4096));
    z.next_in = const_cast<Bytef*>(in);
    z.avail_in = (uInt)n;
    size_t done = 0;
    int r = Z_OK;
    while (true) {
        if (done == out.size()) {
            if (expect) break;
            out.resize(out.size() * 2);
        }
        z.next_out = out.data() + done;
        z.avail_out = (uInt)(out.size() - done);
        r = inflate(&z, Z_NO_FLUSH);
        done = out.size() - z.avail_out;
        if (r == Z_STREAM_END || r != Z_OK) break;
        if (z.avail_in == 0 && z.avail_out != 0) break;
    }
    inflateEnd(&z);
    out.resize(done);
    return r == Z_STREAM_END || (r == Z_OK && done == expect) || (r == Z_BUF_ERROR && done > 0);
}

bool inflateZlib(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    return inflateWith(in, n, out, expect, 15);
}
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    return inflateWith(in, n, out, expect, -15);
}

bool unBzip2(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
#ifdef KZ_HAVE_BZIP2
    bz_stream b{};
    if (BZ2_bzDecompressInit(&b, 0, 0) != BZ_OK) return false;
    out.resize(expect ? expect : std::max<size_t>(n * 6, 4096));
    b.next_in = (char*)in;
    b.avail_in = (unsigned)n;
    size_t done = 0;
    int r = BZ_OK;
    while (true) {
        if (done == out.size()) {
            if (expect) break;
            out.resize(out.size() * 2);
        }
        b.next_out = (char*)out.data() + done;
        b.avail_out = (unsigned)(out.size() - done);
        r = BZ2_bzDecompress(&b);
        done = out.size() - b.avail_out;
        if (r != BZ_OK) break;
        if (b.avail_in == 0 && b.avail_out != 0) break;
    }
    BZ2_bzDecompressEnd(&b);
    out.resize(done);
    return r == BZ_STREAM_END || (expect && done == expect);
#else
    (void)in, (void)n, (void)out, (void)expect;
    return false;
#endif
}

static bool runLzma(lzma_stream& s, const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.resize(expect ? expect : std::max<size_t>(n * 4, 4096));
    s.next_in = in;
    s.avail_in = n;
    size_t done = 0;
    lzma_ret r = LZMA_OK;
    while (true) {
        if (done == out.size()) {
            if (expect) break;
            out.resize(out.size() * 2);
        }
        s.next_out = out.data() + done;
        s.avail_out = out.size() - done;
        r = lzma_code(&s, LZMA_FINISH);
        done = out.size() - s.avail_out;
        if (r != LZMA_OK) break;
    }
    lzma_end(&s);
    out.resize(done);
    return r == LZMA_STREAM_END || (expect && done == expect) || (r == LZMA_BUF_ERROR && done > 0);
}

bool unXz(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_auto_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return false;
    return runLzma(s, in, n, out, expect);
}

bool unLzmaRaw(const uint8_t* props, const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    lzma_filter f[2] = {{LZMA_FILTER_LZMA1, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
    if (lzma_properties_decode(&f[0], nullptr, props, 5) != LZMA_OK) return false;
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r = lzma_raw_decoder(&s, f);
    free(f[0].options);
    if (r != LZMA_OK) return false;
    return runLzma(s, in, n, out, expect);
}

bool unZstd(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    ZSTD_DStream* d = ZSTD_createDStream();
    if (!d) return false;
    out.resize(expect ? expect : std::max<size_t>(n * 4, 4096));
    ZSTD_inBuffer ib{in, n, 0};
    size_t done = 0, r = 0;
    while (true) {
        if (done == out.size()) {
            if (expect) break;
            out.resize(out.size() * 2);
        }
        ZSTD_outBuffer ob{out.data() + done, out.size() - done, 0};
        r = ZSTD_decompressStream(d, &ob, &ib);
        done += ob.pos;
        if (ZSTD_isError(r) || r == 0) break;
        if (ib.pos == ib.size && ob.pos < ob.size) break;
    }
    ZSTD_freeDStream(d);
    out.resize(done);
    return !ZSTD_isError(r) && (r == 0 || (expect && done == expect));
}

bool unLz4Block(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    while (i < n) {
        uint32_t token = in[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint32_t b;
            do {
                if (i >= n) return false;
                b = in[i++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - i) return false;
        out.insert(out.end(), in + i, in + i + lit);
        i += lit;
        if (i >= n) break;  // the last sequence has literals only
        if (n - i < 2) return false;
        size_t off = in[i] | in[i + 1] << 8;
        i += 2;
        size_t len = token & 15;
        if (len == 15) {
            uint32_t b;
            do {
                if (i >= n) return false;
                b = in[i++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (!off || off > out.size()) return false;
        size_t from = out.size() - off;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
        if (expect && out.size() > expect) return false;
    }
    return true;
}

// LZO1X, decoded from the format's description (each instruction's meaning depends on how many
// literals the one before it copied).
bool unLzo1x(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    auto lits = [&](size_t len) {
        if (len > n - i) return false;
        out.insert(out.end(), in + i, in + i + len);
        i += len;
        return true;
    };
    auto copy = [&](size_t dist, size_t len) {
        if (!dist || dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
        return true;
    };
    auto extLen = [&](size_t base) -> size_t {  // a length of 0 means: zero bytes (255 each), then one more
        size_t len = 0;
        while (i < n && in[i] == 0) len += 255, ++i;
        if (i >= n) return SIZE_MAX;
        return len + base + in[i++];
    };
    int state = 0;
    if (n && in[0] > 17) {
        uint32_t t = in[i++] - 17;
        if (!lits(t)) return false;
        state = t < 4 ? (int)t : 4;
    }
    while (i < n) {
        uint32_t t = in[i++];
        size_t len, dist;
        int next;
        if (t < 16) {
            if (state == 0) {  // a long run of literals
                len = t ? t + 3 : extLen(15) + 3;
                if (len == SIZE_MAX || !lits(len)) return false;
                state = 4;
                continue;
            }
            if (i >= n) return false;
            next = t & 3;
            if (state < 4) dist = ((size_t)in[i++] << 2) + ((t >> 2) & 3) + 1, len = 2;
            else dist = ((size_t)in[i++] << 2) + ((t >> 2) & 3) + 2049, len = 3;
        } else if (t >= 64) {
            if (i >= n) return false;
            next = t & 3;
            dist = ((size_t)in[i++] << 3) + ((t >> 2) & 7) + 1;
            len = t >= 128 ? 5 + ((t >> 5) & 3) : 3 + ((t >> 5) & 1);
        } else if (t >= 32) {
            len = (t & 31) ? (t & 31) + 2 : extLen(31) + 2;
            if (len == SIZE_MAX || n - i < 2) return false;
            uint32_t d = in[i] | in[i + 1] << 8;
            i += 2;
            dist = (d >> 2) + 1;
            next = d & 3;
        } else {  // 16..31
            len = (t & 7) ? (t & 7) + 2 : extLen(7) + 2;
            if (len == SIZE_MAX || n - i < 2) return false;
            uint32_t d = in[i] | in[i + 1] << 8;
            i += 2;
            dist = 16384 + ((size_t)(t & 8) << 11) + (d >> 2);
            next = d & 3;
            if (dist == 16384) return true;  // end of stream
        }
        if (!copy(dist, len)) return false;
        state = next;
        if (next && !lits((size_t)next)) return false;
        if (expect && out.size() > expect + 3) return false;
    }
    return true;
}

// LZNT1 (NTFS compression): 4 KiB chunks, each stored or LZ77-compressed with position-dependent
// split between length and distance bits.
bool unLznt1(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    while (i + 2 <= n && (!expect || out.size() < expect)) {
        uint32_t h = in[i] | in[i + 1] << 8;
        if (h == 0) break;
        size_t len = (h & 0x0FFF) + 1;
        i += 2;
        if (len > n - i) return false;
        const uint8_t* c = in + i;
        i += len;
        if (!(h & 0x8000)) {
            out.insert(out.end(), c, c + len);
            continue;
        }
        size_t chunkStart = out.size(), k = 0;
        while (k < len) {
            uint32_t flags = c[k++];
            for (int b = 0; b < 8 && k < len; ++b, flags >>= 1) {
                if (!(flags & 1)) {
                    out.push_back(c[k++]);
                    continue;
                }
                if (k + 2 > len) return false;
                uint32_t tok = c[k] | c[k + 1] << 8;
                k += 2;
                size_t pos = out.size() - chunkStart;
                uint32_t lenBits = 12;
                for (size_t p = pos - 1; p >= 0x10; p >>= 1) lenBits--;
                size_t mlen = (tok & ((1u << lenBits) - 1)) + 3, dist = (tok >> lenBits) + 1;
                if (dist > pos) return false;
                size_t from = out.size() - dist;
                for (size_t m = 0; m < mlen; ++m) out.push_back(out[from + m]);
            }
        }
    }
    return true;
}

// ADC (Apple Data Compression, old DMGs).
bool unAdc(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    while (i < n && (!expect || out.size() < expect)) {
        uint32_t b = in[i++];
        if (b & 0x80) {
            size_t len = (b & 0x7F) + 1;
            if (len > n - i) return false;
            out.insert(out.end(), in + i, in + i + len);
            i += len;
            continue;
        }
        size_t len, off;
        if (b & 0x40) {
            if (n - i < 2) return false;
            len = (b & 0x3F) + 4;
            off = (size_t)in[i] << 8 | in[i + 1];
            i += 2;
        } else {
            if (i >= n) return false;
            len = ((b >> 2) & 0xF) + 3;
            off = (size_t)(b & 3) << 8 | in[i++];
        }
        if (off + 1 > out.size()) return false;
        size_t from = out.size() - off - 1;
        for (size_t k = 0; k < len; ++k) out.push_back(out[from + k]);
    }
    return true;
}

}  // namespace kz::native
