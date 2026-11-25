// Microsoft's LZ77+Huffman codecs: LZX (CHM, WIM, Windows compact files) and XPRESS Huffman (WIM,
// Windows compact files). Both read 16-bit little-endian words, most significant bit first.
#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

// A bit reader over 16-bit little-endian words, most significant bit first.
struct Bits16 {
    const uint8_t* p;
    size_t n, pos = 0;  // next byte to load
    uint64_t buf = 0;   // left-aligned
    int count = 0;
    Bits16(const uint8_t* d, size_t len) : p(d), n(len) {}
    void fill() {
        while (count <= 48) {
            uint64_t w = 0;
            if (pos + 1 < n) w = p[pos] | (uint32_t)p[pos + 1] << 8;
            pos += 2;
            buf |= w << (48 - count);
            count += 16;
        }
    }
    uint32_t peek(int k) {
        if (count < k) fill();
        return k ? (uint32_t)(buf >> (64 - k)) : 0;
    }
    void skip(int k) {
        if (count < k) fill();
        buf <<= k;
        count -= k;
    }
    uint32_t get(int k) {
        uint32_t v = peek(k);
        skip(k);
        return v;
    }
    bool overrun() const { return pos > n + 8; }
    // Byte position of the next unread whole 16-bit word (bits buffered but unused are dropped).
    size_t bytePos() const { return pos - (size_t)(count / 16) * 2; }
};

// Canonical Huffman decoding: a direct table for short codes, a walk for long ones.
struct Huff {
    static constexpr int kTable = 10;
    std::vector<uint16_t> table;  // (symbol << 5) | length, 0 = longer code
    std::vector<uint16_t> sorted, counts;
    std::vector<uint8_t> lens;
    int maxLen = 16;
    bool build(const uint8_t* len, size_t n, int maxBits = 16) {
        maxLen = maxBits;
        lens.assign(len, len + n);
        counts.assign(maxLen + 1, 0);
        for (size_t i = 0; i < n; ++i)
            if (len[i]) {
                if (len[i] > maxLen) return false;
                counts[len[i]]++;
            }
        sorted.clear();
        for (int l = 1; l <= maxLen; ++l)
            for (size_t i = 0; i < n; ++i)
                if (len[i] == l) sorted.push_back((uint16_t)i);
        table.assign(1u << kTable, 0);
        uint32_t code = 0;
        size_t k = 0;
        for (int l = 1; l <= maxLen; ++l) {
            for (uint32_t c = 0; c < counts[l]; ++c, ++code, ++k) {
                if (l <= kTable) {
                    uint32_t first = code << (kTable - l), num = 1u << (kTable - l);
                    if (first + num > table.size()) return false;  // over-subscribed
                    for (uint32_t j = 0; j < num; ++j) table[first + j] = (uint16_t)(sorted[k] << 5 | l);
                }
            }
            code <<= 1;
        }
        return true;
    }
    template <class B>
    int decode(B& b) const {
        uint32_t t = table[b.peek(kTable)];
        if (t) {
            b.skip(t & 31);
            return t >> 5;
        }
        // Longer code: walk the canonical code lengths.
        uint32_t bits = b.peek(maxLen);
        uint32_t code = 0, first = 0, index = 0;
        for (int l = 1; l <= maxLen; ++l) {
            code |= (bits >> (maxLen - l)) & 1;
            uint32_t cnt = counts[l];
            if (code - first < cnt) {
                b.skip(l);
                return sorted[index + code - first];
            }
            index += cnt;
            first = (first + cnt) << 1;
            code <<= 1;
        }
        return -1;
    }
};

const uint32_t kSlots[] = {30, 32, 34, 36, 38, 42, 50};  // position slots for window bits 15..21

}  // namespace

