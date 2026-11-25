// ARJ archives: a header per file (60 EA ...), then its data, stored (method 0), LZ77 + Huffman
// (methods 1-3, the same coding, only the compressor's effort differs) or ARJ's fast coding (4).
#include <zlib.h>

#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

// Most significant bit first, from bytes.
struct MsbBits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int count = 0;
    uint32_t peek(int k) {
        while (count < k) {
            buf = buf << 8 | (pos < n ? p[pos] : 0);
            ++pos;
            count += 8;
        }
        return (buf >> (count - k)) & ((1u << k) - 1);
    }
    void skip(int k) {
        peek(k);
        count -= k;
    }
    uint32_t get(int k) {
        if (!k) return 0;
        uint32_t v = peek(k);
        skip(k);
        return v;
    }
    bool over() const { return pos > n + 4; }
};

// Canonical Huffman over code lengths (ARJ builds its tables the same way).
struct Code {
    uint16_t count[17] = {};
    std::vector<uint16_t> sorted;
    int single = -1;  // a "code" with one symbol and no bits
    bool build(const uint8_t* len, int n) {
        std::fill(std::begin(count), std::end(count), 0);
        sorted.clear();
        single = -1;
        for (int i = 0; i < n; ++i)
            if (len[i] > 16) return false;
            else if (len[i]) count[len[i]]++;
        for (int l = 1; l <= 16; ++l)
            for (int i = 0; i < n; ++i)
                if (len[i] == l) sorted.push_back((uint16_t)i);
        return true;
    }
    int decode(MsbBits& b) const {
        if (single >= 0) return single;
        uint32_t bits = b.peek(16), code = 0, first = 0, index = 0;
        for (int l = 1; l <= 16; ++l) {
            code |= (bits >> (16 - l)) & 1;
            if (code - first < count[l]) {
                b.skip(l);
                return sorted[index + code - first];
            }
            index += count[l];
            first = (first + count[l]) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const int kNC = 510, kNT = 19, kMaxNP = 32;

bool readPtLen(MsbBits& b, Code& c, uint8_t* lens, int nn, int nbit, int special) {
    int n = (int)b.get(nbit);
    std::fill(lens, lens + nn, 0);
    if (n == 0) {
        c.build(lens, nn);
        c.single = (int)b.get(nbit);
        return true;
    }
    if (n > nn) return false;
    int i = 0;
    while (i < n) {
        uint32_t v = b.peek(3);
        b.skip(3);
        if (v == 7) {
            while (b.get(1)) {
                if (++v > 16) return false;
            }
        }
        lens[i++] = (uint8_t)v;
        if (i == special) {
            int z = (int)b.get(2);
            while (z-- > 0 && i < nn) lens[i++] = 0;
        }
    }
    return c.build(lens, nn);
}

bool readCLen(MsbBits& b, Code& pt, Code& c, uint8_t* lens) {
    int n = (int)b.get(9);
    std::fill(lens, lens + kNC, 0);
    if (n == 0) {
        c.build(lens, kNC);
        c.single = (int)b.get(9);
        return true;
    }
    if (n > kNC) return false;
    int i = 0;
    while (i < n) {
        int k = pt.decode(b);
        if (k < 0) return false;
        if (k <= 2) {
            int z = k == 0 ? 1 : k == 1 ? (int)b.get(4) + 3 : (int)b.get(9) + 20;
            while (z-- > 0 && i < kNC) lens[i++] = 0;
        } else {
            lens[i++] = (uint8_t)(k - 2);
        }
    }
    return c.build(lens, kNC);
}

bool lzhStatic(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t size, int np, int pbit) {
    MsbBits b{in, n};
    Code cc, pc, tc;
    uint8_t clen[kNC], plen[kMaxNP];
    uint32_t block = 0;
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        if (block == 0) {
            block = b.get(16);
            if (!readPtLen(b, tc, plen, kNT, 5, 3) || !readCLen(b, tc, cc, clen) || !readPtLen(b, pc, plen, np, pbit, -1))
                return false;
        }
        --block;
        int c = cc.decode(b);
        if (c < 0 || b.over()) return false;
        if (c < 256) {
            out.push_back((uint8_t)c);
            continue;
        }
        uint32_t len = (uint32_t)c - 253;
        int p = pc.decode(b);
        if (p < 0) return false;
        uint32_t dist = p == 0 ? 0 : (1u << (p - 1)) + b.get(p - 1);
        dist += 1;
        if (dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (uint32_t k = 0; k < len && out.size() < size; ++k) out.push_back(out[from + k]);
    }
    return true;
}

// Method 4: unary-prefixed lengths and positions.
bool unArjFast(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t size) {
    MsbBits b{in, n};
    auto value = [&](int start, int stop) {
        uint32_t plus = 0, pwr = 1u << start;
        int width = start;
        for (; width < stop; ++width) {
            if (!b.get(1)) break;
            plus += pwr;
            pwr <<= 1;
        }
        return b.get(width) + plus;
    };
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        uint32_t c = value(0, 7);
        if (b.over()) return false;
        if (c == 0) {
            out.push_back((uint8_t)b.get(8));
            continue;
        }
        uint32_t len = c - 1 + 3;
        uint32_t dist = value(9, 13) + 1;
        if (dist > out.size()) return false;
        size_t from = out.size() - dist;
        for (uint32_t k = 0; k < len && out.size() < size; ++k) out.push_back(out[from + k]);
    }
    return true;
}

struct ArjHeader {
    std::vector<uint8_t> h;
    uint64_t next = 0;  // where the data (or next header) starts
};

// The basic header at `at` (60 EA, size, header, CRC, then extended headers). False at the end marker.
bool readHeader(Stream& s, uint64_t at, ArjHeader& out, bool& end) {
    end = false;
    std::vector<uint8_t> a = s.getUpTo(at, 4);
    if (a.size() < 4 || a[0] != 0x60 || a[1] != 0xEA) return false;
    uint32_t size = a[2] | a[3] << 8;
    if (size == 0) {
        end = true;
        return true;
    }
    if (size > 2600) return false;
    out.h = s.get(at + 4, size);
    std::vector<uint8_t> crc = s.get(at + 4 + size, 4);
    if (out.h.size() != size || crc.size() != 4) return false;
    if (crc32(0, out.h.data(), size) != Span(crc).u32(0)) return false;
    uint64_t p = at + 4 + size + 4;
    for (int guard = 0; guard < 64; ++guard) {  // extended headers: size, data, CRC; size 0 ends them
        std::vector<uint8_t> e = s.getUpTo(p, 2);
        if (e.size() < 2) return false;
        uint32_t el = e[0] | e[1] << 8;
        p += 2;
        if (!el) break;
        p += el + 4;
    }
    out.next = p;
    return true;
}

uint64_t findArj(Stream& s) {
    ArjHeader h;
    bool end;
    if (readHeader(s, 0, h, end) && !end) return 0;
    // Self-extracting programs: the archive follows the program.
    std::vector<uint8_t> head = s.getUpTo(0, 2);
    if (!Span(head).eq(0, "MZ", 2)) return UINT64_MAX;
    std::vector<uint8_t> buf = s.getUpTo(0, (size_t)std::min<uint64_t>(s.size(), 1u << 20));
    for (size_t i = 2; i + 4 < buf.size(); ++i)
        if (buf[i] == 0x60 && buf[i + 1] == 0xEA && readHeader(s, i, h, end) && !end && h.h.size() > 6 && h.h[6] == 2)
            return i;
    return UINT64_MAX;
}

bool openArj(Ctx& ctx, StreamPtr s) {
    uint64_t at = findArj(*s);
    if (at == UINT64_MAX) return false;
    ArjHeader main;
    bool end;
    if (!readHeader(*s, at, main, end) || end) return false;
    at = main.next;
    ctx.formats.push_back("ARJ");
    for (int guard = 0; guard < 1000000; ++guard) {
        ArjHeader fh;
        if (!readHeader(*s, at, fh, end)) {
            ctx.out.warning = "The archive ends early or is damaged";
            break;
        }
        if (end) break;
        Span H(fh.h);
        uint32_t first = H.u8(0), flags = H.u8(4), method = H.u8(5), type = H.u8(6);
        uint32_t dos = H.u32(8), csize = H.u32(12), osize = H.u32(16), crc = H.u32(20);
        std::string name = fh.h.size() > first ? cstr(fh.h.data() + first, fh.h.size() - first) : "";
        uint64_t data = fh.next;
        at = data + csize;
        std::replace(name.begin(), name.end(), '\\', '/');
        std::string path, part;
        for (size_t i = 0; i <= name.size(); ++i) {
            if (i < name.size() && name[i] != '/') {
                part += name[i];
                continue;
            }
            if (!part.empty() && part != "." && part != ".." && !(part.size() == 2 && part[1] == ':'))
                path += (path.empty() ? "" : "/") + component(part);
            part.clear();
        }
        if (path.empty()) continue;
        int64_t mt = fromDos(dos >> 16, dos & 0xFFFF);
        if (type == 3) {
            ctx.out.items.push_back(dirItem(ctx.prefix + path, mt));
            continue;
        }
        if (type != 0 && type != 1) continue;  // volume labels and such
        if (flags & 0x04) ctx.out.warning = "This archive continues on another volume, which isn't read";
        Item it;
        it.path = ctx.prefix + path;
        it.size = osize;
        it.mtime = mt;
        if (flags & 0x01) {
            it.read = [](const Sink&, std::string& err) {
                err = "This file is password-protected (ARJ garbling), which isn't supported";
                return false;
            };
        } else if (method == 0) {
            if (data + csize > s->size()) continue;
            it = rangeItem(ctx.prefix + path, s, data, csize, mt);
        } else {
            it.read = [s, data, csize, osize, method, crc](const Sink& sink, std::string& err) {
                std::vector<uint8_t> in = s->get(data, csize), out;
                if (in.size() != csize) {
                    err = "The archive ends early";
                    return false;
                }
                bool ok = method <= 3 ? lzhStatic(in.data(), in.size(), out, osize, 17, 5) : method == 4 && unArjFast(in.data(), in.size(), out, osize);
                if (!ok) {
                    err = method > 4 ? "Unknown ARJ method" : "Damaged compressed data";
                    return false;
                }
                if (crc32(0, out.data(), (uInt)out.size()) != crc) {
                    err = "CRC error (the file is damaged)";
                    return false;
                }
                sink(out.data(), out.size());
                return true;
            };
        }
        ctx.out.items.push_back(std::move(it));
    }
    return true;
}

}  // namespace

bool unLzhStatic(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t size, int np, int pbit) {
    return np > 0 && np <= kMaxNP && lzhStatic(in, n, out, size, np, pbit);
}

const Handler hArj = {"ARJ",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          if (h.size() >= 2 && h[0] == 0x60 && h[1] == 0xEA) return findArj(s) == 0;
                          return false;  // self-extracting ARJ programs are found after the PE checks fail
                      },
                      openArj};

}  // namespace kz::native
