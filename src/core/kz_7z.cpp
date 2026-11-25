#include "kz_7z.hpp"

#include <lzma.h>
#include <zlib.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "kz_crypto.hpp"
#include "kz_sniff.hpp"
#include "native/nat.hpp"

namespace kz::sevenzip {

using native::StreamPtr;

namespace {

const uint8_t kSig[6] = {'7', 'z', 0xBC, 0xAF, 0x27, 0x1C};

enum : uint64_t {
    kEnd = 0x00, kHeader = 0x01, kArchiveProperties = 0x02, kAdditionalStreamsInfo = 0x03, kMainStreamsInfo = 0x04,
    kFilesInfo = 0x05, kPackInfo = 0x06, kUnpackInfo = 0x07, kSubStreamsInfo = 0x08, kSize = 0x09, kCRC = 0x0A,
    kFolder = 0x0B, kCodersUnpackSize = 0x0C, kNumUnpackStream = 0x0D, kEmptyStream = 0x0E, kEmptyFile = 0x0F,
    kAnti = 0x10, kName = 0x11, kMTime = 0x14, kWinAttributes = 0x15, kEncodedHeader = 0x17, kDummy = 0x19,
};

// Method ids, as numbers (the id bytes read big-endian).
enum : uint64_t {
    mCopy = 0x00, mDelta = 0x03, mLzma2 = 0x21, mArm64 = 0x0A, mRiscv = 0x0B, mLzma = 0x030101,
    mX86 = 0x03030103, mPpc = 0x03030205, mIa64 = 0x03030401, mArm = 0x03030501, mArmt = 0x03030701,
    mSparc = 0x03030805, mBcj2 = 0x0303011B, mPpmd = 0x030401, mDeflate = 0x040108, mDeflate64 = 0x040109,
    mBzip2 = 0x040202, mAes = 0x06F10701,
};

// ------------------------------------------------------------------------------------ header reading
struct Rd {
    const uint8_t* p;
    size_t n, i = 0;
    bool bad = false;
    Rd(const uint8_t* d, size_t len) : p(d), n(len) {}
    uint8_t byte() {
        if (i >= n) {
            bad = true;
            return 0;
        }
        return p[i++];
    }
    uint64_t num() {
        uint8_t first = byte(), mask = 0x80;
        uint64_t v = 0;
        for (int k = 0; k < 8; ++k) {
            if (!(first & mask)) return v | (uint64_t)(first & (mask - 1)) << (8 * k);
            v |= (uint64_t)byte() << (8 * k);
            mask >>= 1;
        }
        return v;
    }
    uint32_t u32() {
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) v |= (uint32_t)byte() << (8 * k);
        return v;
    }
    uint64_t u64() { return (uint64_t)u32() | (uint64_t)u32() << 32; }
    void skip(uint64_t k) {
        if (k > n - i) bad = true, i = n;
        else i += (size_t)k;
    }
    // A count that must fit what is left (guards against absurd allocations from damaged headers).
    size_t count(size_t perItemBytes = 1) {
        uint64_t c = num();
        if (c > (n - i) / std::max<size_t>(perItemBytes, 1) + 1 || c > (1u << 28)) {
            bad = true;
            return 0;
        }
        return (size_t)c;
    }
    std::vector<bool> bits(size_t c) {
        std::vector<bool> v(c);
        uint8_t b = 0, m = 0;
        for (size_t k = 0; k < c; ++k) {
            if (!m) b = byte(), m = 0x80;
            v[k] = (b & m) != 0;
            m >>= 1;
        }
        return v;
    }
    std::vector<bool> defined(size_t c) { return byte() ? std::vector<bool>(c, true) : bits(c); }
};

struct Coder {
    uint64_t method = 0;
    uint32_t nIn = 1, nOut = 1;
    std::vector<uint8_t> props;
};

struct Folder {
    std::vector<Coder> coders;
    std::vector<std::pair<uint64_t, uint64_t>> bind;  // (in index, out index)
    std::vector<uint64_t> packed;                     // in indexes fed straight from pack streams
    std::vector<uint64_t> sizes;                      // per out stream
    bool crcDef = false;
    uint32_t crc = 0;
    size_t firstPack = 0;
    uint64_t mainOut() const {
        for (uint64_t o = 0; o < sizes.size(); ++o) {
            bool bound = false;
            for (auto& b : bind) bound |= b.second == o;
            if (!bound) return o;
        }
        return 0;
    }
    uint64_t size() const { return sizes.empty() ? 0 : sizes[(size_t)mainOut()]; }
    bool encrypted() const {
        for (auto& c : coders)
            if (c.method == mAes) return true;
        return false;
    }
};

struct Streams {
    uint64_t packPos = 0;
    std::vector<uint64_t> packSizes;
    std::vector<Folder> folders;
    std::vector<size_t> numUnpack;  // substreams per folder
    std::vector<uint64_t> subSizes;
    std::vector<bool> subCrcDef;
    std::vector<uint32_t> subCrc;
};

void readPackInfo(Rd& r, Streams& s) {
    s.packPos = r.num();
    size_t n = r.count();
    s.packSizes.assign(n, 0);
    for (;;) {
        uint64_t id = r.num();
        if (id == kEnd || r.bad) break;
        if (id == kSize) {
            for (auto& x : s.packSizes) x = r.num();
        } else if (id == kCRC) {
            auto def = r.defined(n);
            for (bool d : def)
                if (d) r.u32();
        } else {
            r.skip(r.num());
        }
    }
}

void readFolder(Rd& r, Folder& f) {
    size_t nc = r.count();
    uint64_t totalIn = 0, totalOut = 0;
    for (size_t c = 0; c < nc && !r.bad; ++c) {
        Coder k;
        uint8_t flags = r.byte();
        size_t idSize = flags & 0x0F;
        for (size_t j = 0; j < idSize; ++j) k.method = k.method << 8 | r.byte();
        if (flags & 0x10) {
            k.nIn = (uint32_t)r.num();
            k.nOut = (uint32_t)r.num();
        }
        if (flags & 0x20) {
            size_t ps = r.count();
            for (size_t j = 0; j < ps; ++j) k.props.push_back(r.byte());
        }
        if ((flags & 0x80) || k.nIn > 64 || k.nOut != 1) r.bad = true;  // alternative methods: not in use
        totalIn += k.nIn;
        totalOut += k.nOut;
        f.coders.push_back(std::move(k));
    }
    if (totalOut == 0) {
        r.bad = true;
        return;
    }
    for (uint64_t b = 0; b + 1 < totalOut && !r.bad; ++b) {
        uint64_t in = r.num(), out = r.num();
        f.bind.emplace_back(in, out);
    }
    uint64_t nPacked = totalIn - f.bind.size();
    if (nPacked == 1) {
        for (uint64_t i = 0; i < totalIn; ++i) {
            bool bound = false;
            for (auto& b : f.bind) bound |= b.first == i;
            if (!bound) {
                f.packed.push_back(i);
                break;
            }
        }
    } else {
        for (uint64_t j = 0; j < nPacked && !r.bad; ++j) f.packed.push_back(r.num());
    }
    f.sizes.assign((size_t)totalOut, 0);
}

void readUnpackInfo(Rd& r, Streams& s) {
    if (r.num() != kFolder) {
        r.bad = true;
        return;
    }
    size_t nf = r.count();
    if (r.byte() != 0) {  // "external": never used
        r.bad = true;
        return;
    }
    s.folders.resize(nf);
    for (auto& f : s.folders) readFolder(r, f);
    if (r.num() != kCodersUnpackSize) {
        r.bad = true;
        return;
    }
    for (auto& f : s.folders)
        for (auto& x : f.sizes) x = r.num();
    for (;;) {
        uint64_t id = r.num();
        if (id == kEnd || r.bad) break;
        if (id == kCRC) {
            auto def = r.defined(nf);
            for (size_t i = 0; i < nf; ++i)
                if (def[i]) s.folders[i].crcDef = true, s.folders[i].crc = r.u32();
        } else {
            r.skip(r.num());
        }
    }
}

void readSubStreams(Rd& r, Streams& s) {
    size_t nf = s.folders.size();
    s.numUnpack.assign(nf, 1);
    uint64_t id = r.num();
    if (id == kNumUnpackStream) {
        for (auto& x : s.numUnpack) x = r.count();
        id = r.num();
    }
    bool haveSizes = id == kSize;
    for (size_t i = 0; i < nf; ++i) {
        size_t k = s.numUnpack[i];
        if (!k) continue;
        uint64_t sum = 0;
        for (size_t j = 1; j < k && haveSizes; ++j) {
            uint64_t x = r.num();
            s.subSizes.push_back(x);
            sum += x;
        }
        if (!haveSizes && k > 1) r.bad = true;
        uint64_t fs = s.folders[i].size();
        s.subSizes.push_back(fs >= sum ? fs - sum : 0);
    }
    if (haveSizes) id = r.num();
    size_t unknown = 0;
    for (size_t i = 0; i < nf; ++i)
        if (s.numUnpack[i] != 1 || !s.folders[i].crcDef) unknown += s.numUnpack[i];
    std::vector<bool> def;
    std::vector<uint32_t> crcs;
    for (;;) {
        if (id == kEnd || r.bad) break;
        if (id == kCRC) {
            def = r.defined(unknown);
            for (bool d : def) crcs.push_back(d ? r.u32() : 0);
        } else {
            r.skip(r.num());
        }
        id = r.num();
    }
    size_t u = 0;
    for (size_t i = 0; i < nf; ++i) {
        if (s.numUnpack[i] == 1 && s.folders[i].crcDef) {
            s.subCrcDef.push_back(true);
            s.subCrc.push_back(s.folders[i].crc);
            continue;
        }
        for (size_t j = 0; j < s.numUnpack[i]; ++j, ++u) {
            bool d = u < def.size() && def[u];
            s.subCrcDef.push_back(d);
            s.subCrc.push_back(d ? crcs[u] : 0);
        }
    }
}

void readStreams(Rd& r, Streams& s) {
    bool sub = false;
    for (;;) {
        uint64_t id = r.num();
        if (id == kEnd || r.bad) break;
        if (id == kPackInfo) readPackInfo(r, s);
        else if (id == kUnpackInfo) readUnpackInfo(r, s);
        else if (id == kSubStreamsInfo) readSubStreams(r, s), sub = true;
        else r.bad = true;
    }
    if (!sub) {  // one stream per folder
        s.numUnpack.assign(s.folders.size(), 1);
        for (auto& f : s.folders) {
            s.subSizes.push_back(f.size());
            s.subCrcDef.push_back(f.crcDef);
            s.subCrc.push_back(f.crc);
        }
    }
    size_t pack = 0;
    for (auto& f : s.folders) {
        f.firstPack = pack;
        pack += f.packed.size();
    }
    if (pack > s.packSizes.size()) r.bad = true;
}

struct File {
    std::string name;
    bool hasStream = true, isDir = false, anti = false;
    int64_t mtime = 0;
    uint32_t attrib = 0;
    bool attribDef = false;
};

void readFiles(Rd& r, std::vector<File>& files) {
    size_t n = r.count();
    files.assign(n, File());
    std::vector<bool> emptyStream(n, false), emptyFile, anti;
    size_t numEmpty = 0;
    for (;;) {
        uint64_t type = r.num();
        if (type == kEnd || r.bad) break;
        uint64_t size = r.num();
        if (size > r.n - r.i) {
            r.bad = true;
            break;
        }
        size_t start = r.i;
        Rd p(r.p + start, (size_t)size);
        switch (type) {
            case kEmptyStream:
                emptyStream = p.bits(n);
                numEmpty = (size_t)std::count(emptyStream.begin(), emptyStream.end(), true);
                break;
            case kEmptyFile: emptyFile = p.bits(numEmpty); break;
            case kAnti: anti = p.bits(numEmpty); break;
            case kName: {
                if (p.byte() != 0) break;  // external: never used
                size_t f = 0, from = p.i;
                for (size_t q = p.i; q + 1 < p.n && f < n; q += 2)
                    if (p.p[q] == 0 && p.p[q + 1] == 0) {
                        files[f++].name = native::utf16le(p.p + from, (q - from) / 2);
                        from = q + 2;
                    }
                break;
            }
            case kMTime: {
                auto def = p.defined(n);
                if (p.byte() != 0) break;
                for (size_t f = 0; f < n; ++f)
                    if (def[f]) files[f].mtime = native::fromFiletime(p.u64());
                break;
            }
            case kWinAttributes: {
                auto def = p.defined(n);
                if (p.byte() != 0) break;
                for (size_t f = 0; f < n; ++f)
                    if (def[f]) files[f].attrib = p.u32(), files[f].attribDef = true;
                break;
            }
            default: break;
        }
        r.i = start + (size_t)size;
    }
    size_t e = 0;
    for (size_t f = 0; f < n; ++f) {
        File& x = files[f];
        x.hasStream = !emptyStream[f];
        if (!x.hasStream) {
            bool isEmptyFile = e < emptyFile.size() && emptyFile[e];
            x.anti = e < anti.size() && anti[e];
            x.isDir = !isEmptyFile;
            ++e;
        }
        if (x.attribDef && (x.attrib & 0x10)) x.isDir = true;
        std::replace(x.name.begin(), x.name.end(), '\\', '/');
        while (!x.name.empty() && x.name.back() == '/') x.name.pop_back();
    }
}

// ------------------------------------------------------------------------------------- decoding
// A decoded stream, read from the front. read() fills up to `n` bytes; 0 = the end (or an error,
// which is then in `err`).
struct In {
    virtual ~In() = default;
    virtual size_t read(uint8_t* d, size_t n) = 0;
    std::shared_ptr<std::string> err;
};
using InPtr = std::unique_ptr<In>;

struct PackIn : In {
    StreamPtr s;
    uint64_t off, left;
    PackIn(StreamPtr st, uint64_t o, uint64_t len) : s(std::move(st)), off(o), left(len) {}
    size_t read(uint8_t* d, size_t n) override {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n) return 0;
        if (!s->readAt(off, d, n)) {
            *err = "The archive is cut short (a missing part, or an incomplete download?)";
            left = 0;
            return 0;
        }
        off += n, left -= n;
        return n;
    }
};

