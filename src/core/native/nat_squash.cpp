// SquashFS 4 (gzip, LZMA, LZO, XZ, LZ4, Zstandard) and CramFS.
#include <map>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

// ========================================================================================= SquashFS
struct Squash {
    StreamPtr s;
    uint32_t comp = 1, blockSize = 131072;
    uint64_t inodeTable = 0, dirTable = 0, fragTable = 0;
    uint32_t fragments = 0;
    std::map<uint64_t, std::pair<std::vector<uint8_t>, uint64_t>> meta;  // block position -> (data, next block)

    bool decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, size_t expect) {
        switch (comp) {
            case 1: return inflateZlib(in.data(), in.size(), out, expect);
            case 2:
            case 4: return unXz(in.data(), in.size(), out, expect);
            case 3: return unLzo1x(in.data(), in.size(), out, expect);
            case 5: return unLz4Block(in.data(), in.size(), out, expect);
            case 6: return unZstd(in.data(), in.size(), out, expect);
            default: return false;
        }
    }

    // A metadata block (8 KiB once decompressed) at an absolute position.
    const std::pair<std::vector<uint8_t>, uint64_t>* block(uint64_t at) {
        auto it = meta.find(at);
        if (it != meta.end()) return &it->second;
        std::vector<uint8_t> h = s->getUpTo(at, 2);
        if (h.size() < 2) return nullptr;
        uint32_t hdr = Span(h).u16(0), len = hdr & 0x7FFF;
        std::vector<uint8_t> raw = s->getUpTo(at + 2, len), out;
        if (raw.size() < len) return nullptr;
        if (hdr & 0x8000) out = raw;
        else if (!decompress(raw, out, 8192)) return nullptr;
        if (meta.size() > 4096) meta.clear();
        return &(meta[at] = {std::move(out), at + 2 + len});
    }

    // `n` bytes of metadata starting at (block, offset) within a table, crossing blocks as needed.
    std::vector<uint8_t> read(uint64_t table, uint64_t blk, uint32_t off, size_t n) {
        std::vector<uint8_t> v;
        uint64_t at = table + blk;
        while (v.size() < n) {
            auto* b = block(at);
            if (!b) break;
            if (off < b->first.size()) {
                size_t take = std::min(n - v.size(), b->first.size() - off);
                v.insert(v.end(), b->first.begin() + off, b->first.begin() + off + take);
                off = 0;
            } else {
                off -= (uint32_t)b->first.size();
            }
            at = b->second;
        }
        return v;
    }
};

struct SqFile {
    uint64_t start = 0, size = 0;
    uint32_t frag = 0xFFFFFFFF, fragOff = 0;
    std::vector<uint32_t> blocks;
};

bool sqFileRead(std::shared_ptr<Squash> q, const SqFile& f, const Sink& sink, std::string& err) {
    uint64_t at = f.start, left = f.size;
    std::vector<uint8_t> out;
    for (uint32_t e : f.blocks) {
        if (!left) break;
        uint32_t len = e & 0xFFFFFF;
        size_t want = (size_t)std::min<uint64_t>(q->blockSize, left);
        if (len == 0) {
            out.assign(want, 0);  // sparse
        } else {
            std::vector<uint8_t> raw = q->s->getUpTo(at, len);
            at += len;
            if (e & 0x1000000) out = raw;
            else if (!q->decompress(raw, out, q->blockSize)) {
                err = "Damaged compressed block";
                return false;
            }
        }
        size_t take = std::min(want, out.size());
        if (!sink(out.data(), take)) return true;
        left -= take;
    }
    if (left && f.frag != 0xFFFFFFFF) {  // the tail lives in a shared fragment block
        std::vector<uint8_t> ptr = q->s->getUpTo(q->fragTable + (f.frag / 512) * 8, 8);
        std::vector<uint8_t> ent = q->read(Span(ptr).u64(0), 0, (f.frag % 512) * 16, 16);
        Span E(ent);
        uint64_t fstart = E.u64(0);
        uint32_t fsize = E.u32(8);
        std::vector<uint8_t> raw = q->s->getUpTo(fstart, fsize & 0xFFFFFF);
        if (fsize & 0x1000000) out = raw;
        else if (!q->decompress(raw, out, q->blockSize)) {
            err = "Damaged fragment block";
            return false;
        }
        if (f.fragOff + left > out.size()) {
            err = "Damaged fragment block";
            return false;
        }
        sink(out.data() + f.fragOff, (size_t)left);
    }
    return true;
}

