#include "kz_gpu.hpp"

#include <atomic>

namespace kz::gpu {

namespace {
std::atomic<bool> g_enabled{false};
}

void setEnabled(bool on) { g_enabled = on; }
bool enabled() { return g_enabled; }

}  // namespace kz::gpu

#ifndef _WIN32

namespace kz::gpu {
bool supported() { return false; }
bool available() { return false; }
void warmUp() {}
bool ready() { return false; }
bool unusable() { return true; }
std::string adapterName() { return {}; }
size_t batchBytes() { return 0; }
class Batch {};
void BatchDeleter::operator()(Batch* b) const { delete b; }
BatchPtr begin(const std::vector<DeflateJob*>&, int, bool& busy, std::string& err) {
    busy = false;
    err = "no graphics card support in this build";
    return nullptr;
}
bool finish(Batch&, std::string&) { return false; }
bool ready(Batch&) { return true; }
void encode(DeflateJob&) {}
}  // namespace kz::gpu

#else

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>

namespace kz::gpu {

namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kBlock = 32 * 1024;   // one GPU thread group's share; also one deflate block
constexpr uint32_t kWindow = 32 * 1024;  // deflate's reach back
constexpr size_t kBatch = 32 * 1024 * 1024;

// zlib's search settings per level: chain depth, length that ends the search, longest match that
// still waits a byte to look for a better one (0: greedy).
struct LevelCfg {
    uint32_t chain, nice, lazy;
};
// Levels 8 and 9 search 512 and 1024 deep rather than zlib's 1024 and 4096: Windows resets a card that
// works on one dispatch for more than about two seconds.
const LevelCfg kLevels[10] = {{4, 8, 0},     {4, 8, 0},      {8, 16, 0},     {32, 32, 0},      {16, 16, 4},
                              {32, 32, 16},  {128, 128, 16}, {256, 128, 32}, {512, 258, 128}, {1024, 258, 258}};

// ---------------------------------------------------------------------------------------------
// The compute shader. One group of 32 threads per block of up to 32 KiB, in rounds of 32
// positions from 32 KiB before the block (same piece, or the piece's dictionary) to its end:
//   1. every thread hashes its position and links it into a hash chain, as zlib does: a head per
//      hash holds the newest position, and each position's link the one before (card memory),
//   2. threads whose position is inside the block walk their chain for the longest match within
//      deflate's 32 KiB reach, as deep as the level asks (skipping positions already covered by a
//      chosen match),
//   3. thread 0 picks matches the way zlib's lazy parser does and writes tokens: a literal byte, or
//      0x80000000 | length << 16 | (distance - 1).
// Threads of one round link themselves in any order, so a chain may list a few same-round
// positions out of order; positions at or after the searcher's own are skipped.
const char kShader[] = R"HLSL(
ByteAddressBuffer src : register(t0);
StructuredBuffer<uint4> blk : register(t1);   // start, length, history start, first token
RWByteAddressBuffer tok : register(u0);
RWByteAddressBuffer cnt : register(u1);
RWByteAddressBuffer chain : register(u2);     // per group: 64Ki (link, 4 bytes) pairs, then 32Ki hash heads
cbuffer P : register(b0) { uint lazyLimit; uint niceLen; uint maxChain; uint blockOffset; };

#define HB 15
#define HN (1u << HB)
#define NONE 0xFFFFFFFFu
groupshared uint sl[32];
groupshared uint sd[32];
groupshared uint sNext;

uint ld4(uint a) {
    uint w = a & ~3u;
    uint s = (a & 3u) << 3;
    uint lo = src.Load(w);
    uint hi = src.Load(w + 4);
    return s != 0 ? ((lo >> s) | (hi << (32u - s))) : lo;
}
uint ld1(uint a) { return (src.Load(a & ~3u) >> ((a & 3u) << 3)) & 0xFFu; }
uint hsh(uint v) { return (v * 2654435761u) >> (32 - HB); }

uint matchLen(uint a, uint b, uint maxLen) {
    uint l = 4;
    while (l + 4 <= maxLen) {
        uint x = ld4(a + l) ^ ld4(b + l);
        if (x != 0) return l + (firstbitlow(x) >> 3);
        l += 4;
    }
    while (l < maxLen && ld1(a + l) == ld1(b + l)) l++;
    return l;
}

[numthreads(32, 1, 1)]
void main(uint3 g : SV_GroupID, uint t : SV_GroupIndex) {
    uint bi = g.x + blockOffset;
    uint4 b = blk[bi];
    uint start = b.x, end = b.x + b.y, look = b.z, outAt = b.w * 4;
    uint cb = g.x * (65536u * 2u + HN) * 4u;
    uint hb = cb + 65536u * 8u;
    for (uint i = t; i < HN; i += 32) chain.Store(hb + i * 4, NONE);
    if (t == 0) sNext = start;
    AllMemoryBarrierWithGroupSync();

    uint next = start, nTok = 0, pLen = 0, pDist = 0;
    bool pend = false;
    for (uint base = look; base < end; base += 32) {
        uint p = base + t;
        uint key = 0, c = NONE;
        bool hashed = p + 4 <= end;
        if (hashed) {
            key = ld4(p);
            chain.InterlockedExchange(hb + hsh(key) * 4, p - look, c);
            chain.Store2(cb + (p - look) * 8, uint2(c, key));
        }
        AllMemoryBarrierWithGroupSync();

        uint bl = 0, bd = 0;
        if (hashed && p >= start && p >= sNext) {
            uint maxLen = min(258u, end - p);
            for (uint steps = 0; c != NONE && steps < maxChain;) {
                uint cp = look + c;
                uint2 e = chain.Load2(cb + c * 8);  // the candidate's own link and first 4 bytes
                if (cp < p) {
                    uint d = p - cp;
                    if (d > 32768u) break;
                    // Same first 4 bytes, and (to beat the best so far) the same byte where it ended.
                    if (e.y == key && (bl == 0 || ld1(cp + bl) == ld1(p + bl))) {
                        uint l = matchLen(cp, p, maxLen);
                        if (l > bl) {
                            bl = l;
                            bd = d;
                            if (l >= niceLen || l == maxLen) break;
                        }
                    }
                    steps++;
                }
                c = e.x;
            }
        }
        sl[t] = bl;
        sd[t] = bd;
        GroupMemoryBarrierWithGroupSync();
        if (t == 0 && base + 32 > start) {
            uint j0 = base < start ? start - base : 0;
            uint n = min(32u, end - base);
            for (uint j = j0; j < n; j++) {
                uint q = base + j;
                uint l = sl[j], d = sd[j];
                if (pend) {
                    pend = false;
                    if (l > pLen) {  // a longer match one byte on: the byte before goes as a literal
                        tok.Store(outAt + nTok * 4, ld1(next));
                        nTok++;
                        next = q;
                    } else {
                        tok.Store(outAt + nTok * 4, 0x80000000u | (pLen << 16) | (pDist - 1));
                        nTok++;
                        next += pLen;
                        continue;
                    }
                } else if (q < next) {
                    continue;
                }
                if (l >= 4) {
                    if (l < lazyLimit) {
                        pend = true;
                        pLen = l;
                        pDist = d;
                    } else {
                        tok.Store(outAt + nTok * 4, 0x80000000u | (l << 16) | (d - 1));
                        nTok++;
                        next = q + l;
                    }
                } else {
                    tok.Store(outAt + nTok * 4, ld1(q));
                    nTok++;
                    next = q + 1;
                }
            }
            sNext = next;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (t == 0) {
        if (pend) {
            tok.Store(outAt + nTok * 4, 0x80000000u | (pLen << 16) | (pDist - 1));
            nTok++;
        }
        cnt.Store(bi * 4, nTok);
    }
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Deflate block writer (RFC 1951) for the tokens the card found.

const uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                               31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Tables {
    uint8_t lenSym[259];     // match length -> length code index (0..28)
    uint8_t distLo[256];     // distance-1 < 256 -> distance code
    uint8_t distHi[256];     // (distance-1) >> 7 -> distance code
    uint8_t fixedLit[288], fixedDist[30];
    uint16_t fixedLitCode[288], fixedDistCode[30];
    Tables();
};

uint16_t reverseBits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++, v >>= 1) r = (r << 1) | (v & 1);
    return (uint16_t)r;
}

// Canonical codes (bit-reversed, since deflate sends codes starting from their top bit).
void makeCodes(const uint8_t* len, int n, uint16_t* code) {
    int count[16] = {0}, next[16] = {0};
    for (int i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    for (int b = 1, c = 0; b < 16; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = c;
    }
    for (int i = 0; i < n; i++)
        code[i] = len[i] ? reverseBits((uint32_t)next[len[i]]++, len[i]) : 0;
}

Tables::Tables() {
    for (int c = 0; c < 29; c++)
        for (int l = kLenBase[c]; l < kLenBase[c] + (1 << kLenExtra[c]) && l <= 258; l++) lenSym[l] = (uint8_t)c;
    lenSym[258] = 28;
    for (int c = 0; c < 30; c++)
        for (int d = kDistBase[c]; d < kDistBase[c] + (1 << kDistExtra[c]) && d <= 32768; d++) {
            if (d - 1 < 256) distLo[d - 1] = (uint8_t)c;
            else distHi[(d - 1) >> 7] = (uint8_t)c;
        }
    for (int i = 0; i < 288; i++) fixedLit[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    for (int i = 0; i < 30; i++) fixedDist[i] = 5;
    makeCodes(fixedLit, 288, fixedLitCode);
    makeCodes(fixedDist, 30, fixedDistCode);
}

const Tables& tables() {
    static const Tables t;
    return t;
}

inline int distSym(uint32_t d) {
    const Tables& T = tables();
    return d - 1 < 256 ? T.distLo[d - 1] : T.distHi[(d - 1) >> 7];
}

// Huffman code lengths no longer than `limit` (lengths over it are folded back the way miniz does).
void huffLengths(const uint32_t* freq, int n, int limit, uint8_t* len) {
    struct S {
        uint32_t f;
        uint16_t s;
    };
    S sym[288];
    int m = 0;
    for (int i = 0; i < n; i++) {
        len[i] = 0;
        if (freq[i]) sym[m++] = {freq[i], (uint16_t)i};
    }
    if (m == 0) return;
    if (m == 1) {
        len[sym[0].s] = 1;
        return;
    }
    std::sort(sym, sym + m, [](const S& a, const S& b) { return a.f != b.f ? a.f < b.f : a.s < b.s; });
    // Two-queue Huffman: leaves are sorted and new internal nodes come out in increasing weight.
    uint64_t w[576];
    int parent[576];
    for (int i = 0; i < m; i++) w[i] = sym[i].f;
    int li = 0, ii = m, nn = m;
    auto take = [&]() {
        if (li < m && (ii >= nn || w[li] <= w[ii])) return li++;
        return ii++;
    };
    for (int k = 0; k < m - 1; k++) {
        int a = take(), b = take();
        w[nn] = w[a] + w[b];
        parent[a] = parent[b] = nn;
        nn++;
    }
    int depth[576];
    depth[nn - 1] = 0;
    for (int i = nn - 2; i >= 0; i--) depth[i] = depth[parent[i]] + 1;
    int count[300] = {0};
    int maxd = 0;
    for (int i = 0; i < m; i++) {
        count[depth[i]]++;
        maxd = std::max(maxd, depth[i]);
    }
    if (maxd > limit) {
        for (int d = limit + 1; d <= maxd; d++) {
            count[limit] += count[d];
            count[d] = 0;
        }
        uint32_t total = 0;
        for (int d = limit; d > 0; d--) total += (uint32_t)count[d] << (limit - d);
        while (total != (1u << limit)) {
            count[limit]--;
            for (int d = limit - 1; d > 0; d--)
                if (count[d]) {
                    count[d]--;
                    count[d + 1] += 2;
                    break;
                }
            total--;
        }
        maxd = limit;
    }
    // The rarest symbols get the longest codes.
    int i = 0;
    for (int d = maxd; d > 0; d--)
        for (int c = count[d]; c > 0; c--) len[sym[i++].s] = (uint8_t)d;
}

struct BitWriter {
    std::vector<uint8_t>& o;
    uint64_t acc = 0;
    int n = 0;
    explicit BitWriter(std::vector<uint8_t>& out) : o(out) {}
    void put(uint32_t v, int bits) {
        acc |= (uint64_t)v << n;
        n += bits;
        if (n >= 32) {
            uint8_t b[4] = {(uint8_t)acc, (uint8_t)(acc >> 8), (uint8_t)(acc >> 16), (uint8_t)(acc >> 24)};
            o.insert(o.end(), b, b + 4);
            acc >>= 32;
            n -= 32;
        }
    }
    void align() {
        while (n > 0) {
            o.push_back((uint8_t)acc);
            acc >>= 8;
            n -= 8;
        }
        n = 0;
        acc = 0;
    }
};

inline bool isMatch(uint32_t t) { return (t & 0x80000000u) != 0; }
inline uint32_t tLen(uint32_t t) { return (t >> 16) & 0x1FF; }
inline uint32_t tDist(uint32_t t) { return (t & 0xFFFF) + 1; }

// One deflate block for `n` tokens covering raw[0..rawLen): dynamic, fixed or stored, whichever is
// smallest.
void writeBlock(BitWriter& bw, const uint32_t* tk, uint32_t n, const uint8_t* raw, uint32_t rawLen, bool last) {
    const Tables& T = tables();
    uint32_t lf[286] = {0}, df[30] = {0};
    uint64_t extra = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = tk[i];
        if (isMatch(t)) {
            int ls = T.lenSym[tLen(t)], ds = distSym(tDist(t));
            lf[257 + ls]++;
            df[ds]++;
            extra += kLenExtra[ls] + kDistExtra[ds];
        } else {
            lf[t & 0xFF]++;
        }
    }
    lf[256] = 1;

    uint8_t ll[286], dl[30];
    huffLengths(lf, 286, 15, ll);
    huffLengths(df, 30, 15, dl);
    int hlit = 286;
    while (hlit > 257 && ll[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && dl[hdist - 1] == 0) hdist--;
    if (dl[0] == 0 && hdist == 1) dl[0] = 1;  // at least one distance code must be described

    // Code lengths, run-length coded: symbol | repeat << 8.
    uint8_t all[316];
    std::memcpy(all, ll, hlit);
    std::memcpy(all + hlit, dl, hdist);
    int total = hlit + hdist;
    uint32_t rle[316];
    int nr = 0;
    uint32_t cf[19] = {0};
    for (int i = 0; i < total;) {
        uint8_t v = all[i];
        int run = 1;
        while (i + run < total && all[i + run] == v) run++;
        int left = run;
        if (v == 0) {
            while (left >= 11) {
                int r = std::min(left, 138);
                rle[nr++] = 18u | (uint32_t)(r - 11) << 8;
                cf[18]++;
                left -= r;
            }
            if (left >= 3) {
                rle[nr++] = 17u | (uint32_t)(left - 3) << 8;
                cf[17]++;
                left = 0;
            }
        } else {
            rle[nr++] = v;
            cf[v]++;
            left--;
            while (left >= 3) {
                int r = std::min(left, 6);
                rle[nr++] = 16u | (uint32_t)(r - 3) << 8;
                cf[16]++;
                left -= r;
            }
        }
        while (left-- > 0) {
            rle[nr++] = v;
            cf[v]++;
        }
        i += run;
    }
    uint8_t cl[19];
    huffLengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && cl[kClOrder[hclen - 1]] == 0) hclen--;

    uint64_t dyn = 3 + 14 + 3ull * hclen + extra;
    for (int s = 0; s < 19; s++) dyn += (uint64_t)cf[s] * cl[s];
    dyn += 2ull * cf[16] + 3ull * cf[17] + 7ull * cf[18];
    uint64_t fix = 3 + extra;
    for (int s = 0; s < 286; s++) {
        dyn += (uint64_t)lf[s] * ll[s];
        fix += (uint64_t)lf[s] * T.fixedLit[s];
    }
    for (int s = 0; s < 30; s++) {
        dyn += (uint64_t)df[s] * dl[s];
        fix += (uint64_t)df[s] * 5;
    }
    uint64_t stored = 3 + 7 + 32 + 8ull * rawLen;

    if (stored <= dyn && stored <= fix) {
        bw.put(last ? 1 : 0, 3);
        bw.align();
        uint8_t h[4] = {(uint8_t)rawLen, (uint8_t)(rawLen >> 8), (uint8_t)~rawLen, (uint8_t)(~rawLen >> 8)};
        bw.o.insert(bw.o.end(), h, h + 4);
        bw.o.insert(bw.o.end(), raw, raw + rawLen);
        return;
    }

    const uint8_t* L;
    const uint8_t* D;
    uint16_t lc[288], dc[30];
    const uint16_t* LC;
    const uint16_t* DC;
    if (fix < dyn) {
        bw.put((last ? 1 : 0) | (1 << 1), 3);
        L = T.fixedLit;
        D = T.fixedDist;
        LC = T.fixedLitCode;
        DC = T.fixedDistCode;
    } else {
        bw.put((last ? 1 : 0) | (2 << 1), 3);
        bw.put((uint32_t)(hlit - 257), 5);
        bw.put((uint32_t)(hdist - 1), 5);
        bw.put((uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++) bw.put(cl[kClOrder[i]], 3);
        uint16_t cc[19];
        makeCodes(cl, 19, cc);
        for (int i = 0; i < nr; i++) {
            uint32_t s = rle[i] & 0xFF, r = rle[i] >> 8;
            bw.put(cc[s], cl[s]);
            if (s == 16) bw.put(r, 2);
            else if (s == 17) bw.put(r, 3);
            else if (s == 18) bw.put(r, 7);
        }
        makeCodes(ll, 286, lc);
        makeCodes(dl, 30, dc);
        L = ll;
        D = dl;
        LC = lc;
        DC = dc;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = tk[i];
        if (isMatch(t)) {
            uint32_t len = tLen(t), dist = tDist(t);
            int ls = T.lenSym[len], ds = distSym(dist);
            bw.put(LC[257 + ls], L[257 + ls]);
            if (kLenExtra[ls]) bw.put(len - kLenBase[ls], kLenExtra[ls]);
            bw.put(DC[ds], D[ds]);
            if (kDistExtra[ds]) bw.put(dist - kDistBase[ds], kDistExtra[ds]);
        } else {
            bw.put(LC[t & 0xFF], L[t & 0xFF]);
        }
    }
    bw.put(LC[256], L[256]);
}

// ---------------------------------------------------------------------------------------------
// Direct3D 11 device, loaded at run time so a PC without it simply goes without.

struct Blk {
    uint32_t start, len, look, tok;
};

// One batch's buffers. Two slots, so one batch can be uploaded or read back while the card works on
// the other.
struct Slot {
    bool busy = false;
    size_t srcCap = 0, blkCap = 0, tokCap = 0;
    ComPtr<ID3D11Buffer> upload, src, blk, tok, cnt, tokRead, cntRead;
    ComPtr<ID3D11ShaderResourceView> srcView, blkView;
    ComPtr<ID3D11UnorderedAccessView> tokView, cntView;
    ComPtr<ID3D11Fence> fence;
    uint64_t fenceValue = 0;
    HANDLE event = nullptr;
};

struct Device {
    std::mutex m;
    bool tried = false, ok = false, broken = false;
    std::string name;
    size_t vram = 0;
    uint32_t groupsPerDispatch = 512;
    HMODULE d3d = nullptr, dxgi = nullptr, compiler = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer> params, chain;
    ComPtr<ID3D11UnorderedAccessView> chainView;
    Slot slots[2];

    bool init();
    bool ensure(Slot& s, size_t srcBytes, size_t blocks, size_t tokens);
};

Device& device() {
    static Device* d = new Device;  // never destroyed: a warm-up thread may still be using it at exit
    return *d;
}

bool makeBuffer(ID3D11Device* dev, size_t bytes, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc,
                ComPtr<ID3D11Buffer>& buf) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (UINT)bytes;
    d.Usage = usage;
    d.BindFlags = bind;
    d.CPUAccessFlags = cpu;
    d.MiscFlags = misc;
    if (misc & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) d.StructureByteStride = 16;
    return SUCCEEDED(dev->CreateBuffer(&d, nullptr, buf.ReleaseAndGetAddressOf()));
}

bool makeRaw(ID3D11Device* dev, size_t bytes, UINT bind, ComPtr<ID3D11Buffer>& buf) {
    return makeBuffer(dev, bytes, D3D11_USAGE_DEFAULT, bind, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, buf);
}

bool rawSrv(ID3D11Device* dev, ID3D11Buffer* buf, size_t bytes, ComPtr<ID3D11ShaderResourceView>& v) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    d.BufferEx.NumElements = (UINT)(bytes / 4);
    d.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    return SUCCEEDED(dev->CreateShaderResourceView(buf, &d, v.ReleaseAndGetAddressOf()));
}

bool rawUav(ID3D11Device* dev, ID3D11Buffer* buf, size_t bytes, ComPtr<ID3D11UnorderedAccessView>& v) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(bytes / 4);
    d.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(dev->CreateUnorderedAccessView(buf, &d, v.ReleaseAndGetAddressOf()));
}

size_t roundUp(size_t v, size_t to) { return (v + to - 1) / to * to; }

constexpr size_t kChainPerGroup = (65536 * 2 + 32768) * 4;  // links with keys, then hash heads

// Compiling the shader takes a few hundred milliseconds, so the compiled code is kept in
// %LOCALAPPDATA%\Kompressor\gpu, named after a hash of the source (a changed shader gets a new file).
std::wstring shaderCachePath() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (const char* c = kShader; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
    wchar_t name[64];
    swprintf(name, 64, L"deflate-%016llx.cso", (unsigned long long)h);
    std::wstring dir = std::wstring(base) + L"\\Kompressor";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\gpu";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}

std::vector<char> readAll(const std::wstring& path) {
    std::vector<char> out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1 << 24)) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) || got != out.size()) out.clear();
    }
    CloseHandle(f);
    return out;
}

