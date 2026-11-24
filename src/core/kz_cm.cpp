#include "kz_cm.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#include <emmintrin.h>
#define KZ_CM_SSE2 1
#else
#define KZ_CM_SSE2 0
#endif
#if defined(_MSC_VER) || defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#define KZ_PREFETCH(p) _mm_prefetch((const char*)(p), _MM_HINT_T0)
#else
#define KZ_PREFETCH(p) ((void)0)
#endif

// Everything here is integer arithmetic so that an archive made on one machine decodes bit-exactly on
// any other (no floating point, no platform-dependent tables).

namespace kz::cm {

namespace {

// ---------------------------------------------------------------- logistic helpers
// squash(d) = 4096 / (1 + e^-d/256), d in [-2047, 2047], by interpolation in a 33-point table.
const int kSquashPts[33] = {1,    2,    3,    6,    10,   16,   27,   45,   73,   120,  194,
                            310,  488,  747,  1101, 1546, 2047, 2549, 2994, 3348, 3607, 3785,
                            3901, 3975, 4024, 4050, 4068, 4079, 4085, 4089, 4092, 4093, 4094};

inline int squashCalc(int d) {
    if (d > 2047) d = 2047;
    if (d < -2047) d = -2047;
    int w = d & 127;
    int i = (d >> 7) + 16;
    return (kSquashPts[i] * (128 - w) + kSquashPts[i + 1] * w + 64) >> 7;
}

struct Tables {
    short stretch[4096];
    int squash[4096];  // squash(d - 2048)
    int dt[1024];      // adaptation rate 1/(n+1.5) in 16.16
    Tables() {
        for (int i = 0; i < 4096; ++i) squash[i] = squashCalc(i - 2048);
        int pi = 0;
        for (int x = -2047; x <= 2047; ++x) {
            int v = squashCalc(x);
            for (int j = pi; j <= v; ++j) stretch[j] = (short)x;
            pi = v + 1;
        }
        for (int j = pi; j < 4096; ++j) stretch[j] = 2047;
        for (int n = 0; n < 1024; ++n) dt[n] = (2 * 65536) / (2 * n + 3);
    }
};
// Built once at program start; a function-local static would add a thread-safe init check to
// every one of the ~100 lookups per coded bit.
const Tables g_tabs;
inline const Tables& tabs() { return g_tabs; }

inline int squash(int d) {
    if (d > 2047) d = 2047;
    if (d < -2047) d = -2047;
    return tabs().squash[d + 2048];
}
inline int stretch(int p) { return tabs().stretch[p]; }

inline uint64_t hmix(uint64_t x) {
    x ^= x >> 31;
    x *= 0x7fb5d329728ea185ull;
    x ^= x >> 27;
    x *= 0x81dadef4bc2dd44dull;
    x ^= x >> 33;
    return x;
}
inline uint64_t hash2(uint64_t a, uint64_t b) { return hmix(a * 0x9E3779B97F4A7C15ull + b); }
inline uint64_t hash3(uint64_t a, uint64_t b, uint64_t c) { return hash2(hash2(a, b), c); }

// ---------------------------------------------------------------- adaptive bit counter
// 32-bit cell holding two estimates of the probability of a 1:
//   bits 16..31  slow: rate 1/(n+1.5), so it averages over everything seen (up to `limit`)
//   bits  6..15  fast: rate 1/4, tracks what happened recently
//   bits  0..5   n, the observation count (saturating)
constexpr uint32_t kInit = (0x8000u << 16) | (512u << 6);
constexpr uint32_t kCountMask = 63;

inline int cellP(uint32_t s) { return (int)(s >> 20); }               // slow, 12-bit
inline int cellFast(uint32_t s) { return (int)((s >> 4) & 0xFFC); }   // fast, 12-bit
inline uint32_t cellN(uint32_t s) { return s & kCountMask; }

inline void cellUpdate(uint32_t& s, int y, uint32_t limit) {
    uint32_t n = s & kCountMask;
    int p = (int)(s >> 16);
    p += (int)(((y ? 65535 : 0) - p) * (int64_t)tabs().dt[n] >> 16);
    int f = (int)((s >> 6) & 1023);
    f += ((y ? 1023 : 0) - f) >> 2;
    if (n < limit) ++n;
    s = ((uint32_t)p << 16) | ((uint32_t)f << 6) | n;
}

// ---------------------------------------------------------------- bit histories (Ultimate)
// A state stands for a pair of counts (n0, n1) of zeros and ones seen in a context. Seeing a bit
// increments its count and halves a large opposite count, so old evidence fades. Only pairs whose
// smaller count is small are kept (a long one-sided run is far more useful to remember exactly than
// a large balanced count), which fits every state in a byte.
struct HistoryTable {
    static constexpr int kCap[6] = {40, 24, 12, 8, 6, 5};  // max of the larger count, by the smaller
    int count = 0;
    uint8_t n0[256] = {}, n1[256] = {};
    uint8_t next[256][2] = {};
    static bool allowed(int a, int b) {
        int m = std::min(a, b), M = std::max(a, b);
        return m <= 5 && M <= kCap[m];
    }
    int find(int a, int b) const {
        for (int i = 0; i < count; ++i)
            if (n0[i] == a && n1[i] == b) return i;
        return -1;
    }
    HistoryTable() {
        // enumerate in order of total count so state 0 is (0,0)
        for (int t = 0; t <= 80; ++t)
            for (int a = 0; a <= t; ++a) {
                int b = t - a;
                if (allowed(a, b)) {
                    n0[count] = (uint8_t)a;
                    n1[count] = (uint8_t)b;
                    ++count;
                }
            }
        for (int i = 0; i < count; ++i)
            for (int y = 0; y < 2; ++y) {
                int a = n0[i], b = n1[i];
                int& mine = y ? b : a;
                int& other = y ? a : b;
                ++mine;
                if (other > 2) other = other / 2 + 1;
                while (!allowed(a, b)) {
                    if (a >= b) --a;
                    else --b;
                }
                next[i][y] = (uint8_t)find(a, b);
            }
    }
};
const HistoryTable g_hist;

// Per-model map from a bit history to a probability, learned as the data goes by.
struct HistoryMap {
    uint32_t t[256];
    HistoryMap() {
        for (int i = 0; i < 256; ++i) {
            // start from the counts themselves: (n1 + 0.4) / (n + 0.8)
            int a = i < g_hist.count ? g_hist.n0[i] : 0, b = i < g_hist.count ? g_hist.n1[i] : 0;
            uint32_t pr = (uint32_t)(((uint64_t)(b * 10 + 4) << 16) / (uint64_t)((a + b) * 10 + 8));
            t[i] = (std::min<uint32_t>(pr, 65535) << 16) | 0;
        }
    }
    int p(int st) const { return (int)(t[st] >> 20); }
    void update(int st, int y) {
        uint32_t& c = t[st];
        uint32_t n = c & 1023;
        int pr = (int)(c >> 16);
        pr += (int)(((y ? 65535 : 0) - pr) * (int64_t)tabs().dt[n] >> 16);
        if (n < 1023) ++n;
        c = ((uint32_t)pr << 16) | n;
    }
};

// Ultimate cell: slow probability (bits 16..31), history state (8..15), count (0..5).
constexpr uint32_t kInitU = 0x8000u << 16;
inline int cellState(uint32_t s) { return (int)((s >> 8) & 255); }
inline void cellUpdateU(uint32_t& s, int y, uint32_t limit) {
    uint32_t n = s & kCountMask;
    int p = (int)(s >> 16);
    p += (int)(((y ? 65535 : 0) - p) * (int64_t)tabs().dt[n] >> 16);
    uint32_t st = g_hist.next[(s >> 8) & 255][y];
    if (n < limit) ++n;
    s = ((uint32_t)p << 16) | (st << 8) | n;
}

// ---------------------------------------------------------------- hashed context table
// Buckets of 16 cells (one cache line): cell 0 holds a check value, cells 1..15 the nodes of the
// binary tree for one nibble. Two candidate buckets per context; the less used one is replaced.
class HashTable {
public:
    void init(size_t buckets, uint32_t initCell = kInit) {
        init_ = initCell;
        mask_ = buckets - 1;
        t_.reset(new uint32_t[buckets * 16 + 16]);
        // align to 64 bytes
        uintptr_t a = reinterpret_cast<uintptr_t>(t_.get());
        base_ = reinterpret_cast<uint32_t*>((a + 63) & ~uintptr_t(63));
        std::memset(base_, 0, buckets * 16 * sizeof(uint32_t));
    }
    void prefetch(uint64_t h) const {
        size_t b = (size_t)(h & mask_);
        KZ_PREFETCH(base_ + b * 16);
        KZ_PREFETCH(base_ + (b ^ 1) * 16);
    }
    uint32_t* find(uint64_t h) {
        uint32_t chk = (uint32_t)(h >> 32) | 1u;
        size_t b = (size_t)(h & mask_);
        uint32_t* a = base_ + b * 16;
        if (a[0] == chk) return a;
        uint32_t* c = base_ + (b ^ 1) * 16;
        if (c[0] == chk) return c;
        uint32_t* r = cellN(a[1]) <= cellN(c[1]) ? a : c;
        r[0] = chk;
        for (int k = 1; k < 16; ++k) r[k] = init_;
        return r;
    }

private:
    std::unique_ptr<uint32_t[]> t_;
    uint32_t* base_ = nullptr;
    uint32_t init_ = kInit;
    size_t mask_ = 0;
};

// ---------------------------------------------------------------- mixer
// Single-layer network: out = sum(x_i * w_i), weights picked from one of `sets` by a context and
// trained online toward the coded bit.
struct Mixer {
    // Weights are 16-bit with 1.0 = 8192; inputs are 16-bit stretch values. The dot product adds
    // pairwise products shifted right by 8 (exactly what SSE2 pmaddwd + psrad does), so the SIMD and
    // scalar paths give identical results on every machine.
    std::vector<int16_t> wv;
    int16_t* w = nullptr;
    int n = 0, np = 0, sel = 0, out = 0, lr = 0;
    void init(int inputs, int sets, int initW, int rate) {
        n = inputs;
        np = (inputs + 7) & ~7;
        wv.assign((size_t)sets * np + 8, 0);
        uintptr_t a = reinterpret_cast<uintptr_t>(wv.data());
        w = reinterpret_cast<int16_t*>((a + 15) & ~uintptr_t(15));
        for (int s = 0; s < sets; ++s)
            for (int i = 0; i < inputs; ++i) w[(size_t)s * np + i] = (int16_t)initW;
        lr = rate;
    }
    int mix(const int16_t* x, int s) {
        sel = s * np;
        const int16_t* wp = w + sel;
        int dot = 0;
#if KZ_CM_SSE2
        __m128i sum = _mm_setzero_si128();
        for (int i = 0; i < np; i += 8) {
            __m128i m = _mm_madd_epi16(_mm_load_si128((const __m128i*)(x + i)), _mm_load_si128((const __m128i*)(wp + i)));
            sum = _mm_add_epi32(sum, _mm_srai_epi32(m, 8));
        }
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 8));
        sum = _mm_add_epi32(sum, _mm_srli_si128(sum, 4));
        dot = _mm_cvtsi128_si32(sum);
#else
        for (int i = 0; i < np; i += 2) dot += ((int)x[i] * wp[i] + (int)x[i + 1] * wp[i + 1]) >> 8;
#endif
        out = std::clamp(dot >> 5, -2047, 2047);
        return out;
    }
    void train(const int16_t* x, int y) {
        int err = ((y << 12) - squash(out)) * lr;
        int16_t* wp = w + sel;
#if KZ_CM_SSE2
        __m128i e = _mm_set1_epi16((short)err), one = _mm_set1_epi16(1);
        for (int i = 0; i < np; i += 8) {
            __m128i xi = _mm_load_si128((const __m128i*)(x + i));
            __m128i t = _mm_mulhi_epi16(_mm_adds_epi16(xi, xi), e);
            t = _mm_srai_epi16(_mm_adds_epi16(t, one), 1);
            __m128i* wq = (__m128i*)(wp + i);
            _mm_store_si128(wq, _mm_adds_epi16(_mm_load_si128(wq), t));
        }
#else
        for (int i = 0; i < np; ++i) {
            int x2 = std::clamp(2 * (int)x[i], -32768, 32767);
            int t = (x2 * err) >> 16;
            t = std::clamp(t + 1, -32768, 32767) >> 1;
            wp[i] = (int16_t)std::clamp((int)wp[i] + t, -32768, 32767);
        }
#endif
    }
};

