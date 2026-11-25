// Inno Setup installers: the files a setup.exe would install. The loader's offset table (an RCDATA
// resource) points at setup-0 (a 64-byte version ID, then LZMA-compressed blocks holding the setup
// header and its entry records) and setup-1 (the file data: "zlb\x1A" chunks, stored, zlib, bzip2,
// LZMA, LZMA2 or Zstandard, often one solid chunk for everything). Records are their strings (length-prefixed,
// UTF-16 in Unicode builds) followed by the record's fixed fields, so reading them needs each
// version's field sizes: kLayouts below.
#include <zlib.h>
#define LZMA_API_STATIC
#include <lzma.h>
#include <zstd.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>

#include "../kz_hash.hpp"
#include "nat.hpp"

namespace kz::native {

namespace {

const char kLdrId[12] = {'r', 'D', 'l', 'P', 't', 'S', '\xCD', '\xE6', '\xD7', '\x7B', '\x0B', '\x2A'};

// ------------------------------------------------------------------------------- version layouts
// A record: how many (wide) strings, then ANSI strings, then how many bytes of fixed fields.
struct Rec {
    int strings, ansi, tail;
};

// Where a file location record keeps its fields (they moved and grew between versions).
struct LocFields {
    int start, startSize, sub, size, packed, hash, hashSize, time, flags, flagsSize;
};
// Bit numbers in the location's flags: time in UTC, CALL filter applied, encrypted, compressed.
struct LocBits {
    int utc, call, encrypted, compressed;
};

struct Layout {
    const char* id;  // the setup-0 ID, "Inno Setup Setup Data (6.7.0)"
    Rec header;
    // Where in the header's fixed fields the entry counts are: language, custom message, permission,
    // type, component, task, dir, ISSig key (-1 when the version has none), file, file location.
    int countAt[10];
    int compressAt;  // TSetupCompressMethod: stored, zip, bzip, lzma, lzma2 (, zstd)
    Rec language, customMessage, permission, type, component, task, dir, issigKey, file;
    int fileLocationAt;  // in the file record's fixed fields
    Rec location;
    LocFields loc;
    LocBits bits;
    bool encryptionHeader;  // a TSetupEncryptionHeader (with its CRC) follows the ID (6.5 and later)
    bool block64;           // compressed blocks give their size as an Int64 (6.7 and later)
};

// Every Inno Setup 6 and 7 layout, worked out from each version's Struct.pas (generated, not typed in:
// field sizes follow Delphi's rules - enums 1 byte, sets 1 byte per 8 members with 3 rounded to 4).
// The comment after each ID lists the releases that write it.
const Layout kLayouts[] = {
    {"Inno Setup Setup Data (6.0.0)",  // 6_0_0, 6_0_5
     {30, 4, 200}, {32, 36, 40, 44, 48, 52, 56, -1, 60, 64}, 180,
     {6, 4, 25}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {0, 0, 0},
     {10, 0, 43}, 20,
     {0, 0, 74}, {8, 4, 12, 20, 28, 36, 20, 56, 72, 2}, {2, 4, 6, 7},
     false, false},
    {"Inno Setup Setup Data (6.1.0)",  // 6_1_0, 6_1_2, 6_2_0, 6_2_2
     {30, 4, 200}, {32, 36, 40, 44, 48, 52, 56, -1, 60, 64}, 180,
     {6, 4, 25}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {0, 0, 0},
     {10, 0, 43}, 20,
     {0, 0, 74}, {8, 4, 12, 20, 28, 36, 20, 56, 72, 2}, {2, 4, 6, 7},
     false, false},
    {"Inno Setup Setup Data (6.3.0)",  // 6_3_0, 6_3_3
     {32, 4, 166}, {0, 4, 8, 12, 16, 20, 24, -1, 28, 32}, 148,
     {6, 4, 21}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {0, 0, 0},
     {10, 0, 43}, 20,
     {0, 0, 75}, {8, 4, 12, 20, 28, 36, 20, 56, 72, 2}, {2, 4, 6, 7},
     false, false},
    {"Inno Setup Setup Data (6.4.0.1)",  // 6_4_0
     {32, 4, 177}, {0, 4, 8, 12, 16, 20, 24, -1, 28, 32}, 160,
     {6, 4, 21}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {0, 0, 0},
     {10, 0, 43}, 20,
     {0, 0, 87}, {8, 4, 12, 20, 28, 36, 32, 68, 84, 2}, {2, 4, 6, 7},
     false, false},
    {"Inno Setup Setup Data (6.4.3)",  // 6_4_3
     {33, 4, 177}, {0, 4, 8, 12, 16, 20, 24, -1, 28, 32}, 160,
     {6, 4, 21}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {0, 0, 0},
     {10, 0, 43}, 20,
     {0, 0, 85}, {8, 4, 12, 20, 28, 36, 32, 68, 84, 1}, {1, 2, 3, 4},
     false, false},
    {"Inno Setup Setup Data (6.5.0)",  // 6_5_0
     {34, 4, 133}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 116,
     {6, 4, 21}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 77}, 53,
     {0, 0, 85}, {8, 4, 12, 20, 28, 36, 32, 68, 84, 1}, {1, 2, 3, 4},
     true, false},
    {"Inno Setup Setup Data (6.5.2)",  // 6_5_4
     {34, 4, 141}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 124,
     {6, 4, 21}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 77}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, false},
    {"Inno Setup Setup Data (6.6.0)",  // 6_6_0
     {34, 4, 149}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 132,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 77}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, false},
    {"Inno Setup Setup Data (6.6.1)",  // 6_6_1
     {34, 4, 150}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 133,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 42}, {6, 0, 26}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 77}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, false},
    {"Inno Setup Setup Data (6.7.0)",  // 6_7_0, 6_7_4, 7_0_0_0
     {39, 4, 162}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 143,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 39}, {6, 0, 23}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 80}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, true},
    {"Inno Setup Setup Data (7.0.0.1)",  // 7_0_0_1, 7_0_0_2
     {39, 4, 162}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 143,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 39}, {6, 0, 23}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 80}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, true},
    {"Inno Setup Setup Data (7.0.0.3)",  // 7_0_0, 7_0_1, 7_0_2, 7_1_0
     {39, 4, 166}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 147,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 39}, {6, 0, 23}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 81}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, true},
    {"Inno Setup Setup Data (7.1.1)",  // 7_1_1
     {38, 4, 166}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 147,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 39}, {6, 0, 23}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 81}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, true},
    {"Inno Setup Setup Data (7.2.0)",  // 7_2_0_0
     {38, 4, 166}, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36}, 147,
     {4, 4, 19}, {2, 0, 4}, {0, 1, 0}, {4, 0, 30}, {5, 0, 39}, {6, 0, 23}, {7, 0, 27}, {3, 0, 0},
     {15, 1, 81}, 53,
     {0, 0, 89}, {8, 8, 16, 24, 32, 40, 32, 72, 88, 1}, {1, 2, 3, 4},
     true, true},
};

