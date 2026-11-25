// ext2, ext3 and ext4 (extents, 64-bit, flexible groups, inline data, fast symlinks).
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

struct Ext {
    StreamPtr s;
    uint64_t block = 1024;
    uint32_t inodesPerGroup = 0, inodeSize = 128, descSize = 32, groups = 0;
    uint64_t gdtAt = 0;
    bool hasFiletype = true;
    std::vector<uint8_t> gdt;

    std::vector<uint8_t> inode(uint32_t n) {
        if (!n || !inodesPerGroup) return {};
        uint32_t g = (n - 1) / inodesPerGroup, idx = (n - 1) % inodesPerGroup;
        Span G(gdt);
        size_t o = (size_t)g * descSize;
        if (!G.has(o, 32)) return {};
        uint64_t table = G.u32(o + 8) | (descSize >= 64 ? (uint64_t)G.u32(o + 0x28) << 32 : 0);
        return s->getUpTo(table * block + (uint64_t)idx * inodeSize, inodeSize);
    }

    // Block runs of an inode's data: (byte offset on disk or kHole, length), covering `size` bytes.
    bool runs(const std::vector<uint8_t>& ino, uint64_t size, std::vector<std::pair<uint64_t, uint64_t>>& out) {
        Span I(ino);
        uint32_t flags = I.u32(32);
        uint64_t nblocks = (size + block - 1) / block;
        std::vector<std::pair<uint64_t, uint64_t>> map;  // (logical block, physical block or kHole) per extent, lengths below
        struct Piece {
            uint64_t logical, physical, len;
        };
        std::vector<Piece> pieces;
        if (flags & 0x80000) {  // extents
            std::function<bool(const uint8_t*, size_t, int)> walk = [&](const uint8_t* p, size_t n, int depth) -> bool {
                Span E(p, n);
                if (E.u16(0) != 0xF30A || depth > 8) return false;
                uint32_t entries = E.u16(2), d = E.u16(6);
                for (uint32_t i = 0; i < entries; ++i) {
                    size_t o = 12 + i * 12;
                    if (!E.has(o, 12)) break;
                    if (d == 0) {
                        uint32_t len = E.u16(o + 4);
                        bool uninit = len > 32768;
                        if (uninit) len -= 32768;
                        uint64_t phys = (uint64_t)E.u16(o + 6) << 32 | E.u32(o + 8);
                        pieces.push_back({E.u32(o), uninit ? kHole : phys, len});
                    } else {
                        uint64_t leaf = (uint64_t)E.u16(o + 8) << 32 | E.u32(o + 4);
                        std::vector<uint8_t> b = s->getUpTo(leaf * block, (size_t)block);
                        if (!walk(b.data(), b.size(), depth + 1)) return false;
                    }
                }
                return true;
            };
            if (!walk(ino.data() + 40, 60, 0)) return false;
        } else {  // classic block map: 12 direct, then single, double and triple indirect blocks
            uint64_t logical = 0;
            uint32_t per = (uint32_t)(block / 4);
            std::function<void(uint32_t, int)> ind = [&](uint32_t b, int level) {
                if (logical >= nblocks) return;
                if (!b) {
                    uint64_t span = 1;
                    for (int k = 0; k < level; ++k) span *= per;
                    logical += span;
                    return;
                }
                if (level == 0) {
                    pieces.push_back({logical++, b, 1});
                    return;
                }
                std::vector<uint8_t> t = s->getUpTo((uint64_t)b * block, (size_t)block);
                for (uint32_t k = 0; k < per && logical < nblocks; ++k) ind(Span(t).u32(k * 4), level - 1);
            };
            for (int k = 0; k < 12 && logical < nblocks; ++k) ind(I.u32(40 + k * 4), 0);
            for (int lvl = 1; lvl <= 3 && logical < nblocks; ++lvl) ind(I.u32(40 + (11 + lvl) * 4), lvl);
        }
        std::sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.logical < b.logical; });
        uint64_t at = 0;  // in blocks
        for (auto& p : pieces) {
            if (p.logical >= nblocks) break;
            if (p.logical > at) addRun(out, kHole, (p.logical - at) * block);
            if (p.logical < at) continue;
            uint64_t len = std::min(p.len, nblocks - p.logical);
            addRun(out, p.physical == kHole ? kHole : p.physical * block, len * block);
            at = p.logical + len;
        }
        if (at < nblocks) addRun(out, kHole, (nblocks - at) * block);
        // Trim to the file's size.
        uint64_t total = 0;
        for (auto& r : out) total += r.second;
        if (total > size && !out.empty()) out.back().second -= total - size;
        (void)map;
        return true;
    }

    std::vector<uint8_t> data(const std::vector<uint8_t>& ino, uint64_t size) {
        Span I(ino);
        if (I.u32(32) & 0x10000000) return std::vector<uint8_t>(ino.begin() + 40, ino.begin() + 40 + std::min<uint64_t>(60, size));
        std::vector<std::pair<uint64_t, uint64_t>> r;
        if (!runs(ino, size, r)) return {};
        std::vector<uint8_t> v;
        for (auto& [off, len] : r) {
            if (off == kHole) v.insert(v.end(), (size_t)len, 0);
            else {
                std::vector<uint8_t> d = s->getUpTo(off, (size_t)len);
                v.insert(v.end(), d.begin(), d.end());
            }
        }
        return v;
    }
};