// ---------------------------------------------------------------- APM / SSE
// Maps a probability to a refined one, per context, by interpolating between 33 trained points.
class Apm {
public:
    void init(int contexts) {
        t_.resize((size_t)contexts * 33);
        for (int c = 0; c < contexts; ++c)
            for (int j = 0; j < 33; ++j) t_[(size_t)c * 33 + j] = (uint16_t)(squash((j - 16) * 128) * 16);
    }
    int p(int pr, int cx) {
        int s = stretch(pr) + 2048;
        int lo = s >> 7;
        w_ = s & 127;
        idx_ = (size_t)cx * 33 + lo;
        return (t_[idx_] * (128 - w_) + t_[idx_ + 1] * w_) >> 11;
    }
    void update(int y, int rate) {
        int g = y ? 65535 : 0;
        int a = t_[idx_], b = t_[idx_ + 1];
        a += ((g - a) * (128 - w_)) >> (rate + 7);
        b += ((g - b) * w_) >> (rate + 7);
        t_[idx_] = (uint16_t)std::clamp(a, 16, 65535 - 16);
        t_[idx_ + 1] = (uint16_t)std::clamp(b, 16, 65535 - 16);
    }

private:
    std::vector<uint16_t> t_;
    size_t idx_ = 0;
    int w_ = 0;
};

