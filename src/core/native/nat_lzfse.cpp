// Apple's LZVN (byte-oriented LZ77) and LZFSE (LZ77 with finite-state entropy coding), used by
// compressed HFS+/APFS files and DMG disk images. Decoders only.
#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

// ============================================================================================== LZVN
// Decodes one LZVN stream (up to its end-of-stream opcode) onto `out`. False when damaged.
bool lzvnRun(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t limit) {
    size_t i = 0;
    uint32_t D = 0;
    auto lit = [&](uint32_t L) {
        if (i + L > n) return false;
        out.insert(out.end(), in + i, in + i + L);
        i += L;
        return true;
    };
    auto match = [&](uint32_t M) {
        if (D == 0 || D > out.size()) return false;
        size_t from = out.size() - D;
        for (uint32_t k = 0; k < M; ++k) out.push_back(out[from + k]);
        return true;
    };
    while (i < n) {
        if (out.size() > limit) return false;
        uint8_t b = in[i];
        uint32_t L = 0, M = 0;
        if (b >= 0xF0) {  // match with the previous distance
            if (b == 0xF0) {
                if (i + 1 >= n) return false;
                M = in[i + 1] + 16u;
                i += 2;
            } else {
                M = b & 15;
                i += 1;
            }
            if (!match(M)) return false;
            continue;
        }
        if (b >= 0xE0) {  // literals only
            if (b == 0xE0) {
                if (i + 1 >= n) return false;
                L = in[i + 1] + 16u;
                i += 2;
            } else {
                L = b & 15;
                i += 1;
            }
            if (!lit(L)) return false;
            continue;
        }
        if (b >= 0xA0 && b <= 0xBF) {  // medium distance
            if (i + 2 >= n) return false;
            uint32_t w = in[i + 1] | in[i + 2] << 8;
            L = (b >> 3) & 3;
            M = (((b & 7u) << 2) | (w & 3)) + 3;
            i += 3;
            if (!lit(L)) return false;
            D = w >> 2;
            if (!match(M)) return false;
            continue;
        }
        if ((b >= 0x70 && b <= 0x7F) || (b >= 0xD0 && b <= 0xDF)) return false;  // undefined
        L = b >> 6;
        M = ((b >> 3) & 7) + 3;
        switch (b & 7) {
            case 6:  // previous distance (with no literals: end of stream / no-ops)
                if (b == 0x06) return true;
                if (b == 0x0E || b == 0x16) {
                    ++i;
                    continue;
                }
                if (L == 0) return false;
                ++i;
                if (!lit(L) || !match(M)) return false;
                break;
            case 7:  // large distance
                if (i + 2 >= n) return false;
                D = in[i + 1] | in[i + 2] << 8;
                i += 3;
                if (!lit(L) || !match(M)) return false;
                break;
            default:  // small distance
                if (i + 1 >= n) return false;
                D = (b & 7u) << 8 | in[i + 1];
                i += 2;
                if (!lit(L) || !match(M)) return false;
                break;
        }
    }
    return true;  // ran out of input without an end marker: take what there is
}

// ============================================================================================= LZFSE
const uint8_t kLBits[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 5, 8};
const int32_t kLBase[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 28, 60};
const uint8_t kMBits[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 8, 11};
const int32_t kMBase[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 24, 56, 312};

struct DTables {
    uint8_t bits[64];
    int32_t base[64];
    DTables() {
        int32_t b = 0;
        for (int i = 0; i < 64; ++i) {
            bits[i] = (uint8_t)(i < 4 ? 0 : i / 4 - 1);
            base[i] = b;
            b += 1 << bits[i];
        }
    }
};
const DTables& dTables() {
    static const DTables t;
    return t;
}

int clz32(uint32_t x) {
    int n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) x <<= 1, ++n;
    return n;
}

struct LitEntry {
    uint8_t symbol;
    int8_t k;
    int16_t delta;
};
struct ValEntry {
    uint8_t total, vbits;
    int16_t delta;
    int32_t vbase;
};

bool buildLit(int states, const uint16_t* freq, int nsym, std::vector<LitEntry>& t) {
    t.assign((size_t)states, {0, 0, 0});
    int nclz = clz32((uint32_t)states), sum = 0;
    size_t at = 0;
    for (int i = 0; i < nsym; ++i) {
        int f = freq[i];
        if (!f) continue;
        sum += f;
        if (sum > states) return false;
        int k = clz32((uint32_t)f) - nclz, j0 = ((2 * states) >> k) - f;
        for (int j = 0; j < f; ++j) {
            LitEntry e;
            e.symbol = (uint8_t)i;
            if (j < j0) e.k = (int8_t)k, e.delta = (int16_t)(((f + j) << k) - states);
            else e.k = (int8_t)(k - 1), e.delta = (int16_t)((j - j0) << (k - 1));
            t[at++] = e;
        }
    }
    return true;
}

