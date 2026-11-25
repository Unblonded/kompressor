#include "kz_inflate.hpp"

#include <algorithm>
#include <cstring>

namespace kz {

namespace {
constexpr unsigned kTableBits = 15;
const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint32_t kDistBase[32] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,    33,
                                49,   65,   97,   129,  193,  257,   385,   513,   769,   1025,  1537,
                                2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
const uint8_t kDistExtra[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
}  // namespace

Inflater::Inflater(std::function<size_t(uint8_t*, size_t)> src, bool deflate64) : src_(std::move(src)), d64_(deflate64) {}

bool Inflater::fill(unsigned k) {
    while (bitcnt_ < k) {
        if (inAt_ == inHave_) {
            inHave_ = inEnd_ ? 0 : src_(in_.data(), in_.size());
            pulled_ += inHave_;
            inAt_ = 0;
            if (!inHave_) {
                inEnd_ = true;
                err_ = "The data ends too soon";
                return false;
            }
        }
        bitbuf_ |= (uint64_t)in_[inAt_++] << bitcnt_;
        bitcnt_ += 8;
    }
    return true;
}

bool Inflater::bits(unsigned k, uint32_t& v) {
    if (k == 0) {
        v = 0;
        return true;
    }
    if (!fill(k)) return false;
    v = (uint32_t)(bitbuf_ & ((1ull << k) - 1));
    bitbuf_ >>= k;
    bitcnt_ -= k;
    return true;
}

// Canonical Huffman code -> a full 15-bit lookup table: entry = symbol << 4 | length (0 = invalid).
bool Inflater::buildTable(const uint8_t* lens, unsigned n, std::vector<uint32_t>& t) {
    t.assign(1u << kTableBits, 0);
    unsigned count[16] = {}, next[16] = {};
    for (unsigned i = 0; i < n; ++i) count[lens[i]]++;
    count[0] = 0;
    unsigned code = 0;
    for (unsigned l = 1; l < 16; ++l) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    int left = 1;
    for (unsigned l = 1; l < 16; ++l) {
        left <<= 1;
        left -= (int)count[l];
        if (left < 0) return false;  // over-subscribed
    }
    for (unsigned s = 0; s < n; ++s) {
        unsigned l = lens[s];
        if (!l) continue;
        unsigned c = next[l]++, rev = 0;
        for (unsigned i = 0; i < l; ++i) rev |= ((c >> i) & 1) << (l - 1 - i);
        for (unsigned i = rev; i < (1u << kTableBits); i += 1u << l) t[i] = s << 4 | l;
    }
    return true;
}

bool Inflater::symbol(const std::vector<uint32_t>& t, unsigned& sym) {
    // Fill as far as the input allows: the last code of a stream may need fewer than 15 bits.
    while (bitcnt_ < kTableBits) {
        if (inAt_ == inHave_ && !inEnd_) {
            inHave_ = src_(in_.data(), in_.size());
            pulled_ += inHave_;
            inAt_ = 0;
            if (!inHave_) inEnd_ = true;
        }
        if (inAt_ == inHave_) break;
        bitbuf_ |= (uint64_t)in_[inAt_++] << bitcnt_;
        bitcnt_ += 8;
    }
    uint32_t e = t[(size_t)(bitbuf_ & ((1u << kTableBits) - 1))];
    unsigned l = e & 15;
    if (!l || l > bitcnt_) {
        err_ = l ? "The data ends too soon" : "The data is damaged (bad Huffman code)";
        return false;
    }
    bitbuf_ >>= l;
    bitcnt_ -= l;
    sym = e >> 4;
    return true;
}

void Inflater::put(uint8_t b) {
    ring_[pos_ & (kRing - 1)] = b;
    ++pos_;
    ++pending_;
}

bool Inflater::header() {
    uint32_t f, type;
    if (!bits(1, f) || !bits(2, type)) return false;
    final_ = f != 0;
    if (type == 0) {
        bitbuf_ >>= bitcnt_ & 7;
        bitcnt_ -= bitcnt_ & 7;
        uint32_t len, nlen;
        if (!bits(16, len) || !bits(16, nlen)) return false;
        if ((len ^ 0xFFFF) != nlen) {
            err_ = "The data is damaged (stored block length)";
            return false;
        }
        storedLeft_ = len;
        st_ = St::Stored;
        return true;
    }
    uint8_t lens[320] = {};
    unsigned nlit = 288, ndist = 32;
    if (type == 1) {
        for (unsigned i = 0; i < 144; ++i) lens[i] = 8;
        for (unsigned i = 144; i < 256; ++i) lens[i] = 9;
        for (unsigned i = 256; i < 280; ++i) lens[i] = 7;
        for (unsigned i = 280; i < 288; ++i) lens[i] = 8;
        for (unsigned i = 0; i < 32; ++i) lens[288 + i] = 5;
    } else if (type == 2) {
        uint32_t hlit, hdist, hclen;
        if (!bits(5, hlit) || !bits(5, hdist) || !bits(4, hclen)) return false;
        nlit = hlit + 257, ndist = hdist + 1;
        uint8_t cl[19] = {};
        for (unsigned i = 0; i < hclen + 4; ++i) {
            uint32_t v;
            if (!bits(3, v)) return false;
            cl[kClOrder[i]] = (uint8_t)v;
        }
        std::vector<uint32_t> ct;
        if (!buildTable(cl, 19, ct)) {
            err_ = "The data is damaged (code lengths)";
            return false;
        }
        unsigned i = 0;
        while (i < nlit + ndist) {
            unsigned sym;
            if (!symbol(ct, sym)) return false;
            uint32_t rep = 0;
            uint8_t val = 0;
            if (sym < 16) {
                lens[i++] = (uint8_t)sym;
                continue;
            }
            if (sym == 16) {
                if (i == 0 || !bits(2, rep)) {
                    if (err_.empty()) err_ = "The data is damaged (code lengths)";
                    return false;
                }
                val = lens[i - 1], rep += 3;
            } else if (sym == 17) {
                if (!bits(3, rep)) return false;
                rep += 3;
            } else {
                if (!bits(7, rep)) return false;
                rep += 11;
            }
            if (i + rep > nlit + ndist) {
                err_ = "The data is damaged (code lengths)";
                return false;
            }
            while (rep--) lens[i++] = val;
        }
        // The distance lengths follow the literal/length ones directly.
        uint8_t dl[32] = {};
        std::memcpy(dl, lens + nlit, ndist);
        std::memset(lens + nlit, 0, 320 - nlit);
        std::memcpy(lens + 288, dl, 32);
    } else {
        err_ = "The data is damaged (block type 3)";
        return false;
    }
    if (!buildTable(lens, nlit, lit_) || !buildTable(lens + 288, ndist, dist_)) {
        err_ = "The data is damaged (Huffman tables)";
        return false;
    }
    st_ = St::Huffman;
    return true;
}

bool Inflater::step() {
    switch (st_) {
        case St::End: return false;
        case St::Header:
            if (!header()) return false;
            return true;
        case St::Stored: {
            // Leftover bits are whole bytes after the alignment: take those first.
            while (storedLeft_ && bitcnt_ >= 8) {
                put((uint8_t)bitbuf_);
                bitbuf_ >>= 8;
                bitcnt_ -= 8;
                --storedLeft_;
            }
            while (storedLeft_ && pending_ < 65536) {
                if (inAt_ == inHave_) {
                    inHave_ = inEnd_ ? 0 : src_(in_.data(), in_.size());
            pulled_ += inHave_;
                    inAt_ = 0;
                    if (!inHave_) {
                        inEnd_ = true;
                        err_ = "The data ends too soon";
                        return false;
                    }
                }
                size_t take = std::min<size_t>({storedLeft_, inHave_ - inAt_, (size_t)(65536 - pending_)});
                for (size_t i = 0; i < take; ++i) put(in_[inAt_ + i]);
                inAt_ += take;
                storedLeft_ -= (uint32_t)take;
            }
            if (!storedLeft_) st_ = final_ ? St::End : St::Header;
            if (st_ == St::End) done_ = true;
            return true;
        }
        case St::Huffman: {
            // Up to a few symbols at a time (the caller loops).
            for (int k = 0; k < 64 && pending_ < 65536; ++k) {
                unsigned sym;
                if (!symbol(lit_, sym)) return false;
                if (sym < 256) {
                    put((uint8_t)sym);
                    continue;
                }
                if (sym == 256) {
                    st_ = final_ ? St::End : St::Header;
                    if (final_) done_ = true;
                    return true;
                }
                sym -= 257;
                if (sym >= 29) {
                    err_ = "The data is damaged (length code)";
                    return false;
                }
                uint32_t len, extra;
                if (d64_ && sym == 28) {
                    if (!bits(16, extra)) return false;
                    len = 3 + extra;
                } else {
                    if (!bits(kLenExtra[sym], extra)) return false;
                    len = kLenBase[sym] + extra;
                }
                unsigned dsym;
                if (!symbol(dist_, dsym)) return false;
                if (dsym >= (d64_ ? 32u : 30u)) {
                    err_ = "The data is damaged (distance code)";
                    return false;
                }
                if (!bits(kDistExtra[dsym], extra)) return false;
                uint32_t dist = kDistBase[dsym] + extra;
                if (dist > pos_ || dist > (d64_ ? 65536u : 32768u)) {
                    err_ = "The data is damaged (distance too far back)";
                    return false;
                }
                for (uint32_t i = 0; i < len; ++i) put(ring_[(pos_ - dist) & (kRing - 1)]);
            }
            return true;
        }
    }
    return false;
}

size_t Inflater::read(uint8_t* out, size_t n) {
    n = std::min<size_t>(n, 65536);  // keeps what is waiting plus the window inside the ring
    while (pending_ < n && st_ != St::End && err_.empty())
        if (!step()) break;
    size_t give = (size_t)std::min<uint64_t>(n, pending_);
    uint64_t from = pos_ - pending_;
    for (size_t i = 0; i < give; ++i) out[i] = ring_[(from + i) & (kRing - 1)];
    pending_ -= give;
    return give;
}

}  // namespace kz