// ---------------------------------------------------------------- the model
constexpr int kMaxHashed = 28;
constexpr int kMinMatch = 6;
constexpr int kLongMatch = 20;
constexpr int kVeryLongMatch = 32;

// One table shared by every hashed context: busy models take more of it, idle ones less.
size_t bucketsFor(size_t n, int level = 0) {
    size_t cap = level ? (1u << 24) : (1u << 22);  // 256 MiB, or 1 GiB for Ultimate
    size_t want = level ? n * 4 : n * 2;
    size_t b = 4096;
    while (b < want && b < cap) b <<= 1;
    return b;
}
int matchBitsFor(size_t n, int level = 0) {
    int cap = level ? 24 : 22;
    int b = 12;
    while ((1ull << b) < n && b < cap) ++b;
    return b;
}

class Predictor {
public:
    Predictor(const uint8_t* buf, size_t n, Profile prof, int level) : buf_(buf), n_(n), prof_(prof), u_(level > 0) {
        nh_ = prof == Text ? 13 : 18;
        if (u_) {
            nh_ += 4;
        }
        ninputs_ = 1 + 2 + nh_ * 3 + 6 + (u_ ? 3 : 0);
        hist_ = u_;
        ht_.init(bucketsFor(n, level), hist_ ? kInitU : kInit);
        if (hist_) hmap_.resize(kMaxHashed);
        o0_.assign(256, kInit);
        o1_.assign(65536, kInit);
        int mb = matchBitsFor(n, level);
        mtab_.assign((size_t)1 << mb, 0);
        mshift_ = 64 - mb;
        msm_.assign(64 * 2, kInit);
        last1_.assign(256, 0);
        last2_.assign(65536, 0);
        lastPos_.assign(256, 0);
        lastDist_.assign(256, 0);
        int w0 = 8192 * 2 / ninputs_;
        // Tuned on a mix of source code, prose, x86 DLLs and font data.
        const int lr = 2, lr2 = 1;
        mA_.init(ninputs_, 256, w0, lr);
        mB_.init(ninputs_, 256, w0, lr);
        mC_.init(ninputs_, 64, w0, lr);
        mD_.init(ninputs_, 16 * 8, w0, lr);
        mE_.init(ninputs_, 256, w0, lr);
        m2_.init(5, 8, 8192 / 5, lr2);
        apm4_.init(1024 * 256);
        mtab2_.assign(mtab_.size(), 0);
        msm2_.assign(64 * 2, kInit);
        apm1_.init(256);
        apm2_.init(65536);
        apm3_.init(64 * 256);
        if (u_) {
            mF_.init(ninputs_, 2048, w0, lr);
            mG_.init(ninputs_, 1024, w0, lr);
            mH_.init(ninputs_, 4096, w0, lr);
            m2u_.init(8, 256, 8192 / 8, lr2);
            apm5_.init(4096 * 256);
            apm6_.init(4096 * 256);
            mtab3_.assign(mtab_.size(), 0);
            msm3_.assign(64 * 2, kInit);
        }
        byteUpdate();
        nibbleUpdate();
        predict();
    }