bool buildVal(int states, const uint16_t* freq, int nsym, const uint8_t* vbits, const int32_t* vbase, std::vector<ValEntry>& t) {
    t.assign((size_t)states, {0, 0, 0, 0});
    int nclz = clz32((uint32_t)states), sum = 0;
    size_t at = 0;
    for (int i = 0; i < nsym; ++i) {
        int f = freq[i];
        if (!f) continue;
        sum += f;
        if (sum > states) return false;
        int k = clz32((uint32_t)f) - nclz, j0 = ((2 * states) >> k) - f;
        for (int j = 0; j < f; ++j) {
            ValEntry e;
            e.vbits = vbits[i];
            e.vbase = vbase[i];
            if (j < j0) e.total = (uint8_t)(k + vbits[i]), e.delta = (int16_t)(((f + j) << k) - states);
            else e.total = (uint8_t)(k - 1 + vbits[i]), e.delta = (int16_t)((j - j0) << (k - 1));
            t[at++] = e;
        }
    }
    return true;
}

// The FSE bit stream: read backwards from the end of its payload.
struct FseIn {
    uint64_t accum = 0;
    int nbits = 0;
    const uint8_t* start;
    const uint8_t* p;  // bytes before p are still to read
    static uint64_t load(const uint8_t* q, int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v |= (uint64_t)q[i] << (8 * i);
        return v;
    }
    bool init(int n) {  // n: the (non-positive) count of padding bits in the last byte
        if (n) {
            if (p < start + 8) return false;
            p -= 8;
            accum = load(p, 8);
            nbits = n + 64;
        } else {
            if (p < start + 7) return false;
            p -= 7;
            accum = load(p, 7);
            nbits = 56;
        }
        return nbits >= 56 && nbits < 64 && (nbits == 64 || (accum >> nbits) == 0);
    }
    bool flush() {
        int k = (63 - nbits) & -8;
        if (p - k / 8 < start) return false;
        p -= k / 8;
        uint64_t in = load(p, 8);
        if (k) accum = (accum << k) | (in & ((1ull << k) - 1));
        nbits += k;
        return true;
    }
    uint64_t pull(int n) {
        nbits -= n;
        uint64_t r = accum >> nbits;
        accum &= nbits ? (1ull << nbits) - 1 : 0;
        return r;
    }
};

uint32_t freqValue(uint32_t bits, int& nb) {
    static const int8_t nbits[32] = {2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14,
                                     2, 3, 2, 5, 2, 3, 2, 8, 2, 3, 2, 5, 2, 3, 2, 14};
    static const int8_t value[32] = {0, 2, 1, 4, 0, 3, 1, -1, 0, 2, 1, 5, 0, 3, 1, -1,
                                     0, 2, 1, 6, 0, 3, 1, -1, 0, 2, 1, 7, 0, 3, 1, -1};
    uint32_t b = bits & 31;
    nb = nbits[b];
    if (nb == 8) return 8 + ((bits >> 4) & 0xF);
    if (nb == 14) return 24 + ((bits >> 4) & 0x3FF);
    return (uint32_t)value[b];
}

uint64_t field(uint64_t v, int at, int n) { return (v >> at) & ((1ull << n) - 1); }