void writeAll(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, data, (DWORD)n, &put, nullptr) && put == n;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

bool Device::init() {
    d3d = LoadLibraryW(L"d3d11.dll");
    dxgi = LoadLibraryW(L"dxgi.dll");
    compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!d3d || !dxgi || !compiler) return false;
    auto createDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(d3d, "D3D11CreateDevice");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto createFactory = (CreateFactory)GetProcAddress(dxgi, "CreateDXGIFactory1");
    auto compile = (pD3DCompile)GetProcAddress(compiler, "D3DCompile");
    if (!createDevice || !createFactory || !compile) return false;

    // The card with the most memory of its own (skips the software renderer and virtual displays,
    // which have none).
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(createFactory(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf()))) return false;
    ComPtr<IDXGIAdapter1> best;
    DXGI_ADAPTER_DESC1 bestDesc{};
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; factory->EnumAdapters1(i, a.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (FAILED(a->GetDesc1(&desc))) continue;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || desc.VendorId == 0x1414) continue;
        if (!best || desc.DedicatedVideoMemory > bestDesc.DedicatedVideoMemory) {
            best = a;
            bestDesc = desc;
        }
    }
    if (!best) return false;
    D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    // Every use is behind our own lock, and the driver's helper threads would only compete with the
    // compression threads (they made opening the card take seconds while every core was busy).
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    if (FAILED(createDevice(best.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, want, 2, D3D11_SDK_VERSION,
                            dev.GetAddressOf(), &got, ctx.GetAddressOf())))
        return false;
    // Fences (Windows 10 1703 and later) let a thread sleep until the card is done instead of
    // holding the device while it waits.
    if (FAILED(dev.As(&dev5)) || FAILED(ctx.As(&ctx4))) {
        dev5.Reset();
        ctx4.Reset();
    }

    std::wstring cachePath = shaderCachePath();
    std::vector<char> cached = cachePath.empty() ? std::vector<char>() : readAll(cachePath);
    if (cached.empty() ||
        FAILED(dev->CreateComputeShader(cached.data(), cached.size(), nullptr, cs.ReleaseAndGetAddressOf()))) {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(compile(kShader, sizeof(kShader) - 1, "kz_deflate", nullptr, nullptr, "main", "cs_5_0",
                           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf()))) {
            return false;
        }
        if (FAILED(dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                                            cs.ReleaseAndGetAddressOf())))
            return false;
        if (!cachePath.empty()) writeAll(cachePath, code->GetBufferPointer(), code->GetBufferSize());
    }
    if (!makeBuffer(dev.Get(), 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, params))
        return false;

    // As many groups at once as fill a big card, in at most an eighth of its memory.
    vram = bestDesc.DedicatedVideoMemory;
    groupsPerDispatch = (uint32_t)std::clamp<size_t>(vram / 8 / kChainPerGroup, 128, 2048);
    size_t bytes = groupsPerDispatch * kChainPerGroup;
    if (!makeRaw(dev.Get(), bytes, D3D11_BIND_UNORDERED_ACCESS, chain) || !rawUav(dev.Get(), chain.Get(), bytes, chainView))
        return false;

    char nm[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, bestDesc.Description, -1, nm, sizeof nm, nullptr, nullptr);
    name = n > 0 ? std::string(nm) : "graphics card";
    return true;
}

