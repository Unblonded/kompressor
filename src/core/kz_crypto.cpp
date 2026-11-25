#include "kz_crypto.hpp"

#include <algorithm>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define KZ_AESNI 1
#include <wmmintrin.h>
#include <emmintrin.h>
#ifdef _MSC_VER
#include <intrin.h>
#define KZ_NI_FN
#else
#define KZ_NI_FN __attribute__((target("aes,sse2")))
#endif
#endif

namespace kz::crypto {

namespace {

inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void putBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}

// ---------------------------------------------------------------- AES tables, computed once
struct Tables {
    uint8_t S[256], Si[256];
    uint32_t Td[4][256];
    Tables() {
        auto mul = [](uint8_t a, uint8_t b) {
            uint8_t r = 0;
            while (b) {
                if (b & 1) r ^= a;
                a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
                b >>= 1;
            }
            return r;
        };
        for (int x = 0; x < 256; ++x) {
            uint8_t inv = 0;
            if (x)
                for (int y = 1; y < 256; ++y)
                    if (mul((uint8_t)x, (uint8_t)y) == 1) {
                        inv = (uint8_t)y;
                        break;
                    }
            auto rotl = [](uint8_t v, int n) { return (uint8_t)((v << n) | (v >> (8 - n))); };
            uint8_t s = (uint8_t)(inv ^ rotl(inv, 1) ^ rotl(inv, 2) ^ rotl(inv, 3) ^ rotl(inv, 4) ^ 0x63);
            S[x] = s;
            Si[s] = (uint8_t)x;
        }
        for (int x = 0; x < 256; ++x) {
            uint8_t s = Si[x];
            uint32_t w = (uint32_t)mul(s, 0x0e) << 24 | (uint32_t)mul(s, 0x09) << 16 | (uint32_t)mul(s, 0x0d) << 8 | mul(s, 0x0b);
            Td[0][x] = w;
            Td[1][x] = ror(w, 8);
            Td[2][x] = ror(w, 16);
            Td[3][x] = ror(w, 24);
        }
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

// AES-256 encryption key schedule: 60 words.
void expandKey(const uint8_t key[32], uint32_t w[60]) {
    const Tables& T = tables();
    static const uint32_t rcon[7] = {0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000, 0x20000000, 0x40000000};
    for (int i = 0; i < 8; ++i) w[i] = be32(key + 4 * i);
    for (int i = 8; i < 60; ++i) {
        uint32_t t = w[i - 1];
        if (i % 8 == 0) {
            t = ror(t, 24);  // RotWord
            t = (uint32_t)T.S[t >> 24] << 24 | (uint32_t)T.S[(t >> 16) & 0xff] << 16 | (uint32_t)T.S[(t >> 8) & 0xff] << 8 | T.S[t & 0xff];
            t ^= rcon[i / 8 - 1];
        } else if (i % 8 == 4) {
            t = (uint32_t)T.S[t >> 24] << 24 | (uint32_t)T.S[(t >> 16) & 0xff] << 16 | (uint32_t)T.S[(t >> 8) & 0xff] << 8 | T.S[t & 0xff];
        }
        w[i] = w[i - 8] ^ t;
    }
}

// Decryption round keys for the table version (the "equivalent inverse cipher").
void decryptKeys(const uint32_t ek[60], uint32_t dk[60]) {
    const Tables& T = tables();
    for (int r = 0; r <= 14; ++r)
        for (int j = 0; j < 4; ++j) dk[4 * r + j] = ek[4 * (14 - r) + j];
    for (int i = 4; i < 56; ++i) {
        uint32_t w = dk[i];
        dk[i] = T.Td[0][T.S[w >> 24]] ^ T.Td[1][T.S[(w >> 16) & 0xff]] ^ T.Td[2][T.S[(w >> 8) & 0xff]] ^ T.Td[3][T.S[w & 0xff]];
    }
}

void decryptBlockTable(const uint32_t* rk, const uint8_t in[16], uint8_t out[16]) {
    const Tables& T = tables();
    uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1], s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    for (int r = 1; r < 14; ++r) {
        rk += 4;
        uint32_t t0 = T.Td[0][s0 >> 24] ^ T.Td[1][(s3 >> 16) & 0xff] ^ T.Td[2][(s2 >> 8) & 0xff] ^ T.Td[3][s1 & 0xff] ^ rk[0];
        uint32_t t1 = T.Td[0][s1 >> 24] ^ T.Td[1][(s0 >> 16) & 0xff] ^ T.Td[2][(s3 >> 8) & 0xff] ^ T.Td[3][s2 & 0xff] ^ rk[1];
        uint32_t t2 = T.Td[0][s2 >> 24] ^ T.Td[1][(s1 >> 16) & 0xff] ^ T.Td[2][(s0 >> 8) & 0xff] ^ T.Td[3][s3 & 0xff] ^ rk[2];
        uint32_t t3 = T.Td[0][s3 >> 24] ^ T.Td[1][(s2 >> 16) & 0xff] ^ T.Td[2][(s1 >> 8) & 0xff] ^ T.Td[3][s0 & 0xff] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((uint32_t)T.Si[a >> 24] << 24 | (uint32_t)T.Si[(b >> 16) & 0xff] << 16 | (uint32_t)T.Si[(c >> 8) & 0xff] << 8 |
                T.Si[d & 0xff]) ^ k;
    };
    putBe32(out, last(s0, s3, s2, s1, rk[0]));
    putBe32(out + 4, last(s1, s0, s3, s2, rk[1]));
    putBe32(out + 8, last(s2, s1, s0, s3, rk[2]));
    putBe32(out + 12, last(s3, s2, s1, s0, rk[3]));
}

#ifdef KZ_AESNI
bool cpuHasAes() {
#ifdef _MSC_VER
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 25) & 1;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("aes");
#endif
}

KZ_NI_FN void niKeys(const uint32_t ek[60], uint8_t dk[15][16]) {
    uint8_t enc[15][16];
    for (int r = 0; r < 15; ++r)
        for (int j = 0; j < 4; ++j) putBe32(enc[r] + 4 * j, ek[4 * r + j]);
    std::memcpy(dk[0], enc[14], 16);
    for (int i = 1; i < 14; ++i)
        _mm_storeu_si128((__m128i*)dk[i], _mm_aesimc_si128(_mm_loadu_si128((const __m128i*)enc[14 - i])));
    std::memcpy(dk[14], enc[0], 16);
}

KZ_NI_FN void niCbcDecrypt(const uint8_t dk[15][16], uint8_t iv[16], uint8_t* p, size_t n) {
    __m128i k[15];
    for (int i = 0; i < 15; ++i) k[i] = _mm_loadu_si128((const __m128i*)dk[i]);
    __m128i prev = _mm_loadu_si128((const __m128i*)iv);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {  // four blocks at a time: CBC decryption has no chain between them
        __m128i c0 = _mm_loadu_si128((const __m128i*)(p + i)), c1 = _mm_loadu_si128((const __m128i*)(p + i + 16));
        __m128i c2 = _mm_loadu_si128((const __m128i*)(p + i + 32)), c3 = _mm_loadu_si128((const __m128i*)(p + i + 48));
        __m128i x0 = _mm_xor_si128(c0, k[0]), x1 = _mm_xor_si128(c1, k[0]), x2 = _mm_xor_si128(c2, k[0]), x3 = _mm_xor_si128(c3, k[0]);
        for (int r = 1; r < 14; ++r) {
            x0 = _mm_aesdec_si128(x0, k[r]);
            x1 = _mm_aesdec_si128(x1, k[r]);
            x2 = _mm_aesdec_si128(x2, k[r]);
            x3 = _mm_aesdec_si128(x3, k[r]);
        }
        x0 = _mm_xor_si128(_mm_aesdeclast_si128(x0, k[14]), prev);
        x1 = _mm_xor_si128(_mm_aesdeclast_si128(x1, k[14]), c0);
        x2 = _mm_xor_si128(_mm_aesdeclast_si128(x2, k[14]), c1);
        x3 = _mm_xor_si128(_mm_aesdeclast_si128(x3, k[14]), c2);
        _mm_storeu_si128((__m128i*)(p + i), x0);
        _mm_storeu_si128((__m128i*)(p + i + 16), x1);
        _mm_storeu_si128((__m128i*)(p + i + 32), x2);
        _mm_storeu_si128((__m128i*)(p + i + 48), x3);
        prev = c3;
    }
    for (; i + 16 <= n; i += 16) {
        __m128i c = _mm_loadu_si128((const __m128i*)(p + i));
        __m128i x = _mm_xor_si128(c, k[0]);
        for (int r = 1; r < 14; ++r) x = _mm_aesdec_si128(x, k[r]);
        x = _mm_xor_si128(_mm_aesdeclast_si128(x, k[14]), prev);
        _mm_storeu_si128((__m128i*)(p + i), x);
        prev = c;
    }
    _mm_storeu_si128((__m128i*)iv, prev);
}
#endif

}  // namespace

