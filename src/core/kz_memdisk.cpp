#include "kz_memdisk.hpp"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace kz {

namespace {

constexpr uint32_t kBlock = 512;
constexpr uint64_t kChunk = 1ull << 20;        // memory is taken and given back this much at a time
constexpr uint32_t kOurMaxRecv = 262144;       // the largest data segment we accept
constexpr uint32_t kOurMaxBurst = 1u << 20;
constexpr uint32_t kOurFirstBurst = 262144;
constexpr uint32_t kCmdWindow = 128;           // commands the initiator may have queued
constexpr uint32_t kNoTag = 0xffffffffu;

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void put24(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 16), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v); }
void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v >> 16)), put16(p + 2, uint16_t(v)); }
void put64(uint8_t* p, uint64_t v) { put32(p, uint32_t(v >> 32)), put32(p + 4, uint32_t(v)); }

// ------------------------------------------------------------------------------------------ MD5 (CHAP)
class Md5 {
public:
    void update(const void* data, size_t n) {
        auto* p = static_cast<const uint8_t*>(data);
        len_ += n;
        while (n) {
            size_t k = (std::min)(n, size_t(64) - used_);
            std::memcpy(buf_ + used_, p, k);
            used_ += k, p += k, n -= k;
            if (used_ == 64) block(buf_), used_ = 0;
        }
    }
    void final(uint8_t out[16]) {
        uint64_t bits = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (used_ != 56) update(&pad, 1);
        uint8_t l[8];
        for (int i = 0; i < 8; ++i) l[i] = uint8_t(bits >> (8 * i));
        update(l, 8);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(h_[i] >> (8 * j));
    }

private:
    static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const int S[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
        static uint32_t K[64];
        static bool ready = [] {
            for (int i = 0; i < 64; ++i) K[i] = uint32_t(std::floor(std::fabs(std::sin(double(i + 1))) * 4294967296.0));
            return true;
        }();
        (void)ready;
        uint32_t M[16];
        for (int i = 0; i < 16; ++i) M[i] = p[i * 4] | p[i * 4 + 1] << 8 | p[i * 4 + 2] << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) f = (b & c) | (~b & d), g = i;
            else if (i < 32) f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
            else if (i < 48) f = b ^ c ^ d, g = (3 * i + 5) % 16;
            else f = c ^ (b | ~d), g = (7 * i) % 16;
            f += a + K[i] + M[g];
            a = d, d = c, c = b;
            b += rol(f, S[(i / 16) * 4 + i % 4]);
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d;
    }
    uint32_t h_[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t used_ = 0;
};

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15];
    return s;
}

bool md5Works() {
    Md5 m;
    m.update("abc", 3);
    uint8_t o[16];
    m.final(o);
    return hex(o, 16) == "900150983cd24fb0d6963f7d28e17f72";
}

// "0x..." (or plain hex) to bytes.
bool unhex(std::string s, std::vector<uint8_t>& out) {
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
    if (s.empty() || s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        int hi = v(s[i]), lo = v(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

// ------------------------------------------------------------------------------------------ the blocks
// The disk's contents, in 1 MiB pieces of memory taken when first written. Pieces never written read as
// zeros, and a piece Windows trims completely is given back.
class Store {
public:
    explicit Store(uint64_t size) : size_(size), chunks_(size_t((size + kChunk - 1) / kChunk), nullptr) {}
    ~Store() {
        for (uint8_t* c : chunks_)
            if (c) VirtualFree(c, 0, MEM_RELEASE);
    }
    uint64_t size() const { return size_; }
    uint64_t resident() const { return resident_.load(); }

    void read(uint64_t off, uint8_t* dst, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        while (len) {
            size_t i = size_t(off / kChunk), o = size_t(off % kChunk), n = (std::min)(len, size_t(kChunk) - o);
            if (chunks_[i]) std::memcpy(dst, chunks_[i] + o, n);
            else std::memset(dst, 0, n);
            off += n, dst += n, len -= n;
        }
    }
    // False when the memory couldn't be had.
    bool write(uint64_t off, const uint8_t* src, size_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        while (len) {
            size_t i = size_t(off / kChunk), o = size_t(off % kChunk), n = (std::min)(len, size_t(kChunk) - o);
            if (!chunks_[i] && !allZero(src, n)) {
                chunks_[i] = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (!chunks_[i]) return false;
                resident_ += kChunk;
            }
            if (chunks_[i]) std::memcpy(chunks_[i] + o, src, n);
            off += n, src += n, len -= n;
        }
        return true;
    }
    void unmap(uint64_t off, uint64_t len) {
        std::lock_guard<std::mutex> lk(mu_);
        uint64_t end = (std::min)(off + len, size_);
        while (off < end) {
            size_t i = size_t(off / kChunk), o = size_t(off % kChunk);
            uint64_t n = (std::min)(end - off, kChunk - o);
            if (chunks_[i]) {
                bool whole = o == 0 && (n == kChunk || off + n == size_);
                if (whole) {
                    VirtualFree(chunks_[i], 0, MEM_RELEASE);
                    chunks_[i] = nullptr;
                    resident_ -= kChunk;
                } else {
                    std::memset(chunks_[i] + o, 0, size_t(n));
                }
            }
            off += n;
        }
    }

private:
    static bool allZero(const uint8_t* p, size_t n) {
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            if (v) return false;
        }
        for (; i < n; ++i)
            if (p[i]) return false;
        return true;
    }
    uint64_t size_;
    std::vector<uint8_t*> chunks_;
    std::atomic<uint64_t> resident_{0};
    std::mutex mu_;
};

// ------------------------------------------------------------------------------------------ sockets
bool recvAll(SOCKET s, void* p, size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        int k = recv(s, c, int((std::min)(n, size_t(1) << 30)), 0);
        if (k <= 0) return false;
        c += k, n -= size_t(k);
    }
    return true;
}

bool sendAll(SOCKET s, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        int k = send(s, c, int((std::min)(n, size_t(1) << 30)), 0);
        if (k <= 0) return false;
        c += k, n -= size_t(k);
    }
    return true;
}