// One "bvx2" block (header already checked); appends its n_raw bytes to `out`.
bool lzfseBlock(const uint8_t* b, size_t n, std::vector<uint8_t>& out, size_t& used) {
    if (n < 32) return false;
    Span B(b, n);
    uint32_t raw = B.u32(4);
    uint64_t v0 = B.u64(8), v1 = B.u64(16), v2 = B.u64(24);
    uint32_t nLit = (uint32_t)field(v0, 0, 20), nLitPay = (uint32_t)field(v0, 20, 20), nMatch = (uint32_t)field(v0, 40, 20);
    int litBits = (int)field(v0, 60, 3) - 7;
    uint16_t litState[4] = {(uint16_t)field(v1, 0, 10), (uint16_t)field(v1, 10, 10), (uint16_t)field(v1, 20, 10),
                            (uint16_t)field(v1, 30, 10)};
    uint32_t nLmdPay = (uint32_t)field(v1, 40, 20);
    int lmdBits = (int)field(v1, 60, 3) - 7;
    uint32_t hsize = (uint32_t)field(v2, 0, 32);
    uint16_t lS = (uint16_t)field(v2, 32, 10), mS = (uint16_t)field(v2, 42, 10), dS = (uint16_t)field(v2, 52, 10);
    if (hsize < 32 || hsize > n || (uint64_t)hsize + nLitPay + nLmdPay > n || nLit > 1u << 20) return false;
    // Symbol frequencies, variable-length coded.
    uint16_t freq[360] = {};
    {
        const uint8_t* s = b + 32;
        const uint8_t* e = b + hsize;
        uint32_t acc = 0;
        int an = 0;
        for (int i = 0; i < 360; ++i) {
            while (s < e && an + 8 <= 32) acc |= (uint32_t)*s++ << an, an += 8;
            int nb = 0;
            uint32_t f = freqValue(acc, nb);
            if (nb > an) return false;
            freq[i] = (uint16_t)f;
            acc >>= nb;
            an -= nb;
        }
    }
    const uint16_t *lf = freq, *mf = freq + 20, *df = freq + 40, *litf = freq + 104;
    std::vector<LitEntry> litT;
    std::vector<ValEntry> lT, mT, dT;
    const DTables& DT = dTables();
    if (!buildLit(1024, litf, 256, litT) || !buildVal(64, lf, 20, kLBits, kLBase, lT) ||
        !buildVal(64, mf, 20, kMBits, kMBase, mT) || !buildVal(256, df, 64, DT.bits, DT.base, dT))
        return false;
    // Literals: four interleaved states.
    std::vector<uint8_t> lits(nLit + 8);
    {
        FseIn in;
        in.start = b + hsize;
        in.p = b + hsize + nLitPay;
        if (!in.init(litBits)) return false;
        uint32_t st[4] = {litState[0], litState[1], litState[2], litState[3]};
        for (uint32_t i = 0; i < nLit; i += 4) {
            if (!in.flush()) return false;
            for (int k = 0; k < 4; ++k) {
                if (st[k] >= 1024) return false;
                LitEntry e = litT[st[k]];
                st[k] = (uint32_t)(e.delta + (int32_t)in.pull(e.k));
                lits[i + k] = e.symbol;
            }
        }
    }
    // Literal/match/distance triples.
    FseIn in;
    in.start = b + hsize + nLitPay;
    in.p = in.start + nLmdPay;
    if (!in.init(lmdBits)) return false;
    uint32_t ls = lS, ms = mS, ds = dS;
    size_t litAt = 0, target = out.size() + raw;
    int32_t D = 0;
    auto val = [&](uint32_t& st, const std::vector<ValEntry>& t) -> int64_t {
        if (st >= t.size()) return -1;
        ValEntry e = t[st];
        uint32_t x = (uint32_t)in.pull(e.total);
        st = (uint32_t)(e.delta + (int32_t)(x >> e.vbits));
        return e.vbase + (int64_t)(x & ((1u << e.vbits) - 1));
    };
    for (uint32_t i = 0; i < nMatch; ++i) {
        if (!in.flush()) return false;
        int64_t L = val(ls, lT), M = val(ms, mT), nd = val(ds, dT);
        if (L < 0 || M < 0 || nd < 0) return false;
        if (nd) D = (int32_t)nd;
        if (litAt + (size_t)L > nLit || out.size() + (size_t)L + (size_t)M > target) return false;
        out.insert(out.end(), lits.begin() + (ptrdiff_t)litAt, lits.begin() + (ptrdiff_t)(litAt + L));
        litAt += (size_t)L;
        if (M) {
            if (D <= 0 || (size_t)D > out.size()) return false;
            size_t from = out.size() - (size_t)D;
            for (int64_t k = 0; k < M; ++k) out.push_back(out[from + (size_t)k]);
        }
    }
    if (out.size() != target) return false;
    used = (size_t)hsize + nLitPay + nLmdPay;
    return true;
}

}  // namespace

bool unLzvn(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    bool ok = lzvnRun(in, n, out, expect ? expect + 1024 : SIZE_MAX / 2);
    if (expect && out.size() > expect) out.resize(expect);
    return ok && (!expect || out.size() == expect);
}

bool unLzfse(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t expect) {
    out.clear();
    out.reserve(expect);
    size_t i = 0;
    while (i + 4 <= n) {
        Span S(in + i, n - i);
        uint32_t magic = S.u32(0);
        if (magic == 0x24787662) return !expect || out.size() >= expect;  // "bvx$": the end
        if (magic == 0x2D787662) {  // "bvx-": stored
            uint32_t raw = S.u32(4);
            if (i + 8 + raw > n) return false;
            out.insert(out.end(), in + i + 8, in + i + 8 + raw);
            i += 8 + raw;
        } else if (magic == 0x6E787662) {  // "bvxn": an LZVN block
            uint32_t raw = S.u32(4), pay = S.u32(8);
            if (i + 12 + pay > n) return false;
            std::vector<uint8_t> part;
            if (!lzvnRun(in + i + 12, pay, part, raw + 1024) || part.size() < raw) return false;
            out.insert(out.end(), part.begin(), part.begin() + raw);
            i += 12 + pay;
        } else if (magic == 0x32787662) {  // "bvx2"
            size_t used = 0;
            if (!lzfseBlock(in + i, n - i, out, used)) return false;
            i += used;
        } else {
            return false;  // "bvx1" (uncompressed tables) isn't written by Apple's tools
        }
    }
    return !expect || out.size() >= expect;
}

}  // namespace kz::native
