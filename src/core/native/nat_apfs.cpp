// APFS (Apple File System) containers: the newest checkpoint's superblock -> container object map ->
// each volume's superblock -> its object map and file-system B-tree, whose records (inodes, directory
// entries, file extents, extended attributes) make up the tree. Encrypted volumes can't be read.
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "nat.hpp"

namespace kz::native {

namespace {

struct Apfs {
    StreamPtr s;
    uint32_t bs = 4096;
    uint64_t xid = UINT64_MAX;

    std::vector<uint8_t> block(uint64_t paddr) {
        if (paddr == 0 || paddr > s->size() / bs) return {};
        return s->get(paddr * bs, bs);
    }

    // Visits every leaf entry of the B-tree rooted at `root` (virtual oids go through `omap`).
    using Visit = std::function<void(const uint8_t* k, size_t kl, const uint8_t* v, size_t vl)>;
    bool walk(uint64_t root, const std::unordered_map<uint64_t, uint64_t>* omap, const Visit& visit) {
        uint32_t fk = 0, fv = 0;
        std::set<uint64_t> seen;
        return node(root, omap, visit, fk, fv, true, 0, seen);
    }

    bool node(uint64_t oid, const std::unordered_map<uint64_t, uint64_t>* omap, const Visit& visit, uint32_t& fk, uint32_t& fv,
              bool isRoot, int depth, std::set<uint64_t>& seen) {
        if (depth > 16 || !seen.insert(oid).second) return false;
        uint64_t paddr = oid;
        if (omap) {
            auto it = omap->find(oid);
            if (it == omap->end()) return false;
            paddr = it->second;
        }
        std::vector<uint8_t> b = block(paddr);
        if (b.size() != bs) return false;
        Span B(b);
        uint32_t type = B.u32(24) & 0xFFFF;
        if (type != 2 && type != 3) return false;  // btree root / node
        uint32_t flags = B.u16(32), level = B.u16(34), nkeys = B.u32(36);
        uint32_t tsOff = B.u16(40), tsLen = B.u16(42);
        bool root = flags & 1, fixed = flags & 4;
        if (root && isRoot) {  // tree info at the end of the root: fixed key/value sizes
            fk = B.u32(bs - 40 + 8);
            fv = B.u32(bs - 40 + 12);
        }
        size_t toc = 56 + tsOff, keys = 56 + (size_t)tsOff + tsLen, vend = bs - (root ? 40 : 0);
        for (uint32_t i = 0; i < nkeys; ++i) {
            uint32_t ko, kl, vo, vl;
            if (fixed) {
                ko = B.u16(toc + i * 4), vo = B.u16(toc + i * 4 + 2);
                kl = fk, vl = level ? 8 : fv;
            } else {
                ko = B.u16(toc + i * 8), kl = B.u16(toc + i * 8 + 2), vo = B.u16(toc + i * 8 + 4), vl = B.u16(toc + i * 8 + 6);
            }
            if (keys + ko + kl > bs || vo > vend || vl > vo) continue;
            const uint8_t* k = b.data() + keys + ko;
            const uint8_t* v = b.data() + vend - vo;
            if (level) {
                if (vl < 8) continue;
                node(Span(v, 8).u64(0), omap, visit, fk, fv, false, depth + 1, seen);
            } else {
                visit(k, kl, v, vl);
            }
        }
        return true;
    }