uint64_t inodeSize(const Span& I) { return I.u32(4) | (uint64_t)I.u32(108) << 32; }

void walkDir(Ctx& ctx, Ext& e, uint32_t dirIno, const std::string& path, int depth, std::set<uint32_t>& seen) {
    std::vector<uint8_t> ino = e.inode(dirIno);
    if (ino.empty() || depth > 64) return;
    std::vector<uint8_t> d = e.data(ino, std::min<uint64_t>(inodeSize(Span(ino)), 256u << 20));
    Span D(d);
    for (size_t o = 0; o + 8 <= d.size();) {
        uint32_t child = D.u32(o), rec = D.u16(o + 4);
        uint32_t nameLen = e.hasFiletype ? D.u8(o + 6) : D.u16(o + 6);
        if (rec < 8 || !D.has(o, rec)) break;
        if (child && nameLen && D.has(o + 8, nameLen)) {
            std::string name((const char*)d.data() + o + 8, nameLen);
            if (name != "." && name != "..") {
                std::vector<uint8_t> ci = e.inode(child);
                Span C(ci);
                uint32_t mode = C.u16(0);
                uint64_t size = inodeSize(C);
                int64_t mt = (int32_t)C.u32(16);
                std::string full = path + component(name);
                uint32_t kind = mode & 0xF000;
                if (kind == 0x4000) {
                    ctx.out.items.push_back(dirItem(ctx.prefix + full, mt));
                    if (seen.insert(child).second) walkDir(ctx, e, child, full + "/", depth + 1, seen);
                } else if (kind == 0x8000) {
                    if (C.u32(32) & 0x10000000) {  // inline data
                        ctx.out.items.push_back(builtItem(ctx.prefix + full, e.data(ci, size), mt));
                    } else {
                        std::vector<std::pair<uint64_t, uint64_t>> r;
                        e.runs(ci, size, r);
                        ctx.out.items.push_back(runsItem(ctx.prefix + full, e.s, std::move(r), size, mt));
                    }
                } else if (kind == 0xA000) {  // symlink: listed as a small file holding the target
                    std::vector<uint8_t> target = size < 60 && !(C.u32(32) & 0x80000) ? std::vector<uint8_t>(ci.begin() + 40, ci.begin() + 40 + size)
                                                                                      : e.data(ci, std::min<uint64_t>(size, 4096));
                    Item it = builtItem(ctx.prefix + full, target, mt);
                    ctx.out.items.push_back(std::move(it));
                }
            }
        }
        o += rec;
    }
}

bool openExt(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> sb = s->getUpTo(1024, 1024);
    Span S(sb);
    if (S.u16(56) != 0xEF53) return false;
    Ext e;
    e.s = s;
    uint32_t logBlock = S.u32(24);
    if (logBlock > 6) return false;
    e.block = 1024ull << logBlock;
    e.inodesPerGroup = S.u32(40);
    uint32_t blocksPerGroup = S.u32(32), rev = S.u32(76), incompat = S.u32(96);
    e.inodeSize = rev >= 1 ? S.u16(88) : 128;
    e.hasFiletype = incompat & 0x2;
    e.descSize = (incompat & 0x80) && S.u16(0xFE) >= 32 ? S.u16(0xFE) : 32;
    uint64_t blocks = S.u32(4) | ((incompat & 0x80) ? (uint64_t)S.u32(0x150) << 32 : 0);
    uint32_t firstData = S.u32(20);
    if (!blocksPerGroup || !e.inodesPerGroup || e.inodeSize < 128) return false;
    e.groups = (uint32_t)((blocks - firstData + blocksPerGroup - 1) / blocksPerGroup);
    e.gdtAt = (uint64_t)(firstData + 1) * e.block;
    e.gdt = s->getUpTo(e.gdtAt, (size_t)e.groups * e.descSize);
    if (incompat & 0x10000) {  // encrypted
        ctx.err = "This ext4 volume uses encryption; encrypted folders can't be read";
    }
    std::string ver = (incompat & 0x40) ? "ext4" : (S.u32(92) & 0x4) ? "ext3" : "ext2";
    ctx.formats.push_back(ver);
    std::set<uint32_t> seen{2};
    walkDir(ctx, e, 2, "", 0, seen);
    return true;
}

}  // namespace

const Handler hExt = {"Ext",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          return h.size() >= 2048 ? Span(h).u16(1024 + 56) == 0xEF53 : Span(s.getUpTo(1024 + 56, 2)).u16(0) == 0xEF53;
                      },
                      openExt};

}  // namespace kz::native