bool Device::ensure(Slot& s, size_t srcBytes, size_t blocks, size_t tokens) {
    ID3D11Device* D = dev.Get();
    if (!s.event && dev5) {
        if (FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), (void**)s.fence.GetAddressOf())))
            return false;
        s.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // manual reset: ready() only peeks
        if (!s.event) return false;
    }
    if (srcBytes > s.srcCap) {
        size_t cap = roundUp(std::max(srcBytes, kBatch + kBatch / 8), 1 << 20);
        if (!makeRaw(D, cap, D3D11_BIND_SHADER_RESOURCE, s.src) || !rawSrv(D, s.src.Get(), cap, s.srcView) ||
            !makeBuffer(D, cap, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE, 0, s.upload))
            return false;
        s.srcCap = cap;
    }
    if (blocks > s.blkCap) {
        size_t cap = roundUp(std::max<size_t>(blocks, 2048), 1024);
        if (!makeBuffer(D, cap * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, s.blk))
            return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = (UINT)cap;
        if (FAILED(D->CreateShaderResourceView(s.blk.Get(), &v, s.blkView.ReleaseAndGetAddressOf()))) return false;
        if (!makeRaw(D, cap * 4, D3D11_BIND_UNORDERED_ACCESS, s.cnt) || !rawUav(D, s.cnt.Get(), cap * 4, s.cntView) ||
            !makeBuffer(D, cap * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, s.cntRead))
            return false;
        s.blkCap = cap;
    }
    if (tokens > s.tokCap) {
        size_t cap = roundUp(std::max(tokens, kBatch + kBatch / 8), 1 << 18);
        if (!makeRaw(D, cap * 4, D3D11_BIND_UNORDERED_ACCESS, s.tok) || !rawUav(D, s.tok.Get(), cap * 4, s.tokView) ||
            !makeBuffer(D, cap * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, s.tokRead))
            return false;
        s.tokCap = cap;
    }
    return true;
}

}  // namespace