struct Pdu {
    uint8_t h[48];
    std::vector<uint8_t> data;
    uint8_t op() const { return h[0] & 0x3f; }
    bool immediate() const { return (h[0] & 0x40) != 0; }
    uint32_t itt() const { return be32(h + 16); }
};

bool readPdu(SOCKET s, Pdu& p) {
    if (!recvAll(s, p.h, 48)) return false;
    size_t ahs = size_t(p.h[4]) * 4;
    uint32_t dsl = be24(p.h + 5);
    if (dsl > kOurMaxRecv) return false;
    uint8_t skip[1024];
    if (ahs > sizeof skip || (ahs && !recvAll(s, skip, ahs))) return false;
    p.data.resize((size_t(dsl) + 3) & ~size_t(3));
    if (!p.data.empty() && !recvAll(s, p.data.data(), p.data.size())) return false;
    p.data.resize(dsl);
    return true;
}

bool sendPdu(SOCKET s, uint8_t* h, const uint8_t* data, size_t len) {
    put24(h + 5, uint32_t(len));
    static const char zeros[4] = {};
    size_t pad = (4 - len % 4) % 4;
    WSABUF b[3];
    DWORD n = 0;
    b[n].len = 48, b[n++].buf = reinterpret_cast<char*>(h);
    if (len) b[n].len = ULONG(len), b[n++].buf = const_cast<char*>(reinterpret_cast<const char*>(data));
    if (pad) b[n].len = ULONG(pad), b[n++].buf = const_cast<char*>(zeros);
    DWORD sent = 0;
    if (WSASend(s, b, n, &sent, 0, nullptr, nullptr) != 0) return false;
    // A blocking send normally takes it all; if not, send the rest piece by piece.
    size_t skip = sent;
    for (DWORD i = 0; i < n; ++i) {
        if (skip >= b[i].len) {
            skip -= b[i].len;
            continue;
        }
        if (!sendAll(s, b[i].buf + skip, b[i].len - skip)) return false;
        skip = 0;
    }
    return true;
}

using Keys = std::vector<std::pair<std::string, std::string>>;

Keys parseKeys(const std::vector<uint8_t>& d) {
    Keys out;
    size_t start = 0;
    for (size_t i = 0; i <= d.size(); ++i) {
        if (i < d.size() && d[i] != 0) continue;
        if (i > start) {
            std::string kv(reinterpret_cast<const char*>(d.data()) + start, i - start);
            size_t eq = kv.find('=');
            if (eq != std::string::npos) out.emplace_back(kv.substr(0, eq), kv.substr(eq + 1));
        }
        start = i + 1;
    }
    return out;
}

bool listHas(const std::string& list, const std::string& v) {
    size_t a = 0;
    while (a <= list.size()) {
        size_t b = list.find(',', a);
        if (b == std::string::npos) b = list.size();
        if (list.compare(a, b - a, v) == 0) return true;
        a = b + 1;
    }
    return false;
}

uint32_t toU32(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 10)); }

}  // namespace

// ============================================================================================== target
struct MemDisk::Impl {
    std::unique_ptr<Store> store;
    std::string target, chapName = "kompressor", chapSecret, serial;
    uint8_t naa[8] = {};
    uint16_t port = 0;
    SOCKET listener = INVALID_SOCKET;
    std::thread acceptor;
    std::mutex mu;
    std::vector<SOCKET> live;
    std::vector<std::thread> workers;
    std::atomic<bool> stopping{false};
    std::atomic<uint16_t> nextTsih{1};
    bool wsa = false;

    void acceptLoop();
    void serve(SOCKET s);
};