    int p() const { return pr_; }

    void update(int y) {
        cellUpdate(*s0_, y, 60);
        cellUpdate(*s1_, y, 60);
        if (hist_) {
            for (int i = 0; i < nh_; ++i) {
                hmap_[i].update(cellState(*cell_[i]), y);
                cellUpdateU(*cell_[i], y, ulimit_);
            }
        } else {
            for (int i = 0; i < nh_; ++i) cellUpdate(*cell_[i], y, u_ ? ulimit_ : limit_);
        }
        if (msmIdx_ >= 0) cellUpdate(msm_[msmIdx_], y, 63);
        mA_.train(x_, y);
        mB_.train(x_, y);
        mC_.train(x_, y);
        mD_.train(x_, y);
        mE_.train(x_, y);
        if (msm2Idx_ >= 0) cellUpdate(msm2_[msm2Idx_], y, 63);
        if (u_) {
            mF_.train(x_, y);
            mG_.train(x_, y);
            mH_.train(x_, y);
            m2u_.train(x2_, y);
            apm5_.update(y, kApmRate);
            apm6_.update(y, kApmRate);
            if (msm3Idx_ >= 0) cellUpdate(msm3_[msm3Idx_], y, 63);
        } else {
            m2_.train(x2_, y);
        }
        apm1_.update(y, kApmRate);
        apm2_.update(y, kApmRate);
        apm3_.update(y, kApmRate);
        apm4_.update(y, kApmRate);

        c0_ = (c0_ << 1) | (uint32_t)y;
        nib_ = (nib_ << 1) | y;
        ++bitPos_;
        if (bitPos_ == 8) {
            c8_ = (c8_ << 8) | (c0_ & 255);
            c0_ = 1;
            bitPos_ = 0;
            ++pos_;
            byteUpdate();
            nibbleUpdate();
        } else if (bitPos_ == 4) {
            nibbleUpdate();
        }
        predict();
    }

private:
    uint8_t back(size_t d) const { return d <= pos_ ? buf_[pos_ - d] : 0; }

