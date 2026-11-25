// HFS+ / HFSX (Mac OS Extended): a volume header at 1 KiB, then B-trees for the catalog (folders and
// files), extent overflow (pieces of fragmented files past the first eight) and attributes (where
// transparently compressed files keep their "decmpfs" data). All big-endian.
#include <algorithm>
#include <map>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

struct Fork {
    uint64_t size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> ext;  // (start block, block count)
};

Fork readFork(const Span& S, size_t o) {
    Fork f;
    f.size = S.b64(o);
    for (int i = 0; i < 8; ++i) {
        uint32_t st = S.b32(o + 16 + i * 8), n = S.b32(o + 20 + i * 8);
        if (n) f.ext.push_back({st, n});
    }
    return f;
}

int64_t macTime(uint32_t t) { return t ? (int64_t)t - 2082844800 : 0; }

struct Hfs {
    StreamPtr vol;
    uint32_t bs = 4096;
    // Extra extents of fragmented forks: (file ID, fork type) -> (first file block, start, count).
    std::map<std::pair<uint32_t, uint8_t>, std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>> overflow;

    StreamPtr fork(uint32_t id, uint8_t type, const Fork& f) {
        std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> all;
        uint32_t fb = 0;
        for (auto& e : f.ext) all.push_back({fb, e.first, e.second}), fb += e.second;
        auto it = overflow.find({id, type});
        if (it != overflow.end()) all.insert(all.end(), it->second.begin(), it->second.end());
        std::sort(all.begin(), all.end());
        std::vector<ExtentStream::Extent> ext;
        for (auto& [fileBlock, start, count] : all) {
            uint64_t at = (uint64_t)fileBlock * bs, len = (uint64_t)count * bs;
            if (at >= f.size) break;
            len = std::min(len, f.size - at);
            ext.push_back({at, len, vol, (uint64_t)start * bs, {}});
        }
        return std::make_shared<ExtentStream>(f.size, std::move(ext));
    }
};

// Every leaf record of a B-tree file, in order: (record bytes). False when the tree is damaged.
bool leaves(Stream& t, const std::function<void(const uint8_t*, size_t)>& rec) {
    std::vector<uint8_t> h = t.getUpTo(0, 512);
    Span H(h);
    if (h.size() < 40 || (int8_t)H.u8(8) != 1) return false;
    uint32_t first = H.b32(14 + 10), nodeSize = H.b16(14 + 18), total = H.b32(14 + 22);
    if (nodeSize < 512 || nodeSize > 32768) return false;
    std::set<uint32_t> seen;
    for (uint32_t node = first; node && node < total && seen.insert(node).second;) {
        std::vector<uint8_t> nd = t.get((uint64_t)node * nodeSize, nodeSize);
        if (nd.size() != nodeSize) return false;
        Span N(nd);
        if ((int8_t)N.u8(8) != -1) return false;
        uint32_t count = N.b16(10);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t a = N.b16(nodeSize - 2 * (i + 1)), b = N.b16(nodeSize - 2 * (i + 2));
            if (a < 14 || b <= a || b > nodeSize) continue;
            rec(nd.data() + a, b - a);
        }
        node = N.b32(0);
    }
    return true;
}

struct CatFile {
    uint32_t parent, id;
    std::string name;
    std::vector<uint8_t> rec;  // the 248-byte file record
};
struct CatDir {
    uint32_t parent;
    std::string name;
    int64_t mtime;
};

}  // namespace