// ============================================================================================== LZX
struct LzxDecoder::State {
    int wbits;
    uint32_t wsize, wpos = 0, framePos = 0;  // framePos: where the next frame starts in the window
    std::vector<uint8_t> win;
    uint32_t r[3] = {1, 1, 1};
    uint32_t slots, mainSyms;
    uint8_t mainLen[656 + 8] = {}, lenLen[249] = {};
    Huff mainT, lenT, alignT;
    bool started = false, intel = false;
    uint32_t intelSize = 0;
    int blockType = 0;
    uint32_t blockLeft = 0, blockLen = 0;
    uint64_t frames = 0;
    uint32_t extra[51], base[51];
};

LzxDecoder::LzxDecoder(int windowBits) : st_(new State) {
    st_->wbits = std::clamp(windowBits, 15, 21);
    st_->wsize = 1u << st_->wbits;
    st_->win.assign(st_->wsize, 0);
    st_->slots = kSlots[st_->wbits - 15];
    st_->mainSyms = 256 + 8 * st_->slots;
    uint32_t b = 0;
    for (uint32_t i = 0; i < 51; ++i) {
        st_->extra[i] = i < 4 ? 0 : std::min<uint32_t>((i - 2) / 2, 17);
        st_->base[i] = b;
        b += 1u << st_->extra[i];
    }
    reset();
}

LzxDecoder::~LzxDecoder() { delete st_; }

void LzxDecoder::reset() {
    State& s = *st_;
    s.r[0] = s.r[1] = s.r[2] = 1;
    std::memset(s.mainLen, 0, sizeof s.mainLen);
    std::memset(s.lenLen, 0, sizeof s.lenLen);
    s.started = false;
    s.intel = false;
    s.blockLeft = 0;
    s.blockType = 0;
    s.frames = 0;
    s.wpos = 0;
    s.framePos = 0;
}

// Reads code lengths [from, to) as deltas from the previous ones, through a 20-symbol pretree.
static bool readLengths(Bits16& b, uint8_t* lens, uint32_t from, uint32_t to) {
    uint8_t pl[20];
    for (int i = 0; i < 20; ++i) pl[i] = (uint8_t)b.get(4);
    Huff pre;
    if (!pre.build(pl, 20)) return false;
    for (uint32_t i = from; i < to;) {
        int z = pre.decode(b);
        if (z < 0) return false;
        if (z == 17) {
            uint32_t run = 4 + b.get(4);
            while (run-- && i < to) lens[i++] = 0;
        } else if (z == 18) {
            uint32_t run = 20 + b.get(5);
            while (run-- && i < to) lens[i++] = 0;
        } else if (z == 19) {
            uint32_t run = 4 + b.get(1);
            int z2 = pre.decode(b);
            if (z2 < 0 || z2 > 16) return false;
            uint8_t v = (uint8_t)((lens[i] + 17 - z2) % 17);
            while (run-- && i < to) lens[i++] = v;
        } else {
            lens[i] = (uint8_t)((lens[i] + 17 - z) % 17);
            ++i;
        }
        if (b.overrun()) return false;
    }
    return true;
}