    void byteUpdate() {
        const uint32_t c4 = (uint32_t)c8_;
        const uint8_t c1 = (uint8_t)c4, c2 = (uint8_t)(c4 >> 8);

        if (pos_ > 0) {
            // --- match model: follow the current match, or look for a new one
            if (mlen_ > 0) {
                if (buf_[mptr_] == c1) {
                    if (mlen_ < 65535) ++mlen_;
                    ++mptr_;
                } else {
                    mlen_ = 0;
                }
            }
            if (pos_ >= (size_t)kMinMatch) {
                size_t h = (size_t)(hmix(c8_ & 0xFFFFFFFFFFFFull) >> mshift_);
                if (mlen_ == 0) {
                    uint32_t cand = mtab_[h];
                    if (cand > 0) {
                        size_t L = 0;
                        while (L < 64 && L < cand && buf_[cand - 1 - L] == buf_[pos_ - 1 - L]) ++L;
                        if (L >= (size_t)kMinMatch) {
                            mlen_ = (int)L;
                            mptr_ = cand;
                        }
                    }
                }
                mtab_[h] = (uint32_t)pos_;
            }
            if (mlen2_ > 0) {
                if (buf_[mptr2_] == c1) {
                    if (mlen2_ < 65535) ++mlen2_;
                    ++mptr2_;
                } else {
                    mlen2_ = 0;
                }
            }
            if (pos_ >= (size_t)kLongMatch) {
                uint64_t hh = 0;
                for (int j = 1; j <= kLongMatch; ++j) hh = (hh + buf_[pos_ - j] + 1) * 0x2F0F3A49D2C1B78Bull;
                size_t h2 = (size_t)(hmix(hh) >> mshift_);
                if (mlen2_ == 0) {
                    uint32_t cand = mtab2_[h2];
                    if (cand > 0) {
                        size_t L = 0;
                        while (L < 400 && L < cand && buf_[cand - 1 - L] == buf_[pos_ - 1 - L]) ++L;
                        if (L >= (size_t)kLongMatch) {
                            mlen2_ = (int)L;
                            mptr2_ = cand;
                        }
                    }
                }
                mtab2_[h2] = (uint32_t)pos_;
            }
            if (u_) {  // very long repeats (whole files, sections): min 32 bytes
                if (mlen3_ > 0) {
                    if (buf_[mptr3_] == c1) {
                        if (mlen3_ < 65535) ++mlen3_;
                        ++mptr3_;
                    } else {
                        mlen3_ = 0;
                    }
                }
                if (pos_ >= (size_t)kVeryLongMatch) {
                    uint64_t hh = 0;
                    for (int j = 1; j <= kVeryLongMatch; ++j) hh = (hh + buf_[pos_ - j] + 1) * 0x9E3779B97F4A7C15ull;
                    size_t h3 = (size_t)(hmix(hh) >> mshift_);
                    if (mlen3_ == 0) {
                        uint32_t cand = mtab3_[h3];
                        if (cand > 0) {
                            size_t L = 0;
                            while (L < 1000 && L < cand && buf_[cand - 1 - L] == buf_[pos_ - 1 - L]) ++L;
                            if (L >= (size_t)kVeryLongMatch) {
                                mlen3_ = (int)L;
                                mptr3_ = cand;
                            }
                        }
                    }
                    mtab3_[h3] = (uint32_t)pos_;
                }
            }
            // --- indirect: what followed this byte / byte pair last time
            if (pos_ >= 2) {
                last1_[c2] = c1;
                last2_[(size_t)(c4 >> 8) & 0xFFFF] = c1;
            }
            // --- record (stride) detection: a byte value recurring at a constant distance
            uint32_t d = (uint32_t)(pos_ - lastPos_[c1]);
            if (lastPos_[c1] && d == lastDist_[c1] && d > 1 && d <= 4096) {
                if (d == stride_) strideConf_ = std::min(strideConf_ + 1, 15);
                else if (strideConf_ > 0) --strideConf_;
                else stride_ = d;
            }
            lastDist_[c1] = d;
            lastPos_[c1] = (uint32_t)pos_;
        }

        const uint64_t o2 = c4 & 0xFFFF, o3 = c4 & 0xFFFFFF;
        int k = 0;
        ctx_[k++] = hash2(1, o2);
        ctx_[k++] = hash2(2, o3);
        ctx_[k++] = hash2(3, c4);
        ctx_[k++] = hash2(4, c8_ & 0xFFFFFFFFFFull);
        ctx_[k++] = hash2(5, c8_ & 0xFFFFFFFFFFFFull);
        ctx_[k++] = hash2(6, c8_);
        uint8_t i1 = last1_[c1];
        uint16_t i2 = last2_[o2];
        if (prof_ == Text) {
            bool letter = (c1 >= 'a' && c1 <= 'z') || (c1 >= 'A' && c1 <= 'Z') || c1 >= 128 || (c1 >= '0' && c1 <= '9');
            if (letter) {
                uint8_t lc = (c1 >= 'A' && c1 <= 'Z') ? (uint8_t)(c1 + 32) : c1;
                word0_ = hash2(word0_ + 1, lc);
                ++wordLen_;
            } else {
                wordLen_ = 0;
                if (word0_) {
                    word2_ = word1_;
                    word1_ = word0_;
                    word0_ = 0;
                }
            }
            if (c1 == '\n') {
                prevLine_ = lineStart_;
                lineStart_ = pos_;
            }
            size_t col = pos_ - lineStart_;
            bool hasAbove = prevLine_ + col < lineStart_;
            uint8_t above = hasAbove ? buf_[prevLine_ + col] : 0;
            uint8_t aboveNext = (prevLine_ + col + 1 < lineStart_) ? buf_[prevLine_ + col + 1] : 0;
            ctx_[k++] = hash2(10, word0_ ? word0_ : (0x100000000ull | c1));
            ctx_[k++] = hash3(11, word0_, word1_);
            ctx_[k++] = hash3(12, hash2(word0_, word1_), word2_);
            ctx_[k++] = hash3(13, word1_, c1);
            ctx_[k++] = hash3(14, above, ((uint64_t)std::min<size_t>(col, 255) << 8) | c1);
            ctx_[k++] = hash3(15, above, ((uint64_t)aboveNext << 8) | c1);
            ctx_[k++] = hash3(16, i2, o2);
        } else {
            uint8_t r1 = 0, r2 = 0;
            if (strideConf_ >= 2) {
                r1 = back(stride_);
                r2 = back(stride_ * 2);
            }
            ctx_[k++] = hash2(20, c4 & 0xFFFF00);        // skip the last byte
            ctx_[k++] = hash2(21, c4 & 0xFF00FF);        // bytes 1 and 3
            ctx_[k++] = hash2(22, c4 & 0xF0F0F0F0u);     // high nibbles of the last 4
            ctx_[k++] = hash2(23, c4 & 0xFF00);          // byte 2 alone
            ctx_[k++] = hash2(24, c4 & 0xFF0000);        // byte 3 alone
            ctx_[k++] = hash3(25, ((uint64_t)stride_ << 16) | ((uint64_t)r1 << 8) | r2, strideConf_ >= 2);
            ctx_[k++] = hash3(26, ((uint64_t)stride_ << 16) | ((uint64_t)r1 << 8) | c1, strideConf_ >= 2);
            ctx_[k++] = hash3(27, i1, c1);
            ctx_[k++] = hash3(28, i2, o2);
            // strings inside binaries: ASCII or UTF-16 (zero bytes between letters are skipped)
            bool letter = (c1 >= 'a' && c1 <= 'z') || (c1 >= 'A' && c1 <= 'Z') || (c1 >= '0' && c1 <= '9');
            if (letter) {
                bword_ = hash2(bword_ + 1, (uint8_t)(c1 | 0x20));
            } else if (c1 != 0) {
                bword_ = 0;
            }
            ctx_[k++] = hash3(29, bword_, c1 == 0);
            ctx_[k++] = hash3(30, pos_ & 3, c4 & 0xFFFF);   // fields of 4-byte records / pointer tables
            ctx_[k++] = hash3(31, pos_ & 7, c8_ & 0xFF00FFull);
        }
        if (u_) {
            if (prof_ == Text) {
                ctx_[k++] = hash3(43, word0_, word2_);                     // skip one word
                ctx_[k++] = hash2(44, c4 & 0xDFDFDFDFu);                   // order 4, case folded
                ctx_[k++] = hash3(45, word0_, (uint64_t)last1_[c1] << 8 | c1);
                ctx_[k++] = hash3(46, hash2(word1_, word2_), c1);
            } else {
                uint8_t r1 = strideConf_ >= 2 ? back(stride_) : 0;
                ctx_[k++] = hash3(47, (c8_ >> 16) & 0xFFFFFF, c1);              // bytes 1, 3-5
                ctx_[k++] = hash3(48, last1_[i1], (uint64_t)i1 << 8 | c1);      // two-step indirect
                ctx_[k++] = hash3(49, pos_ & 15, c4 & 0xFFFFFF);                // 16-byte structures
                ctx_[k++] = hash3(50, ((uint64_t)stride_ << 16) | r1, c4 & 0xFFFF);
            }
        }
    }