// The contents of a compressed file: a 16-byte decmpfs header (little-endian), then inline data, or
// chunks in the resource fork. Shared with APFS.
bool decmpfs(const std::vector<uint8_t>& attr, StreamPtr rsrc, const Sink& sink, std::string& err) {
    Span A(attr);
    uint32_t type = A.u32(4);
    uint64_t size = A.u64(8);
    const uint8_t* d = attr.data() + 16;
    size_t dn = attr.size() > 16 ? attr.size() - 16 : 0;
    std::vector<uint8_t> out;
    auto bad = [&]() {
        err = "Damaged compressed file";
        return false;
    };
    auto emit = [&](const std::vector<uint8_t>& v) { return sink(v.data(), v.size()); };
    switch (type) {
        case 1:
        case 9:  // stored in the attribute
            out.assign(d, d + std::min<size_t>(dn, (size_t)size));
            emit(out);
            return true;
        case 3:  // zlib in the attribute
            if (dn && (d[0] & 0x0F) == 0x0F) out.assign(d + 1, d + dn);
            else if (!inflateZlib(d, dn, out, (size_t)size)) return bad();
            emit(out);
            return true;
        case 7:  // LZVN in the attribute
            if (dn && d[0] == 0x06) out.assign(d + 1, d + dn);
            else if (!unLzvn(d, dn, out, (size_t)size)) return bad();
            emit(out);
            return true;
        case 11:  // LZFSE in the attribute
            if (dn && d[0] == 0xFF) out.assign(d + 1, d + dn);
            else if (!unLzfse(d, dn, out, (size_t)size)) return bad();
            emit(out);
            return true;
        case 4:
        case 8:
        case 10:
        case 12:
            break;
        default:
            err = "Unknown kind of file compression (" + std::to_string(type) + ")";
            return false;
    }
    if (!rsrc || rsrc->size() < 8) return bad();
    if (type == 10) {  // stored in the resource fork
        std::vector<uint8_t> v = rsrc->getUpTo(0, (size_t)std::min<uint64_t>(size, rsrc->size()));
        emit(v);
        return true;
    }
    // Chunk table: (offset, size) pairs after a resource-fork header for zlib; plain offsets otherwise.
    std::vector<std::pair<uint64_t, uint64_t>> chunks;
    if (type == 4) {
        std::vector<uint8_t> h = rsrc->getUpTo(0, 16);
        uint32_t dataOff = Span(h).b32(0);
        std::vector<uint8_t> c = rsrc->getUpTo(dataOff + 4, 4);
        uint32_t count = Span(c).u32(0);
        if (count > (1u << 24)) return bad();
        std::vector<uint8_t> t = rsrc->getUpTo(dataOff + 8, (size_t)count * 8);
        Span T(t);
        for (uint32_t i = 0; i < count; ++i)
            chunks.push_back({(uint64_t)dataOff + 4 + T.u32(i * 8), T.u32(i * 8 + 4)});
    } else {
        std::vector<uint8_t> c = rsrc->getUpTo(0, 4);
        uint32_t first = Span(c).u32(0);
        if (first < 8 || first % 4 || first > (64u << 20)) return bad();
        std::vector<uint8_t> t = rsrc->getUpTo(0, first);
        Span T(t);
        for (uint32_t i = 0; i + 1 < first / 4; ++i) chunks.push_back({T.u32(i * 4), T.u32(i * 4 + 4) - (uint64_t)T.u32(i * 4)});
    }
    uint64_t left = size;
    for (auto& [off, len] : chunks) {
        if (!left) break;
        size_t want = (size_t)std::min<uint64_t>(left, 65536);
        std::vector<uint8_t> in = rsrc->getUpTo(off, (size_t)len);
        if (in.size() != len || in.empty()) return bad();
        out.clear();
        bool ok;
        if (type == 4) ok = (in[0] & 0x0F) == 0x0F ? (out.assign(in.begin() + 1, in.end()), true) : inflateZlib(in.data(), in.size(), out, want);
        else if (type == 8) ok = in[0] == 0x06 ? (out.assign(in.begin() + 1, in.end()), true) : unLzvn(in.data(), in.size(), out, want);
        else ok = in[0] == 0xFF ? (out.assign(in.begin() + 1, in.end()), true) : unLzfse(in.data(), in.size(), out, want);
        if (!ok) return bad();
        if (out.size() > want) out.resize(want);
        if (!emit(out)) return true;
        left -= out.size();
    }
    return left == 0 ? true : bad();
}