bool LzxDecoder::decode(const uint8_t* in, size_t n, uint8_t* out, size_t outLen) {
    State& s = *st_;
    Bits16 b(in, n);
    if (!s.started) {
        if (wim_) {
            s.intel = e8_;
            s.intelSize = e8Size_;
        } else if (b.get(1)) {
            uint32_t hi = b.get(16), lo = b.get(16);
            s.intel = true;
            s.intelSize = hi << 16 | lo;
        }
        s.started = true;
    }
    size_t done = 0;
    while (done < outLen) {
        uint32_t frame = (uint32_t)std::min<size_t>(32768, outLen - done);
        uint32_t frameStart = s.framePos, target = s.framePos + frame;
        if (target > s.wsize) return false;  // frames never wrap inside the window
        while (s.wpos < target) {
            if (s.blockLeft == 0) {
                if (s.blockType == 3 && (s.blockLen & 1)) {  // padding after an odd-sized stored block
                    size_t bp = b.bytePos() + 1;
                    b = Bits16(in, n);
                    b.pos = bp;
                }
                s.blockType = (int)b.get(3);
                if (wim_) {
                    if (b.get(1)) {
                        s.blockLen = 32768;
                    } else {
                        s.blockLen = b.get(16);
                        if (s.wbits >= 16) s.blockLen = s.blockLen << 8 | b.get(8);  // big chunks: 24 bits
                    }
                } else {
                    uint32_t hi = b.get(16), lo = b.get(8);
                    s.blockLen = hi << 8 | lo;
                }
                s.blockLeft = s.blockLen;
                if (s.blockType == 2) {
                    uint8_t al[8];
                    for (int i = 0; i < 8; ++i) al[i] = (uint8_t)b.get(3);
                    if (!s.alignT.build(al, 8)) return false;
                }
                if (s.blockType == 1 || s.blockType == 2) {
                    if (!readLengths(b, s.mainLen, 0, 256) || !readLengths(b, s.mainLen, 256, s.mainSyms)) return false;
                    if (!s.mainT.build(s.mainLen, s.mainSyms)) return false;
                    if (!readLengths(b, s.lenLen, 0, 249)) return false;
                    if (!s.lenT.build(s.lenLen, 249)) return false;
                } else if (s.blockType == 3) {
                    // Align to the next 16-bit word (a whole word of padding when already aligned).
                    if (b.count % 16 == 0) b.skip(16);
                    else b.skip(b.count % 16);
                    size_t bp = b.bytePos();
                    if (bp + 12 > n) return false;
                    for (int i = 0; i < 3; ++i) s.r[i] = Span(in + bp + i * 4, 4).u32(0);
                    b = Bits16(in, n);
                    b.pos = bp + 12;
                } else {
                    return false;
                }
                if (!s.blockLeft) return false;
            }
            uint32_t run = std::min(s.blockLeft, target - s.wpos);
            if (s.blockType == 3) {
                size_t bp = b.bytePos();
                if (bp + run > n) return false;
                std::memcpy(s.win.data() + s.wpos, in + bp, run);
                s.wpos += run;
                s.blockLeft -= run;
                b = Bits16(in, n);
                b.pos = bp + run;
                continue;
            }
            int64_t left = run;
            while (left > 0) {
                int sym = s.mainT.decode(b);
                if (sym < 0 || b.overrun()) return false;
                if (sym < 256) {
                    s.win[s.wpos++] = (uint8_t)sym;
                    --left;
                    continue;
                }
                sym -= 256;
                uint32_t len = sym & 7, slot = (uint32_t)sym >> 3;
                if (len == 7) {
                    int l = s.lenT.decode(b);
                    if (l < 0) return false;
                    len += (uint32_t)l;
                }
                len += 2;
                uint32_t off;
                if (slot == 0) {
                    off = s.r[0];
                } else if (slot == 1) {
                    off = s.r[1];
                    s.r[1] = s.r[0];
                    s.r[0] = off;
                } else if (slot == 2) {
                    off = s.r[2];
                    s.r[2] = s.r[0];
                    s.r[0] = off;
                } else {
                    uint32_t ex = s.extra[slot];
                    if (s.blockType == 2 && ex >= 3) {
                        uint32_t v = ex > 3 ? b.get((int)ex - 3) << 3 : 0;
                        int a = s.alignT.decode(b);
                        if (a < 0) return false;
                        off = s.base[slot] + v + (uint32_t)a - 2;
                    } else {
                        off = s.base[slot] + b.get((int)ex) - 2;
                    }
                    s.r[2] = s.r[1];
                    s.r[1] = s.r[0];
                    s.r[0] = off;
                }
                if (off == 0 || off >= s.wsize) return false;
                // Some compressors let a match run over the window's end; it carries on at the start.
                uint32_t mask = s.wsize - 1, from = (s.wpos + s.wsize - off) & mask;
                for (uint32_t k = 0; k < len; ++k) {
                    s.win[s.wpos++ & mask] = s.win[from];
                    from = (from + 1) & mask;
                }
                left -= len;
            }
            uint32_t used = (uint32_t)((int64_t)run - left);  // a match may run past the frame's end
            s.blockLeft = used > s.blockLeft ? 0 : s.blockLeft - used;
        }
        // The frame's bytes (a match may have run a little into the next frame: those stay in the window).
        std::memcpy(out + done, s.win.data() + frameStart, frame);
        // E8 call translation: per 32 KiB frame for CHM/CAB, over the whole chunk for WIM (below).
        bool last = done + frame >= outLen;
        uint32_t span = wim_ ? (uint32_t)(done + frame) : frame;
        if (s.intel && span > 10 && (wim_ ? last : s.frames < 32768)) {
            uint8_t* f = wim_ ? out : out + done;
            int64_t cur = wim_ ? 0 : (int64_t)s.frames * 32768;
            for (uint32_t i = 0; i + 10 <= span; ++i) {
                if (f[i] != 0xE8) continue;
                int32_t abs = (int32_t)Span(f + i + 1, 4).u32(0);
                int64_t pos = cur + i;
                if (abs >= -pos && abs < (int64_t)s.intelSize) {
                    int32_t rel = abs >= 0 ? (int32_t)(abs - pos) : (int32_t)(abs + s.intelSize);
                    for (int k = 0; k < 4; ++k) f[i + 1 + k] = uint8_t((uint32_t)rel >> (8 * k));
                }
                i += 4;
            }
        }
        done += frame;
        s.frames++;
        s.framePos = target;
        if (s.framePos >= s.wsize) {  // the window is a ring
            s.framePos -= s.wsize;
            s.wpos -= s.wsize;
        }
        if (!wim_) {  // frames end on a 16-bit boundary
            if (b.count % 16) b.skip(b.count % 16);
        }
    }
    return true;
}