    void nibbleUpdate() {
        nib_ = 1;
        uint64_t salt = bitPos_ == 0 ? 0 : (uint64_t)c0_ * 0x9E3779B97F4A7C15ull;
        uint64_t h[kMaxHashed];
        // Issue every memory fetch first so the cache misses overlap instead of queueing.
        for (int i = 0; i < nh_; ++i) {
            h[i] = hash2(ctx_[i], salt);
            ht_.prefetch(h[i]);
        }
        seen_ = 0;
        for (int i = 0; i < nh_; ++i) {
            bucket_[i] = ht_.find(h[i]);
            if (i < 6 && cellN(bucket_[i][1])) ++seen_;  // how many plain orders have history
        }
    }

    void predict() {
        const uint8_t c1 = (uint8_t)c8_;
        int k = 0;
        x_[k++] = 256;
        s0_ = &o0_[c0_];
        s1_ = &o1_[((size_t)c1 << 8) | c0_];
        x_[k++] = stretch(cellP(*s0_));
        x_[k++] = stretch(cellP(*s1_));
        for (int i = 0; i < nh_; ++i) {
            cell_[i] = &bucket_[i][nib_];
            uint32_t s = *cell_[i];
            int st = stretch(cellP(s));
            uint32_t n = cellN(s);
            x_[k++] = st;
            x_[k++] = n == 0 ? 0 : (st * (int)std::min<uint32_t>(n, 31)) >> 4;
            x_[k++] = hist_ ? stretch(hmap_[i].p(cellState(s))) : n == 0 ? 0 : stretch(cellFast(s));
        }

        int lenB = 0;
        msmIdx_ = -1;
        if (mlen_ > 0) {
            int expect = buf_[mptr_] | 256;
            if ((uint32_t)(expect >> (8 - bitPos_)) == c0_) {
                int bit = (expect >> (7 - bitPos_)) & 1;
                int L = std::min(mlen_, 31);
                lenB = mlen_ < 16 ? 1 : mlen_ < 32 ? 2 : 3;
                msmIdx_ = L * 2 + bit;
                x_[k++] = stretch(cellP(msm_[msmIdx_]));
                x_[k++] = (bit ? 1 : -1) * std::min(mlen_, 32) * 32;
                x_[k++] = (bit ? 1 : -1) * 256;
            } else {
                mlen_ = 0;
            }
        }
        while (k < 1 + 2 + nh_ * 3 + 3) x_[k++] = 0;
        k = 1 + 2 + nh_ * 3 + 3;
        msm2Idx_ = -1;
        if (mlen2_ > 0) {
            int expect = buf_[mptr2_] | 256;
            if ((uint32_t)(expect >> (8 - bitPos_)) == c0_) {
                int bit = (expect >> (7 - bitPos_)) & 1;
                msm2Idx_ = std::min(mlen2_ / 8, 31) * 2 + bit;
                x_[k++] = stretch(cellP(msm2_[msm2Idx_]));
                x_[k++] = (bit ? 1 : -1) * std::min(mlen2_, 64) * 16;
                x_[k++] = (bit ? 1 : -1) * 256;
            } else {
                mlen2_ = 0;
            }
        }
        if (u_) {
            k = 1 + 2 + nh_ * 3 + 6;
            msm3Idx_ = -1;
            if (mlen3_ > 0) {
                int expect = buf_[mptr3_] | 256;
                if ((uint32_t)(expect >> (8 - bitPos_)) == c0_) {
                    int bit = (expect >> (7 - bitPos_)) & 1;
                    msm3Idx_ = std::min(mlen3_ / 16, 31) * 2 + bit;
                    x_[k++] = stretch(cellP(msm3_[msm3Idx_]));
                    x_[k++] = (bit ? 1 : -1) * std::min(mlen3_, 128) * 12;
                    x_[k++] = (bit ? 1 : -1) * 512;
                } else {
                    mlen3_ = 0;
                }
            }
        }
        while (k < ninputs_) x_[k++] = 0;

        x2_[0] = mA_.mix(x_, (int)c0_);
        x2_[1] = mB_.mix(x_, (int)c1);
        x2_[2] = mC_.mix(x_, lenB * 8 + bitPos_);
        x2_[3] = mD_.mix(x_, std::min(seen_, 15) * 8 + bitPos_);
        x2_[4] = mE_.mix(x_, (int)(uint8_t)(c8_ >> 8));
        int s;
        if (u_) {
            x2_[5] = mF_.mix(x_, (int)((c0_ << 3) | std::min(lenB + (mlen3_ > 0 ? 4 : 0), 7)));
            x2_[6] = mG_.mix(x_, (int)(((c8_ >> 8) & 0xF0) | ((c8_ & 0xF0) >> 4)) * 4 + (bitPos_ >> 1));
            int hsel;
            if (prof_ == Text) {
                uint8_t c1c = (uint8_t)c8_;
                int cls = (c1c >= 'a' && c1c <= 'z') ? 0 : (c1c >= 'A' && c1c <= 'Z') ? 1 : (c1c >= '0' && c1c <= '9') ? 2
                        : c1c == ' ' ? 3 : c1c == 10 ? 4 : c1c < 128 ? 5 : 6;
                hsel = (std::min<int>(wordLen_, 15) * 8 + cls) * 8 + bitPos_;
            } else {
                int phase = (strideConf_ >= 2 && stride_) ? (int)(pos_ % stride_) & 63 : 64 + (int)(pos_ & 7);
                hsel = phase * 8 + bitPos_;
            }
            x2_[7] = mH_.mix(x_, hsel);
            s = m2u_.mix(x2_, (int)c0_);
        } else {
            s = m2_.mix(x2_, bitPos_);
        }
        int pm = squash(s);
        int a1 = apm1_.p(pm, (int)c0_);
        int a2 = apm2_.p(pm, (int)(c0_ | ((uint32_t)c1 << 8)));
        int a3 = apm3_.p(pm, (int)(c0_ | (uint32_t)(std::min(mlen_, 63) << 8)));
        int a4 = apm4_.p(pm, (int)(((hash2(99, c8_ & 0xFFFF) >> 40) & 1023) << 8 | c0_));
        int pr = (pm + a1 + a2 * 2 + a3 * 2 + a4 * 2 + 4) >> 3;
        if (u_) {
            int a5 = apm5_.p(pm, (int)(((hash2(98, c8_ & 0xFFFFFF) >> 40) & 4095) << 8 | c0_));
            int a6 = apm6_.p(pm, (int)(((hash2(97, ctx_[prof_ == Text ? 6 : 7]) >> 40) & 4095) << 8 | c0_));
            pr = (pr * 4 + a5 * 2 + a6 * 2 + 4) >> 3;
        }
        pr_ = std::clamp(pr, 1, 4095);
    }