// Reads exactly `limit` bytes of another stream (Copy).
struct LimitIn : In {
    InPtr in;
    uint64_t left;
    LimitIn(InPtr i, uint64_t l) : in(std::move(i)), left(l) {}
    size_t read(uint8_t* d, size_t n) override {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n) return 0;
        size_t got = in->read(d, n);
        left -= got;
        return got;
    }
};

// Fills `n` bytes from `in` unless it ends first.
size_t readFull(In& in, uint8_t* d, size_t n) {
    size_t got = 0;
    while (got < n) {
        size_t k = in.read(d + got, n - got);
        if (!k) break;
        got += k;
    }
    return got;
}

struct AesIn : In {
    InPtr in;
    crypto::Aes256CbcDecrypt aes;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1 << 16);
    size_t at = 0, have = 0;
    AesIn(InPtr i, const uint8_t key[32], const uint8_t iv[16]) : in(std::move(i)), aes(key, iv) {}
    size_t read(uint8_t* d, size_t n) override {
        if (at == have) {
            size_t got = readFull(*in, buf.data(), buf.size());
            got &= ~(size_t)15;
            if (!got) return 0;
            aes.decrypt(buf.data(), got);
            at = 0, have = got;
        }
        n = std::min(n, have - at);
        std::memcpy(d, buf.data() + at, n);
        at += n;
        return n;
    }
};