class Batch {
public:
    Slot* slot = nullptr;
    std::vector<DeflateJob*> jobs;
    std::vector<Blk> blocks;
    std::vector<size_t> firstBlock;
    size_t tokens = 0;
    bool done = false;
};

void BatchDeleter::operator()(Batch* b) const {
    if (!b) return;
    if (b->slot && !b->done) {  // dropped unfinished: wait for the card before the slot is reused
        std::string err;
        finish(*b, err);
    }
    delete b;
}

bool supported() { return true; }

std::atomic<int> g_state{0};  // 0 not started, 1 starting, 2 ready, 3 not usable

bool available() {
    Device& d = device();
    std::lock_guard<std::mutex> lk(d.m);
    if (!d.tried) {
        d.tried = true;
        g_state = 1;
        d.ok = d.init();
    }
    bool ok = d.ok && !d.broken;
    g_state = ok ? 2 : 3;
    return ok;
}

void warmUp() {
    int expect = 0;
    if (g_state.compare_exchange_strong(expect, 1))
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);  // ahead of busy workers
            available();
        }).detach();
}

bool ready() { return g_state == 2 && !device().broken; }

bool unusable() { return g_state == 3 || (g_state == 2 && device().broken); }

std::string adapterName() {
    Device& d = device();
    std::lock_guard<std::mutex> lk(d.m);
    return d.ok ? d.name : std::string();
}