namespace {

struct Result {
    uint8_t status = 0;  // 0 good, 2 check condition
    uint8_t sense[18] = {};
    std::vector<uint8_t> in;  // data for the initiator
    void check(uint8_t key, uint8_t asc, uint8_t ascq) {
        status = 2;
        std::memset(sense, 0, sizeof sense);
        sense[0] = 0x70, sense[2] = key, sense[7] = 10, sense[12] = asc, sense[13] = ascq;
    }
};

struct WriteTask {
    uint32_t itt = 0, ttt = 0, r2tSN = 0, got = 0;
    uint8_t lun[8] = {}, cdb[16] = {};
    std::vector<uint8_t> buf;
};

class Session {
public:
    Session(MemDisk::Impl& d, SOCKET s) : d_(d), s_(s) {}

    void run() {
        Pdu p;
        if (!readPdu(s_, p) || !login(p)) return;
        while (readPdu(s_, p)) {
            bool ok = true;
            switch (p.op()) {
                case 0x00: ok = nopOut(p); break;
                case 0x01: ok = scsiCommand(p); break;
                case 0x02: ok = taskManagement(p); break;
                case 0x04: ok = text(p); break;
                case 0x05: ok = dataOut(p); break;
                case 0x06: logout(p); return;
                default: ok = reject(p, 0x04); break;
            }
            if (!ok) return;
        }
    }

private:
    // ------------------------------------------------------------------------------------ helpers
    void seq(uint8_t* h) {
        put32(h + 28, expCmdSN_);
        put32(h + 32, expCmdSN_ + kCmdWindow - 1);
    }
    void advance(const Pdu& p) {
        if (!p.immediate()) expCmdSN_ = be32(p.h + 24) + 1;
    }
    bool lun0(const uint8_t* lun) const {
        if (be16(lun) & 0x3fff) return false;
        for (int i = 2; i < 8; ++i)
            if (lun[i]) return false;
        return true;
    }

    // ------------------------------------------------------------------------------------ login
    bool login(Pdu& p) {
        bool first = true, authed = false, sentRecv = false;
        uint8_t chapId = 0;
        std::vector<uint8_t> challenge;
        std::random_device rd;
        for (;;) {
            if (p.op() != 0x03) return false;
            uint8_t flags = p.h[1];
            bool T = (flags & 0x80) != 0;
            int csg = (flags >> 2) & 3, nsg = flags & 3;
            uint8_t isid[6];
            std::memcpy(isid, p.h + 8, 6);
            uint32_t itt = p.itt();
            expCmdSN_ = be32(p.h + 24);
            Keys keys = parseKeys(p.data);
            auto get = [&](const char* k) -> const std::string* {
                for (auto& kv : keys)
                    if (kv.first == k) return &kv.second;
                return nullptr;
            };
            std::string out;
            auto add = [&](const std::string& k, const std::string& v) {
                out += k + "=" + v;
                out.push_back('\0');
            };
            uint8_t cls = 0, det = 0;
            auto fail = [&](uint8_t c, uint8_t d) {
                if (!cls) cls = c, det = d;
            };

            if (first) {
                const std::string* type = get("SessionType");
                discovery_ = type && *type == "Discovery";
                const std::string* tn = get("TargetName");
                if (!get("InitiatorName")) fail(2, 7);
                else if (!discovery_ && (!tn || *tn != d_.target)) fail(2, 3);
                add("TargetPortalGroupTag", "1");
            }
            if (csg == 0) {
                if (const std::string* m = get("AuthMethod")) {
                    if (listHas(*m, "CHAP")) add("AuthMethod", "CHAP");
                    else fail(2, 1);
                }
                if (const std::string* a = get("CHAP_A")) {
                    if (!listHas(*a, "5")) {
                        fail(2, 1);
                    } else {
                        chapId = uint8_t(rd());
                        challenge.resize(16);
                        for (auto& b : challenge) b = uint8_t(rd());
                        add("CHAP_A", "5");
                        add("CHAP_I", std::to_string(chapId));
                        add("CHAP_C", "0x" + hex(challenge.data(), challenge.size()));
                    }
                }
                const std::string* n = get("CHAP_N");
                const std::string* r = get("CHAP_R");
                if (n || r) {
                    std::vector<uint8_t> got;
                    uint8_t want[16];
                    Md5 m;
                    m.update(&chapId, 1);
                    m.update(d_.chapSecret.data(), d_.chapSecret.size());
                    m.update(challenge.data(), challenge.size());
                    m.final(want);
                    if (!n || !r || challenge.empty() || *n != d_.chapName || !unhex(*r, got) || got.size() != 16 ||
                        std::memcmp(got.data(), want, 16) != 0)
                        fail(2, 1);
                    else authed = true;
                    challenge.clear();  // one answer per challenge
                }
                if (T && !authed) fail(2, 1);  // leaving the security stage without logging in
            } else if (!authed) {
                fail(2, 1);  // skipped the security stage
            }
            negotiate(keys, add);
            if (csg == 1 && !sentRecv) {
                add("MaxRecvDataSegmentLength", std::to_string(kOurMaxRecv));
                sentRecv = true;
            }
            bool transit = T && !cls;
            bool full = transit && nsg == 3;
            if (full) tsih_ = d_.nextTsih++;

            uint8_t h[48] = {};
            h[0] = 0x23;
            h[1] = uint8_t((transit ? 0x80 : 0) | (csg << 2) | (transit ? nsg : 0));
            std::memcpy(h + 8, isid, 6);
            put16(h + 14, full ? tsih_ : 0);
            put32(h + 16, itt);
            put32(h + 24, statSN_++);
            seq(h);
            h[36] = cls, h[37] = det;
            if (cls) out.clear();
            if (!sendPdu(s_, h, reinterpret_cast<const uint8_t*>(out.data()), out.size()) || cls) return false;
            if (full) return true;
            first = false;
            if (!readPdu(s_, p)) return false;
        }
    }