// ---------------------------------------------------------------------------------- the loader
uint64_t findOffsetTable(Stream& s) {
    std::vector<uint8_t> h = s.getUpTo(0, 4096);
    Span H(h);
    if (!H.eq(0, "MZ", 2)) return 0;
    uint32_t pe = H.u32(0x3C);
    if (!H.eq(pe, "PE\0\0", 4)) return 0;
    uint32_t nsec = H.u16(pe + 6), opt = H.u16(pe + 20), magic = H.u16(pe + 24);
    uint32_t dirs = pe + 24 + (magic == 0x20B ? 112 : 96);
    uint32_t resRva = H.u32(dirs + 2 * 8), table = pe + 24 + opt;
    if (!resRva) return 0;
    for (uint32_t i = 0; i < nsec && i < 96; ++i) {
        uint32_t o = table + i * 40;
        uint32_t va = H.u32(o + 12), vsize = H.u32(o + 8), raw = H.u32(o + 16), at = H.u32(o + 20);
        if (resRva < va || resRva >= va + std::max(vsize, raw)) continue;
        // The table is a small RCDATA resource; look for its ID in the resource section.
        uint64_t len = std::min<uint64_t>(raw, 32u << 20);
        std::vector<uint8_t> sec = s.getUpTo(at, (size_t)len);
        for (size_t k = 0; k + sizeof kLdrId <= sec.size(); ++k)
            if (sec[k] == 'r' && std::memcmp(&sec[k], kLdrId, sizeof kLdrId) == 0) return at + k;
        return 0;
    }
    return 0;
}