// LZMA / LZMA2, with BCJ-type filters and Delta in front of them, through liblzma.
struct XzIn : In {
    InPtr in;
    lzma_stream z = LZMA_STREAM_INIT;
    std::vector<lzma_filter> filters;
    std::vector<void*> opts;
    std::vector<uint8_t> ibuf = std::vector<uint8_t>(1 << 16);
    uint64_t left;
    bool ended = false, inEnd = false;
    XzIn(InPtr i, uint64_t outSize) : in(std::move(i)), left(outSize) {}
    ~XzIn() override {
        lzma_end(&z);
        for (void* o : opts) std::free(o);
    }
    bool start() {
        filters.push_back({LZMA_VLI_UNKNOWN, nullptr});
        return lzma_raw_decoder(&z, filters.data()) == LZMA_OK;
    }
    size_t read(uint8_t* d, size_t n) override {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n || ended) return 0;
        z.next_out = d;
        z.avail_out = n;
        while (z.avail_out == n) {
            if (z.avail_in == 0 && !inEnd) {
                size_t got = in->read(ibuf.data(), ibuf.size());
                if (!got) inEnd = true;
                z.next_in = ibuf.data();
                z.avail_in = got;
            }
            lzma_ret r = lzma_code(&z, LZMA_RUN);
            if (r == LZMA_STREAM_END) {
                ended = true;
                break;
            }
            if (r != LZMA_OK) {
                if (err->empty()) *err = "The data is damaged (LZMA error " + std::to_string((int)r) + ")";
                ended = true;
                break;
            }
            if (inEnd && z.avail_in == 0 && z.avail_out == n) {
                if (err->empty()) *err = "The data ends too soon";
                ended = true;
                break;
            }
        }
        size_t got = n - z.avail_out;
        left -= got;
        return got;
    }
};