    template <class Add>
    void negotiate(const Keys& keys, Add& add) {
        for (auto& [k, v] : keys) {
            if (k == "InitiatorName" || k == "InitiatorAlias" || k == "TargetName" || k == "SessionType" || k == "AuthMethod" ||
                k.compare(0, 5, "CHAP_") == 0)
                continue;
            if (k == "HeaderDigest" || k == "DataDigest") add(k, "None");
            else if (k == "MaxRecvDataSegmentLength") theirMaxRecv_ = (std::max)(512u, (std::min)(toU32(v), 16u << 20));
            else if (k == "MaxBurstLength") add(k, std::to_string(maxBurst_ = (std::max)(512u, (std::min)(toU32(v), kOurMaxBurst))));
            else if (k == "FirstBurstLength") add(k, std::to_string(firstBurst_ = (std::max)(512u, (std::min)(toU32(v), kOurFirstBurst))));
            else if (k == "InitialR2T") {
                // "No" lets the initiator send a write's first part without waiting for an R2T: one round
                // trip less per write.
                initialR2T_ = v != "No";
                add(k, initialR2T_ ? "Yes" : "No");
            }
            else if (k == "ImmediateData") add(k, v == "Yes" ? "Yes" : "No");
            else if (k == "MaxConnections" || k == "MaxOutstandingR2T") add(k, "1");
            else if (k == "DataPDUInOrder" || k == "DataSequenceInOrder") add(k, "Yes");
            else if (k == "ErrorRecoveryLevel") add(k, "0");
            else if (k == "DefaultTime2Wait" || k == "DefaultTime2Retain") add(k, v);
            else if (k == "IFMarker" || k == "OFMarker") add(k, "No");
            else if (k == "IFMarkInt" || k == "OFMarkInt") add(k, "Irrelevant");
            else add(k, "NotUnderstood");
        }
    }

    // ------------------------------------------------------------------------------------ PDUs
    bool nopOut(const Pdu& p) {
        advance(p);
        if (p.itt() == kNoTag) return true;
        uint8_t h[48] = {};
        h[0] = 0x20, h[1] = 0x80;
        std::memcpy(h + 8, p.h + 8, 8);
        put32(h + 16, p.itt());
        put32(h + 20, kNoTag);
        put32(h + 24, statSN_++);
        seq(h);
        return sendPdu(s_, h, p.data.data(), p.data.size());
    }

    bool taskManagement(const Pdu& p) {
        advance(p);
        int fn = p.h[1] & 0x7f;
        uint32_t ref = be32(p.h + 20);
        for (auto it = writes_.begin(); it != writes_.end();) {
            if (fn != 1 || it->second.itt == ref) it = writes_.erase(it);
            else ++it;
        }
        uint8_t h[48] = {};
        h[0] = 0x22, h[1] = 0x80, h[2] = 0;  // function complete
        put32(h + 16, p.itt());
        put32(h + 24, statSN_++);
        seq(h);
        return sendPdu(s_, h, nullptr, 0);
    }

    bool text(const Pdu& p) {
        advance(p);
        std::string out;
        for (auto& [k, v] : parseKeys(p.data)) {
            if (k == "SendTargets") {
                out += "TargetName=" + d_.target;
                out.push_back('\0');
                out += "TargetAddress=127.0.0.1:" + std::to_string(d_.port) + ",1";
                out.push_back('\0');
            }
        }
        uint8_t h[48] = {};
        h[0] = 0x24, h[1] = 0x80;
        std::memcpy(h + 8, p.h + 8, 8);
        put32(h + 16, p.itt());
        put32(h + 20, kNoTag);
        put32(h + 24, statSN_++);
        seq(h);
        return sendPdu(s_, h, reinterpret_cast<const uint8_t*>(out.data()), out.size());
    }

    void logout(const Pdu& p) {
        advance(p);
        uint8_t h[48] = {};
        h[0] = 0x26, h[1] = 0x80;
        put32(h + 16, p.itt());
        put32(h + 24, statSN_++);
        seq(h);
        sendPdu(s_, h, nullptr, 0);
    }

    bool reject(const Pdu& p, uint8_t reason) {
        uint8_t h[48] = {};
        h[0] = 0x3f, h[1] = 0x80, h[2] = reason;
        put32(h + 16, kNoTag);
        put32(h + 24, statSN_++);
        seq(h);
        return sendPdu(s_, h, p.h, 48);
    }