struct Offsets {
    uint64_t setup0 = 0, setup1 = 0;
};

bool readOffsets(Stream& s, uint64_t at, Offsets& o) {
    std::vector<uint8_t> t = s.getUpTo(at, 64);
    Span T(t);
    uint32_t rev = T.u32(12);
    if (rev == 2) {  // 64-bit offsets (6.5 and later)
        if (T.n < 64 || (uint32_t)crc32(0, t.data(), 60) != T.u32(60)) return false;  // a padding field, then the CRC
        o.setup0 = T.u64(40);
        o.setup1 = T.u64(48);
    } else if (rev == 1) {
        if (T.n < 44 || (uint32_t)crc32(0, t.data(), 40) != T.u32(40)) return false;
        o.setup0 = T.u32(32);
        o.setup1 = T.u32(36);
    } else {
        return false;
    }
    return o.setup0 && o.setup0 < s.size();
}

// A TCompressedBlock: CRC + header (stored size, compressed flag), then CRC'd 4 KiB chunks of an
// LZMA1 stream (5 property bytes first).
bool readBlock(Stream& s, uint64_t pos, bool size64, std::vector<uint8_t>& out, uint64_t& next, std::string& err) {
    const size_t hdr = size64 ? 13 : 9;
    std::vector<uint8_t> h = s.getUpTo(pos, hdr);
    Span H(h);
    if (H.n < hdr || (uint32_t)crc32(0, h.data() + 4, (uInt)hdr - 4) != H.u32(0)) return err = "Damaged setup data header", false;
    uint64_t stored = size64 ? H.u64(4) : H.u32(4);
    bool compressed = H.u8(hdr - 1) != 0;
    if (pos + hdr > s.size() || stored > s.size() - pos - hdr || stored > (512u << 20)) return err = "Damaged setup data header", false;
    std::vector<uint8_t> raw = s.get(pos + hdr, (size_t)stored), data;
    if (raw.size() != stored) return err = "Setup data is cut short", false;
    data.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        if (raw.size() - i < 5) return err = "Damaged setup data", false;
        size_t len = std::min<size_t>(4096, raw.size() - i - 4);
        if ((uint32_t)crc32(0, raw.data() + i + 4, (uInt)len) != Span(raw).u32(i)) return err = "Setup data fails its CRC check", false;
        data.insert(data.end(), raw.begin() + i + 4, raw.begin() + i + 4 + len);
        i += 4 + len;
    }
    next = pos + hdr + stored;
    if (!compressed) {
        out = std::move(data);
        return true;
    }
    if (data.size() < 5 || !unLzmaRaw(data.data(), data.data() + 5, data.size() - 5, out, 0)) return err = "Damaged setup data", false;
    return true;
}

// Walks records in a decompressed block.
struct Records {
    const std::vector<uint8_t>& d;
    size_t at = 0;
    bool ok = true;
    explicit Records(const std::vector<uint8_t>& v) : d(v) {}
    bool take(size_t n) {
        if (!ok || n > d.size() - at) return ok = false;
        at += n;
        return true;
    }
    // One record: its strings into `str` (when given), the fixed fields' start returned.
    size_t rec(const Rec& r, std::vector<std::string>* str = nullptr) {
        for (int i = 0; i < r.strings + r.ansi; ++i) {
            uint32_t len = Span(d).u32(at);
            if (!take(4)) return 0;
            size_t p = at;
            if (!take(len)) return 0;
            if (str) str->push_back(i < r.strings ? utf16le(d.data() + p, len / 2) : std::string((const char*)d.data() + p, len));
        }
        size_t fixed = at;
        take((size_t)r.tail);
        return fixed;
    }
    void skip(const Rec& r, int64_t count) {
        for (int64_t i = 0; i < count && ok; ++i) rec(r);
    }
};

