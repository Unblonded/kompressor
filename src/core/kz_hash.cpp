#include "kz_hash.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>

#include "kz_archive.hpp"

namespace kz {

namespace {

inline uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) s[2 * i] = d[p[i] >> 4], s[2 * i + 1] = d[p[i] & 15];
    return s;
}

// The 64-byte block buffering shared by MD5, SHA-1 and SHA-256.
struct Blocks {
    uint8_t buf[64];
    size_t used = 0;
    uint64_t total = 0;
    template <class F>
    void update(const uint8_t* p, size_t n, F&& block) {
        total += n;
        if (used) {
            size_t k = std::min(n, 64 - used);
            std::memcpy(buf + used, p, k);
            used += k, p += k, n -= k;
            if (used < 64) return;
            block(buf);
            used = 0;
        }
        for (; n >= 64; p += 64, n -= 64) block(p);
        std::memcpy(buf, p, n);
        used = n;
    }
    // Padding: 0x80, zeros, then the bit length (little- or big-endian).
    template <class F>
    void finish(bool bigEndian, F&& block) {
        uint64_t bits = total * 8;
        uint8_t pad[72] = {0x80};
        size_t k = used < 56 ? 56 - used : 120 - used;
        uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (bigEndian ? 56 - 8 * i : 8 * i));
        uint64_t keep = total;
        update(pad, k, block);
        update(len, 8, block);
        total = keep;
    }
};

struct Md5 {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    Blocks b;
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
            0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
            0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                                  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (uint32_t)p[4 * i + 3] << 24;
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) f = (bb & c) | (~bb & d), g = i;
            else if (i < 32) f = (d & bb) | (~d & c), g = (5 * i + 1) & 15;
            else if (i < 48) f = bb ^ c ^ d, g = (3 * i + 5) & 15;
            else f = c ^ (bb | ~d), g = (7 * i) & 15;
            uint32_t t = d;
            d = c;
            c = bb;
            bb = bb + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d;
    }
    void update(const uint8_t* p, size_t n) { b.update(p, n, [this](const uint8_t* x) { block(x); }); }
    std::string hex() {
        b.finish(false, [this](const uint8_t* x) { block(x); });
        uint8_t o[16];
        for (int i = 0; i < 16; ++i) o[i] = (uint8_t)(h[i / 4] >> (8 * (i % 4)));
        return toHex(o, 16);
    }
};

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    Blocks b;
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else f = bb ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(bb, 30), bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    }
    void update(const uint8_t* p, size_t n) { b.update(p, n, [this](const uint8_t* x) { block(x); }); }
    std::string hex() {
        b.finish(true, [this](const uint8_t* x) { block(x); });
        uint8_t o[20];
        for (int i = 0; i < 20; ++i) o[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        return toHex(o, 20);
    }
};

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Blocks b;
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & bb) ^ (a & c) ^ (bb & c);
            uint32_t t2 = S0 + mj;
            hh = g, g = f, f = e, e = d + t1, d = c, c = bb, bb = a, a = t1 + t2;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) { b.update(p, n, [this](const uint8_t* x) { block(x); }); }
    std::string hex() {
        b.finish(true, [this](const uint8_t* x) { block(x); });
        uint8_t o[32];
        for (int i = 0; i < 32; ++i) o[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
        return toHex(o, 32);
    }
};

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool isHex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

}  // namespace

struct Digest::State {
    uLong crc = 0;
    Md5 md5;
    Sha1 sha1;
    Sha256 sha256;
};

Digest::Digest(HashKind k) : kind_(k), st_(new State) { st_->crc = crc32(0, nullptr, 0); }
Digest::~Digest() { delete st_; }

void Digest::update(const uint8_t* p, size_t n) {
    switch (kind_) {
        case kHashCrc32:
            while (n) {
                uInt k = (uInt)std::min<size_t>(n, 1u << 30);
                st_->crc = crc32(st_->crc, p, k);
                p += k, n -= k;
            }
            break;
        case kHashMd5: st_->md5.update(p, n); break;
        case kHashSha1: st_->sha1.update(p, n); break;
        case kHashSha256: st_->sha256.update(p, n); break;
    }
}