struct ZlibIn : In {
    InPtr in;
    z_stream z{};
    std::vector<uint8_t> ibuf = std::vector<uint8_t>(1 << 16);
    uint64_t left;
    bool ended = false, inEnd = false, ok = false;
    ZlibIn(InPtr i, uint64_t outSize) : in(std::move(i)), left(outSize) { ok = inflateInit2(&z, -15) == Z_OK; }
    ~ZlibIn() override {
        if (ok) inflateEnd(&z);
    }
    size_t read(uint8_t* d, size_t n) override {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n || ended) return 0;
        z.next_out = d;
        z.avail_out = (uInt)n;
        while (z.avail_out == n) {
            if (z.avail_in == 0 && !inEnd) {
                size_t got = in->read(ibuf.data(), ibuf.size());
                if (!got) inEnd = true;
                z.next_in = ibuf.data();
                z.avail_in = (uInt)got;
            }
            int r = inflate(&z, Z_NO_FLUSH);
            if (r == Z_STREAM_END) {
                ended = true;
                break;
            }
            if (r != Z_OK && r != Z_BUF_ERROR) {
                if (err->empty()) *err = "The data is damaged (Deflate error)";
                ended = true;
                break;
            }
            if (inEnd && z.avail_in == 0 && z.avail_out == n) {
                if (err->empty()) *err = "The data ends too soon";
                ended = true;
                break;
            }
        }
        size_t got = n - z.avail_out;
        left -= got;
        return got;
    }
};

#ifdef KZ_HAVE_BZIP2
struct Bz2In : In {
    InPtr in;
    bz_stream z{};
    std::vector<uint8_t> ibuf = std::vector<uint8_t>(1 << 16);
    uint64_t left;
    bool ended = false, inEnd = false, ok = false;
    Bz2In(InPtr i, uint64_t outSize) : in(std::move(i)), left(outSize) { ok = BZ2_bzDecompressInit(&z, 0, 0) == BZ_OK; }
    ~Bz2In() override {
        if (ok) BZ2_bzDecompressEnd(&z);
    }
    size_t read(uint8_t* d, size_t n) override {
        n = (size_t)std::min<uint64_t>(n, left);
        if (!n || ended) return 0;
        z.next_out = (char*)d;
        z.avail_out = (unsigned)n;
        while (z.avail_out == n) {
            if (z.avail_in == 0 && !inEnd) {
                size_t got = in->read(ibuf.data(), ibuf.size());
                if (!got) inEnd = true;
                z.next_in = (char*)ibuf.data();
                z.avail_in = (unsigned)got;
            }
            int r = BZ2_bzDecompress(&z);
            if (r == BZ_STREAM_END) {
                // 7-Zip may write several bzip2 streams back to back (multithreaded): start the next.
                BZ2_bzDecompressEnd(&z);
                char *ni = z.next_in, *no = z.next_out;
                unsigned na = z.avail_in, nao = z.avail_out;
                z = bz_stream{};
                ok = BZ2_bzDecompressInit(&z, 0, 0) == BZ_OK;
                z.next_in = ni, z.avail_in = na;
                z.next_out = no, z.avail_out = nao;
                if (!ok || (na == 0 && inEnd)) {
                    ended = true;
                    break;
                }
                continue;
            }
            if (r != BZ_OK) {
                if (err->empty()) *err = "The data is damaged (BZip2 error)";
                ended = true;
                break;
            }
            if (inEnd && z.avail_in == 0 && z.avail_out == n) {
                if (err->empty()) *err = "The data ends too soon";
                ended = true;
                break;
            }
        }
        size_t got = n - z.avail_out;
        left -= got;
        return got;
    }
};
#endif

// Byte-wise reading over a stream (for BCJ2's four inputs).
struct Bytes {
    In* in;
    uint8_t buf[1 << 15];
    size_t at = 0, have = 0;
    bool end = false;
    explicit Bytes(In* i) : in(i) {}
    int get() {
        if (at == have) {
            have = end ? 0 : in->read(buf, sizeof buf);
            at = 0;
            if (!have) {
                end = true;
                return -1;
            }
        }
        return buf[at++];
    }
};

// x86 BCJ2: calls and jumps were moved to their own streams, chosen by a range coder.
struct Bcj2In : In {
    InPtr s[4];
    std::unique_ptr<Bytes> main, call, jump, rc;
    uint16_t prob[2 + 256];
    uint32_t range = 0xFFFFFFFF, code = 0;
    uint64_t outPos = 0, size;
    uint8_t prev = 0;
    uint8_t pend[4];
    int npend = 0, pendAt = 0;
    bool bad = false;
    Bcj2In(InPtr a, InPtr b, InPtr c, InPtr d, uint64_t outSize) : size(outSize) {
        s[0] = std::move(a), s[1] = std::move(b), s[2] = std::move(c), s[3] = std::move(d);
        main = std::make_unique<Bytes>(s[0].get());
        call = std::make_unique<Bytes>(s[1].get());
        jump = std::make_unique<Bytes>(s[2].get());
        rc = std::make_unique<Bytes>(s[3].get());
        for (auto& p : prob) p = 1024;
        for (int i = 0; i < 5; ++i) code = code << 8 | (uint32_t)std::max(rc->get(), 0);
    }
    void normalize() {
        if (range < (1u << 24)) {
            range <<= 8;
            code = code << 8 | (uint32_t)std::max(rc->get(), 0);
        }
    }
    size_t read(uint8_t* d, size_t n) override {
        size_t o = 0;
        while (o < n && outPos < size && !bad) {
            if (pendAt < npend) {
                d[o++] = pend[pendAt++];
                ++outPos;
                continue;
            }
            int c = main->get();
            if (c < 0) {
                if (err->empty()) *err = "The data ends too soon (BCJ2)";
                bad = true;
                break;
            }
            uint8_t b = (uint8_t)c;
            d[o++] = b;
            ++outPos;
            bool isJ = (b & 0xFE) == 0xE8 || (prev == 0x0F && (b & 0xF0) == 0x80);
            if (!isJ || outPos >= size) {
                prev = b;
                continue;
            }
            uint16_t& p = b == 0xE8 ? prob[prev] : b == 0xE9 ? prob[256] : prob[257];
            uint32_t bound = (range >> 11) * p;
            if (code < bound) {
                range = bound;
                p = (uint16_t)(p + ((2048 - p) >> 5));
                normalize();
                prev = b;
            } else {
                range -= bound;
                code -= bound;
                p = (uint16_t)(p - (p >> 5));
                normalize();
                Bytes& from = b == 0xE8 ? *call : *jump;
                uint32_t src = 0;
                for (int i = 0; i < 4; ++i) {
                    int x = from.get();
                    if (x < 0) {
                        if (err->empty()) *err = "The data ends too soon (BCJ2)";
                        bad = true;
                        break;
                    }
                    src = src << 8 | (uint32_t)x;
                }
                uint32_t dest = src - (uint32_t)(outPos + 4);
                for (int i = 0; i < 4; ++i) pend[i] = (uint8_t)(dest >> (8 * i));
                npend = 4, pendAt = 0;
                prev = (uint8_t)(dest >> 24);
            }
        }
        return o;
    }
};