// -------------------------------------------------------------------------------- file data
// Inno's own x86 CALL/JMP filter, undone over each 64 KiB buffer of a file (the compiler's buffer size).
void untransformCalls(uint8_t* p, size_t size, uint32_t addrOffset) {
    if (size < 5) return;
    size -= 4;
    for (size_t i = 0; i < size;) {
        if (p[i] != 0xE8 && p[i] != 0xE9) {
            ++i;
            continue;
        }
        ++i;
        if (p[i + 3] == 0x00 || p[i + 3] == 0xFF) {
            uint32_t addr = (addrOffset + (uint32_t)i + 4) & 0xFFFFFF;
            uint32_t rel = (p[i] | p[i + 1] << 8 | p[i + 2] << 16) - addr;
            if (rel & 0x800000) p[i + 3] = (uint8_t)~p[i + 3];
            p[i] = (uint8_t)rel, p[i + 1] = (uint8_t)(rel >> 8), p[i + 2] = (uint8_t)(rel >> 16);
        }
        i += 4;
    }
}

enum Method { kStored, kZip, kBzip, kLzma, kLzma2, kZstd };

// Decodes one chunk front to back. Kept between files: in a solid installer the next file usually
// continues where the last one ended, so extracting everything decodes the chunk once.
class ChunkCursor {
public:
    ChunkCursor(StreamPtr s, uint64_t off, uint64_t len, Method m) : s_(std::move(s)), off_(off), len_(len), m_(m) {}
    ~ChunkCursor() { close(); }

