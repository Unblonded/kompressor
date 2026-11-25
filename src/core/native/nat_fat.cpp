// FAT12/16/32 (with long names) and exFAT.
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

// ============================================================================================== FAT
struct Fat {
    StreamPtr s;
    uint32_t bps = 512, spc = 1, type = 16;
    uint64_t fatOff = 0, rootOff = 0, dataOff = 0;
    uint32_t rootEntries = 0, rootCluster = 0, clusters = 0;
    std::vector<uint8_t> fat;
    uint64_t clusterBytes() const { return (uint64_t)bps * spc; }
    uint64_t clusterAt(uint32_t c) const { return dataOff + (uint64_t)(c - 2) * clusterBytes(); }
    uint32_t next(uint32_t c) const {
        Span F(fat);
        if (type == 12) {
            uint32_t v = F.u16(c + c / 2);
            return c & 1 ? v >> 4 : v & 0xFFF;
        }
        if (type == 16) return F.u16(c * 2);
        return F.u32(c * 4) & 0x0FFFFFFF;
    }
    bool isEnd(uint32_t v) const {
        return v < 2 || (type == 12 ? v >= 0xFF7 : type == 16 ? v >= 0xFFF7 : v >= 0x0FFFFFF7) || v >= clusters + 2;
    }
    // Runs of a cluster chain (at most `limit` bytes).
    std::vector<std::pair<uint64_t, uint64_t>> chain(uint32_t c, uint64_t limit) const {
        std::vector<std::pair<uint64_t, uint64_t>> runs;
        uint64_t got = 0;
        std::set<uint32_t> seen;
        while (!isEnd(c) && got < limit && seen.insert(c).second) {
            uint64_t take = std::min(clusterBytes(), limit - got);
            addRun(runs, clusterAt(c), take);
            got += take;
            c = next(c);
        }
        return runs;
    }
};

std::vector<uint8_t> readRuns(Stream& s, const std::vector<std::pair<uint64_t, uint64_t>>& runs) {
    std::vector<uint8_t> v;
    for (auto& [off, len] : runs) {
        std::vector<uint8_t> d = s.getUpTo(off, (size_t)len);
        v.insert(v.end(), d.begin(), d.end());
        if (d.size() < len) break;
    }
    return v;
}

void fatDir(Ctx& ctx, Fat& f, const std::vector<uint8_t>& d, const std::string& path, int depth, std::set<uint32_t>& seenDirs) {
    Span D(d);
    std::vector<uint16_t> lfn;  // UTF-16 pieces of the long name being collected (in order)
    std::string lfnName;
    int lfnSeq = 0;
    for (size_t o = 0; o + 32 <= d.size(); o += 32) {
        uint32_t first = d[o], attr = d[o + 11];
        if (first == 0) break;
        if (first == 0xE5) {  // deleted
            lfnName.clear();
            continue;
        }
        if (attr == 0x0F) {  // a piece of a long name; pieces come last-first
            int seq = first & 0x1F;
            uint16_t part[13];
            const size_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (int k = 0; k < 13; ++k) part[k] = (uint16_t)D.u16(o + at[k]);
            std::vector<uint16_t> chunk(part, part + 13);
            if (first & 0x40) lfn.clear(), lfnSeq = seq;
            lfn.insert(lfn.begin(), chunk.begin(), chunk.end());
            if (seq == 1) {
                std::vector<uint8_t> bytes;
                for (uint16_t u : lfn) {
                    if (u == 0 || u == 0xFFFF) break;
                    bytes.push_back(uint8_t(u)), bytes.push_back(uint8_t(u >> 8));
                }
                lfnName = utf16le(bytes.data(), bytes.size() / 2);
                (void)lfnSeq;
            }
            continue;
        }
        if (attr & 0x08) {  // volume label
            lfnName.clear();
            continue;
        }
        std::string name = lfnName;
        lfnName.clear();
        if (name.empty()) {
            // 8.3 name; the NT case flags make base and extension lower case.
            uint32_t nt = d[o + 12];
            std::string base = latin1(d.data() + o, 8), ext = latin1(d.data() + o + 8, 3);
            while (!base.empty() && base.back() == ' ') base.pop_back();
            while (!ext.empty() && ext.back() == ' ') ext.pop_back();
            if (first == 0x05 && !base.empty()) base[0] = (char)0xE5;
            if (nt & 0x08) for (auto& c : base) c = (char)std::tolower((unsigned char)c);
            if (nt & 0x10) for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
            name = ext.empty() ? base : base + "." + ext;
        }
        if (name == "." || name == "..") continue;
        uint32_t cl = D.u16(o + 26) | (f.type == 32 ? D.u16(o + 20) << 16 : 0);
        int64_t mt = fromDos(D.u16(o + 24), D.u16(o + 22));
        std::string full = path + component(name);
        if (attr & 0x10) {
            ctx.out.items.push_back(dirItem(ctx.prefix + full, mt));
            if (depth < 64 && cl >= 2 && seenDirs.insert(cl).second)
                fatDir(ctx, f, readRuns(*f.s, f.chain(cl, 64u << 20)), full + "/", depth + 1, seenDirs);
            continue;
        }
        uint32_t size = D.u32(o + 28);
        ctx.out.items.push_back(runsItem(ctx.prefix + full, f.s, size ? f.chain(cl, size) : std::vector<std::pair<uint64_t, uint64_t>>{},
                                         size, mt));
    }
}

