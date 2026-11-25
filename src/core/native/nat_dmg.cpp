// Apple disk images (UDIF .dmg): a "koly" trailer pointing at an XML property list whose "blkx"
// entries ("mish" block maps) say where each run of disk sectors is kept and how it's compressed
// (zlib, bzip2, ADC, LZFSE, LZMA, raw, or zeros). The runs together make up the disk, which is then
// explored like any other (partition map, HFS+/APFS...).
#include <algorithm>

#include "nat.hpp"

namespace kz::native {

namespace {

struct Run {
    uint64_t sector, count;  // in 512-byte sectors on the disk
    uint32_t type;
    uint64_t off, len;  // in the image file
};

class DmgDisk : public Stream {
public:
    DmgDisk(StreamPtr src, std::vector<Run> runs, uint64_t sectors) : src_(std::move(src)), runs_(std::move(runs)), size_(sectors * 512) {
        std::sort(runs_.begin(), runs_.end(), [](const Run& a, const Run& b) { return a.sector < b.sector; });
    }
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off > size_ || n > size_ - off) return false;
        auto* out = static_cast<uint8_t*>(dst);
        while (n) {
            uint64_t sec = off / 512;
            auto it = std::upper_bound(runs_.begin(), runs_.end(), sec, [](uint64_t s, const Run& r) { return s < r.sector; });
            const Run* r = nullptr;
            if (it != runs_.begin() && sec < std::prev(it)->sector + std::prev(it)->count) r = &*std::prev(it);
            uint64_t runStart = r ? r->sector * 512 : off, runEnd = r ? (r->sector + r->count) * 512 : (it == runs_.end() ? size_ : it->sector * 512);
            size_t take = (size_t)std::min<uint64_t>(n, runEnd - off);
            if (!take) return false;
            if (!r || r->type == 0 || r->type == 2) {
                std::memset(out, 0, take);
            } else if (r->type == 1) {
                if (!src_->readAt(r->off + (off - runStart), out, take)) return false;
            } else {
                const std::vector<uint8_t>* d = unpack(*r);
                if (!d) return false;
                size_t within = (size_t)(off - runStart);
                size_t have = within < d->size() ? std::min(take, d->size() - within) : 0;
                if (have) std::memcpy(out, d->data() + within, have);
                if (have < take) std::memset(out + have, 0, take - have);
            }
            out += take;
            off += take;
            n -= take;
        }
        return true;
    }

private:
    const std::vector<uint8_t>* unpack(const Run& r) {
        for (auto& c : cache_)
            if (c.first == r.off && !c.second.empty()) return &c.second;
        if (r.len > (256u << 20) || r.count > (1u << 20)) return nullptr;
        std::vector<uint8_t> in = src_->get(r.off, (size_t)r.len), out;
        if (in.size() != r.len) return nullptr;
        size_t want = (size_t)(r.count * 512);
        bool ok = false;
        switch (r.type) {
            case 0x80000004: ok = unAdc(in.data(), in.size(), out, want); break;
            case 0x80000005: ok = inflateZlib(in.data(), in.size(), out, want); break;
            case 0x80000006: ok = unBzip2(in.data(), in.size(), out, want); break;
            case 0x80000007: ok = unLzfse(in.data(), in.size(), out, want); break;
            case 0x80000008: ok = unXz(in.data(), in.size(), out, want); break;
            default: return nullptr;
        }
        if (!ok && out.empty()) return nullptr;
        auto& slot = cache_[next_++ % 4];
        slot = {r.off, std::move(out)};
        return &slot.second;
    }

    StreamPtr src_;
    std::vector<Run> runs_;
    uint64_t size_;
    std::pair<uint64_t, std::vector<uint8_t>> cache_[4];
    size_t next_ = 0;
};

std::vector<uint8_t> unbase64(const char* p, size_t n) {
    std::vector<uint8_t> out;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; ++i) {
        char c = p[i];
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) bits -= 8, out.push_back((uint8_t)(acc >> bits));
    }
    return out;
}

bool openDmg(Ctx& ctx, StreamPtr s) {
    if (s->size() < 1024) return false;
    std::vector<uint8_t> kv = s->get(s->size() - 512, 512);
    Span K(kv);
    if (!K.eq(0, "koly", 4)) return false;
    uint64_t dataOff = K.b64(24), xmlOff = K.b64(216), xmlLen = K.b64(224), sectors = K.b64(492);
    if (K.b32(60) > 1) ctx.out.warning = "This image is split into segments; only the first is read";
    if (!xmlLen || xmlOff + xmlLen > s->size() || xmlLen > (64u << 20)) {
        ctx.err = "This disk image has no block map Kompressor can read (an old resource-fork image)";
        return false;
    }
    std::vector<uint8_t> xv = s->get(xmlOff, (size_t)xmlLen);
    std::string xml(xv.begin(), xv.end());
    size_t at = xml.find("<key>blkx</key>");
    if (at == std::string::npos) {
        ctx.err = "Damaged disk image (no block map)";
        return false;
    }
    size_t arrEnd = xml.find("</array>", at);
    std::vector<Run> runs;
    uint64_t maxSector = 0;
    for (size_t p = xml.find("<data>", at); p != std::string::npos && p < arrEnd; p = xml.find("<data>", p)) {
        size_t e = xml.find("</data>", p);
        if (e == std::string::npos) break;
        std::vector<uint8_t> m = unbase64(xml.data() + p + 6, e - p - 6);
        p = e;
        Span M(m);
        if (!M.eq(0, "mish", 4)) continue;
        uint64_t first = M.b64(8), base = M.b64(24);
        uint32_t count = M.b32(200);
        for (uint32_t i = 0; i < count && M.has(204 + (size_t)i * 40, 40); ++i) {
            size_t o = 204 + (size_t)i * 40;
            Run r{first + M.b64(o + 8), M.b64(o + 16), M.b32(o), dataOff + base + M.b64(o + 24), M.b64(o + 32)};
            if (r.type == 0xFFFFFFFF) break;
            if (r.type == 0x7FFFFFFE || !r.count) continue;  // a comment
            maxSector = std::max(maxSector, r.sector + r.count);
            runs.push_back(r);
        }
    }
    if (runs.empty()) {
        ctx.err = "Damaged disk image (empty block map)";
        return false;
    }
    if (!sectors) sectors = maxSector;
    auto disk = std::make_shared<DmgDisk>(s, std::move(runs), std::max(sectors, maxSector));
    ctx.formats.push_back("DMG");
    Ctx sub{ctx.out, ctx.prefix, "", ctx.depth + 1, {}, {}};
    if (explore(sub, disk)) {
        ctx.formats.insert(ctx.formats.end(), sub.formats.begin(), sub.formats.end());
        return true;
    }
    // Nothing recognised inside: offer the disk itself.
    ctx.out.items.push_back(streamItem(ctx.prefix + "disk.img", disk, 0));
    if (!sub.err.empty()) ctx.out.warning = sub.err;
    return true;
}

}  // namespace

const Handler hDmg = {"DMG",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          if (Span(h).eq(0, "encrcdsa", 8) || Span(h).eq(0, "cdsaencr", 8)) return false;
                          if (s.size() < 1024) return false;
                          std::vector<uint8_t> k = s.get(s.size() - 512, 8);
                          return Span(k).eq(0, "koly", 4);
                      },
                      openDmg};

}  // namespace kz::native