// ===================================================================================== XPRESS Huffman
bool unXpressHuff(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t pos = 0;
    while (out.size() < expect) {
        if (pos + 256 + 4 > n) return false;
        uint8_t lens[512];
        for (int i = 0; i < 256; ++i) lens[2 * i] = in[pos + i] & 15, lens[2 * i + 1] = in[pos + i] >> 4;
        pos += 256;
        Huff h;
        if (!h.build(lens, 512, 15)) return false;
        size_t blockEnd = std::min(expect, out.size() + 65536);
        // MS-XCA keeps a 32-bit window of bits and reads extra length bytes straight from the input.
        uint32_t bits = (uint32_t)(in[pos] | in[pos + 1] << 8) << 16 | (uint32_t)(in[pos + 2] | in[pos + 3] << 8);
        pos += 4;
        int extra = 16;
        auto refill = [&]() {
            if (extra < 0) {
                uint32_t w = pos + 1 < n ? (uint32_t)(in[pos] | in[pos + 1] << 8) : 0;
                bits |= w << (-extra);
                extra += 16;
                pos += 2;
            }
        };
        while (out.size() < blockEnd) {
            // Decode a symbol from the top 15 bits.
            uint32_t top = bits >> 17;
            int sym = -1, len = 0;
            {
                uint32_t t = h.table[top >> (15 - Huff::kTable)];
                if (t) sym = t >> 5, len = t & 31;
                else {
                    uint32_t code = 0, first = 0, index = 0;
                    for (int l = 1; l <= 15; ++l) {
                        code |= (top >> (15 - l)) & 1;
                        uint32_t cnt = h.counts[l];
                        if (code - first < cnt) {
                            sym = h.sorted[index + code - first];
                            len = l;
                            break;
                        }
                        index += cnt;
                        first = (first + cnt) << 1;
                        code <<= 1;
                    }
                }
            }
            if (sym < 0) return false;
            bits <<= len;
            extra -= len;
            refill();
            if (sym < 256) {
                out.push_back((uint8_t)sym);
                continue;
            }
            if (sym == 256 && pos >= n && out.size() >= blockEnd) break;
            sym -= 256;
            uint32_t mlen = sym & 15, obits = (uint32_t)sym >> 4;
            if (mlen == 15) {
                if (pos >= n) return false;
                mlen = in[pos++];
                if (mlen == 255) {
                    if (pos + 2 > n) return false;
                    mlen = in[pos] | in[pos + 1] << 8;
                    pos += 2;
                    if (mlen == 0) {
                        if (pos + 4 > n) return false;
                        mlen = Span(in + pos, 4).u32(0);
                        pos += 4;
                    }
                    if (mlen < 15) return false;
                    mlen -= 15;
                }
                mlen += 15;
            }
            mlen += 3;
            uint32_t off = obits ? bits >> (32 - obits) : 0;
            off += 1u << obits;
            bits = obits ? bits << obits : bits;
            extra -= (int)obits;
            refill();
            if (off > out.size()) return false;
            size_t from = out.size() - off;
            // A match may run on past the 64 KiB block's end; the next block counts from where it stops.
            for (uint32_t k = 0; k < mlen && out.size() < expect; ++k) out.push_back(out[from + k]);
        }
        if (out.size() > expect) out.resize(expect);
        // The next 64 KiB block (if any) starts with its own table, right where this one's input ends.
        if (out.size() < expect) pos -= (extra >= 16 ? 2 : 0);
    }
    return true;
}