    // `n` bytes at `sub` of the decoded chunk.
    bool read(uint64_t sub, uint8_t* dst, size_t n, std::string& err) {
        if (!open_ || sub < out_) {
            close();
            if (!start(err)) return false;
        }
        std::vector<uint8_t> scratch;
        while (out_ < sub) {
            scratch.resize((size_t)std::min<uint64_t>(sub - out_, 1 << 20));
            if (!produce(scratch.data(), scratch.size(), err)) return false;
        }
        return produce(dst, n, err);
    }
    std::mutex mu;

private:
    bool start(std::string& err) {
        std::vector<uint8_t> id = s_->getUpTo(off_, 4);
        if (id.size() != 4 || std::memcmp(id.data(), "zlb\x1A", 4) != 0) return err = "Damaged file data (no chunk marker)", false;
        in_ = 4;
        out_ = 0;
        buf_.clear();
        bufAt_ = 0;
        if (m_ == kZip) {
            z_ = z_stream{};
            if (inflateInit(&z_) != Z_OK) return err = "zlib failed to start", false;
        } else if (m_ == kBzip) {
#ifdef KZ_HAVE_BZIP2
            bz_ = bz_stream{};
            if (BZ2_bzDecompressInit(&bz_, 0, 0) != BZ_OK) return err = "bzip2 failed to start", false;
#else
            return err = "bzip2 is not available in this build", false;
#endif
        } else if (m_ == kZstd) {
            zs_ = ZSTD_createDStream();
            if (!zs_ || ZSTD_isError(ZSTD_initDStream(zs_))) return err = "Zstandard failed to start", false;
        } else if (m_ == kLzma || m_ == kLzma2) {
            uint8_t props[5];
            size_t np = m_ == kLzma ? 5 : 1;
            if (!pull(props, np)) return err = "Damaged file data", false;
            lzma_filter f[2] = {{m_ == kLzma ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
            if (lzma_properties_decode(&f[0], nullptr, props, np) != LZMA_OK) return err = "Damaged LZMA properties", false;
            xz_ = LZMA_STREAM_INIT;
            lzma_ret r = lzma_raw_decoder(&xz_, f);
            free(f[0].options);
            if (r != LZMA_OK) return err = "LZMA failed to start", false;
        }
        open_ = true;
        return true;
    }
    void close() {
        if (!open_) return;
        if (m_ == kZip) inflateEnd(&z_);
#ifdef KZ_HAVE_BZIP2
        else if (m_ == kBzip) BZ2_bzDecompressEnd(&bz_);
#endif
        else if (m_ == kLzma || m_ == kLzma2) lzma_end(&xz_);
        else if (m_ == kZstd && zs_) ZSTD_freeDStream(zs_), zs_ = nullptr;
        open_ = false;
    }
    // Raw (compressed) bytes of the chunk, in order.
    size_t pullSome(uint8_t* dst, size_t n) {
        uint64_t left = 4 + len_ - in_;
        size_t k = (size_t)std::min<uint64_t>(n, left);
        if (!k || !s_->readAt(off_ + in_, dst, k)) return 0;
        in_ += k;
        return k;
    }
    bool pull(uint8_t* dst, size_t n) {
        while (n) {
            size_t k = pullSome(dst, n);
            if (!k) return false;
            dst += k, n -= k;
        }
        return true;
    }
    // Exactly n decoded bytes.
    bool produce(uint8_t* dst, size_t n, std::string& err) {
        size_t done = 0;
        while (done < n) {
            if (m_ == kStored) {
                if (!pull(dst + done, n - done)) return err = "File data is cut short", false;
                done = n;
                break;
            }
            if (bufAt_ == buf_.size()) {  // refill compressed input
                buf_.resize(256 * 1024);
                size_t k = pullSome(buf_.data(), buf_.size());
                buf_.resize(k);
                bufAt_ = 0;
                eof_ = k == 0;
            }
            size_t avail = buf_.size() - bufAt_, before = done;
            const uint8_t* src = buf_.data() + bufAt_;
            size_t used = 0;
            bool end = false, bad = false;
            if (m_ == kZip) {
                z_.next_in = const_cast<Bytef*>(src), z_.avail_in = (uInt)avail;
                z_.next_out = dst + done, z_.avail_out = (uInt)std::min<size_t>(n - done, 1u << 30);
                int r = inflate(&z_, Z_NO_FLUSH);
                used = avail - z_.avail_in;
                done = (size_t)(z_.next_out - dst);
                end = r == Z_STREAM_END;
                bad = r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR;
#ifdef KZ_HAVE_BZIP2
            } else if (m_ == kBzip) {
                bz_.next_in = (char*)src, bz_.avail_in = (unsigned)avail;
                bz_.next_out = (char*)dst + done, bz_.avail_out = (unsigned)std::min<size_t>(n - done, 1u << 30);
                int r = BZ2_bzDecompress(&bz_);
                used = avail - bz_.avail_in;
                done = (size_t)((uint8_t*)bz_.next_out - dst);
                end = r == BZ_STREAM_END;
                bad = r != BZ_OK && r != BZ_STREAM_END;
#endif
            } else if (m_ == kZstd) {
                ZSTD_inBuffer ib{src, avail, 0};
                ZSTD_outBuffer ob{dst + done, n - done, 0};
                size_t r = ZSTD_decompressStream(zs_, &ob, &ib);
                used = ib.pos;
                done += ob.pos;
                bad = ZSTD_isError(r);
                end = !bad && r == 0 && done < n && ib.pos == ib.size && eof_;
            } else {
                xz_.next_in = src, xz_.avail_in = avail;
                xz_.next_out = dst + done, xz_.avail_out = n - done;
                lzma_ret r = lzma_code(&xz_, eof_ ? LZMA_FINISH : LZMA_RUN);
                used = avail - xz_.avail_in;
                done = (size_t)(xz_.next_out - dst);
                end = r == LZMA_STREAM_END;
                bad = r != LZMA_OK && r != LZMA_STREAM_END && !(r == LZMA_BUF_ERROR && done > before);
            }
            bufAt_ += used;
            if (bad) return err = "Damaged file data", false;
            if (done < n && (end || (eof_ && done == before && used == 0))) return err = "File data is cut short", false;
        }
        out_ += n;
        return true;
    }

    StreamPtr s_;
    uint64_t off_, len_;
    Method m_;
    bool open_ = false, eof_ = false;
    uint64_t in_ = 0, out_ = 0;
    std::vector<uint8_t> buf_;
    size_t bufAt_ = 0;
    z_stream z_{};
#ifdef KZ_HAVE_BZIP2
    bz_stream bz_{};
#endif
    lzma_stream xz_ = LZMA_STREAM_INIT;
    ZSTD_DStream* zs_ = nullptr;
};

struct Location {
    uint64_t start = 0, sub = 0, size = 0, packed = 0;
    std::string hash;  // SHA-256, or SHA-1 before 6.4
    int64_t mtime = 0;
    uint8_t flags = 0;  // 1 version info, 2 UTC time, 4 call-optimized, 8 encrypted, 16 compressed
};

// "{app}\bin\x.dll" -> "bin/x.dll"; other folders keep their constant as a top folder ("{sys}/x.dll").
std::string destPath(std::string d) {
    std::replace(d.begin(), d.end(), '\\', '/');
    if (d.compare(0, 6, "{app}/") == 0) d.erase(0, 6);
    std::string out, part;
    for (size_t i = 0; i <= d.size(); ++i) {
        if (i == d.size() || d[i] == '/') {
            if (!part.empty() && part != "." && part != "..") out += (out.empty() ? "" : "/") + component(part);
            part.clear();
        } else {
            part += d[i];
        }
    }
    return out;
}

bool openInno(Ctx& ctx, StreamPtr s) {
    uint64_t tableAt = findOffsetTable(*s);
    Offsets o;
    if (!tableAt || !readOffsets(*s, tableAt, o)) return false;
    std::vector<uint8_t> idb = s->getUpTo(o.setup0, 64);
    std::string id = cstr(idb.data(), idb.size());
    if (id.compare(0, 22, "Inno Setup Setup Data ") != 0) return false;
    const Layout* L = nullptr;
    for (auto& l : kLayouts)
        if (id == l.id) L = &l;
    if (!L) {
        ctx.out.warning = "Inno Setup installer, data version " + id.substr(22) + ": Kompressor reads the files of Inno Setup 6 and 7 "
                          "installers; this one's are listed as the program's parts instead";
        return false;
    }

    uint64_t pos = o.setup0 + 64;
    if (L->encryptionHeader) {
        std::vector<uint8_t> e = s->getUpTo(pos, 53);
        if (e.size() < 53 || (uint32_t)crc32(0, e.data() + 4, 49) != Span(e).u32(0)) return ctx.err = "Damaged Inno Setup data", false;
        if (e[4] == 2) {  // euFull: even the file list is encrypted
            ctx.out.warning = "This Inno Setup installer is encrypted, file list included: it needs its password, which "
                              "Kompressor can't use for Inno Setup yet";
            return false;
        }
        pos += 53;
    }
    std::vector<uint8_t> b1, b2;
    uint64_t next = 0, next2 = 0;
    if (!readBlock(*s, pos, L->block64, b1, next, ctx.err) || !readBlock(*s, next, L->block64, b2, next2, ctx.err)) return false;

    // The header, then every record list up to the files.
    Records r(b1);
    std::vector<std::string> hs;
    size_t hf = r.rec(L->header, &hs);
    if (!r.ok) return ctx.err = "Damaged Inno Setup header", false;
    Span H(b1.data() + hf, (size_t)L->header.tail);
    auto count = [&](int k) { return L->countAt[k] < 0 ? 0 : (int32_t)H.u32((size_t)L->countAt[k]); };
    uint32_t cm = H.u8((size_t)L->compressAt);
    if (cm > kZstd) return ctx.err = "Unknown Inno Setup compression method", false;
    Method method = (Method)cm;
    r.skip(L->language, count(0));
    r.skip(L->customMessage, count(1));
    r.skip(L->permission, count(2));
    r.skip(L->type, count(3));
    r.skip(L->component, count(4));
    r.skip(L->task, count(5));
    r.skip(L->dir, count(6));
    r.skip(L->issigKey, count(7));
    struct FileRec {
        std::string dest;
        int32_t loc;
    };
    std::vector<FileRec> files;
    for (int32_t i = 0; i < count(8) && r.ok; ++i) {
        std::vector<std::string> fs;
        size_t f = r.rec(L->file, &fs);
        if (r.ok && fs.size() > 1) files.push_back({fs[1], (int32_t)Span(b1.data() + f, (size_t)L->file.tail).u32((size_t)L->fileLocationAt)});
    }
    if (!r.ok) return ctx.err = "Damaged Inno Setup file list", false;

    Records r2(b2);
    std::vector<Location> locs;
    for (int32_t i = 0; i < count(9) && r2.ok; ++i) {
        size_t f = r2.rec(L->location);
        if (!r2.ok) break;
        Span F(b2.data() + f, (size_t)L->location.tail);
        const LocFields& q = L->loc;
        Location l;
        l.start = q.startSize == 8 ? F.u64((size_t)q.start) : F.u32((size_t)q.start);
        l.sub = F.u64((size_t)q.sub), l.size = F.u64((size_t)q.size), l.packed = F.u64((size_t)q.packed);
        static const char* hx = "0123456789abcdef";
        for (int k = 0; k < q.hashSize; ++k) l.hash += hx[F.u8((size_t)(q.hash + k)) >> 4], l.hash += hx[F.u8((size_t)(q.hash + k)) & 15];
        uint32_t fl = q.flagsSize == 2 ? F.u16((size_t)q.flags) : F.u8((size_t)q.flags);
        auto has = [&](int b) { return (fl >> b & 1) != 0; };
        l.flags = (uint8_t)((has(L->bits.utc) ? 2 : 0) | (has(L->bits.call) ? 4 : 0) | (has(L->bits.encrypted) ? 8 : 0) |
                            (has(L->bits.compressed) ? 16 : 0));
        l.mtime = fromFiletime(F.u64((size_t)q.time));
        if (!(l.flags & 2) && l.mtime) {  // local time (of the PC that built it; this PC's zone is the best guess)
            time_t t = (time_t)l.mtime;
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            tm.tm_isdst = -1;
            time_t utc = std::mktime(&tm);
            if (utc != (time_t)-1) l.mtime = (int64_t)utc;
        }
        if (F.u32(0) != 0 || F.u32(4) != 0) l.flags |= 0x80;  // in another disk slice
        locs.push_back(l);
    }
    if (!r2.ok) return ctx.err = "Damaged Inno Setup file locations", false;

    // One decoder per chunk, shared by the files in it.
    std::map<uint64_t, std::shared_ptr<ChunkCursor>> chunks;
    std::map<std::string, int> seen;
    bool spanned = false, encrypted = false;
    for (auto& f : files) {
        if (f.loc < 0 || (size_t)f.loc >= locs.size()) continue;  // downloaded, external or uninstaller
        const Location& l = locs[(size_t)f.loc];
        std::string path = destPath(f.dest);
        if (path.empty()) continue;
        if (int n = seen[path]++) path += "~" + std::to_string(n + 1);  // the same file for other languages / components
        Item it;
        it.path = ctx.prefix + path;
        it.size = l.size;
        it.mtime = l.mtime;
        if (l.flags & 0x80 || !o.setup1) {
            spanned = true;
            it.read = [](const Sink&, std::string& err) { return err = "This file is on another disk of the installer", false; };
        } else if (l.flags & 8) {
            encrypted = true;
            it.read = [](const Sink&, std::string& err) { return err = "This file is encrypted (the installer has a password)", false; };
        } else {
            auto& c = chunks[l.start];
            Method m = l.flags & 16 ? method : kStored;
            if (!c) c = std::make_shared<ChunkCursor>(s, o.setup1 + l.start, l.packed, m);
            auto cur = c;
            Location loc = l;
            it.read = [cur, loc](const Sink& sink, std::string& err) {
                std::lock_guard<std::mutex> lock(cur->mu);
                Digest sha(loc.hash.size() == 40 ? kHashSha1 : kHashSha256);
                std::vector<uint8_t> buf(65536);
                uint64_t left = loc.size, at = loc.sub;
                uint32_t addr = 0;
                while (left) {
                    size_t n = (size_t)std::min<uint64_t>(left, buf.size());
                    if (!cur->read(at, buf.data(), n, err)) return false;
                    if (loc.flags & 4) untransformCalls(buf.data(), n, addr);
                    addr += (uint32_t)n;
                    sha.update(buf.data(), n);
                    at += n, left -= n;
                    if (!sink(buf.data(), n)) return true;
                }
                if (sha.hex() != loc.hash) return err = "Checksum mismatch: the installer is damaged", false;
                return true;
            };
        }
        ctx.out.items.push_back(std::move(it));
    }
    std::string ver = id.substr(23, id.find(')') == std::string::npos ? std::string::npos : id.find(')') - 23);
    ctx.formats.push_back("Inno Setup " + ver);
    if (spanned) ctx.out.warning = "Some files are on other disks of this installer (setup-*.bin), which aren't read";
    else if (encrypted) ctx.out.warning = "The installer's files are encrypted; Kompressor can't use Inno Setup passwords yet";
    return true;
}

}  // namespace

const Handler hInno = {"Inno Setup",
                       [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                           return Span(h).eq(0, "MZ", 2) && findOffsetTable(s) != 0;
                       },
                       openInno};

}  // namespace kz::native