    bool scsiCommand(Pdu& p) {
        advance(p);
        if (discovery_) return reject(p, 0x04);
        uint32_t edtl = be32(p.h + 20);
        bool write = (p.h[1] & 0x20) != 0;
        if (write && edtl) {
            WriteTask t;
            t.itt = p.itt();
            std::memcpy(t.lun, p.h + 8, 8);
            std::memcpy(t.cdb, p.h + 32, 16);
            if (edtl > 64u << 20) return false;
            t.buf.resize(edtl);
            size_t imm = (std::min)(p.data.size(), size_t(edtl));
            std::memcpy(t.buf.data(), p.data.data(), imm);
            t.got = uint32_t(imm);
            if (t.got >= edtl) return execute(t.itt, t.lun, t.cdb, t.buf.data(), edtl, edtl, 0);
            t.ttt = nextTtt_++;
            if (nextTtt_ == kNoTag) nextTtt_ = 1;
            // Without InitialR2T the initiator sends up to FirstBurstLength (immediate data included)
            // unsolicited; R2Ts ask for the rest after that.
            uint32_t unsolicited = initialR2T_ ? t.got : (std::min)(edtl, firstBurst_);
            if (t.got >= unsolicited && !sendR2T(t)) return false;
            uint32_t itt = t.itt;
            writes_[itt] = std::move(t);
            return true;
        }
        return execute(p.itt(), p.h + 8, p.h + 32, nullptr, 0, edtl, 0);
    }

    bool sendR2T(WriteTask& t) {
        uint32_t want = (std::min)(uint32_t(t.buf.size()) - t.got, maxBurst_);
        uint8_t h[48] = {};
        h[0] = 0x31, h[1] = 0x80;
        std::memcpy(h + 8, t.lun, 8);
        put32(h + 16, t.itt);
        put32(h + 20, t.ttt);
        put32(h + 24, statSN_);  // the next StatSN, not used up
        seq(h);
        put32(h + 36, t.r2tSN++);
        put32(h + 40, t.got);
        put32(h + 44, want);
        return sendPdu(s_, h, nullptr, 0);
    }

    bool dataOut(const Pdu& p) {
        auto it = writes_.find(p.itt());
        if (it == writes_.end()) return true;  // for a task that was aborted
        WriteTask& t = it->second;
        uint32_t off = be32(p.h + 40);
        if (uint64_t(off) + p.data.size() > t.buf.size()) return false;
        std::memcpy(t.buf.data() + off, p.data.data(), p.data.size());
        t.got = (std::max)(t.got, uint32_t(off + p.data.size()));
        if (!(p.h[1] & 0x80)) return true;  // more of this burst to come
        if (t.got < t.buf.size()) return sendR2T(t);
        WriteTask done = std::move(t);
        writes_.erase(it);
        return execute(done.itt, done.lun, done.cdb, done.buf.data(), uint32_t(done.buf.size()), uint32_t(done.buf.size()),
                       done.r2tSN);
    }

    // ------------------------------------------------------------------------------------ SCSI
    bool execute(uint32_t itt, const uint8_t* lun, const uint8_t* cdb, const uint8_t* out, uint32_t outLen, uint32_t edtl,
                 uint32_t r2ts) {
        Result r;
        scsi(lun, cdb, out, outLen, r);
        return respond(itt, lun, r, edtl, r2ts);
    }