    const uint8_t* buf_;
    size_t n_;
    Profile prof_;
    bool u_ = false;
    int hist_ = 0;
    std::vector<HistoryMap> hmap_;
    int wordLen_ = 0;
    Mixer mH_;
    Mixer mF_, mG_, m2u_;
    Apm apm5_, apm6_;
    std::vector<uint32_t> mtab3_, msm3_;
    size_t mptr3_ = 0;
    int mlen3_ = 0, msm3Idx_ = -1;
    int nh_ = 0, ninputs_ = 0;
    static constexpr uint32_t limit_ = 20;
    uint32_t ulimit_ = 20;     // count cap: low = adapts to changing data
    static constexpr int kApmRate = 5;
    size_t pos_ = 0;
    uint32_t c0_ = 1;
    int bitPos_ = 0;
    int nib_ = 1;
    int seen_ = 0;
    uint64_t c8_ = 0;
    uint64_t ctx_[kMaxHashed] = {};
    uint32_t* bucket_[kMaxHashed] = {};
    uint32_t* cell_[kMaxHashed] = {};
    HashTable ht_;
    std::vector<uint32_t> o0_, o1_;
    uint32_t *s0_ = nullptr, *s1_ = nullptr;
    // text
    uint64_t word0_ = 0, word1_ = 0, word2_ = 0, bword_ = 0;
    size_t lineStart_ = 0, prevLine_ = 0;
    // binary
    std::vector<uint8_t> last1_;
    std::vector<uint16_t> last2_;
    std::vector<uint32_t> lastPos_, lastDist_;
    uint32_t stride_ = 0;
    int strideConf_ = 0;
    // match
    std::vector<uint32_t> mtab_;
    int mshift_ = 0;
    size_t mptr_ = 0;
    int mlen_ = 0;
    std::vector<uint32_t> msm_;
    int msmIdx_ = -1;
    // mixing
    alignas(16) int16_t x_[(1 + 2 + kMaxHashed * 3 + 9 + 7) & ~7] = {};
    alignas(16) int16_t x2_[8] = {};
    Mixer mA_, mB_, mC_, mD_, mE_, m2_;
    std::vector<uint32_t> mtab2_, msm2_;
    size_t mptr2_ = 0;
    int mlen2_ = 0, msm2Idx_ = -1;
    Apm apm1_, apm2_, apm3_, apm4_;
    int pr_ = 2048;
};