size_t batchBytes() { return kBatch; }

BatchPtr begin(const std::vector<DeflateJob*>& jobs, int level, bool& busy, std::string& err) {
    busy = false;
    if (!available()) {
        err = "no usable graphics card";
        return nullptr;
    }
    Device& D = device();
    BatchPtr b(new Batch);
    b->jobs = jobs;

    // Lay the jobs out one after another, each as [history | data], and cut them into blocks.
    b->firstBlock.resize(jobs.size() + 1);
    size_t srcBytes = 0;
    for (size_t j = 0; j < jobs.size(); j++) {
        const DeflateJob& job = *jobs[j];
        size_t hist = std::min<size_t>(job.dictLen, kWindow);
        size_t slot = srcBytes, dataAt = slot + hist;
        b->firstBlock[j] = b->blocks.size();
        for (size_t off = 0; off < job.len; off += kBlock) {
            Blk k;
            k.start = (uint32_t)(dataAt + off);
            k.len = (uint32_t)std::min<size_t>(kBlock, job.len - off);
            k.look = (uint32_t)std::max<size_t>(slot, dataAt + off >= kWindow ? dataAt + off - kWindow : 0);
            k.tok = (uint32_t)b->tokens;
            b->tokens += k.len;
            b->blocks.push_back(k);
        }
        srcBytes = roundUp(dataAt + job.len, 4);
    }
    b->firstBlock[jobs.size()] = b->blocks.size();
    if (b->blocks.empty()) {  // nothing for the card (empty pieces)
        b->done = true;
        return b;
    }
    size_t srcAlloc = srcBytes + 16;  // the shader reads a word past the end
    if (srcBytes > 0xF0000000ull || b->tokens > 0x3C000000ull) {
        err = "batch too big for the graphics card";
        return nullptr;
    }

    std::lock_guard<std::mutex> lk(D.m);
    auto fail = [&](const char* what) {
        D.broken = true;
        err = std::string("graphics card: ") + what;
        return nullptr;
    };
    Slot* s = !D.slots[0].busy ? &D.slots[0] : !D.slots[1].busy ? &D.slots[1] : nullptr;
    if (!s) {
        busy = true;
        return nullptr;
    }
    if (!D.ensure(*s, srcAlloc, b->blocks.size(), b->tokens)) return fail("out of memory");

    D3D11_MAPPED_SUBRESOURCE mu;
    if (FAILED(D.ctx->Map(s->upload.Get(), 0, D3D11_MAP_WRITE, 0, &mu))) return fail("lost the device");
    uint8_t* up = (uint8_t*)mu.pData;
    for (size_t j = 0; j < jobs.size(); j++) {
        const DeflateJob& job = *jobs[j];
        if (b->firstBlock[j] == b->firstBlock[j + 1]) continue;
        size_t hist = std::min<size_t>(job.dictLen, kWindow);
        size_t dataAt = b->blocks[b->firstBlock[j]].start;
        if (hist) std::memcpy(up + dataAt - hist, job.dict + job.dictLen - hist, hist);
        std::memcpy(up + dataAt, job.data, job.len);
    }
    std::memset(up + srcBytes, 0, 16);
    D.ctx->Unmap(s->upload.Get(), 0);
    D3D11_BOX ub{0, 0, 0, (UINT)srcAlloc, 1, 1};
    D.ctx->CopySubresourceRegion(s->src.Get(), 0, 0, 0, 0, s->upload.Get(), 0, &ub);
    D3D11_BOX bbox{0, 0, 0, (UINT)(b->blocks.size() * 16), 1, 1};
    D.ctx->UpdateSubresource(s->blk.Get(), 0, &bbox, b->blocks.data(), 0, 0);

    ID3D11ShaderResourceView* srvs[2] = {s->srcView.Get(), s->blkView.Get()};
    ID3D11UnorderedAccessView* uavs[3] = {s->tokView.Get(), s->cntView.Get(), D.chainView.Get()};
    ID3D11Buffer* cbs[1] = {D.params.Get()};
    D.ctx->CSSetShader(D.cs.Get(), nullptr, 0);
    D.ctx->CSSetShaderResources(0, 2, srvs);
    D.ctx->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    const LevelCfg& lc = kLevels[std::clamp(level, 1, 9)];
    // Slices of groups share the chain memory, one slice after another.
    for (size_t at = 0; at < b->blocks.size(); at += D.groupsPerDispatch) {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(D.ctx->Map(D.params.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return fail("lost the device");
        uint32_t prm[4] = {lc.lazy, lc.nice, lc.chain, (uint32_t)at};
        std::memcpy(ms.pData, prm, sizeof prm);
        D.ctx->Unmap(D.params.Get(), 0);
        D.ctx->CSSetConstantBuffers(0, 1, cbs);
        D.ctx->Dispatch((UINT)std::min<size_t>(D.groupsPerDispatch, b->blocks.size() - at), 1, 1);
    }
    ID3D11ShaderResourceView* nos[2] = {nullptr, nullptr};
    ID3D11UnorderedAccessView* nou[3] = {nullptr, nullptr, nullptr};
    D.ctx->CSSetShaderResources(0, 2, nos);
    D.ctx->CSSetUnorderedAccessViews(0, 3, nou, nullptr);
    D3D11_BOX cb{0, 0, 0, (UINT)(b->blocks.size() * 4), 1, 1};
    D.ctx->CopySubresourceRegion(s->cntRead.Get(), 0, 0, 0, 0, s->cnt.Get(), 0, &cb);
    D3D11_BOX tb{0, 0, 0, (UINT)(b->tokens * 4), 1, 1};
    D.ctx->CopySubresourceRegion(s->tokRead.Get(), 0, 0, 0, 0, s->tok.Get(), 0, &tb);
    if (s->fence) {
        ResetEvent(s->event);
        D.ctx4->Signal(s->fence.Get(), ++s->fenceValue);
        if (FAILED(s->fence->SetEventOnCompletion(s->fenceValue, s->event))) return fail("lost the device");
    }
    D.ctx->Flush();
    s->busy = true;
    b->slot = s;
    return b;
}

bool ready(Batch& b) {
    if (b.done || !b.slot || !b.slot->event) return true;
    return WaitForSingleObject(b.slot->event, 0) == WAIT_OBJECT_0;
}

bool finish(Batch& b, std::string& err) {
    if (b.done) return true;
    b.done = true;
    Device& D = device();
    Slot& s = *b.slot;
    if (s.event) WaitForSingleObject(s.event, INFINITE);  // sleeps until the card is done

    std::lock_guard<std::mutex> lk(D.m);
    s.busy = false;
    auto fail = [&](const char* what) {
        D.broken = true;
        err = std::string("graphics card: ") + what;
        return false;
    };
    if (D.broken) return fail("failed earlier");
    D3D11_MAPPED_SUBRESOURCE mc{}, mt{};
    if (FAILED(D.ctx->Map(s.cntRead.Get(), 0, D3D11_MAP_READ, 0, &mc))) return fail("lost the device");
    if (FAILED(D.ctx->Map(s.tokRead.Get(), 0, D3D11_MAP_READ, 0, &mt))) {
        D.ctx->Unmap(s.cntRead.Get(), 0);
        return fail("lost the device");
    }
    const uint32_t* counts = (const uint32_t*)mc.pData;
    const uint32_t* toks = (const uint32_t*)mt.pData;
    bool bad = false;
    for (size_t j = 0; j < b.jobs.size() && !bad; j++) {
        DeflateJob& job = *b.jobs[j];
        size_t b0 = b.firstBlock[j], b1 = b.firstBlock[j + 1];
        size_t total = 0;
        job.blockTokens.assign(b1 - b0, 0);
        for (size_t k = b0; k < b1; k++) {
            uint32_t n = counts[k];
            if (n > b.blocks[k].len) bad = true;
            job.blockTokens[k - b0] = n;
            total += n;
        }
        if (bad) break;
        job.tokens.resize(total);
        uint32_t* to = job.tokens.data();
        for (size_t k = b0; k < b1; k++) {
            std::memcpy(to, toks + b.blocks[k].tok, counts[k] * 4ull);
            to += counts[k];
        }
    }
    D.ctx->Unmap(s.cntRead.Get(), 0);
    D.ctx->Unmap(s.tokRead.Get(), 0);
    if (bad) return fail("returned bad data");
    return true;
}

void encode(DeflateJob& job) {
    job.out.clear();
    job.out.reserve(job.len / 2 + 64);
    BitWriter bw(job.out);
    size_t nb = job.blockTokens.size();
    const uint32_t* tk = job.tokens.data();
    for (size_t k = 0; k < nb; k++) {
        size_t off = k * kBlock;
        uint32_t len = (uint32_t)std::min<size_t>(kBlock, job.len - off);
        writeBlock(bw, tk, job.blockTokens[k], job.data + off, len, job.final && k + 1 == nb);
        tk += job.blockTokens[k];
    }
    if (job.final && nb == 0) {
        bw.put(1 | (1 << 1), 3);  // an empty final block
        bw.put(0, 7);
    } else if (!job.final) {
        bw.put(0, 3);  // empty stored block: the sync-flush marker, ending on a byte boundary
        bw.align();
        uint8_t m[4] = {0, 0, 0xFF, 0xFF};
        job.out.insert(job.out.end(), m, m + 4);
    }
    bw.align();
    std::vector<uint32_t>().swap(job.tokens);
}

}  // namespace kz::gpu

#endif