    void scsi(const uint8_t* lun, const uint8_t* cdb, const uint8_t* out, uint32_t outLen, Result& r) {
        Store& st = *d_.store;
        uint64_t blocks = st.size() / kBlock;
        uint8_t op = cdb[0];
        if (op == 0xA0) {  // REPORT LUNS
            r.in.assign(16, 0);
            put32(r.in.data(), 8);
            r.in.resize((std::min)(r.in.size(), size_t(be32(cdb + 6))));
            return;
        }
        if (op == 0x12) return inquiry(lun0(lun), cdb, r);
        if (!lun0(lun)) return r.check(0x05, 0x25, 0x00);  // no such LUN
        switch (op) {
            case 0x00:  // TEST UNIT READY
            case 0x1B:  // START STOP UNIT
            case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
            case 0x35:  // SYNCHRONIZE CACHE (10)
            case 0x91:  // SYNCHRONIZE CACHE (16)
            case 0x2F:  // VERIFY (10)
            case 0x8F:  // VERIFY (16)
            case 0x15:  // MODE SELECT (6)
            case 0x55:  // MODE SELECT (10)
                return;
            case 0x03: {  // REQUEST SENSE
                r.in.assign(18, 0);
                r.in[0] = 0x70, r.in[7] = 10;
                r.in.resize((std::min)(r.in.size(), size_t(cdb[4])));
                return;
            }
            case 0x25: {  // READ CAPACITY (10)
                r.in.assign(8, 0);
                uint64_t last = blocks - 1;
                put32(r.in.data(), last > 0xffffffffull ? 0xffffffffu : uint32_t(last));
                put32(r.in.data() + 4, kBlock);
                return;
            }
            case 0x9E: {  // SERVICE ACTION IN (16)
                if ((cdb[1] & 0x1f) != 0x10) return r.check(0x05, 0x24, 0x00);
                r.in.assign(32, 0);  // READ CAPACITY (16)
                put64(r.in.data(), blocks - 1);
                put32(r.in.data() + 8, kBlock);
                r.in[13] = 3;     // 4 KiB physical blocks
                r.in[14] = 0xC0;  // thin provisioned (Windows trims), trimmed blocks read as zeros
                r.in.resize((std::min)(r.in.size(), size_t(be32(cdb + 10))));
                return;
            }
            case 0x1A:
            case 0x5A: return modeSense(cdb, r);
            case 0x08:
            case 0x28:
            case 0x88:
            case 0x0A:
            case 0x2A:
            case 0x8A: {
                uint64_t lba;
                uint32_t count;
                if (op == 0x08 || op == 0x0A) {
                    lba = uint64_t(cdb[1] & 0x1f) << 16 | uint64_t(cdb[2]) << 8 | cdb[3];
                    count = cdb[4] ? cdb[4] : 256;
                } else if (op == 0x28 || op == 0x2A) {
                    lba = be32(cdb + 2);
                    count = be16(cdb + 7);
                } else {
                    lba = be64(cdb + 2);
                    count = be32(cdb + 10);
                }
                if (lba > blocks || count > blocks - lba) return r.check(0x05, 0x21, 0x00);  // out of range
                uint64_t bytes = uint64_t(count) * kBlock;
                if (bytes > (64u << 20)) return r.check(0x05, 0x24, 0x00);
                bool isWrite = op == 0x0A || op == 0x2A || op == 0x8A;
                if (!isWrite) {
                    r.in.resize(size_t(bytes));
                    st.read(lba * kBlock, r.in.data(), size_t(bytes));
                } else {
                    size_t n = (std::min)(size_t(bytes), size_t(outLen));
                    if (n && !st.write(lba * kBlock, out, n)) return r.check(0x07, 0x27, 0x07);  // out of memory
                }
                return;
            }
            case 0x42: {  // UNMAP
                if (outLen < 8) return;
                size_t len = (std::min)(size_t(outLen), size_t(8) + be16(out + 2));
                for (size_t o = 8; o + 16 <= len; o += 16) {
                    uint64_t lba = be64(out + o);
                    uint32_t count = be32(out + o + 8);
                    if (lba > blocks || count > blocks - lba) return r.check(0x05, 0x21, 0x00);
                    st.unmap(lba * kBlock, uint64_t(count) * kBlock);
                }
                return;
            }
            default: return r.check(0x05, 0x20, 0x00);  // unsupported command
        }
    }

    void inquiry(bool here, const uint8_t* cdb, Result& r) {
        size_t alloc = be16(cdb + 3);
        std::vector<uint8_t>& d = r.in;
        if (cdb[1] & 1) {
            if (!here) return r.check(0x05, 0x25, 0x00);
            uint8_t page = cdb[2];
            if (page == 0x00) {
                d = {0, 0x00, 0, 6, 0x00, 0x80, 0x83, 0xB0, 0xB1, 0xB2};
            } else if (page == 0x80) {
                d = {0, 0x80, 0, uint8_t(d_.serial.size())};
                d.insert(d.end(), d_.serial.begin(), d_.serial.end());
            } else if (page == 0x83) {
                d = {0, 0x83, 0, 0};
                // NAA (locally assigned) ...
                d.insert(d.end(), {0x01, 0x03, 0, 8});
                d.insert(d.end(), d_.naa, d_.naa + 8);
                // ... and a T10 vendor ID.
                std::string t10 = std::string(MemDisk::vendor()) + d_.serial;
                d.insert(d.end(), {0x02, 0x01, 0, uint8_t(t10.size())});
                d.insert(d.end(), t10.begin(), t10.end());
                put16(d.data() + 2, uint16_t(d.size() - 4));
            } else if (page == 0xB0) {  // block limits
                d.assign(64, 0);
                d[1] = 0xB0, d[3] = 0x3C;
                put16(d.data() + 6, 8);               // optimal granularity: 4 KiB
                put32(d.data() + 8, (64u << 20) / kBlock);  // largest transfer
                put32(d.data() + 12, kOurMaxBurst / kBlock);
                put32(d.data() + 20, 0xffffffffu);    // largest UNMAP
                put32(d.data() + 24, 256);            // UNMAP ranges per command
                put32(d.data() + 28, uint32_t(kChunk / kBlock));  // trims free memory a megabyte at a time
                put32(d.data() + 32, 0x80000000u);    // ...aligned to the start
            } else if (page == 0xB1) {  // block device characteristics
                d.assign(64, 0);
                d[1] = 0xB1, d[3] = 0x3C;
                put16(d.data() + 4, 1);  // not rotating: an SSD to Windows
            } else if (page == 0xB2) {  // logical block provisioning
                d = {0, 0xB2, 0, 4, 0, 0x80 | 0x04, 0x02, 0};  // UNMAP, trimmed reads zeros, thin
            } else {
                return r.check(0x05, 0x24, 0x00);
            }
        } else {
            if (cdb[2]) return r.check(0x05, 0x24, 0x00);
            d.assign(36, 0);
            d[0] = here ? 0x00 : 0x7F;  // disk, or "no LUN here"
            d[2] = 0x06;                // SPC-4
            d[3] = 0x02;
            d[4] = 31;
            d[7] = 0x02;  // command queueing
            auto field = [&](size_t at, size_t n, const char* s) {
                std::memset(d.data() + at, ' ', n);
                std::memcpy(d.data() + at, s, (std::min)(n, std::strlen(s)));
            };
            field(8, 8, MemDisk::vendor());
            field(16, 16, MemDisk::product());
            field(32, 4, "1.0");
        }
        d.resize((std::min)(d.size(), alloc));
    }