namespace {

bool openHfsAt(Ctx& ctx, StreamPtr vol) {
    std::vector<uint8_t> vh = vol->getUpTo(1024, 512);
    Span V(vh);
    uint32_t sig = V.b16(0);
    if (vh.size() < 512 || (sig != 0x482B && sig != 0x4858)) return false;
    Hfs h;
    h.vol = vol;
    h.bs = V.b32(40);
    if (h.bs < 512 || h.bs > (1u << 20) || (h.bs & (h.bs - 1))) return false;
    // Extents overflow first: the other trees may need it.
    {
        StreamPtr ext = h.fork(3, 0, readFork(V, 192));
        leaves(*ext, [&](const uint8_t* r, size_t n) {
            Span R(r, n);
            if (n < 12 + 64 || R.b16(0) != 10) return;
            uint8_t type = (uint8_t)R.u8(2);
            uint32_t id = R.b32(4), fb = R.b32(8);
            for (int i = 0; i < 8; ++i) {
                uint32_t st = R.b32(12 + i * 8), cnt = R.b32(16 + i * 8);
                if (cnt) h.overflow[{id, type}].push_back({fb, st, cnt}), fb += cnt;
            }
        });
    }
    StreamPtr cat = h.fork(4, 0, readFork(V, 272));
    std::map<uint32_t, CatDir> dirs;
    std::vector<CatFile> files;
    bool ok = leaves(*cat, [&](const uint8_t* r, size_t n) {
        Span R(r, n);
        uint32_t klen = R.b16(0);
        if (klen < 6 || 2 + klen + 2 > n) return;
        uint32_t parent = R.b32(2), nl = R.b16(6);
        if (8 + (size_t)nl * 2 > 2 + klen) return;
        std::string name = utf16be(r + 8, nl);
        size_t d = 2 + klen;
        uint32_t type = R.b16(d);
        if (type == 1 && R.has(d, 88)) {
            dirs[R.b32(d + 8)] = {parent, name, macTime(R.b32(d + 16))};
        } else if (type == 2 && R.has(d, 248)) {
            files.push_back({parent, R.b32(d + 8), name, std::vector<uint8_t>(r + d, r + d + 248)});
        }
    });
    if (!ok) {
        ctx.err = "Damaged Mac volume (catalog)";
        return false;
    }
    // Compressed files keep their header (and small contents) in a "com.apple.decmpfs" attribute.
    std::set<uint32_t> compressed;
    for (auto& f : files)
        if (Span(f.rec).u8(41) & 0x20) compressed.insert(f.id);
    std::map<uint32_t, std::vector<uint8_t>> decmp;
    if (!compressed.empty()) {
        StreamPtr attrs = h.fork(8, 0, readFork(V, 352));
        leaves(*attrs, [&](const uint8_t* r, size_t n) {
            Span R(r, n);
            uint32_t klen = R.b16(0);
            if (klen < 12 || 2 + klen + 16 > n) return;
            uint32_t id = R.b32(4), nl = R.b16(12);
            if (!compressed.count(id) || 14 + (size_t)nl * 2 > 2 + klen) return;
            if (utf16be(r + 14, nl) != "com.apple.decmpfs") return;
            size_t d = 2 + klen;
            uint32_t rt = R.b32(d);
            if (rt == 0x10) {
                uint32_t sz = R.b32(d + 12);
                if (R.has(d + 16, sz)) decmp[id].assign(r + d + 16, r + d + 16 + sz);
            } else if (rt == 0x20 && R.has(d + 8, 80)) {
                StreamPtr fs = h.fork(id, 0xFE, readFork(R, d + 8));  // big attributes live in their own fork
                if (fs->size() < (16u << 20)) decmp[id] = fs->get(0, (size_t)fs->size());
            }
        });
    }
    // Folder paths; the hidden folders holding hard-linked files stay out of the listing.
    std::set<uint32_t> hidden;
    uint32_t privFiles = 0;
    for (auto& [id, d] : dirs) {
        if (d.parent == 2 && (d.name == std::string("\0\0\0\0HFS+ Private Data", 21) || d.name == ".HFS+ Private Directory Data\r")) {
            hidden.insert(id);
            if (d.name[0] == 0) privFiles = id;
        }
    }
    std::map<uint32_t, std::string> paths;
    std::function<bool(uint32_t, std::string&, int)> pathOf = [&](uint32_t id, std::string& out, int depth) {
        if (id == 2) return out.clear(), true;  // the root
        if (depth > 256 || hidden.count(id)) return false;
        auto c = paths.find(id);
        if (c != paths.end()) return out = c->second, true;
        auto it = dirs.find(id);
        if (it == dirs.end()) return false;
        std::string up;
        if (!pathOf(it->second.parent, up, depth + 1)) return false;
        out = (up.empty() ? "" : up + "/") + component(it->second.name);
        paths[id] = out;
        return true;
    };
    std::map<uint32_t, const CatFile*> inodes;  // hard link targets: "iNode<number>"
    for (auto& f : files)
        if (privFiles && f.parent == privFiles && f.name.compare(0, 5, "iNode") == 0)
            inodes[(uint32_t)std::strtoul(f.name.c_str() + 5, nullptr, 10)] = &f;
    ctx.formats.push_back(sig == 0x4858 ? "HFSX" : "HFS+");
    for (auto& [id, d] : dirs) {
        std::string p;
        if (id != 2 && pathOf(id, p, 0) && !p.empty()) ctx.out.items.push_back(dirItem(ctx.prefix + p, d.mtime));
    }
    for (auto& f : files) {
        std::string dir;
        if (!pathOf(f.parent, dir, 0)) continue;
        std::string p = (dir.empty() ? "" : dir + "/") + component(f.name);
        const CatFile* src = &f;
        Span F(f.rec);
        if (F.b32(48) == 0x686C6E6B && F.b32(52) == 0x6866732B) {  // 'hlnk' 'hfs+': a hard link
            auto it = inodes.find(F.b32(44));
            if (it != inodes.end()) src = it->second;
        } else if (F.b32(48) == 0x66647270 && F.b32(52) == 0x4D414353) {  // a hard-linked folder alias
            continue;
        }
        Span S(src->rec);
        int64_t mt = macTime(F.b32(16));
        uint32_t sid = src->id;
        StreamPtr data = h.fork(sid, 0, readFork(S, 88));
        if ((S.u8(41) & 0x20) && decmp.count(sid)) {
            std::vector<uint8_t> attr = decmp[sid];
            StreamPtr rsrc = h.fork(sid, 0xFF, readFork(S, 168));
            Item it;
            it.path = ctx.prefix + p;
            it.size = Span(attr).u64(8);
            it.mtime = mt;
            it.read = [attr, rsrc](const Sink& sink, std::string& err) { return decmpfs(attr, rsrc, sink, err); };
            ctx.out.items.push_back(std::move(it));
            continue;
        }
        ctx.out.items.push_back(streamItem(ctx.prefix + p, data, mt));
    }
    return true;
}

bool openHfs(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> mdb = s->getUpTo(1024, 162);
    Span M(mdb);
    // An old HFS volume wrapping an HFS+ one (early Mac OS X disks).
    if (M.b16(0) == 0x4244 && M.b16(0x7C) == 0x482B) {
        uint64_t blk = M.b32(0x14), first = (uint64_t)M.b16(0x1C) * 512;
        uint64_t at = first + (uint64_t)M.b16(0x7E) * blk, len = (uint64_t)M.b16(0x80) * blk;
        return openHfsAt(ctx, rangeStream(s, at, len));
    }
    return openHfsAt(ctx, s);
}

}  // namespace

const Handler hHfs = {"HFS+",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          Span H(h);
                          uint32_t sig = H.b16(1024);
                          if ((sig == 0x482B && H.b16(1026) == 4) || (sig == 0x4858 && H.b16(1026) == 5)) return true;
                          return sig == 0x4244 && H.b16(1024 + 0x7C) == 0x482B && s.size() > 4096;
                      },
                      openHfs};

}  // namespace kz::native