// Delta on its own (with LZMA under it, liblzma does it instead).
struct DeltaIn : In {
    InPtr in;
    unsigned dist;
    uint8_t hist[256] = {};
    unsigned pos = 0;
    DeltaIn(InPtr i, unsigned d) : in(std::move(i)), dist(d) {}
    size_t read(uint8_t* d, size_t n) override {
        size_t got = in->read(d, n);
        for (size_t i = 0; i < got; ++i) {
            d[i] = (uint8_t)(d[i] + hist[(pos - dist) & 255]);
            hist[pos++ & 255] = d[i];
        }
        return got;
    }
};

std::string methodName(uint64_t m) {
    switch (m) {
        case mPpmd: return "PPMd";
        case mDeflate64: return "Deflate64";
        case mBzip2: return "BZip2";
        default: {
            char b[32];
            std::snprintf(b, sizeof b, "method %llX", (unsigned long long)m);
            return b;
        }
    }
}

bool isFilter(uint64_t m) {
    return m == mX86 || m == mPpc || m == mIa64 || m == mArm || m == mArmt || m == mSparc || m == mArm64 || m == mRiscv ||
           m == mDelta;
}

// ------------------------------------------------------------------------------------- the archive
std::u16string toUtf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = (uint8_t)s[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : c;
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) c = c << 6 | ((uint8_t)s[i] & 0x3F);
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

struct Archive {
    StreamPtr file;
    uint64_t base = 0;  // where the 7z signature is (after a self-extractor)
    std::string password;
    Streams st;
    std::vector<File> files;
    bool headerEncrypted = false;
    std::mutex mu;
    std::map<std::string, std::vector<uint8_t>> keys;  // derived AES keys, by salt + cycles

    struct Cursor {
        InPtr top;
        std::shared_ptr<std::string> err;
        uint64_t pos = 0;
    };
    std::map<size_t, Cursor> cursors;

    bool key(const std::vector<uint8_t>& props, uint8_t out[32], uint8_t iv[16], std::string& err) {
        std::memset(iv, 0, 16);
        if (props.empty()) {
            err = "Damaged encryption settings";
            return false;
        }
        unsigned cycles = props[0] & 0x3F;
        size_t saltSize = 0, ivSize = 0;
        if (props[0] & 0xC0) {
            if (props.size() < 2) {
                err = "Damaged encryption settings";
                return false;
            }
            saltSize = ((props[0] >> 7) & 1) + (props[1] >> 4);
            ivSize = ((props[0] >> 6) & 1) + (props[1] & 0x0F);
        }
        if (2 + saltSize + ivSize > props.size() + (props[0] & 0xC0 ? 0 : 2) || ivSize > 16) {
            err = "Damaged encryption settings";
            return false;
        }
        const uint8_t* salt = props.data() + 2;
        std::memcpy(iv, salt + saltSize, ivSize);
        if (password.empty()) {
            err = "This archive needs a password";
            return false;
        }
        std::string id(salt, salt + saltSize);
        id += (char)cycles;
        auto it = keys.find(id);
        if (it != keys.end()) {
            std::memcpy(out, it->second.data(), 32);
            return true;
        }
        std::u16string pw = toUtf16(password);
        std::vector<uint8_t> pwb;
        for (char16_t c : pw) pwb.push_back((uint8_t)c), pwb.push_back((uint8_t)(c >> 8));
        if (cycles == 0x3F) {
            std::memset(out, 0, 32);
            size_t k = 0;
            for (size_t i = 0; i < saltSize && k < 32; ++i) out[k++] = salt[i];
            for (size_t i = 0; i < pwb.size() && k < 32; ++i) out[k++] = pwb[i];
        } else {
            // SHA-256 over 2^cycles copies of salt + password + an 8-byte counter.
            std::vector<uint8_t> unit(salt, salt + saltSize);
            unit.insert(unit.end(), pwb.begin(), pwb.end());
            size_t ctr = unit.size();
            unit.resize(ctr + 8, 0);
            crypto::Sha256 h;
            uint64_t rounds = 1ull << cycles;
            for (uint64_t r = 0; r < rounds; ++r) {
                h.update(unit.data(), unit.size());
                for (int i = 0; i < 8; ++i)
                    if (++unit[ctr + i] != 0) break;
            }
            h.final(out);
        }
        keys[id].assign(out, out + 32);
        return true;
    }

    uint64_t packOffset(const Streams& s, size_t index) const {
        uint64_t o = base + 32 + s.packPos;
        for (size_t i = 0; i < index && i < s.packSizes.size(); ++i) o += s.packSizes[i];
        return o;
    }

    InPtr coderOut(const Streams& s, const Folder& f, size_t c, const std::shared_ptr<std::string>& err);

    InPtr inStream(const Streams& s, const Folder& f, uint64_t inIndex, const std::shared_ptr<std::string>& err) {
        for (auto& b : f.bind) {
            if (b.first != inIndex) continue;
            uint64_t o = 0;
            for (size_t c = 0; c < f.coders.size(); ++c) {
                if (b.second < o + f.coders[c].nOut) return coderOut(s, f, c, err);
                o += f.coders[c].nOut;
            }
            *err = "Damaged archive header (stream links)";
            return nullptr;
        }
        for (size_t j = 0; j < f.packed.size(); ++j)
            if (f.packed[j] == inIndex) {
                size_t pi = f.firstPack + j;
                if (pi >= s.packSizes.size()) break;
                auto p = std::make_unique<PackIn>(file, packOffset(s, pi), s.packSizes[pi]);
                p->err = err;
                return p;
            }
        *err = "Damaged archive header (stream links)";
        return nullptr;
    }