    void modeSense(const uint8_t* cdb, Result& r) {
        bool ten = cdb[0] == 0x5A;
        int page = cdb[2] & 0x3f;
        size_t alloc = ten ? be16(cdb + 7) : cdb[4];
        std::vector<uint8_t> pages;
        if (page == 0x08 || page == 0x3f) {  // caching: no write cache (memory is already the medium)
            uint8_t c[20] = {0x08, 0x12};
            pages.insert(pages.end(), c, c + 20);
        }
        if (page == 0x0A || page == 0x3f) {  // control
            uint8_t c[12] = {0x0A, 0x0A};
            pages.insert(pages.end(), c, c + 12);
        }
        if (pages.empty() && page != 0x3f && page != 0x08 && page != 0x0A && page != 0x1C) return r.check(0x05, 0x24, 0x00);
        std::vector<uint8_t>& d = r.in;
        if (ten) {
            d.assign(8, 0);
            d.insert(d.end(), pages.begin(), pages.end());
            put16(d.data(), uint16_t(d.size() - 2));
        } else {
            d.assign(4, 0);
            d.insert(d.end(), pages.begin(), pages.end());
            d[0] = uint8_t(d.size() - 1);
        }
        d.resize((std::min)(d.size(), alloc));
    }

    bool respond(uint32_t itt, const uint8_t* lun, Result& r, uint32_t edtl, uint32_t r2ts) {
        size_t have = r.in.size(), n = (std::min)(have, size_t(edtl));
        uint8_t resid = 0;
        uint32_t residual = 0;
        if (have < edtl && !r.in.empty()) resid = 0x02, residual = uint32_t(edtl - have);  // underflow
        if (have > edtl) resid = 0x04, residual = uint32_t(have - edtl);                    // overflow
        if (r.status == 0 && n) {
            uint32_t dataSN = 0;
            for (size_t off = 0; off < n;) {
                size_t seg = (std::min)(size_t(theirMaxRecv_), n - off);
                if (maxBurst_) seg = (std::min)(seg, size_t(maxBurst_) - off % maxBurst_);
                bool last = off + seg == n, burstEnd = (off + seg) % maxBurst_ == 0;
                uint8_t h[48] = {};
                h[0] = 0x25;
                h[1] = uint8_t((last || burstEnd ? 0x80 : 0) | (last ? 0x01 | resid : 0));
                std::memcpy(h + 8, lun, 8);
                put32(h + 16, itt);
                put32(h + 20, kNoTag);
                if (last) put32(h + 24, statSN_++);
                seq(h);
                put32(h + 36, dataSN++);
                put32(h + 40, uint32_t(off));
                if (last) put32(h + 44, residual);
                if (!sendPdu(s_, h, r.in.data() + off, seg)) return false;
                off += seg;
            }
            return true;
        }
        uint8_t h[48] = {};
        h[0] = 0x21;
        h[1] = uint8_t(0x80 | (r.status == 0 ? resid : 0));
        h[3] = r.status;
        put32(h + 16, itt);
        put32(h + 24, statSN_++);
        seq(h);
        put32(h + 36, r2ts);
        if (r.status == 0) put32(h + 44, residual);
        if (r.status == 2) {
            uint8_t s[20];
            put16(s, 18);
            std::memcpy(s + 2, r.sense, 18);
            return sendPdu(s_, h, s, sizeof s);
        }
        return sendPdu(s_, h, nullptr, 0);
    }

    MemDisk::Impl& d_;
    SOCKET s_;
    bool discovery_ = false;
    uint16_t tsih_ = 0;
    uint32_t statSN_ = 1, expCmdSN_ = 0, nextTtt_ = 1;
    uint32_t theirMaxRecv_ = 8192, maxBurst_ = 262144, firstBurst_ = 65536;
    bool initialR2T_ = true;
    std::map<uint32_t, WriteTask> writes_;  // by initiator task tag
};

}  // namespace

void MemDisk::Impl::serve(SOCKET s) {
    Session(*this, s).run();
}