// ---------------------------------------------------------------- SHA-256
Sha256::Sha256() {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof h_);
}

void Sha256::block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
}

void Sha256::update(const uint8_t* p, size_t n) {
    total_ += n;
    if (used_) {
        size_t take = std::min(n, (size_t)64 - used_);
        std::memcpy(buf_ + used_, p, take);
        used_ += take, p += take, n -= take;
        if (used_ < 64) return;
        block(buf_);
        used_ = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(p);
    std::memcpy(buf_, p, n);
    used_ = n;
}

void Sha256::final(uint8_t out[32]) {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (used_ != 56) update(&zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(len, 8);
    for (int i = 0; i < 8; ++i) putBe32(out + 4 * i, h_[i]);
}

// ---------------------------------------------------------------- AES-256-CBC
Aes256CbcDecrypt::Aes256CbcDecrypt(const uint8_t key[32], const uint8_t iv[16]) {
    uint32_t ek[60];
    expandKey(key, ek);
    decryptKeys(ek, rk_);
    std::memcpy(iv_, iv, 16);
#ifdef KZ_AESNI
    static const bool ni = cpuHasAes();
    if (ni) {
        niKeys(ek, ni_);
        useNi_ = true;
    }
#endif
}

void Aes256CbcDecrypt::decrypt(uint8_t* p, size_t n) {
    n &= ~(size_t)15;
#ifdef KZ_AESNI
    if (useNi_) {
        niCbcDecrypt(ni_, iv_, p, n);
        return;
    }
#endif
    uint8_t c[16], o[16];
    for (size_t i = 0; i < n; i += 16) {
        std::memcpy(c, p + i, 16);
        decryptBlockTable(rk_, c, o);
        for (int j = 0; j < 16; ++j) p[i + j] = o[j] ^ iv_[j];
        std::memcpy(iv_, c, 16);
    }
}

bool selfTest() {
    // SHA-256("abc")
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    Sha256 s;
    s.update((const uint8_t*)"abc", 3);
    uint8_t d[32];
    s.final(d);
    if (std::memcmp(d, abc, 32) != 0) return false;
    // FIPS-197 C.3: AES-256, key 00..1f.
    uint8_t key[32], iv[16] = {};
    for (int i = 0; i < 32; ++i) key[i] = (uint8_t)i;
    static const uint8_t ct[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    uint8_t b[64];
    for (int i = 0; i < 4; ++i) std::memcpy(b + 16 * i, ct, 16);  // 4 blocks: CBC xors blocks 2..4 with ct
    Aes256CbcDecrypt a(key, iv);
    a.decrypt(b, 64);
    if (std::memcmp(b, pt, 16) != 0) return false;
    for (int i = 1; i < 4; ++i)
        for (int j = 0; j < 16; ++j)
            if (b[16 * i + j] != (uint8_t)(pt[j] ^ ct[j])) return false;
    uint32_t ek[60], dk[60];
    expandKey(key, ek);
    decryptKeys(ek, dk);
    uint8_t o[16];
    decryptBlockTable(dk, ct, o);  // the table version too, whichever the CPU uses
    return std::memcmp(o, pt, 16) == 0;
}

}  // namespace kz::crypto