void sqDir(Ctx& ctx, std::shared_ptr<Squash> q, uint32_t startBlock, uint32_t offset, uint32_t size, const std::string& path, int depth,
           std::set<uint64_t>& seen) {
    if (size < 3 || depth > 64) return;
    std::vector<uint8_t> d = q->read(q->dirTable, startBlock, offset, size - 3);
    Span D(d);
    size_t o = 0;
    while (o + 12 <= d.size()) {
        uint32_t count = D.u32(o) + 1, iblock = D.u32(o + 4);
        o += 12;
        for (uint32_t i = 0; i < count && o + 8 <= d.size(); ++i) {
            uint32_t ioff = D.u16(o), nlen = D.u16(o + 6) + 1;
            if (!D.has(o + 8, nlen)) return;
            std::string name((const char*)d.data() + o + 8, nlen);
            o += 8 + nlen;
            // The inode.
            std::vector<uint8_t> in = q->read(q->inodeTable, iblock, ioff, 64);
            Span I(in);
            uint32_t type = I.u16(0);
            int64_t mt = I.u32(8);
            std::string full = path + component(name);
            if (type == 1 || type == 8) {
                uint32_t sb, off, sz;
                if (type == 1) sb = I.u32(16), sz = I.u16(24), off = I.u16(26);
                else sz = I.u32(20), sb = I.u32(24), off = I.u16(34);
                ctx.out.items.push_back(dirItem(ctx.prefix + full, mt));
                if (seen.insert((uint64_t)iblock << 16 | ioff).second) sqDir(ctx, q, sb, off, sz, full + "/", depth + 1, seen);
            } else if (type == 2 || type == 9) {
                SqFile f;
                size_t list;
                if (type == 2) f.start = I.u32(16), f.frag = I.u32(20), f.fragOff = I.u32(24), f.size = I.u32(28), list = 32;
                else f.start = I.u64(16), f.size = I.u64(24), f.frag = I.u32(44), f.fragOff = I.u32(48), list = 56;
                uint64_t nb = f.frag == 0xFFFFFFFF ? (f.size + q->blockSize - 1) / q->blockSize : f.size / q->blockSize;
                if (nb > (1u << 24)) continue;
                std::vector<uint8_t> bl = q->read(q->inodeTable, iblock, (uint32_t)(ioff + list), (size_t)nb * 4);
                for (size_t k = 0; k + 4 <= bl.size(); k += 4) f.blocks.push_back(Span(bl).u32(k));
                Item it;
                it.path = ctx.prefix + full;
                it.size = f.size;
                it.mtime = mt;
                it.read = [q, f](const Sink& sink, std::string& err) { return sqFileRead(q, f, sink, err); };
                ctx.out.items.push_back(std::move(it));
            } else if (type == 3 || type == 10) {
                uint32_t tl = std::min<uint32_t>(I.u32(20), 4096);
                std::vector<uint8_t> t = q->read(q->inodeTable, iblock, ioff + 24, tl);
                ctx.out.items.push_back(builtItem(ctx.prefix + full, t, mt));
            }
        }
    }
}

bool openSquash(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> sb = s->getUpTo(0, 96);
    Span S(sb);
    if (!S.eq(0, "hsqs", 4)) return false;
    if (S.u16(28) != 4) {
        ctx.err = "Only SquashFS version 4 is supported (this is version " + std::to_string(S.u16(28)) + ")";
        return false;
    }
    auto q = std::make_shared<Squash>();
    q->s = s;
    q->blockSize = S.u32(12);
    q->fragments = S.u32(16);
    q->comp = S.u16(20);
    q->inodeTable = S.u64(64);
    q->dirTable = S.u64(72);
    q->fragTable = S.u64(80);
    if (q->blockSize < 4096 || q->blockSize > (1u << 20) || q->comp < 1 || q->comp > 6) {
        ctx.err = "Damaged or unsupported SquashFS";
        return false;
    }
    uint64_t root = S.u64(32);
    std::vector<uint8_t> ri = q->read(q->inodeTable, root >> 16, (uint32_t)(root & 0xFFFF), 40);
    Span R(ri);
    uint32_t type = R.u16(0), sbk, off, sz;
    if (type == 1) sbk = R.u32(16), sz = R.u16(24), off = R.u16(26);
    else if (type == 8) sz = R.u32(20), sbk = R.u32(24), off = R.u16(34);
    else return false;
    static const char* const kComp[] = {"", "gzip", "LZMA", "LZO", "XZ", "LZ4", "Zstd"};
    ctx.formats.push_back(std::string("SquashFS (") + kComp[q->comp] + ")");
    std::set<uint64_t> seen{root};
    sqDir(ctx, q, sbk, off, sz, "", 0, seen);
    return true;
}