// =============================================================================================== LZMS
// Microsoft's LZMS (solid ESD/WIM resources): an adaptive range coder for the item types and match
// reuse, plus adaptive Huffman codes (rebuilt every few hundred symbols) read backwards from the end
// of the chunk, then an x86 call/jump filter. The Huffman codes must be rebuilt exactly as the
// compressor builds them, so this follows the classic length-limited construction step for step.
namespace {

struct LzmsTables {
    uint32_t offBase[800];
    uint8_t offExtra[799];
    uint32_t lenBase[55];
    uint8_t lenExtra[54];
    LzmsTables() {
        static const uint8_t offRuns[] = {9, 0, 9, 7, 10, 15, 15, 20, 20, 30, 33, 40, 42, 45, 60, 73, 80, 85, 95, 105, 6};
        static const uint8_t lenRuns[] = {27, 4, 6, 4, 5, 2, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1};
        fill(offBase, offExtra, offRuns, sizeof offRuns, 0x7fffffff);
        fill(lenBase, lenExtra, lenRuns, sizeof lenRuns, 0x400108ab);
    }
    // Slot bases grow by a delta that doubles after each run; the last slot ends at `last`.
    static void fill(uint32_t* base, uint8_t* extra, const uint8_t* runs, size_t nr, uint32_t last) {
        uint32_t order = 0, delta = 1, b = 0, slot = 0;
        for (size_t i = 0; i < nr; ++i) {
            for (uint32_t r = runs[i]; r--;) {
                b += delta;
                if (slot) extra[slot - 1] = (uint8_t)order;
                base[slot++] = b;
            }
            delta <<= 1;
            ++order;
        }
        base[slot] = last;
        uint32_t d = last - base[slot - 1], k = 0;
        while (d >> (k + 1)) ++k;
        extra[slot - 1] = (uint8_t)k;
    }
};

const LzmsTables& lzmsTables() {
    static const LzmsTables t;
    return t;
}

uint32_t lzmsOffsetSlots(size_t size) {
    if (size < 2) return 0;
    const LzmsTables& t = lzmsTables();
    uint32_t off = (uint32_t)std::min<size_t>(size - 1, 0x7ffffffe), slot = 0;
    while (slot + 1 < 799 && t.offBase[slot + 1] <= off) ++slot;
    return slot + 1;
}

// Code lengths for `n` symbols (all with nonzero frequency), limited to 15 bits.
void lzmsLengths(const uint32_t* freq, unsigned n, uint8_t* lens) {
    const uint32_t kSymBits = 10, kSymMask = (1u << kSymBits) - 1, kMax = 15;
    if (n == 0) return;
    std::vector<uint32_t> A(n);
    for (unsigned i = 0; i < n; ++i) A[i] = freq[i] << kSymBits | i;
    std::sort(A.begin(), A.end());  // by frequency, then by symbol
    std::memset(lens, 0, n);
    if (n == 1) {
        lens[A[0] & kSymMask] = 1;
        return;
    }
    // The tree, built in place: leaves are consumed from the front, internal nodes take their slots.
    unsigned i = 0, b = 0, e = 0;
    do {
        unsigned m, k;
        if (i != n && (b == e || (A[i] >> kSymBits) <= (A[b] >> kSymBits))) m = i++;
        else m = b++;
        if (i != n && (b == e || (A[i] >> kSymBits) <= (A[b] >> kSymBits))) k = i++;
        else k = b++;
        uint32_t f = (A[m] & ~kSymMask) + (A[k] & ~kSymMask);
        A[m] = (A[m] & kSymMask) | (e << kSymBits);
        A[k] = (A[k] & kSymMask) | (e << kSymBits);
        A[e] = (A[e] & kSymMask) | f;
        ++e;
    } while (n - e > 1);
    // How many codes of each length, with too-deep nodes pulled up to the limit.
    uint32_t counts[kMax + 2] = {};
    counts[1] = 2;
    unsigned root = n - 2;
    A[root] &= kSymMask;
    for (int node = (int)root - 1; node >= 0; --node) {
        unsigned parent = A[node] >> kSymBits;
        unsigned depth = (A[parent] >> kSymBits) + 1, len = depth;
        A[node] = (A[node] & kSymMask) | (depth << kSymBits);
        if (len >= kMax) {
            len = kMax;
            do --len;
            while (counts[len] == 0);
        }
        counts[len]--;
        counts[len + 1] += 2;
    }
    // Longest codes to the rarest symbols.
    i = 0;
    for (unsigned len = kMax; len >= 1; --len)
        for (uint32_t c = counts[len]; c--;) lens[A[i++] & kSymMask] = (uint8_t)len;
}

struct LzmsCode {
    unsigned n = 0, every = 0, left = 0;
    uint32_t freq[800];
    uint8_t lens[800];
    Huff h;
    void init(unsigned syms, unsigned rebuildEvery) {
        n = syms;
        every = rebuildEvery;
        for (unsigned i = 0; i < n; ++i) freq[i] = 1;
        rebuild();
    }
    void rebuild() {
        lzmsLengths(freq, n, lens);
        h.build(lens, n, 15);
        for (unsigned i = 0; i < n; ++i) freq[i] = (freq[i] >> 1) + 1;
        left = every;
    }
    template <class B>
    int decode(B& b) {
        if (!n) return -1;
        int s = h.decode(b);
        if (s < 0) return -1;
        ++freq[s];
        if (--left == 0) rebuild();
        return s;
    }
};

// The Huffman side: 16-bit little-endian words read from the end towards the start.
struct LzmsBits {
    const uint8_t* p;
    size_t next;  // words not read yet
    uint64_t buf = 0;
    int count = 0;
    void fill(int k) {
        while (count < k) {
            uint64_t w = 0;
            if (next) {
                --next;
                w = p[2 * next] | (uint32_t)p[2 * next + 1] << 8;
            }
            buf |= w << (48 - count);
            count += 16;
        }
    }
    uint32_t peek(int k) {
        if (count < k) fill(k);
        return k ? (uint32_t)(buf >> (64 - k)) : 0;
    }
    void skip(int k) {
        if (count < k) fill(k);
        buf <<= k;
        count -= k;
    }
    uint32_t get(int k) {
        uint32_t v = peek(k);
        skip(k);
        return v;
    }
};

struct LzmsProb {
    uint32_t zeros = 48;              // zero bits among the last 64
    uint64_t bits = 0x55555555ull;    // the last 64 bits, newest lowest
};

// The range-coded side: 16-bit little-endian words from the start.
struct LzmsRange {
    const uint8_t* p;
    size_t words, next = 2;
    uint32_t range = 0xffffffff, code;
    LzmsRange(const uint8_t* d, size_t w) : p(d), words(w) { code = word(0) << 16 | word(1); }
    uint32_t word(size_t i) const { return i < words ? p[2 * i] | (uint32_t)p[2 * i + 1] << 8 : 0; }
    int bit(uint32_t& state, uint32_t states, LzmsProb* probs) {
        LzmsProb& e = probs[state];
        uint32_t prob = e.zeros;
        if (prob == 0) prob = 1;
        if (prob >= 64) prob = 63;
        if (!(range & 0xFFFF0000u)) {
            range <<= 16;
            code <<= 16;
            if (next < words) code |= word(next++);
        }
        uint32_t bound = (range >> 6) * prob;
        int b;
        if (code < bound) {
            range = bound;
            b = 0;
        } else {
            range -= bound;
            code -= bound;
            b = 1;
        }
        state = ((state << 1) | (uint32_t)b) & (states - 1);
        e.zeros += (uint32_t)((int32_t)(e.bits >> 63) - b);
        e.bits = e.bits << 1 | (uint64_t)b;
        return b;
    }
};

// Undoes the compressor's x86 filter (relative call/jump/load targets made absolute).
void lzmsX86(uint8_t* data, size_t size) {
    if (size <= 17) return;
    const int32_t kIdWindow = 65535, kMaxTrans = 1023;
    std::vector<int32_t> lastUse(65536, -kIdWindow - 1);
    int32_t closest = -kMaxTrans - 1;
    size_t tail = size - 16;
    uint8_t saved = data[tail];
    data[tail] = 0xE8;  // a sentinel: no opcode in the last 16 bytes is translated
    size_t p = 0;
    for (;;) {
        while (p < tail && data[p] != 0x48 && data[p] != 0x4C && data[p] != 0xE8 && data[p] != 0xE9 &&
               data[p] != 0xF0 && data[p] != 0xFF)
            ++p;
        if (p >= tail) break;
        int32_t maxTrans = kMaxTrans, opLen = 0;
        switch (data[p]) {
            case 0x48:
                if ((data[p + 1] == 0x8B && (data[p + 2] == 0x5 || data[p + 2] == 0xD)) ||
                    (data[p + 1] == 0x8D && (data[p + 2] & 7) == 5))
                    opLen = 3;
                break;
            case 0x4C:
                if (data[p + 1] == 0x8D && (data[p + 2] & 7) == 5) opLen = 3;
                break;
            case 0xE8:
                opLen = 1;
                maxTrans /= 2;
                break;
            case 0xE9:
                p += 5;
                continue;
            case 0xF0:
                if (data[p + 1] == 0x83 && data[p + 2] == 0x05) opLen = 3;
                break;
            case 0xFF:
                if (data[p + 1] == 0x15) opLen = 2;
                break;
        }
        if (!opLen) {
            ++p;
            continue;
        }
        int32_t i = (int32_t)p;
        p += (size_t)opLen;
        if (i - closest <= maxTrans) {
            uint32_t n = Span(data + p, 4).u32(0) - (uint32_t)i;
            for (int k = 0; k < 4; ++k) data[p + k] = uint8_t(n >> (8 * k));
        }
        uint16_t target = (uint16_t)(i + (int32_t)Span(data + p, 2).u16(0));
        i += opLen + 3;
        if (i - lastUse[target] <= kIdWindow) closest = i;
        lastUse[target] = i;
        p += 4;
    }
    data[tail] = saved;
}

}  // namespace