bool fatProbe(const std::vector<uint8_t>& h) {
    Span H(h);
    if (h.size() < 512 || (h[0] != 0xEB && h[0] != 0xE9)) return false;
    if (H.eq(3, "NTFS    ", 8) || H.eq(3, "EXFAT   ", 8)) return false;
    uint32_t bps = H.u16(11), spc = H.u8(13), res = H.u16(14), nf = H.u8(16);
    return (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) && spc && !(spc & (spc - 1)) && res && (nf == 1 || nf == 2) &&
           (H.u16(19) || H.u32(32)) && (H.u16(22) || H.u32(36));
}

bool openFat(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    Span H(h);
    Fat f;
    f.s = s;
    f.bps = H.u16(11);
    f.spc = H.u8(13);
    uint32_t res = H.u16(14), nf = H.u8(16);
    f.rootEntries = H.u16(17);
    uint64_t total = H.u16(19) ? H.u16(19) : H.u32(32), fatSz = H.u16(22) ? H.u16(22) : H.u32(36);
    uint64_t rootSecs = ((uint64_t)f.rootEntries * 32 + f.bps - 1) / f.bps;
    uint64_t firstData = res + nf * fatSz + rootSecs;
    if (total <= firstData) return false;
    f.clusters = (uint32_t)((total - firstData) / f.spc);
    f.type = f.clusters < 4085 ? 12 : f.clusters < 65525 ? 16 : 32;
    f.fatOff = (uint64_t)res * f.bps;
    f.rootOff = (res + nf * fatSz) * f.bps;
    f.dataOff = firstData * f.bps;
    f.rootCluster = H.u32(44);
    f.fat = s->getUpTo(f.fatOff, (size_t)std::min<uint64_t>(fatSz * f.bps, 512ull << 20));
    if (f.fat.empty()) return false;
    std::vector<uint8_t> root = f.type == 32 ? readRuns(*s, f.chain(f.rootCluster, 64u << 20))
                                             : s->getUpTo(f.rootOff, (size_t)f.rootEntries * 32);
    ctx.formats.push_back("FAT" + std::to_string(f.type));
    std::set<uint32_t> seen;
    fatDir(ctx, f, root, "", 0, seen);
    return true;
}