// =========================================================================================== CramFS
struct Cram {
    StreamPtr s;
    bool be = false;
    uint32_t u32(const Span& x, size_t o) const { return be ? x.b32(o) : x.u32(o); }
    uint32_t u16(const Span& x, size_t o) const { return be ? x.b16(o) : x.u16(o); }
};

bool cramRead(std::shared_ptr<Cram> c, uint64_t off, uint32_t size, const Sink& sink, std::string& err) {
    uint32_t nb = (size + 4095) / 4096;
    std::vector<uint8_t> ptrs = c->s->getUpTo(off, (size_t)nb * 4);
    uint64_t prev = off + (uint64_t)nb * 4;
    uint32_t left = size;
    std::vector<uint8_t> out;
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t p = c->u32(Span(ptrs), i * 4);
        bool raw = p & 0x80000000u;
        uint64_t end = p & 0x3FFFFFFF;
        size_t want = std::min<uint32_t>(4096, left);
        if (end <= prev) {
            out.assign(want, 0);  // a hole
        } else {
            std::vector<uint8_t> in = c->s->getUpTo(prev, (size_t)(end - prev));
            if (raw) out = in;
            else if (!inflateZlib(in.data(), in.size(), out, 4096)) {
                err = "Damaged compressed block";
                return false;
            }
            prev = end;
        }
        if (!sink(out.data(), std::min(want, out.size()))) return true;
        left -= (uint32_t)want;
    }
    return true;
}

void cramDir(Ctx& ctx, std::shared_ptr<Cram> c, uint64_t off, uint32_t size, const std::string& path, int depth, std::set<uint64_t>& seen) {
    if (depth > 64) return;
    std::vector<uint8_t> d = c->s->getUpTo(off, size);
    Span D(d);
    for (size_t o = 0; o + 12 <= d.size();) {
        uint32_t mode = c->u16(D, o), w2 = c->u32(D, o + 4), w3 = c->u32(D, o + 8);
        uint32_t fsize = c->be ? w2 >> 8 : w2 & 0xFFFFFF;
        uint32_t nlen = (c->be ? w3 >> 26 : w3 & 0x3F) * 4, doff = (c->be ? w3 & 0x3FFFFFF : w3 >> 6) * 4;
        if (!D.has(o + 12, nlen)) break;
        std::string name = cstr(d.data() + o + 12, nlen);
        o += 12 + nlen;
        std::string full = path + component(name);
        uint32_t kind = mode & 0xF000;
        if (kind == 0x4000) {
            ctx.out.items.push_back(dirItem(ctx.prefix + full, 0));
            if (doff && seen.insert(doff).second) cramDir(ctx, c, doff, fsize, full + "/", depth + 1, seen);
        } else if (kind == 0x8000 || kind == 0xA000) {
            Item it;
            it.path = ctx.prefix + full;
            it.size = fsize;
            uint64_t at = doff;
            it.read = [c, at, fsize](const Sink& sink, std::string& err) { return at ? cramRead(c, at, fsize, sink, err) : true; };
            ctx.out.items.push_back(std::move(it));
        }
    }
}

bool openCramfs(Ctx& ctx, StreamPtr s) {
    for (uint64_t base : {0ull, 512ull}) {
        std::vector<uint8_t> sb = s->getUpTo(base, 76);
        Span S(sb);
        auto c = std::make_shared<Cram>();
        if (S.u32(0) == 0x28CD3D45) c->be = false;
        else if (S.b32(0) == 0x28CD3D45) c->be = true;
        else continue;
        if (!S.eq(16, "Compressed ROMFS", 16)) continue;
        c->s = base ? rangeStream(s, 0, s->size()) : s;
        Span R(sb.data() + 64, 12);
        uint32_t w2 = c->u32(R, 4), w3 = c->u32(R, 8);
        uint32_t size = c->be ? w2 >> 8 : w2 & 0xFFFFFF, off = (c->be ? w3 & 0x3FFFFFF : w3 >> 6) * 4;
        ctx.formats.push_back("CramFS");
        std::set<uint64_t> seen{off};
        cramDir(ctx, c, off, size, "", 0, seen);
        return true;
    }
    return false;
}

}  // namespace

const Handler hSquash = {"SquashFS",
                         [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(0, "hsqs", 4); },
                         openSquash};
const Handler hCramfs = {"CramFS",
                         [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                             Span H(h);
                             return H.eq(16, "Compressed ROMFS", 16) || H.eq(512 + 16, "Compressed ROMFS", 16);
                         },
                         openCramfs};

}  // namespace kz::native
