// CHM (Microsoft compiled HTML help): an ITSF directory over an uncompressed and an LZX section.
#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

uint64_t encint(const uint8_t* p, size_t n, size_t& i) {
    uint64_t v = 0;
    while (i < n) {
        uint8_t b = p[i++];
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    return v;
}

struct ChmEntry {
    std::string name;
    uint64_t section, offset, length;
};

// The LZX section: frames of 32 KiB, a reset table giving each frame's compressed offset, and a decoder
// kept between reads so files read in order don't start over from the last reset point each time.
struct ChmLzx {
    StreamPtr content;  // the compressed stream
    std::vector<uint64_t> table;
    uint64_t rawLen = 0;
    uint32_t windowBits = 16, resetFrames = 2;
    std::unique_ptr<LzxDecoder> dec;
    uint64_t nextFrame = UINT64_MAX;  // the frame `dec` would decode next
    std::vector<uint8_t> frame;
    uint64_t frameIdx = UINT64_MAX;  // which frame `frame` holds

    bool get(uint64_t f, std::string& err) {
        if (f == frameIdx) return true;
        if (f >= table.size()) {
            err = "Damaged help file (past the end of the compressed data)";
            return false;
        }
        uint64_t start = f / resetFrames * resetFrames;
        if (!dec || nextFrame > f || nextFrame < start) {
            dec = std::make_unique<LzxDecoder>((int)windowBits);
            nextFrame = start;
        }
        while (nextFrame <= f) {
            if (nextFrame % resetFrames == 0) dec->reset();
            uint64_t a = table[nextFrame], b = nextFrame + 1 < table.size() ? table[nextFrame + 1] : content->size();
            if (b < a || b > content->size()) {
                err = "Damaged help file (bad reset table)";
                return false;
            }
            std::vector<uint8_t> in = content->get(a, (size_t)(b - a));
            in.resize(in.size() + 16, 0);  // the decoder may peek a little past the frame
            size_t len = (size_t)std::min<uint64_t>(32768, rawLen - nextFrame * 32768);
            frame.assign(len, 0);
            if (!dec->decode(in.data(), in.size(), frame.data(), len)) {
                dec.reset();
                frameIdx = UINT64_MAX;
                err = "Damaged compressed data";
                return false;
            }
            frameIdx = nextFrame++;
        }
        return true;
    }
};

bool openChm(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 0x60);
    Span H(h);
    if (!H.eq(0, "ITSF", 4)) return false;
    uint32_t version = H.u32(4);
    uint64_t dirOff = H.u64(0x48), dirLen = H.u64(0x50);
    uint64_t content = version >= 3 ? H.u64(0x58) : dirOff + dirLen;
    std::vector<uint8_t> dh = s->getUpTo(dirOff, 0x54);
    Span D(dh);
    if (!D.eq(0, "ITSP", 4)) {
        ctx.err = "Damaged help file (no directory)";
        return false;
    }
    uint32_t hlen = D.u32(8), chunk = D.u32(16), first = D.u32(32), chunks = D.u32(44);
    if (chunk < 32 || chunk > (1u << 20) || chunks > (1u << 20)) return false;
    std::vector<ChmEntry> ents;
    // The listing chunks (PMGL) are linked from the first one.
    int32_t c = (int32_t)first;
    for (uint32_t guard = 0; c >= 0 && (uint32_t)c < chunks && guard <= chunks; ++guard) {
        std::vector<uint8_t> ch = s->getUpTo(dirOff + hlen + (uint64_t)c * chunk, chunk);
        Span C(ch);
        if (!C.eq(0, "PMGL", 4)) break;
        size_t end = chunk - std::min<uint32_t>(C.u32(4), chunk), i = 20;
        while (i < end) {
            uint64_t nl = encint(ch.data(), end, i);
            if (nl > end - i) break;
            std::string name((const char*)ch.data() + i, (size_t)nl);
            i += (size_t)nl;
            uint64_t sec = encint(ch.data(), end, i), off = encint(ch.data(), end, i), len = encint(ch.data(), end, i);
            ents.push_back({name, sec, off, len});
        }
        c = (int32_t)C.u32(16);
    }
    auto find = [&](const char* name) -> const ChmEntry* {
        for (auto& e : ents)
            if (e.name == name) return &e;
        return nullptr;
    };
    // Section 1: MSCompressed (LZX).
    auto lzx = std::make_shared<ChmLzx>();
    const ChmEntry* ce = find("::DataSpace/Storage/MSCompressed/Content");
    const ChmEntry* cd = find("::DataSpace/Storage/MSCompressed/ControlData");
    const ChmEntry* rt = find("::DataSpace/Storage/MSCompressed/Transform/{7FC28940-9D31-11D0-9B27-00A0C91E9C7C}/InstanceData/ResetTable");
    bool haveLzx = false;
    if (ce && cd && rt && ce->section == 0 && cd->section == 0 && rt->section == 0) {
        lzx->content = rangeStream(s, content + ce->offset, ce->length);
        std::vector<uint8_t> ctl = s->getUpTo(content + cd->offset, (size_t)std::min<uint64_t>(cd->length, 64));
        Span T(ctl);
        if (T.eq(4, "LZXC", 4)) {
            uint32_t ver = T.u32(8);
            uint64_t reset = T.u32(12), win = T.u32(16);
            if (ver == 2) reset *= 32768, win *= 32768;
            uint32_t bits = 15;
            while (bits < 21 && (1ull << bits) < win) ++bits;
            lzx->windowBits = bits;
            lzx->resetFrames = (uint32_t)std::max<uint64_t>(1, reset / 32768);
            std::vector<uint8_t> rv = s->getUpTo(content + rt->offset, (size_t)std::min<uint64_t>(rt->length, 256u << 20));
            Span R(rv);
            uint32_t n = R.u32(4), es = R.u32(8), th = R.u32(12);
            lzx->rawLen = R.u64(16);
            if (es == 8 && R.has(th, (size_t)n * 8)) {
                for (uint32_t i = 0; i < n; ++i) lzx->table.push_back(R.u64(th + i * 8));
                haveLzx = true;
            }
        }
    }
    ctx.formats.push_back("CHM");
    for (auto& e : ents) {
        if (e.name.empty() || e.name.compare(0, 2, "::") == 0) continue;  // the container's own plumbing
        std::string p = e.name;
        if (p[0] == '/') p.erase(0, 1);
        if (p.empty()) continue;
        if (p.back() == '/') {
            p.pop_back();
            ctx.out.items.push_back(dirItem(ctx.prefix + p, 0));
            continue;
        }
        if (e.section == 0) {
            ctx.out.items.push_back(rangeItem(ctx.prefix + p, s, content + e.offset, e.length, 0));
            continue;
        }
        if (e.section != 1 || !haveLzx) continue;
        Item it;
        it.path = ctx.prefix + p;
        it.size = e.length;
        uint64_t off = e.offset, len = e.length;
        it.read = [lzx, off, len](const Sink& sink, std::string& err) {
            uint64_t at = off, left = len;
            while (left) {
                uint64_t f = at / 32768;
                if (!lzx->get(f, err)) return false;
                size_t within = (size_t)(at % 32768);
                if (within >= lzx->frame.size()) {
                    err = "Damaged help file";
                    return false;
                }
                size_t take = (size_t)std::min<uint64_t>(left, lzx->frame.size() - within);
                if (!sink(lzx->frame.data() + within, take)) return true;
                at += take;
                left -= take;
            }
            return true;
        };
        ctx.out.items.push_back(std::move(it));
    }
    return true;
}

}  // namespace

const Handler hChm = {"CHM", [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(0, "ITSF", 4); },
                      openChm};

}  // namespace kz::native