// ============================================================================================ exFAT
struct ExFat {
    StreamPtr s;
    uint64_t heap = 0, clusterBytes = 0;
    uint32_t clusters = 0;
    std::vector<uint8_t> fat;
    std::vector<std::pair<uint64_t, uint64_t>> chain(uint32_t c, uint64_t len, bool contiguous) const {
        std::vector<std::pair<uint64_t, uint64_t>> runs;
        if (c < 2 || !len) return runs;
        if (contiguous) {
            addRun(runs, heap + (uint64_t)(c - 2) * clusterBytes, len);
            return runs;
        }
        uint64_t got = 0;
        std::set<uint32_t> seen;
        while (c >= 2 && c < clusters + 2 && got < len && seen.insert(c).second) {
            uint64_t take = std::min(clusterBytes, len - got);
            addRun(runs, heap + (uint64_t)(c - 2) * clusterBytes, take);
            got += take;
            c = Span(fat).u32((size_t)c * 4);
        }
        return runs;
    }
};

void exfatDir(Ctx& ctx, ExFat& f, const std::vector<uint8_t>& d, const std::string& path, int depth, std::set<uint32_t>& seenDirs) {
    Span D(d);
    for (size_t o = 0; o + 32 <= d.size(); o += 32) {
        uint32_t type = d[o];
        if (type == 0) break;
        if (type != 0x85) continue;
        uint32_t count = d[o + 1], attr = D.u16(o + 4);
        uint32_t ts = D.u32(o + 12);
        size_t se = o + 32;
        if (count < 2 || !D.has(se, 32) || d[se] != 0xC0) continue;
        uint32_t flags = d[se + 1], nameLen = d[se + 3], first = D.u32(se + 20);
        uint64_t valid = D.u64(se + 8), size = D.u64(se + 24);
        std::vector<uint8_t> name;
        for (uint32_t k = 2; k <= count && name.size() < nameLen * 2; ++k) {
            size_t ne = o + k * 32;
            if (!D.has(ne, 32) || d[ne] != 0xC1) break;
            name.insert(name.end(), d.begin() + ne + 2, d.begin() + ne + 32);
        }
        name.resize(std::min<size_t>(name.size(), nameLen * 2));
        o += count * 32;
        std::string full = path + component(utf16le(name.data(), name.size() / 2));
        int64_t mt = fromDos(ts >> 16, ts & 0xFFFF);
        bool contiguous = flags & 2;
        if (attr & 0x10) {
            ctx.out.items.push_back(dirItem(ctx.prefix + full, mt));
            if (depth < 64 && seenDirs.insert(first).second)
                exfatDir(ctx, f, readRuns(*f.s, f.chain(first, std::min<uint64_t>(size, 256u << 20), contiguous)), full + "/",
                         depth + 1, seenDirs);
            continue;
        }
        ctx.out.items.push_back(runsItem(ctx.prefix + full, f.s, f.chain(first, std::min(valid, size), contiguous), size, mt));
    }
}

bool openExfat(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    Span H(h);
    uint32_t bpsShift = H.u8(108), spcShift = H.u8(109);
    if (bpsShift < 9 || bpsShift > 12 || spcShift > 25 - bpsShift) return false;
    uint64_t bps = 1ull << bpsShift;
    ExFat f;
    f.s = s;
    f.clusterBytes = bps << spcShift;
    f.heap = (uint64_t)H.u32(88) * bps;
    f.clusters = H.u32(92);
    f.fat = s->getUpTo((uint64_t)H.u32(80) * bps, (size_t)std::min<uint64_t>((uint64_t)H.u32(84) * bps, 1ull << 30));
    uint32_t root = H.u32(96);
    // The root directory's size isn't recorded: read its chain (FAT-linked).
    std::vector<uint8_t> rd = readRuns(*s, f.chain(root, 64u << 20, false));
    ctx.formats.push_back("exFAT");
    std::set<uint32_t> seen{root};
    exfatDir(ctx, f, rd, "", 0, seen);
    return true;
}

}  // namespace

const Handler hFat = {"FAT", [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return fatProbe(h); }, openFat};
const Handler hExfat = {"exFAT",
                        [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(3, "EXFAT   ", 8); },
                        openExfat};

}  // namespace kz::native