    // The coder whose output feeds input `inIndex`, when it is one (else -1).
    int feeder(const Folder& f, uint64_t inIndex) {
        for (auto& b : f.bind)
            if (b.first == inIndex) {
                uint64_t o = 0;
                for (size_t c = 0; c < f.coders.size(); ++c) {
                    if (b.second < o + f.coders[c].nOut) return (int)c;
                    o += f.coders[c].nOut;
                }
            }
        return -1;
    }

    InPtr decodeFolder(const Streams& s, size_t fi, const std::shared_ptr<std::string>& err) {
        const Folder& f = s.folders[fi];
        uint64_t mo = f.mainOut(), o = 0;
        for (size_t c = 0; c < f.coders.size(); ++c) {
            if (mo < o + f.coders[c].nOut) return coderOut(s, f, c, err);
            o += f.coders[c].nOut;
        }
        *err = "Damaged archive header";
        return nullptr;
    }

    bool readHeaderBytes(std::vector<uint8_t>& out, bool& needPassword, std::string& err);
};

InPtr Archive::coderOut(const Streams& s, const Folder& f, size_t c, const std::shared_ptr<std::string>& err) {
    const Coder& k = f.coders[c];
    uint64_t firstIn = 0, outIndex = 0;
    for (size_t i = 0; i < c; ++i) firstIn += f.coders[i].nIn, outIndex += f.coders[i].nOut;
    uint64_t size = outIndex < f.sizes.size() ? f.sizes[(size_t)outIndex] : 0;
    auto tag = [&](InPtr p) {
        if (p) p->err = err;
        return p;
    };

    if (k.method == mLzma || k.method == mLzma2 || (isFilter(k.method) && k.nIn == 1)) {
        // A run of filters ending in LZMA / LZMA2 is one liblzma decoder.
        std::vector<const Coder*> chain;
        size_t cur = c;
        uint64_t curIn = firstIn;
        for (;;) {
            const Coder& x = f.coders[cur];
            chain.push_back(&x);
            if (x.method == mLzma || x.method == mLzma2) break;
            int next = feeder(f, curIn);
            if (next < 0 || !(isFilter(f.coders[next].method) || f.coders[next].method == mLzma || f.coders[next].method == mLzma2) ||
                chain.size() >= 4) {
                chain.clear();
                break;
            }
            cur = (size_t)next;
            curIn = 0;
            for (size_t i = 0; i < cur; ++i) curIn += f.coders[i].nIn;
        }
        if (!chain.empty()) {
            auto x = std::make_unique<XzIn>(inStream(s, f, curIn, err), size);
            if (!x->in) return nullptr;
            for (const Coder* q : chain) {
                lzma_filter flt{LZMA_VLI_UNKNOWN, nullptr};
                switch (q->method) {
                    case mLzma: flt.id = LZMA_FILTER_LZMA1; break;
                    case mLzma2: flt.id = LZMA_FILTER_LZMA2; break;
                    case mX86: flt.id = LZMA_FILTER_X86; break;
                    case mPpc: flt.id = LZMA_FILTER_POWERPC; break;
                    case mIa64: flt.id = LZMA_FILTER_IA64; break;
                    case mArm: flt.id = LZMA_FILTER_ARM; break;
                    case mArmt: flt.id = LZMA_FILTER_ARMTHUMB; break;
                    case mSparc: flt.id = LZMA_FILTER_SPARC; break;
#ifdef LZMA_FILTER_ARM64
                    case mArm64: flt.id = LZMA_FILTER_ARM64; break;
#endif
#ifdef LZMA_FILTER_RISCV
                    case mRiscv: flt.id = LZMA_FILTER_RISCV; break;
#endif
                    case mDelta: flt.id = LZMA_FILTER_DELTA; break;
                    default: break;
                }
                if (flt.id == LZMA_VLI_UNKNOWN) {
                    *err = "This archive uses a filter Kompressor can't decode (" + methodName(q->method) + ")";
                    return nullptr;
                }
                if (q->method == mLzma || q->method == mLzma2) {
                    if (lzma_properties_decode(&flt, nullptr, q->props.data(), q->props.size()) != LZMA_OK) {
                        *err = "Damaged LZMA settings";
                        return nullptr;
                    }
                    x->opts.push_back(flt.options);
                } else if (q->method == mDelta) {
                    auto* d = (lzma_options_delta*)std::calloc(1, sizeof(lzma_options_delta));
                    d->type = LZMA_DELTA_TYPE_BYTE;
                    d->dist = (q->props.empty() ? 0 : q->props[0]) + 1u;
                    flt.options = d;
                    x->opts.push_back(d);
                } else if (q->props.size() == 4) {
                    auto* b = (lzma_options_bcj*)std::calloc(1, sizeof(lzma_options_bcj));
                    b->start_offset = (uint32_t)q->props[0] | (uint32_t)q->props[1] << 8 | (uint32_t)q->props[2] << 16 |
                                      (uint32_t)q->props[3] << 24;
                    flt.options = b;
                    x->opts.push_back(b);
                }
                x->filters.push_back(flt);
            }
            x->err = err;
            if (!x->start()) {
                *err = "Can't start the LZMA decoder";
                return nullptr;
            }
            return x;
        }
    }
    switch (k.method) {
        case mCopy: {
            InPtr i = inStream(s, f, firstIn, err);
            return i ? tag(std::make_unique<LimitIn>(std::move(i), size)) : nullptr;
        }
        case mAes: {
            uint8_t key[32], iv[16];
            std::string kerr;
            if (!this->key(k.props, key, iv, kerr)) {
                *err = kerr;
                return nullptr;
            }
            InPtr i = inStream(s, f, firstIn, err);
            if (!i) return nullptr;
            return tag(std::make_unique<LimitIn>(tag(std::make_unique<AesIn>(std::move(i), key, iv)), size));
        }
        case mDeflate: {
            InPtr i = inStream(s, f, firstIn, err);
            return i ? tag(std::make_unique<ZlibIn>(std::move(i), size)) : nullptr;
        }
#ifdef KZ_HAVE_BZIP2
        case mBzip2: {
            InPtr i = inStream(s, f, firstIn, err);
            return i ? tag(std::make_unique<Bz2In>(std::move(i), size)) : nullptr;
        }
#endif
        case mDelta: {
            InPtr i = inStream(s, f, firstIn, err);
            if (!i) return nullptr;
            unsigned dist = (k.props.empty() ? 0 : k.props[0]) + 1u;
            return tag(std::make_unique<LimitIn>(tag(std::make_unique<DeltaIn>(std::move(i), dist)), size));
        }
        case mBcj2: {
            if (k.nIn != 4) break;
            InPtr a = inStream(s, f, firstIn, err), b = inStream(s, f, firstIn + 1, err);
            InPtr cc = inStream(s, f, firstIn + 2, err), d = inStream(s, f, firstIn + 3, err);
            if (!a || !b || !cc || !d) return nullptr;
            return tag(std::make_unique<Bcj2In>(std::move(a), std::move(b), std::move(cc), std::move(d), size));
        }
        default: break;
    }
    *err = "This archive uses " + methodName(k.method) + ", which Kompressor can't decode inside an encrypted 7z yet";
    return nullptr;
}