std::string Digest::hex() {
    switch (kind_) {
        case kHashCrc32: {
            char b[9];
            std::snprintf(b, sizeof b, "%08lx", (unsigned long)st_->crc);
            return b;
        }
        case kHashMd5: return st_->md5.hex();
        case kHashSha1: return st_->sha1.hex();
        default: return st_->sha256.hex();
    }
}

bool hashArchive(const Source& src, const std::vector<std::string>& only, const std::string& password, unsigned which,
                 std::vector<EntryHash>& out, Progress& prog, std::string& err) {
    out.clear();
    static const HashKind kinds[] = {kHashCrc32, kHashMd5, kHashSha1, kHashSha256};
    std::vector<std::unique_ptr<Digest>> d;
    auto wanted = [&](const std::string& path) {
        if (only.empty()) return true;
        for (auto& o : only)
            if (path == o || (path.size() > o.size() && path.compare(0, o.size(), o) == 0 && path[o.size()] == '/')) return true;
        return false;
    };
    EntryVisitor v;
    v.begin = [&](const std::string& path, uint64_t size) {
        if (!wanted(path)) return false;
        out.push_back({path, size});
        d.clear();
        for (HashKind k : kinds) d.push_back(which & k ? std::make_unique<Digest>(k) : nullptr);
        return true;
    };
    v.data = [&](const uint8_t* p, size_t n) {
        for (auto& x : d)
            if (x) x->update(p, n);
        return !prog.cancelled();
    };
    v.end = [&](bool ok, const std::string& e) {
        EntryHash& h = out.back();
        if (!ok) {
            h.error = e.empty() ? "Could not be read" : e;
            return;
        }
        std::string* slot[] = {&h.crc32, &h.md5, &h.sha1, &h.sha256};
        for (size_t i = 0; i < 4; ++i)
            if (d[i]) *slot[i] = d[i]->hex();
    };
    return visitEntries(src, password, v, prog, err);
}