// ---------------------------------------------------------------- arithmetic coder
class Encoder {
public:
    explicit Encoder(std::vector<uint8_t>& out) : out_(out) {}
    void encode(int y, int p) {
        uint32_t xm = x1_ + (uint32_t)(((uint64_t)(x2_ - x1_) * (uint32_t)p) >> 12);
        if (y) x2_ = xm;
        else x1_ = xm + 1;
        while (((x1_ ^ x2_) & 0xFF000000u) == 0) {
            out_.push_back((uint8_t)(x2_ >> 24));
            x1_ <<= 8;
            x2_ = (x2_ << 8) | 255;
        }
    }
    void flush() {
        for (int i = 0; i < 4; ++i) {
            out_.push_back((uint8_t)(x1_ >> 24));
            x1_ <<= 8;
        }
    }

private:
    std::vector<uint8_t>& out_;
    uint32_t x1_ = 0, x2_ = 0xFFFFFFFFu;
};

class Decoder {
public:
    Decoder(const uint8_t* in, size_t n) : in_(in), n_(n) {
        for (int i = 0; i < 4; ++i) x_ = (x_ << 8) | next();
    }
    int decode(int p) {
        uint32_t xm = x1_ + (uint32_t)(((uint64_t)(x2_ - x1_) * (uint32_t)p) >> 12);
        int y = x_ <= xm;
        if (y) x2_ = xm;
        else x1_ = xm + 1;
        while (((x1_ ^ x2_) & 0xFF000000u) == 0) {
            x1_ <<= 8;
            x2_ = (x2_ << 8) | 255;
            x_ = (x_ << 8) | next();
        }
        return y;
    }
    bool overrun() const { return pos_ > n_ + 4; }

private:
    uint32_t next() { return pos_ < n_ ? in_[pos_++] : (++pos_, 0u); }
    const uint8_t* in_;
    size_t n_, pos_ = 0;
    uint32_t x1_ = 0, x2_ = 0xFFFFFFFFu, x_ = 0;
};

// ---------------------------------------------------------------- x86 call/jump transform
// The rel32 operand of every E8/E9 byte becomes an absolute address, so calls to the same function
// turn into identical byte strings. The decision looks only at the opcode byte, which is never
// rewritten (operand bytes are skipped), so the decoder walking the transformed data front to back
// makes exactly the same decisions. (Testing operand bytes would not be safe: a later rewrite can
// change a byte an earlier decision looked at.)
void e8e9(uint8_t* p, size_t n, bool encode) {
    for (size_t i = 0; i + 5 <= n;) {
        if ((p[i] & 0xFE) == 0xE8) {
            uint32_t a = (uint32_t)p[i + 1] | ((uint32_t)p[i + 2] << 8) | ((uint32_t)p[i + 3] << 16) |
                         ((uint32_t)p[i + 4] << 24);
            uint32_t off = (uint32_t)(i + 5);
            a = encode ? a + off : a - off;
            p[i + 1] = (uint8_t)a;
            p[i + 2] = (uint8_t)(a >> 8);
            p[i + 3] = (uint8_t)(a >> 16);
            p[i + 4] = (uint8_t)(a >> 24);
            i += 5;
        } else {
            ++i;
        }
    }
}

}  // namespace

uint64_t memoryFor(size_t n, int level) {
    if (level) {
        uint64_t m = (uint64_t)bucketsFor(n, level) * 64 + ((uint64_t)12 << matchBitsFor(n, level));
        m += (65536ull + 16384 + 262144 + 256 + 2 * 1048576) * 33 * 2 + 65536 * 4 + 65536 * 2 + 4096 * 128;
        return m + n;
    }
    uint64_t m = (uint64_t)bucketsFor(n) * 64;
    m += (uint64_t)8 << matchBitsFor(n);                                         // two match tables
    m += (65536ull + 16384 + 262144 + 256) * 33 * 2 + 65536 * 4 + 65536 * 2 + 4096 * 64;  // SSE, o1, indirect, mixers
    return m + n;  // plus a transformed copy for x86
}

bool compress(const uint8_t* in, size_t n, Profile prof, std::vector<uint8_t>& out, int level, const Tick& tick) {
    out.clear();
    out.reserve(n / 3 + 1024);
    std::vector<uint8_t> tmp;
    const uint8_t* src = in;
    if (prof == X86) {
        tmp.assign(in, in + n);
        e8e9(tmp.data(), n, true);
        src = tmp.data();
    }
    Predictor pred(src, n, prof, level);
    Encoder enc(out);
    for (size_t i = 0; i < n; ++i) {
        int c = src[i];
        for (int b = 7; b >= 0; --b) {
            int y = (c >> b) & 1;
            enc.encode(y, pred.p());
            pred.update(y);
        }
        if (tick && (i & 0xFFFF) == 0xFFFF) tick(1 << 16);
    }
    if (tick && (n & 0xFFFF)) tick(n & 0xFFFF);
    enc.flush();
    return true;
}

bool decompress(const uint8_t* in, size_t packed, uint8_t* out, size_t n, Profile prof, int level, const Tick& tick) {
    Predictor pred(out, n, prof, level);
    Decoder dec(in, packed);
    for (size_t i = 0; i < n; ++i) {
        int c = 1;
        for (int b = 0; b < 8; ++b) {
            int y = dec.decode(pred.p());
            c = (c << 1) | y;
            if (b == 7) out[i] = (uint8_t)c;
            pred.update(y);
        }
        if (dec.overrun()) return false;
        if (tick && (i & 0xFFFF) == 0xFFFF) tick(1 << 16);
    }
    if (tick && (n & 0xFFFF)) tick(n & 0xFFFF);
    if (prof == X86) e8e9(out, n, false);
    return true;
}

}  // namespace kz::cm