bool Archive::readHeaderBytes(std::vector<uint8_t>& out, bool& needPassword, std::string& err) {
    uint8_t sh[32];
    if (!file->readAt(base, sh, 32) || std::memcmp(sh, kSig, 6) != 0) {
        err = "Not a 7z archive";
        return false;
    }
    native::Span sp(sh, 32);
    uint64_t nextOff = sp.u64(12), nextSize = sp.u64(20);
    uint32_t nextCrc = sp.u32(28);
    if (nextSize == 0 || nextSize > (256u << 20) || base + 32 + nextOff + nextSize > file->size()) {
        err = "The archive is damaged or incomplete (its header is missing)";
        return false;
    }
    out.resize((size_t)nextSize);
    if (!file->readAt(base + 32 + nextOff, out.data(), out.size()) || crc32(0, out.data(), (uInt)out.size()) != nextCrc) {
        err = "The archive's header is damaged";
        return false;
    }
    // An encoded (compressed, perhaps encrypted) header: decode it, as many times as it is wrapped.
    for (int depth = 0; depth < 4 && !out.empty() && out[0] == kEncodedHeader; ++depth) {
        Rd r(out.data() + 1, out.size() - 1);
        Streams hs;
        readStreams(r, hs);
        if (r.bad || hs.folders.empty()) {
            err = "The archive's header is damaged";
            return false;
        }
        bool enc = hs.folders[0].encrypted();
        headerEncrypted |= enc;
        if (enc && password.empty()) {
            needPassword = true;
            err = "This archive's file names are encrypted: it needs a password";
            return false;
        }
        auto e = std::make_shared<std::string>();
        InPtr in = decodeFolder(hs, 0, e);
        std::vector<uint8_t> dec;
        if (in) {
            dec.resize((size_t)std::min<uint64_t>(hs.folders[0].size(), 1u << 30));
            dec.resize(readFull(*in, dec.data(), dec.size()));
        }
        bool crcOk = in && dec.size() == hs.folders[0].size() &&
                     (!hs.folders[0].crcDef || crc32(0, dec.data(), (uInt)dec.size()) == hs.folders[0].crc);
        if (!crcOk) {
            if (enc) {
                needPassword = true;
                err = "Wrong password (the archive's file names can't be decrypted with it)";
            } else {
                err = e->empty() ? "The archive's header is damaged" : *e;
            }
            return false;
        }
        out = std::move(dec);
    }
    return true;
}

std::shared_ptr<Archive> load(const Source& src, const std::string& password, bool& needPassword, std::string& err) {
    auto a = std::make_shared<Archive>();
    a->password = password;
    if (src.inMemory()) {
        a->file = native::memStream(src.mem);
    } else {
        std::vector<std::string> vols = volumeSet(src.path);
        if (vols.size() > 1) {
            std::vector<native::ExtentStream::Extent> ext;
            uint64_t at = 0;
            for (auto& v : vols) {
                StreamPtr s = native::fileStream(v);
                if (!s) break;  // a missing part: what's there still reads
                ext.push_back({at, s->size(), s, 0, {}});
                at += s->size();
            }
            a->file = std::make_shared<native::ExtentStream>(at, std::move(ext));
        } else {
            a->file = native::fileStream(src.path);
        }
    }
    if (!a->file) {
        err = "Can't open the archive";
        return nullptr;
    }
    uint8_t head[6];
    if (!a->file->readAt(0, head, 6) || std::memcmp(head, kSig, 6) != 0) {
        std::vector<uint8_t> h = a->file->getUpTo(0, 64 * 1024);
        uint64_t at = 0;
        Sniffed sn = sniffSfx(h.data(), h.size(), a->file->size(),
                              [&](uint64_t off, uint8_t* dst, size_t n) -> size_t {
                                  n = (size_t)std::min<uint64_t>(n, off < a->file->size() ? a->file->size() - off : 0);
                                  return n && a->file->readAt(off, dst, n) ? n : 0;
                              },
                              &at);
        if (!sn || std::strncmp(sn.name, "7-Zip", 5) != 0) {
            err = "Not a 7z archive";
            return nullptr;
        }
        a->base = at;
    }
    std::vector<uint8_t> hdr;
    if (!a->readHeaderBytes(hdr, needPassword, err)) return nullptr;
    Rd r(hdr.data(), hdr.size());
    if (r.num() != kHeader) {
        err = "The archive's header is damaged";
        return nullptr;
    }
    uint64_t id = r.num();
    if (id == kArchiveProperties) {
        for (;;) {
            uint64_t t = r.num();
            if (t == kEnd || r.bad) break;
            r.skip(r.num());
        }
        id = r.num();
    }
    if (id == kAdditionalStreamsInfo) {
        Streams ignored;
        readStreams(r, ignored);
        id = r.num();
    }
    if (id == kMainStreamsInfo) {
        readStreams(r, a->st);
        id = r.num();
    }
    if (id == kFilesInfo) {
        readFiles(r, a->files);
        id = r.num();
    }
    if (r.bad || id != kEnd) {
        err = "The archive's header is damaged";
        return nullptr;
    }
    return a;
}

}  // namespace