bool hashFiles(const std::vector<std::string>& inputs, unsigned which, std::vector<EntryHash>& out, std::string& base,
               Progress& prog, std::string& err) {
    namespace fs = std::filesystem;
    out.clear();
    base.clear();
    if (inputs.empty()) return err = "Nothing to hash", false;
    std::error_code ec;
    // The base: one folder -> itself; otherwise the folder the inputs share.
    std::vector<fs::path> ins;
    for (auto& i : inputs) {
        fs::path p = fs::absolute(u8path(i), ec).lexically_normal();
        if (!p.has_filename() && p.has_parent_path() && p != p.root_path()) p = p.parent_path();  // "dir\" -> "dir"
        ins.push_back(p);
    }
    fs::path b;
    if (ins.size() == 1 && fs::is_directory(ins[0], ec)) {
        b = ins[0];
    } else {
        b = ins[0].parent_path();
        for (size_t i = 1; i < ins.size(); ++i) {
            fs::path parent = ins[i].parent_path(), common;
            auto x = b.begin(), y = parent.begin();
            for (; x != b.end() && y != parent.end(); ++x, ++y) {
                std::string sx = pathToUtf8(*x), sy = pathToUtf8(*y);
                bool same = sx.size() == sy.size() && std::equal(sx.begin(), sx.end(), sy.begin(), [](char a, char c) {
                                return std::tolower((unsigned char)a) == std::tolower((unsigned char)c);
                            });
                if (!same) break;
                common /= *x;
            }
            b = common;
        }
    }
    base = pathToUtf8(b);

    // Every file, with its size, so progress can count bytes.
    struct F {
        fs::path p;
        std::string rel;
        uint64_t size;
    };
    std::vector<F> files;
    auto relOf = [&](const fs::path& p) {
        std::string r = pathToUtf8(p.lexically_relative(b));
        if (r.empty() || r == ".") r = pathToUtf8(p.filename());
        std::replace(r.begin(), r.end(), '\\', '/');
        return r;
    };
    for (auto& in : ins) {
        if (prog.cancelled()) return err = "Cancelled", false;
        if (fs::is_directory(in, ec)) {
            prog.setStatus("Listing " + pathToUtf8(in.filename()));
            for (auto it = fs::recursive_directory_iterator(in, fs::directory_options::skip_permission_denied, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                std::error_code e2;
                if (!it->is_regular_file(e2)) continue;
                files.push_back({it->path(), relOf(it->path()), (uint64_t)it->file_size(e2)});
                if ((files.size() & 1023) == 0 && prog.cancelled()) return err = "Cancelled", false;
            }
            if (ec) prog.warn("Could not list everything in " + pathToUtf8(in) + ": " + ec.message());
            ec.clear();
        } else if (fs::is_regular_file(in, ec)) {
            files.push_back({in, relOf(in), (uint64_t)fs::file_size(in, ec)});
        } else {
            EntryHash h;
            h.path = relOf(in);
            h.error = "Not found";
            out.push_back(h);
        }
    }
    std::sort(files.begin(), files.end(), [](const F& x, const F& y) { return x.rel < y.rel; });
    uint64_t total = 0;
    for (auto& f : files) total += f.size;
    prog.total = total;
    prog.done = 0;

    static const HashKind kinds[] = {kHashCrc32, kHashMd5, kHashSha1, kHashSha256};
    std::vector<uint8_t> buf(1 << 20);
    for (auto& f : files) {
        if (prog.cancelled()) return err = "Cancelled", false;
        prog.setStatus(f.rel);
        EntryHash h;
        h.path = f.rel;
        h.size = f.size;
        std::unique_ptr<Digest> d[4];
        for (int i = 0; i < 4; ++i)
            if (which & kinds[i]) d[i] = std::make_unique<Digest>(kinds[i]);
        std::ifstream in(f.p, std::ios::binary);
        if (!in) {
            h.error = "Could not be opened";
        } else {
            uint64_t got = 0;
            while (in) {
                in.read((char*)buf.data(), (std::streamsize)buf.size());
                size_t n = (size_t)in.gcount();
                if (!n) break;
                for (auto& x : d)
                    if (x) x->update(buf.data(), n);
                got += n;
                prog.done += n;
                if (prog.cancelled()) return err = "Cancelled", false;
            }
            if (in.bad()) h.error = "Could not be read completely";
            else {
                std::string* slot[] = {&h.crc32, &h.md5, &h.sha1, &h.sha256};
                for (int i = 0; i < 4; ++i)
                    if (d[i]) *slot[i] = d[i]->hex();
                h.size = got;
            }
        }
        out.push_back(std::move(h));
    }
    return true;
}

std::string formatHashList(const std::vector<EntryHash>& hashes, HashKind kind) {
    std::string s;
    for (auto& h : hashes) {
        const std::string& x = h.get(kind);
        if (x.empty()) continue;
        if (kind == kHashCrc32) s += h.path + " " + x + "\n";  // .sfv: name, then CRC
        else s += x + " *" + h.path + "\n";
    }
    return s;
}

std::vector<ListedHash> parseHashList(const std::string& text) {
    std::vector<ListedHash> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t nl = text.find('\n', at);
        std::string line = trim(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        at = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.size() >= 3 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) line = line.substr(3);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        ListedHash h;
        // BSD style: "SHA256 (name) = hex"
        size_t open = line.find(" ("), close = line.rfind(") = ");
        if (open != std::string::npos && close != std::string::npos && close > open) {
            h.name = line.substr(open + 2, close - open - 2);
            h.hex = trim(line.substr(close + 4));
        } else {
            size_t sp = line.find_first_of(" \t");
            if (sp == std::string::npos) continue;
            std::string first = line.substr(0, sp), rest = trim(line.substr(sp));
            if (isHex(first) && hashKindOfHex(first)) {  // "hex  name" / "hex *name"
                h.hex = first;
                h.name = !rest.empty() && rest[0] == '*' ? rest.substr(1) : rest;
            } else {  // .sfv: "name crc32"
                size_t last = line.find_last_of(" \t");
                h.name = trim(line.substr(0, last));
                h.hex = line.substr(last + 1);
            }
        }
        h.hex = lowerStr(h.hex);
        if (!isHex(h.hex) || !hashKindOfHex(h.hex) || h.name.empty()) continue;
        std::replace(h.name.begin(), h.name.end(), '\\', '/');
        while (h.name.compare(0, 2, "./") == 0) h.name.erase(0, 2);
        out.push_back(std::move(h));
    }
    return out;
}