bool unLzms(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.assign(expect, 0);
    if (expect == 0) return true;
    if (n < 4) return false;
    const LzmsTables& T = lzmsTables();
    size_t words = n / 2;
    LzmsRange rd(in, words);
    LzmsBits bs{in, words};
    LzmsProb pMain[16], pMatch[32], pLz[64], pLzRep[3][64], pDelta[64], pDeltaRep[3][64];
    uint32_t sMain = 0, sMatch = 0, sLz = 0, sLzRep[3] = {}, sDelta = 0, sDeltaRep[3] = {};
    uint32_t slots = lzmsOffsetSlots(expect);
    auto codes = std::make_unique<LzmsCode[]>(5);
    LzmsCode &lit = codes[0], &lzOff = codes[1], &lenC = codes[2], &dOff = codes[3], &dPow = codes[4];
    lit.init(256, 1024);
    lzOff.init(slots, 1024);
    lenC.init(54, 512);
    dOff.init(slots, 1024);
    dPow.init(8, 512);
    uint32_t lzRec[4] = {1, 2, 3, 4};
    uint64_t dRec[4] = {1, 2, 3, 4};
    unsigned prev = 0;  // 0 literal, 1 LZ match, 2 delta match (the queues update one item late)
    uint8_t* o = out.data();
    size_t pos = 0;
    auto length = [&]() -> int64_t {
        int s = lenC.decode(bs);
        if (s < 0) return -1;
        return (int64_t)T.lenBase[s] + bs.get(T.lenExtra[s]);
    };
    auto offset = [&](LzmsCode& c) -> int64_t {
        int s = c.decode(bs);
        if (s < 0) return -1;
        return (int64_t)T.offBase[s] + bs.get(T.offExtra[s]);
    };
    while (pos < expect) {
        if (!rd.bit(sMain, 16, pMain)) {
            int s = lit.decode(bs);
            if (s < 0) return false;
            o[pos++] = (uint8_t)s;
            prev = 0;
        } else if (!rd.bit(sMatch, 32, pMatch)) {
            uint32_t off;
            if (!rd.bit(sLz, 64, pLz)) {
                int64_t v = offset(lzOff);
                if (v < 0) return false;
                off = (uint32_t)v;
                lzRec[3] = lzRec[2];
                lzRec[2] = lzRec[1];
                lzRec[1] = lzRec[0];
            } else {
                unsigned d = prev & 1;
                if (!rd.bit(sLzRep[0], 64, pLzRep[0])) {
                    off = lzRec[0 + d];
                    lzRec[0 + d] = lzRec[0];
                } else if (!rd.bit(sLzRep[1], 64, pLzRep[1])) {
                    off = lzRec[1 + d];
                    lzRec[1 + d] = lzRec[1];
                    lzRec[1] = lzRec[0];
                } else {
                    off = lzRec[2 + d];
                    lzRec[2 + d] = lzRec[2];
                    lzRec[2] = lzRec[1];
                    lzRec[1] = lzRec[0];
                }
            }
            lzRec[0] = off;
            prev = 1;
            int64_t len = length();
            if (len <= 0 || (uint64_t)len > expect - pos || off == 0 || off > pos) return false;
            for (int64_t k = 0; k < len; ++k, ++pos) o[pos] = o[pos - off];
        } else {
            uint64_t pair;
            if (!rd.bit(sDelta, 64, pDelta)) {
                int pw = dPow.decode(bs);
                int64_t raw = offset(dOff);
                if (pw < 0 || raw < 0) return false;
                pair = (uint64_t)pw << 32 | (uint32_t)raw;
                dRec[3] = dRec[2];
                dRec[2] = dRec[1];
                dRec[1] = dRec[0];
            } else {
                unsigned d = prev >> 1;
                if (!rd.bit(sDeltaRep[0], 64, pDeltaRep[0])) {
                    pair = dRec[0 + d];
                    dRec[0 + d] = dRec[0];
                } else if (!rd.bit(sDeltaRep[1], 64, pDeltaRep[1])) {
                    pair = dRec[1 + d];
                    dRec[1 + d] = dRec[1];
                    dRec[1] = dRec[0];
                } else {
                    pair = dRec[2 + d];
                    dRec[2 + d] = dRec[2];
                    dRec[2] = dRec[1];
                    dRec[1] = dRec[0];
                }
            }
            dRec[0] = pair;
            prev = 2;
            int64_t len = length();
            uint32_t power = (uint32_t)(pair >> 32), raw = (uint32_t)pair;
            if (len <= 0 || power > 31) return false;
            uint64_t span = 1ull << power, off = (uint64_t)raw << power;
            if (off + span > pos || (uint64_t)len > expect - pos) return false;
            for (int64_t k = 0; k < len; ++k, ++pos)
                o[pos] = (uint8_t)(o[pos - off] + o[pos - span] - o[pos - off - span]);
        }
    }
    lzmsX86(o, expect);
    return true;
}

}  // namespace kz::native