bool handles(const Source& src) {
    // Cheap test first: only 7z archives get the header read.
    bool any = false;
    if (const char* e = std::getenv("KZ_NATIVE_7Z")) any = *e == '1';
    bool needPw = false;
    std::string err;
    {
        // A 7z signature, or a Windows program that may carry one (a self-extractor).
        uint8_t h[6] = {};
        if (src.inMemory()) {
            if (src.mem->size() < 6) return false;
            std::memcpy(h, src.mem->data(), 6);
        } else {
            std::vector<std::string> vols = volumeSet(src.path);
            StreamPtr s = native::fileStream(vols.empty() ? src.path : vols[0]);
            if (!s || !s->readAt(0, h, 6)) return false;
        }
        if (std::memcmp(h, kSig, 6) != 0 && !(h[0] == 'M' && h[1] == 'Z')) return false;
    }
    auto a = load(src, "", needPw, err);
    if (!a) return needPw;  // names encrypted
    if (any) return true;
    for (auto& f : a->st.folders)
        if (f.encrypted()) return true;
    return false;
}

bool open(const Source& src, const std::string& password, bool reading, native::Container& out, bool& encrypted,
          bool& needPassword, std::string& err) {
    out = native::Container();
    encrypted = false;
    needPassword = false;
    auto a = load(src, password, needPassword, err);
    if (!a) return false;
    for (auto& f : a->st.folders) encrypted |= f.encrypted();
    encrypted |= a->headerEncrypted;
    out.format = a->headerEncrypted ? "7-Zip (encrypted file names)" : encrypted ? "7-Zip (encrypted)" : "7-Zip";

    if (reading && encrypted && password.empty()) {
        needPassword = true;
        err = "This archive needs a password";
        return false;
    }
    // Check the password now on the first encrypted file small enough to read whole, so a wrong one
    // is reported once instead of as a CRC error on every file.
    if (reading && encrypted && !a->headerEncrypted) {
        size_t si = 0;
        for (size_t fi = 0; fi < a->st.folders.size(); ++fi) {
            const Folder& f = a->st.folders[fi];
            size_t k = a->st.numUnpack[fi];
            if (f.encrypted() && k > 0 && si < a->st.subSizes.size() && a->st.subCrcDef[si] && a->st.subSizes[si] <= (64u << 20)) {
                auto e = std::make_shared<std::string>();
                InPtr in = a->decodeFolder(a->st, fi, e);
                std::vector<uint8_t> buf((size_t)a->st.subSizes[si]);
                size_t got = in ? readFull(*in, buf.data(), buf.size()) : 0;
                if (!in || got != buf.size() || crc32(0, buf.data(), (uInt)buf.size()) != a->st.subCrc[si]) {
                    needPassword = true;
                    err = "Wrong password";
                    return false;
                }
                break;
            }
            si += k;
        }
    }

    // Files with data take the substreams in order, folder by folder.
    size_t folder = 0, inFolder = 0, sub = 0;
    uint64_t offset = 0;
    for (auto& f : a->files) {
        if (f.anti || f.name.empty()) continue;
        if (f.isDir) {
            out.items.push_back(native::dirItem(f.name, f.mtime));
            continue;
        }
        native::Item it;
        it.path = f.name;
        it.mtime = f.mtime;
        if (!f.hasStream) {
            it.size = 0;
            it.read = [](const native::Sink&, std::string&) { return true; };
            out.items.push_back(std::move(it));
            continue;
        }
        while (folder < a->st.numUnpack.size() && inFolder >= a->st.numUnpack[folder]) {
            ++folder;
            inFolder = 0;
            offset = 0;
        }
        if (folder >= a->st.folders.size() || sub >= a->st.subSizes.size()) {
            out.warning = "Some files have no data in the archive (damaged header?)";
            break;
        }
        it.size = a->st.subSizes[sub];
        bool crcDef = a->st.subCrcDef[sub];
        uint32_t crc = a->st.subCrc[sub];
        bool enc = a->st.folders[folder].encrypted();
        size_t fi = folder;
        uint64_t start = offset, size = it.size;
        it.read = [a, fi, start, size, crcDef, crc, enc](const native::Sink& sink, std::string& err) {
            std::lock_guard<std::mutex> lk(a->mu);
            Archive::Cursor& c = a->cursors[fi];
            if (!c.top || c.pos > start || !c.err->empty()) {
                c.err = std::make_shared<std::string>();
                c.top = a->decodeFolder(a->st, fi, c.err);
                c.pos = 0;
                if (!c.top) {
                    err = *c.err;
                    return false;
                }
            }
            std::vector<uint8_t> buf(1 << 16);
            while (c.pos < start) {
                size_t got = c.top->read(buf.data(), (size_t)std::min<uint64_t>(buf.size(), start - c.pos));
                if (!got) {
                    err = c.err->empty() ? "The data ends too soon" : *c.err;
                    return false;
                }
                c.pos += got;
            }
            uint64_t left = size;
            uLong h = crc32(0, nullptr, 0);
            while (left) {
                size_t got = c.top->read(buf.data(), (size_t)std::min<uint64_t>(buf.size(), left));
                if (!got) {
                    err = c.err->empty() ? "The data ends too soon" : *c.err;
                    if (enc) err += " (wrong password?)";
                    return false;
                }
                c.pos += got;
                left -= got;
                h = crc32(h, buf.data(), (uInt)got);
                if (!sink(buf.data(), got)) return true;  // the reader has what it wanted
            }
            if (crcDef && (uint32_t)h != crc) {
                err = enc ? "CRC error: wrong password, or the archive is damaged" : "CRC error: the archive is damaged";
                return false;
            }
            return true;
        };
        out.items.push_back(std::move(it));
        offset += size;
        ++inFolder;
        ++sub;
    }
    return true;
}

}  // namespace kz::sevenzip