void MemDisk::Impl::acceptLoop() {
    while (!stopping) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (stopping) break;
            Sleep(20);
            continue;
        }
        BOOL one = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
        int buf = 4 << 20;
        setsockopt(c, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
        // Acknowledge every segment at once: with delayed ACKs the initiator's small PDUs wait up to
        // 200 ms each, which made writes crawl.
        DWORD freq = 1, ret = 0;
        WSAIoctl(c, SIO_TCP_SET_ACK_FREQUENCY, &freq, sizeof freq, nullptr, 0, &ret, nullptr, nullptr);
        std::lock_guard<std::mutex> lk(mu);
        if (stopping || live.size() >= 4) {
            closesocket(c);
            continue;
        }
        live.push_back(c);
        workers.emplace_back([this, c] {
            serve(c);
            std::lock_guard<std::mutex> lk2(mu);
            live.erase(std::remove(live.begin(), live.end(), c), live.end());
            closesocket(c);
        });
    }
}

MemDisk::MemDisk() = default;
MemDisk::~MemDisk() { stop(); }

const char* MemDisk::vendor() { return "KOMPRESS"; }
const char* MemDisk::product() { return "RAM Drive"; }

bool MemDisk::start(uint64_t size, std::string& err) {
    stop();
    if (!md5Works()) return err = "Internal error: MD5 self-test failed", false;
    auto d = std::make_unique<Impl>();
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return err = "Could not start networking", false;
    d->wsa = true;

    std::random_device rd;
    uint8_t id[8];
    for (auto& b : id) b = uint8_t(rd());
    d->serial = hex(id, 8);
    d->target = "iqn.2025-01.app.kompressor:ramdrive-" + d->serial;
    static const char* abc = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    for (int i = 0; i < 16; ++i) d->chapSecret += abc[rd() % std::strlen(abc)];
    for (auto& b : d->naa) b = uint8_t(rd());
    d->naa[0] = uint8_t(0x30 | (d->naa[0] & 0x0f));  // NAA 3: locally assigned

    size = (std::max)(size, uint64_t(64) << 20);
    size = (size + kChunk - 1) / kChunk * kChunk;
    d->store = std::make_unique<Store>(size);

    d->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (d->listener == INVALID_SOCKET) {
        WSACleanup();
        return err = "Could not open a socket for the drive", false;
    }
    BOOL excl = TRUE;
    setsockopt(d->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int alen = sizeof a;
    if (bind(d->listener, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(d->listener, 4) != 0 ||
        getsockname(d->listener, reinterpret_cast<sockaddr*>(&a), &alen) != 0) {
        closesocket(d->listener);
        WSACleanup();
        return err = "Could not listen for the drive on 127.0.0.1", false;
    }
    d->port = ntohs(a.sin_port);
    Impl* raw = d.get();
    d->acceptor = std::thread([raw] { raw->acceptLoop(); });
    impl_ = std::move(d);
    return true;
}

void MemDisk::stop() {
    if (!impl_) return;
    Impl& d = *impl_;
    d.stopping = true;
    closesocket(d.listener);
    if (d.acceptor.joinable()) d.acceptor.join();
    {
        std::lock_guard<std::mutex> lk(d.mu);
        for (SOCKET c : d.live) shutdown(c, SD_BOTH);
    }
    for (auto& t : d.workers)
        if (t.joinable()) t.join();
    d.store.reset();
    if (d.wsa) WSACleanup();
    impl_.reset();
}

uint16_t MemDisk::port() const { return impl_ ? impl_->port : 0; }
const std::string& MemDisk::targetName() const { return impl_->target; }
const std::string& MemDisk::chapName() const { return impl_->chapName; }
const std::string& MemDisk::chapSecret() const { return impl_->chapSecret; }
const std::string& MemDisk::serial() const { return impl_->serial; }
uint64_t MemDisk::size() const { return impl_ && impl_->store ? impl_->store->size() : 0; }
uint64_t MemDisk::resident() const { return impl_ && impl_->store ? impl_->store->resident() : 0; }

}  // namespace kz

#else  // not Windows: the RAM drive uses the system's own RAM disks there

namespace kz {
struct MemDisk::Impl {};
MemDisk::MemDisk() = default;
MemDisk::~MemDisk() = default;
bool MemDisk::start(uint64_t, std::string& err) { return err = "Only used on Windows", false; }
void MemDisk::stop() {}
uint16_t MemDisk::port() const { return 0; }
static const std::string kEmpty;
const std::string& MemDisk::targetName() const { return kEmpty; }
const std::string& MemDisk::chapName() const { return kEmpty; }
const std::string& MemDisk::chapSecret() const { return kEmpty; }
const std::string& MemDisk::serial() const { return kEmpty; }
uint64_t MemDisk::size() const { return 0; }
uint64_t MemDisk::resident() const { return 0; }
const char* MemDisk::vendor() { return "KOMPRESS"; }
const char* MemDisk::product() { return "RAM Drive"; }
}  // namespace kz

#endif