unsigned hashKindOfHex(const std::string& hex) {
    switch (hex.size()) {
        case 8: return kHashCrc32;
        case 32: return kHashMd5;
        case 40: return kHashSha1;
        case 64: return kHashSha256;
        default: return 0;
    }
}

unsigned hashKindsOfList(const std::vector<ListedHash>& list) {
    unsigned m = 0;
    for (auto& l : list) m |= hashKindOfHex(l.hex);
    return m;
}

std::vector<HashCheck> checkHashes(const std::vector<EntryHash>& hashes, const std::vector<ListedHash>& list,
                                   std::vector<std::string>* notListed) {
    std::map<std::string, size_t> exact, folded;
    for (size_t i = 0; i < hashes.size(); ++i) {
        exact.emplace(hashes[i].path, i);
        folded.emplace(lowerStr(hashes[i].path), i);
    }
    auto endsWith = [](const std::string& a, const std::string& b) {  // a ends with "/" + b
        return a.size() > b.size() && a[a.size() - b.size() - 1] == '/' && a.compare(a.size() - b.size(), b.size(), b) == 0;
    };
    std::vector<bool> used(hashes.size());
    std::vector<HashCheck> out;
    for (auto& l : list) {
        HashCheck c;
        c.name = l.name;
        c.expected = l.hex;
        c.kind = (HashKind)hashKindOfHex(l.hex);
        size_t at = SIZE_MAX;
        auto e = exact.find(l.name);
        if (e != exact.end()) at = e->second;
        else if ((e = folded.find(lowerStr(l.name))) != folded.end()) at = e->second;
        else {  // one side has an extra leading folder
            std::string ln = lowerStr(l.name);
            size_t hits = 0;
            for (size_t i = 0; i < hashes.size(); ++i) {
                std::string hp = lowerStr(hashes[i].path);
                if (endsWith(hp, ln) || endsWith(ln, hp)) at = i, ++hits;
            }
            if (hits != 1) at = SIZE_MAX;
        }
        if (at == SIZE_MAX) {
            c.state = HashCheck::Missing;
        } else {
            used[at] = true;
            const EntryHash& h = hashes[at];
            c.entry = h.path;
            c.actual = h.get(c.kind);
            c.state = !h.error.empty() || c.actual.empty() ? HashCheck::Unreadable
                      : c.actual == c.expected             ? HashCheck::Ok
                                                           : HashCheck::Mismatch;
        }
        out.push_back(std::move(c));
    }
    if (notListed)
        for (size_t i = 0; i < hashes.size(); ++i)
            if (!used[i]) notListed->push_back(hashes[i].path);
    return out;
}

const char* hashName(HashKind k) {
    return k == kHashCrc32 ? "CRC32" : k == kHashMd5 ? "MD5" : k == kHashSha1 ? "SHA-1" : "SHA-256";
}

const char* hashListExt(HashKind k) {
    return k == kHashCrc32 ? ".sfv" : k == kHashMd5 ? ".md5" : k == kHashSha1 ? ".sha1" : ".sha256";
}

}  // namespace kz