    // An object map: virtual oid -> physical block, newest version not after `xid`.
    bool loadOmap(uint64_t omapPaddr, std::unordered_map<uint64_t, uint64_t>& out) {
        std::vector<uint8_t> om = block(omapPaddr);
        Span O(om);
        if (om.size() != bs || (O.u32(24) & 0xFFFF) != 0x0B) return false;
        uint64_t tree = O.u64(48);
        std::unordered_map<uint64_t, uint64_t> bestX;
        return walk(tree, nullptr, [&](const uint8_t* k, size_t kl, const uint8_t* v, size_t vl) {
            if (kl < 16 || vl < 16) return;
            uint64_t oid = Span(k, 16).u64(0), x = Span(k, 16).u64(8);
            uint32_t flags = Span(v, 16).u32(0);
            if (x > xid || (flags & 1)) return;  // newer than the checkpoint, or deleted
            auto it = bestX.find(oid);
            if (it != bestX.end() && it->second >= x) return;
            bestX[oid] = x;
            out[oid] = Span(v, 16).u64(8);
        });
    }
};

struct Inode {
    uint64_t parent = 0, stream = 0, size = 0, usize = 0;
    int64_t mtime = 0;
    uint32_t bsd = 0;
    uint16_t mode = 0;
};
struct Drec {
    uint64_t parent, child;
    std::string name;
    uint32_t type;
};
struct Xattr {
    std::vector<uint8_t> inlineData;
    uint64_t stream = 0, size = 0;
    bool hasStream = false;
};

bool openVolume(Ctx& ctx, Apfs& a, const std::unordered_map<uint64_t, uint64_t>& nxOmap, uint64_t fsOid, const std::string& base,
                std::string& volName) {
    auto it = nxOmap.find(fsOid);
    if (it == nxOmap.end()) return false;
    std::vector<uint8_t> vb = a.block(it->second);
    Span V(vb);
    if (vb.size() != a.bs || !V.eq(32, "APSB", 4)) return false;
    volName = cstr(vb.data() + 704, 256);
    uint64_t fsFlags = V.u64(264), incompat = V.u64(56);
    if (!(fsFlags & 1)) {
        ctx.out.warning = "Volume \"" + volName + "\" is encrypted, so its files can't be read";
        return true;
    }
    bool hashed = (incompat & (1 | 8)) != 0;
    std::unordered_map<uint64_t, uint64_t> volOmap;
    if (!a.loadOmap(V.u64(128), volOmap)) return false;
    uint32_t rootType = V.u32(116);
    bool physicalTree = (rootType & 0x40000000) != 0;  // sealed (signed system) volumes
    std::unordered_map<uint64_t, Inode> inodes;
    std::vector<Drec> drecs;
    std::unordered_map<uint64_t, std::vector<std::tuple<uint64_t, uint64_t, uint64_t>>> extents;  // stream -> (logical, len, phys)
    std::map<std::pair<uint64_t, std::string>, Xattr> xattrs;
    bool ok = a.walk(V.u64(136), physicalTree ? nullptr : &volOmap, [&](const uint8_t* k, size_t kl, const uint8_t* v, size_t vl) {
        if (kl < 8) return;
        Span K(k, kl), W(v, vl);
        uint64_t hdr = K.u64(0), id = hdr & 0x0FFFFFFFFFFFFFFFull;
        uint32_t type = (uint32_t)(hdr >> 60);
        if (type == 3 && vl >= 92) {  // inode
            Inode n;
            n.parent = W.u64(0);
            n.stream = W.u64(8);
            n.mtime = (int64_t)(W.u64(24) / 1000000000ull);
            n.bsd = W.u32(68);
            n.mode = (uint16_t)W.u16(80);
            n.usize = W.u64(84);
            // Extended fields: the data stream gives the size.
            uint32_t nx = W.u16(92), at = 96 + nx * 4;
            for (uint32_t i = 0; i < nx; ++i) {
                uint32_t xt = W.u8(96 + i * 4), xs = W.u16(98 + i * 4);
                if (xt == 8 && W.has(at, 8)) n.size = W.u64(at);
                at += (xs + 7) & ~7u;
            }
            inodes[id] = n;
        } else if (type == 9) {  // directory entry
            size_t nameAt = hashed ? 12 : 10;
            uint32_t nl = hashed ? K.u32(8) & 0x3FF : K.u16(8);
            if (vl < 18 || !K.has(nameAt, nl)) return;
            drecs.push_back({id, W.u64(0), cstr(k + nameAt, nl), W.u16(16) & 15u});
        } else if (type == 8 && kl >= 16 && vl >= 24) {  // file extent
            uint64_t len = W.u64(0) & 0x00FFFFFFFFFFFFFFull;
            extents[id].push_back({K.u64(8), len, W.u64(8)});
        } else if (type == 4 && kl >= 10) {  // extended attribute
            uint32_t nl = K.u16(8);
            if (!K.has(10, nl) || vl < 4) return;
            std::string name = cstr(k + 10, nl);
            Xattr x;
            uint32_t flags = W.u16(0), len = W.u16(2);
            if ((flags & 1) && vl >= 4 + 16) {
                x.hasStream = true;
                x.stream = W.u64(4);
                x.size = W.u64(12);
            } else if (W.has(4, len)) {
                x.inlineData.assign(v + 4, v + 4 + len);
            }
            xattrs[{id, name}] = std::move(x);
        }
    });
    if (!ok) {
        ctx.out.warning = "Volume \"" + volName + "\" is damaged; some files may be missing";
    }
    auto streamOf = [&](uint64_t sid, uint64_t size) -> StreamPtr {
        std::vector<ExtentStream::Extent> ext;
        auto e = extents.find(sid);
        if (e != extents.end()) {
            auto list = e->second;
            std::sort(list.begin(), list.end());
            for (auto& [lo, len, phys] : list) {
                if (lo >= size) break;
                uint64_t take = std::min(len, size - lo);
                if (phys) ext.push_back({lo, take, a.s, phys * a.bs, {}});
            }
        }
        return std::make_shared<ExtentStream>(size, std::move(ext));
    };
    // Paths: directory entries from the root folder (inode 2) down.
    std::unordered_map<uint64_t, std::vector<const Drec*>> children;
    for (auto& d : drecs) children[d.parent].push_back(&d);
    std::set<uint64_t> visited{2};
    std::vector<std::pair<uint64_t, std::string>> todo{{2, ""}};
    while (!todo.empty()) {
        auto [dir, path] = todo.back();
        todo.pop_back();
        for (const Drec* d : children[dir]) {
            if (d->name.empty()) continue;
            std::string p = (path.empty() ? "" : path + "/") + component(d->name);
            auto in = inodes.find(d->child);
            int64_t mt = in != inodes.end() ? in->second.mtime : 0;
            if (d->type == 4) {
                ctx.out.items.push_back(dirItem(base + p, mt));
                if (visited.insert(d->child).second) todo.push_back({d->child, p});
                continue;
            }
            if (in == inodes.end()) continue;
            const Inode& n = in->second;
            if (d->type == 10 || (n.mode & 0xF000) == 0xA000) {  // a symbolic link: its target text
                auto x = xattrs.find({d->child, "com.apple.fs.symlink"});
                std::vector<uint8_t> target;
                if (x != xattrs.end()) target = x->second.inlineData;
                while (!target.empty() && target.back() == 0) target.pop_back();
                ctx.out.items.push_back(builtItem(base + p, std::move(target), mt));
                continue;
            }
            if (d->type != 8 && d->type != 0) continue;  // devices, sockets...
            auto dx = xattrs.find({d->child, "com.apple.decmpfs"});
            if ((n.bsd & 0x20) && dx != xattrs.end()) {
                std::vector<uint8_t> attr = dx->second.hasStream ? streamOf(dx->second.stream, dx->second.size)->get(0, (size_t)std::min<uint64_t>(dx->second.size, 16u << 20))
                                                                  : dx->second.inlineData;
                StreamPtr rsrc;
                auto rx = xattrs.find({d->child, "com.apple.ResourceFork"});
                if (rx != xattrs.end())
                    rsrc = rx->second.hasStream ? streamOf(rx->second.stream, rx->second.size) : memStream(rx->second.inlineData);
                Item it;
                it.path = base + p;
                it.size = Span(attr).u64(8);
                it.mtime = mt;
                it.read = [attr, rsrc](const Sink& sink, std::string& err) { return decmpfs(attr, rsrc, sink, err); };
                ctx.out.items.push_back(std::move(it));
                continue;
            }
            ctx.out.items.push_back(streamItem(base + p, streamOf(n.stream, n.size), mt));
        }
    }
    return true;
}

bool openApfs(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> b0 = s->getUpTo(0, 4096);
    Span S(b0);
    if (!S.eq(32, "NXSB", 4)) return false;
    Apfs a;
    a.s = s;
    a.bs = S.u32(36);
    if (a.bs < 4096 || a.bs > 65536 || (a.bs & (a.bs - 1))) return false;
    std::vector<uint8_t> sb = s->get(0, a.bs);
    // The newest superblock in the checkpoint area (block 0 may be an older copy).
    Span B(sb);
    uint32_t descBlocks = B.u32(104);
    uint64_t descBase = B.u64(112);
    if (!(descBlocks & 0x80000000u) && descBlocks < 65536) {
        uint64_t bestX = B.u64(16);
        for (uint32_t i = 0; i < descBlocks; ++i) {
            std::vector<uint8_t> c = a.block(descBase + i);
            Span C(c);
            if (c.size() == a.bs && (C.u32(24) & 0xFFFF) == 1 && C.eq(32, "NXSB", 4) && C.u64(16) > bestX) {
                bestX = C.u64(16);
                sb = c;
            }
        }
    }
    Span N(sb);
    a.xid = N.u64(16);
    std::unordered_map<uint64_t, uint64_t> nxOmap;
    if (!a.loadOmap(N.u64(160), nxOmap)) {
        ctx.err = "Damaged APFS container (object map)";
        return false;
    }
    std::vector<uint64_t> vols;
    uint32_t maxFs = std::min<uint32_t>(N.u32(180), 100);
    for (uint32_t i = 0; i < maxFs; ++i)
        if (uint64_t oid = N.u64(184 + i * 8)) vols.push_back(oid);
    ctx.formats.push_back("APFS");
    // Several volumes (a Mac's system disk has five or so): one folder each, named after the volume.
    std::set<std::string> used;
    for (size_t i = 0; i < vols.size(); ++i) {
        std::string name;
        std::string base = ctx.prefix;
        if (vols.size() > 1) {
            auto it = nxOmap.find(vols[i]);
            std::vector<uint8_t> vb = it != nxOmap.end() ? a.block(it->second) : std::vector<uint8_t>{};
            std::string vn = vb.size() == a.bs ? component(cstr(vb.data() + 704, 256)) : "";
            if (vn.empty()) vn = "Volume " + std::to_string(i + 1);
            while (!used.insert(vn).second) vn += "_";
            base += vn + "/";
            ctx.out.items.push_back(dirItem(base.substr(0, base.size() - 1), 0));
        }
        if (!openVolume(ctx, a, nxOmap, vols[i], base, name) && ctx.out.warning.empty())
            ctx.out.warning = "A volume couldn't be read";
    }
    return true;
}

}  // namespace

const Handler hApfs = {"APFS",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                           Span H(h);
                           return H.eq(32, "NXSB", 4) && (H.u32(24) & 0xFFFF) == 1;
                       },
                       openApfs};

}  // namespace kz::native
