// Disks: virtual disk images (VHD, VHDX, VDI, VMDK, QCOW2, Android sparse) are read through to the
// disk they hold; partition maps (GPT, MBR, Apple) list each partition as a folder of its files (or as
// an .img when its file system isn't one Kompressor reads).
#include <algorithm>
#include <map>

#include "nat.hpp"

namespace kz::native {

namespace {

// Another file next to `path` (a parent disk, an extent), or an absolute path as given.
std::string sibling(const std::string& path, const std::string& name) {
    if (name.empty()) return {};
    fs::path n = u8path(name);
    if (n.is_absolute()) return name;
    if (path.empty()) return {};
    return pathToUtf8(u8path(path).parent_path() / n);
}

// The parent of a differencing disk, opened as the disk it holds; null when it can't be found.
StreamPtr openParent(const std::vector<std::string>& candidates, int depth, std::string& err) {
    if (depth > 8) return nullptr;
    for (auto& p : candidates) {
        if (p.empty()) continue;
        std::error_code ec;
        if (!fs::is_regular_file(u8path(p), ec)) continue;
        StreamPtr f = fileStream(p);
        if (!f) continue;
        std::string fmt, e;
        StreamPtr d = openDiskImage(f, p, fmt, e, depth + 1);
        return d ? d : f;
    }
    err = "The parent disk of this differencing disk wasn't found; its unchanged parts read as zeros";
    return nullptr;
}

// ============================================================================================== VHD
class VhdDisk : public BlockStream {
public:
    VhdDisk(StreamPtr f, uint64_t size, uint32_t blockSize, std::vector<uint32_t> bat, StreamPtr parent)
        : BlockStream(size, std::min<uint32_t>(blockSize, 1u << 20)), f_(std::move(f)), block_(blockSize),
          bat_(std::move(bat)), parent_(std::move(parent)) {
        bitmapBytes_ = ((block_ / 512 + 7) / 8 + 511) / 512 * 512;
    }

protected:
    bool readBlock(uint64_t index, uint8_t* dst) override {
        uint64_t at = index * bs_;
        uint64_t b = at / block_;
        uint32_t within = (uint32_t)(at % block_);
        uint32_t n = (uint32_t)std::min<uint64_t>(bs_, size_ - at);
        std::memset(dst, 0, bs_);
        uint32_t entry = b < bat_.size() ? bat_[b] : 0xFFFFFFFF;
        if (entry == 0xFFFFFFFF) return parent_ ? parent_->readAt(at, dst, n) : true;
        uint64_t base = (uint64_t)entry * 512;
        if (!f_->readAt(base + bitmapBytes_ + within, dst, n)) {
            // A block cut off at the end of the file: what's there.
            std::vector<uint8_t> part = f_->getUpTo(base + bitmapBytes_ + within, n);
            std::memcpy(dst, part.data(), part.size());
        }
        // Sectors the bitmap marks as never written come from the parent (or are zeros).
        std::vector<uint8_t> bm = f_->getUpTo(base + within / 512 / 8, (n / 512 + 7) / 8 + 1);
        for (uint32_t s = 0; s < n / 512; ++s) {
            uint32_t sec = within / 512 + s;
            size_t byte = sec / 8 - within / 512 / 8;
            bool written = byte < bm.size() && (bm[byte] & (0x80 >> (sec % 8)));
            if (written) continue;
            if (parent_) {
                if (!parent_->readAt(at + (uint64_t)s * 512, dst + s * 512, 512)) std::memset(dst + s * 512, 0, 512);
            } else {
                std::memset(dst + s * 512, 0, 512);
            }
        }
        return true;
    }

private:
    StreamPtr f_;
    uint32_t block_, bitmapBytes_ = 512;
    std::vector<uint32_t> bat_;
    StreamPtr parent_;
};

StreamPtr openVhd(StreamPtr f, const std::string& path, std::string& err, int depth) {
    uint64_t fsize = f->size();
    std::vector<uint8_t> foot = f->getUpTo(fsize >= 512 ? fsize - 512 : 0, 512);
    if (foot.size() < 512 || std::memcmp(foot.data(), "conectix", 8) != 0) foot = f->getUpTo(0, 512);
    Span F(foot);
    if (!F.eq(0, "conectix", 8)) return nullptr;
    uint64_t size = F.b64(48);
    uint32_t type = F.b32(60);
    if (type == 2) return rangeStream(f, 0, std::min(size, fsize >= 512 ? fsize - 512 : 0));  // fixed
    if (type != 3 && type != 4) {
        err = "Unsupported VHD disk type " + std::to_string(type);
        return nullptr;
    }
    std::vector<uint8_t> dh = f->getUpTo(F.b64(16), 1024);
    Span D(dh);
    if (!D.eq(0, "cxsparse", 8)) {
        err = "Damaged VHD (no dynamic disk header)";
        return nullptr;
    }
    uint64_t table = D.b64(16);
    uint32_t entries = D.b32(28), block = D.b32(32);
    if (block < 512 || (block & 511) || entries > (1u << 28)) {
        err = "Damaged VHD (bad block table)";
        return nullptr;
    }
    std::vector<uint8_t> raw = f->getUpTo(table, (size_t)entries * 4);
    std::vector<uint32_t> bat(entries, 0xFFFFFFFF);
    for (size_t i = 0; i < raw.size() / 4; ++i) bat[i] = Span(raw).b32(i * 4);
    StreamPtr parent;
    if (type == 4) {  // differencing: the parent is named in the header's locators
        std::vector<std::string> cand;
        for (int l = 0; l < 8; ++l) {
            size_t o = 576 + l * 24;
            std::string code((const char*)dh.data() + o, 4);
            uint32_t len = D.b32(o + 8);
            uint64_t at = D.b64(o + 16);
            if (!len || len > 4096 || (code != "W2ru" && code != "W2ku")) continue;
            std::vector<uint8_t> v = f->getUpTo(at, len);
            std::string p = utf16le(v.data(), v.size() / 2);
            while (!p.empty() && p.back() == '\0') p.pop_back();
            cand.push_back(code == "W2ru" ? sibling(path, p) : p);
        }
        cand.push_back(sibling(path, utf16be(dh.data() + 64, 256).c_str()));
        parent = openParent(cand, depth, err);
    }
    return std::make_shared<VhdDisk>(f, size, block, std::move(bat), parent);
}

// ============================================================================================= VHDX
class VhdxDisk : public BlockStream {
public:
    VhdxDisk(StreamPtr f, uint64_t size, uint32_t block, uint32_t sector, std::vector<uint64_t> bat, StreamPtr parent)
        : BlockStream(size, std::min<uint32_t>(block, 1u << 20)), f_(std::move(f)), block_(block), sector_(sector),
          bat_(std::move(bat)), parent_(std::move(parent)) {
        ratio_ = (uint64_t)((1ull << 23) * sector_ / block_);
        if (!ratio_) ratio_ = 1;
    }

protected:
    bool readBlock(uint64_t index, uint8_t* dst) override {
        uint64_t at = index * bs_, b = at / block_, within = at % block_;
        uint32_t n = (uint32_t)std::min<uint64_t>(bs_, size_ - at);
        std::memset(dst, 0, bs_);
        uint64_t idx = b + b / ratio_;
        uint64_t e = idx < bat_.size() ? bat_[idx] : 0;
        uint32_t state = e & 7;
        uint64_t off = (e >> 20) << 20;
        if (state == 6 || (state == 7 && !parent_)) {
            std::vector<uint8_t> d = f_->getUpTo(off + within, n);
            std::memcpy(dst, d.data(), d.size());
            return true;
        }
        if (state == 7) {  // partly present: a sector bitmap says which sectors are here
            uint64_t chunk = b / ratio_;
            uint64_t bidx = chunk * (ratio_ + 1) + ratio_;
            uint64_t be = bidx < bat_.size() ? bat_[bidx] : 0;
            std::vector<uint8_t> d = f_->getUpTo(off + within, n);
            std::memcpy(dst, d.data(), d.size());
            uint64_t firstSector = at / sector_ - chunk * ((1ull << 23));  // sector index within the chunk
            std::vector<uint8_t> bm = (be & 7) == 6 ? f_->getUpTo(((be >> 20) << 20) + firstSector / 8, n / sector_ / 8 + 2)
                                                   : std::vector<uint8_t>();
            for (uint32_t s = 0; s < n / sector_; ++s) {
                uint64_t bit = firstSector + s - firstSector / 8 * 8;
                bool here = bit / 8 < bm.size() && (bm[bit / 8] >> (bit % 8) & 1);
                if (!here && !parent_->readAt(at + (uint64_t)s * sector_, dst + s * sector_, sector_))
                    std::memset(dst + s * sector_, 0, sector_);
            }
            return true;
        }
        if (state == 0 && parent_) return parent_->readAt(at, dst, n);
        return true;  // not present / zero / unmapped
    }

private:
    StreamPtr f_;
    uint32_t block_, sector_;
    uint64_t ratio_ = 1;
    std::vector<uint64_t> bat_;
    StreamPtr parent_;
};

StreamPtr openVhdx(StreamPtr f, const std::string& path, std::string& err, int depth) {
    if (f->getUpTo(0, 8) != std::vector<uint8_t>{'v', 'h', 'd', 'x', 'f', 'i', 'l', 'e'}) return nullptr;
    // Of the two headers, the valid one written last.
    std::vector<uint8_t> best;
    uint64_t bestSeq = 0;
    for (uint64_t at : {64ull * 1024, 128ull * 1024}) {
        std::vector<uint8_t> h = f->getUpTo(at, 4096);
        Span H(h);
        if (H.eq(0, "head", 4) && (best.empty() || H.u64(8) > bestSeq)) {
            best = h;
            bestSeq = H.u64(8);
        }
    }
    bool dirty = false;
    if (!best.empty()) {
        for (int i = 0; i < 16; ++i) dirty |= best[48 + i] != 0;  // a log still to replay
    }
    std::vector<uint8_t> rt = f->getUpTo(192 * 1024, 64 * 1024);
    Span R(rt);
    if (!R.eq(0, "regi", 4)) rt = f->getUpTo(256 * 1024, 64 * 1024), R = Span(rt);
    if (!R.eq(0, "regi", 4)) {
        err = "Damaged VHDX (no region table)";
        return nullptr;
    }
    std::vector<uint8_t> gBat = guidBytes("2DC27766-F623-4200-9D64-115E9BFD4A08"),
                         gMeta = guidBytes("8B7CA206-4790-4B9A-B8FE-575F050F886E");
    uint64_t batOff = 0, metaOff = 0;
    uint32_t batLen = 0, metaLen = 0;
    for (uint32_t i = 0; i < R.u32(8) && i < 2047; ++i) {
        size_t o = 16 + i * 32;
        if (!R.has(o, 32)) break;
        if (std::memcmp(rt.data() + o, gBat.data(), 16) == 0) batOff = R.u64(o + 16), batLen = R.u32(o + 24);
        if (std::memcmp(rt.data() + o, gMeta.data(), 16) == 0) metaOff = R.u64(o + 16), metaLen = R.u32(o + 24);
    }
    std::vector<uint8_t> md = f->getUpTo(metaOff, std::min<uint32_t>(metaLen, 16u << 20));
    Span M(md);
    if (!M.eq(0, "metadata", 8)) {
        err = "Damaged VHDX (no metadata)";
        return nullptr;
    }
    auto item = [&](const char* g) -> Span {
        std::vector<uint8_t> id = guidBytes(g);
        for (uint32_t i = 0; i < M.u16(10); ++i) {
            size_t o = 32 + i * 32;
            if (!M.has(o, 32)) break;
            if (std::memcmp(md.data() + o, id.data(), 16) != 0) continue;
            uint32_t off = M.u32(o + 16), len = M.u32(o + 20);
            if (M.has(off, len)) return Span(md.data() + off, len);
        }
        return Span();
    };
    Span params = item("CAA16737-FA36-4D43-B3B6-33F0AA44E76B"), vsize = item("2FA54224-CD1B-4876-B211-5DBED83BF4B8"),
         lsec = item("8141BF1D-A96F-4709-BA47-F233A8FAAB5F"), loc = item("A8D35F2D-B30B-454D-ABF7-D3D84834AB0C");
    uint32_t block = params.u32(0), sector = lsec.u32(0) ? lsec.u32(0) : 512;
    uint64_t size = vsize.u64(0);
    bool hasParent = params.u32(4) & 2;
    if (!block || (block & (block - 1)) || !size) {
        err = "Damaged VHDX (bad disk parameters)";
        return nullptr;
    }
    std::vector<uint8_t> rawBat = f->getUpTo(batOff, batLen);
    std::vector<uint64_t> bat(rawBat.size() / 8);
    for (size_t i = 0; i < bat.size(); ++i) bat[i] = Span(rawBat).u64(i * 8);
    StreamPtr parent;
    if (hasParent && loc.n >= 20) {
        // Parent locator: key/value pairs (UTF-16) such as relative_path and absolute_win32_path.
        std::map<std::string, std::string> kv;
        uint32_t count = loc.u16(18);
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            size_t o = 20 + i * 12;
            uint32_t ko = loc.u32(o), vo = loc.u32(o + 4), kl = loc.u16(o + 8), vl = loc.u16(o + 10);
            if (!loc.has(ko, kl) || !loc.has(vo, vl)) continue;
            kv[utf16le(loc.p + ko, kl / 2)] = utf16le(loc.p + vo, vl / 2);
        }
        parent = openParent({sibling(path, kv["relative_path"]), kv["absolute_win32_path"]}, depth, err);
    }
    if (dirty && err.empty()) err = "This VHDX wasn't closed cleanly; its newest changes may be missing";
    return std::make_shared<VhdxDisk>(f, size, block, sector, std::move(bat), parent);
}

// ============================================================================================== VDI
class VdiDisk : public BlockStream {
public:
    VdiDisk(StreamPtr f, uint64_t size, uint32_t block, uint32_t extra, uint64_t data, std::vector<uint32_t> map)
        : BlockStream(size, block), f_(std::move(f)), extra_(extra), data_(data), map_(std::move(map)) {}

protected:
    bool readBlock(uint64_t index, uint8_t* dst) override {
        std::memset(dst, 0, bs_);
        uint32_t e = index < map_.size() ? map_[index] : 0xFFFFFFFF;
        if (e >= 0xFFFFFFFE) return true;  // free or zero
        std::vector<uint8_t> d = f_->getUpTo(data_ + (uint64_t)e * (bs_ + extra_) + extra_, bs_);
        std::memcpy(dst, d.data(), d.size());
        return true;
    }

private:
    StreamPtr f_;
    uint32_t extra_;
    uint64_t data_;
    std::vector<uint32_t> map_;
};

StreamPtr openVdi(StreamPtr f, std::string& err) {
    std::vector<uint8_t> h = f->getUpTo(0, 512);
    Span H(h);
    if (H.u32(64) != 0xBEDA107F) return nullptr;
    if (H.u32(68) >> 16 != 1) {
        err = "Unsupported VDI version";
        return nullptr;
    }
    uint32_t type = H.u32(76), offBlocks = H.u32(340), offData = H.u32(344), block = H.u32(376), extra = H.u32(380),
             count = H.u32(384);
    uint64_t size = H.u64(368);
    if (type == 2) return rangeStream(f, offData, size);  // fixed
    if (!block || block > (64u << 20) || count > (1u << 28)) {
        err = "Damaged VDI header";
        return nullptr;
    }
    if (type == 4) err = "This VDI is a differencing image; its unchanged parts read as zeros";
    std::vector<uint8_t> raw = f->getUpTo(offBlocks, (size_t)count * 4);
    std::vector<uint32_t> map(count, 0xFFFFFFFF);
    for (size_t i = 0; i < raw.size() / 4; ++i) map[i] = Span(raw).u32(i * 4);
    return std::make_shared<VdiDisk>(f, size, block, extra, offData, std::move(map));
}

// ============================================================================================= VMDK
class VmdkSparse : public BlockStream {
public:
    VmdkSparse(StreamPtr f, uint64_t size, uint32_t grain, uint32_t perGt, std::vector<uint32_t> gd, bool compressed,
               StreamPtr parent)
        : BlockStream(size, grain), f_(std::move(f)), perGt_(perGt), gd_(std::move(gd)), compressed_(compressed),
          parent_(std::move(parent)) {}

protected:
    bool readBlock(uint64_t g, uint8_t* dst) override {
        std::memset(dst, 0, bs_);
        uint64_t gdi = g / perGt_;
        uint32_t gt = gdi < gd_.size() ? gd_[gdi] : 0;
        uint32_t sector = 0;
        if (gt) {
            if (gtCacheIdx_ != gdi) {
                gtCache_ = f_->getUpTo((uint64_t)gt * 512, (size_t)perGt_ * 4);
                gtCacheIdx_ = gdi;
            }
            sector = Span(gtCache_).u32((g % perGt_) * 4);
        }
        if (sector == 0) {
            if (parent_) return parent_->readAt(g * bs_, dst, (size_t)std::min<uint64_t>(bs_, size_ - g * bs_));
            return true;
        }
        if (sector == 1) return true;  // a zero grain
        uint64_t at = (uint64_t)sector * 512;
        if (!compressed_) {
            std::vector<uint8_t> d = f_->getUpTo(at, bs_);
            std::memcpy(dst, d.data(), d.size());
            return true;
        }
        std::vector<uint8_t> hdr = f_->getUpTo(at, 12);
        uint32_t len = Span(hdr).u32(8);
        if (hdr.size() < 12 || len > bs_ * 2 + 4096) return false;
        std::vector<uint8_t> c = f_->getUpTo(at + 12, len), out;
        if (!inflateZlib(c.data(), c.size(), out, bs_)) return false;
        std::memcpy(dst, out.data(), std::min<size_t>(out.size(), bs_));
        return true;
    }

private:
    StreamPtr f_;
    uint32_t perGt_;
    std::vector<uint32_t> gd_;
    bool compressed_;
    StreamPtr parent_;
    uint64_t gtCacheIdx_ = UINT64_MAX;
    std::vector<uint8_t> gtCache_;
};

StreamPtr openVmdkSparse(StreamPtr f, std::string& err, StreamPtr parent = nullptr) {
    std::vector<uint8_t> h = f->getUpTo(0, 512);
    Span H(h);
    if (!H.eq(0, "KDMV", 4)) return nullptr;
    uint64_t gdOff = H.u64(56);
    if (gdOff == UINT64_MAX && f->size() >= 1024) {  // stream-optimized: the real header is the footer
        h = f->getUpTo(f->size() - 1024, 512);
        H = Span(h);
        if (!H.eq(0, "KDMV", 4)) {
            err = "Damaged VMDK (no footer)";
            return nullptr;
        }
        gdOff = H.u64(56);
    }
    uint32_t flags = H.u32(8), perGt = H.u32(44);
    uint64_t capacity = H.u64(12), grain = H.u64(20);
    if (!gdOff) gdOff = H.u64(48);
    if (!grain || grain > 2048 || !perGt || perGt > (1u << 16) || !capacity) {
        err = "Damaged VMDK header";
        return nullptr;
    }
    uint64_t grains = (capacity + grain - 1) / grain, gdEntries = (grains + perGt - 1) / perGt;
    std::vector<uint8_t> raw = f->getUpTo(gdOff * 512, (size_t)gdEntries * 4);
    std::vector<uint32_t> gd(gdEntries, 0);
    for (size_t i = 0; i < raw.size() / 4; ++i) gd[i] = Span(raw).u32(i * 4);
    bool compressed = (flags & 0x10000) && H.u16(77) == 1;
    return std::make_shared<VmdkSparse>(f, capacity * 512, (uint32_t)grain * 512, perGt, std::move(gd), compressed, parent);
}

// A text descriptor naming the extents (files) that make up the disk.
StreamPtr openVmdkDescriptor(StreamPtr f, const std::string& path, std::string& err, int depth) {
    std::vector<uint8_t> t = f->getUpTo(0, 64 * 1024);
    std::string text(t.begin(), t.end());
    if (text.find("# Disk DescriptorFile") == std::string::npos && text.find("createType") == std::string::npos) return nullptr;
    std::vector<ExtentStream::Extent> ext;
    uint64_t at = 0;
    std::string parentName;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() : eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("parentFileNameHint=", 0) == 0) {
            size_t q1 = line.find('"'), q2 = line.rfind('"');
            if (q1 != std::string::npos && q2 > q1) parentName = line.substr(q1 + 1, q2 - q1 - 1);
            continue;
        }
        if (line.rfind("RW ", 0) != 0 && line.rfind("RDONLY ", 0) != 0 && line.rfind("NOACCESS ", 0) != 0) continue;
        // RW <sectors> <TYPE> "<file>" [offset]
        size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
        uint64_t sectors = std::strtoull(line.c_str() + sp1 + 1, nullptr, 10);
        std::string type = line.substr(sp2 + 1, line.find(' ', sp2 + 1) - sp2 - 1);
        size_t q1 = line.find('"'), q2 = line.find('"', q1 + 1);
        std::string file = q1 != std::string::npos && q2 != std::string::npos ? line.substr(q1 + 1, q2 - q1 - 1) : "";
        uint64_t offset = q2 != std::string::npos ? std::strtoull(line.c_str() + q2 + 1, nullptr, 10) : 0;
        ExtentStream::Extent e{at, sectors * 512, nullptr, 0, {}};
        if (type == "FLAT" || type == "VMFS") {
            if (StreamPtr x = fileStream(sibling(path, file))) e.src = x, e.srcOff = offset * 512;
            else err = "A part of this disk (" + file + ") is missing; it reads as zeros";
        } else if (type == "SPARSE" || type == "VMFSSPARSE") {
            StreamPtr x = fileStream(sibling(path, file));
            std::string e2;
            if (StreamPtr d = x ? openVmdkSparse(x, e2) : nullptr) e.src = d;
            else err = "A part of this disk (" + file + ") is missing; it reads as zeros";
        }
        ext.push_back(std::move(e));
        at += sectors * 512;
    }
    if (ext.empty()) return nullptr;
    if (!parentName.empty() && err.empty()) err = "This VMDK is a snapshot; parts unchanged since its parent read as zeros";
    (void)depth;
    return std::make_shared<ExtentStream>(at, std::move(ext));
}

// ============================================================================================ QCOW2
class QcowDisk : public BlockStream {
public:
    QcowDisk(StreamPtr f, uint64_t size, uint32_t bits, std::vector<uint64_t> l1, bool zstd, StreamPtr backing)
        : BlockStream(size, 1u << bits), f_(std::move(f)), bits_(bits), l1_(std::move(l1)), zstd_(zstd),
          backing_(std::move(backing)) {}

protected:
    bool readBlock(uint64_t c, uint8_t* dst) override {
        std::memset(dst, 0, bs_);
        uint64_t perL2 = bs_ / 8;
        uint64_t l1i = c / perL2;
        uint64_t l2off = l1i < l1_.size() ? l1_[l1i] & 0x00FFFFFFFFFFFE00ull : 0;
        uint64_t e = 0;
        if (l2off) {
            if (l2Idx_ != l1i) {
                l2_ = f_->getUpTo(l2off, bs_);
                l2Idx_ = l1i;
            }
            e = Span(l2_).b64((c % perL2) * 8);
        }
        uint32_t n = (uint32_t)std::min<uint64_t>(bs_, size_ - c * bs_);
        if (e & (1ull << 62)) {  // compressed cluster
            uint32_t x = 62 - (bits_ - 8);
            uint64_t host = e & ((1ull << x) - 1);
            uint64_t extra = (e >> x) & ((1ull << (bits_ - 8)) - 1);
            uint64_t len = (extra + 1) * 512 - (host & 511);
            std::vector<uint8_t> in = f_->getUpTo(host, (size_t)len), out;
            bool ok = zstd_ ? unZstd(in.data(), in.size(), out, bs_) : inflateRaw(in.data(), in.size(), out, bs_);
            if (!ok && out.empty()) return false;
            std::memcpy(dst, out.data(), std::min<size_t>(out.size(), bs_));
            return true;
        }
        uint64_t host = e & 0x00FFFFFFFFFFFE00ull;
        if (e & 1) return true;  // reads as zeros
        if (!host) {  // not in this image: the backing image's data, or zeros
            if (backing_ && c * bs_ < backing_->size())
                backing_->readAt(c * bs_, dst, (size_t)std::min<uint64_t>(n, backing_->size() - c * bs_));
            return true;
        }
        std::vector<uint8_t> d = f_->getUpTo(host, n);
        std::memcpy(dst, d.data(), d.size());
        return true;
    }

private:
    StreamPtr f_;
    uint32_t bits_;
    std::vector<uint64_t> l1_;
    bool zstd_;
    StreamPtr backing_;
    uint64_t l2Idx_ = UINT64_MAX;
    std::vector<uint8_t> l2_;
};

StreamPtr openQcow(StreamPtr f, const std::string& path, std::string& err, int depth) {
    std::vector<uint8_t> h = f->getUpTo(0, 4096);
    Span H(h);
    if (!H.eq(0, "QFI\xFB", 4)) return nullptr;
    uint32_t version = H.b32(4);
    if (version < 2) {
        err = "QCOW version 1 images aren't supported";
        return nullptr;
    }
    uint32_t bits = H.b32(20), crypt = H.b32(32), l1Size = H.b32(36);
    uint64_t size = H.b64(24), l1Off = H.b64(40);
    if (crypt) {
        err = "This QCOW image is encrypted";
        return nullptr;
    }
    if (bits < 9 || bits > 21 || l1Size > (1u << 26)) {
        err = "Damaged QCOW header";
        return nullptr;
    }
    bool zstd = false;
    if (version >= 3) {
        uint64_t incompat = H.b64(72);
        if (incompat & 0x10) {
            err = "QCOW images with extended L2 entries aren't supported";
            return nullptr;
        }
        if (H.b32(100) > 104) zstd = H.u8(104) == 1;
    }
    std::vector<uint8_t> raw = f->getUpTo(l1Off, (size_t)l1Size * 8);
    std::vector<uint64_t> l1(l1Size, 0);
    for (size_t i = 0; i < raw.size() / 8; ++i) l1[i] = Span(raw).b64(i * 8);
    StreamPtr backing;
    uint64_t bOff = H.b64(8);
    uint32_t bLen = H.b32(16);
    if (bOff && bLen && bLen < 4096) {
        std::vector<uint8_t> b = f->getUpTo(bOff, bLen);
        backing = openParent({sibling(path, std::string(b.begin(), b.end()))}, depth, err);
    }
    return std::make_shared<QcowDisk>(f, size, bits, std::move(l1), zstd, backing);
}

// =================================================================================== Android sparse
StreamPtr openSparse(StreamPtr f, std::string& err) {
    std::vector<uint8_t> h = f->getUpTo(0, 28);
    Span H(h);
    if (H.u32(0) != 0xED26FF3A) return nullptr;
    uint32_t fileHdr = H.u16(8), chunkHdr = H.u16(10), blk = H.u32(12), total = H.u32(16), chunks = H.u32(20);
    if (!blk || blk % 4 || chunkHdr < 12) {
        err = "Damaged sparse image";
        return nullptr;
    }
    std::vector<ExtentStream::Extent> ext;
    uint64_t at = fileHdr, out = 0;
    for (uint32_t i = 0; i < chunks; ++i) {
        std::vector<uint8_t> c = f->getUpTo(at, chunkHdr);
        Span C(c);
        if (c.size() < 12) break;
        uint32_t type = C.u16(0), blocks = C.u32(4), size = C.u32(8);
        uint64_t len = (uint64_t)blocks * blk;
        if (type == 0xCAC1) ext.push_back({out, len, f, at + chunkHdr, {}});
        else if (type == 0xCAC2) ext.push_back({out, len, nullptr, 0, f->getUpTo(at + chunkHdr, 4)});
        if (type != 0xCAC4) out += len;  // 0xCAC3 "don't care": a hole
        if (size < chunkHdr) break;
        at += size;
    }
    return std::make_shared<ExtentStream>(std::max<uint64_t>(out, (uint64_t)total * blk), std::move(ext));
}

// =================================================================================== partition maps
bool openGpt(Ctx& ctx, StreamPtr s) {
    for (uint32_t ss : {512u, 4096u}) {
        std::vector<uint8_t> h = s->getUpTo(ss, 512);
        Span H(h);
        if (!H.eq(0, "EFI PART", 8)) continue;
        uint64_t table = H.u64(72) * ss;
        uint32_t n = H.u32(80), es = H.u32(84);
        if (es < 128 || es > 4096 || n > 4096) return false;
        std::vector<uint8_t> t = s->getUpTo(table, (size_t)n * es);
        Span T(t);
        int found = 0;
        ctx.formats.push_back("GPT");
        for (uint32_t i = 0; i < n; ++i) {
            size_t o = (size_t)i * es;
            if (!T.has(o, 128)) break;
            bool empty = true;
            for (int k = 0; k < 16; ++k) empty &= t[o + k] == 0;
            if (empty) continue;
            uint64_t first = T.u64(o + 32), last = T.u64(o + 40);
            if (last < first) continue;
            StreamPtr part = rangeStream(s, first * ss, (last - first + 1) * ss);
            std::string num = std::to_string(++found);
            explorePart(ctx, part, num, num + ".img");
        }
        return found > 0;
    }
    return false;
}

bool openMbr(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    Span H(h);
    struct Part {
        uint64_t start, len;
    };
    std::vector<Part> parts;
    for (int i = 0; i < 4; ++i) {
        size_t o = 446 + i * 16;
        uint32_t type = H.u8(o + 4), start = H.u32(o + 8), count = H.u32(o + 12);
        if (!type || !count) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {  // extended: a chain of logical partitions
            uint64_t ext = start, next = 0;
            for (int k = 0; k < 128; ++k) {
                std::vector<uint8_t> e = s->getUpTo((ext + next) * 512, 512);
                Span E(e);
                if (E.u16(510) != 0xAA55) break;
                if (E.u8(446 + 4) && E.u32(446 + 12)) parts.push_back({ext + next + E.u32(446 + 8), E.u32(446 + 12)});
                uint32_t nt = E.u8(462 + 4);
                if (!(nt == 0x05 || nt == 0x0F || nt == 0x85) || !E.u32(462 + 8)) break;
                next = E.u32(462 + 8);
            }
            continue;
        }
        parts.push_back({start, count});
    }
    if (parts.empty()) return false;
    ctx.formats.push_back("MBR");
    int n = 0;
    for (auto& p : parts) {
        if (p.start * 512 >= s->size()) continue;
        std::string num = std::to_string(++n);
        explorePart(ctx, rangeStream(s, p.start * 512, p.len * 512), num, num + ".img");
    }
    return n > 0;
}

bool openApm(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    uint32_t bs = Span(h).b16(2);
    if (bs < 512 || bs > 4096 || (bs & (bs - 1))) bs = 512;
    std::vector<uint8_t> first = s->getUpTo(512, 512);
    uint32_t entries = Span(first).b32(4);
    if (entries == 0 || entries > 256) return false;
    ctx.formats.push_back("APM");
    int n = 0;
    for (uint32_t i = 1; i <= entries; ++i) {
        std::vector<uint8_t> e = s->getUpTo((uint64_t)i * 512, 512);
        Span E(e);
        if (!E.eq(0, "PM", 2)) break;
        std::string type = cstr(e.data() + 48, 32);
        if (type == "Apple_partition_map" || type == "Apple_Free" || type == "Apple_Void") continue;
        uint64_t start = (uint64_t)E.b32(8) * bs, len = (uint64_t)E.b32(12) * bs;
        if (!len || start >= s->size()) continue;
        std::string num = std::to_string(++n);
        explorePart(ctx, rangeStream(s, start, len), num, num + ".img");
    }
    return n > 0;
}

// A virtual disk's handler: read through to the disk and list what's on it.
bool openImage(Ctx& ctx, StreamPtr s) {
    std::string fmt;
    StreamPtr d = openDiskImage(s, ctx.srcPath, fmt, ctx.err, 0);
    if (!d) return false;
    ctx.formats.push_back(fmt);
    Ctx sub{ctx.out, ctx.prefix, "", ctx.depth, {}, {}};
    if (explore(sub, d)) {
        ctx.formats.insert(ctx.formats.end(), sub.formats.begin(), sub.formats.end());
        if (ctx.err.empty()) ctx.err = sub.err;
    } else {
        ctx.out.items.push_back(streamItem(ctx.prefix + "disk.img", d, 0));
    }
    return true;
}

bool tailIs(Stream& s, const char* sig) {
    if (s.size() < 512) return false;
    std::vector<uint8_t> t = s.getUpTo(s.size() - 512, 8);
    return t.size() == 8 && std::memcmp(t.data(), sig, 8) == 0;
}

}  // namespace

StreamPtr openDiskImage(StreamPtr s, const std::string& path, std::string& fmt, std::string& err, int depth) {
    std::vector<uint8_t> h = s->getUpTo(0, 512);
    Span H(h);
    StreamPtr d;
    if (H.eq(0, "vhdxfile", 8)) fmt = "VHDX", d = openVhdx(s, path, err, depth);
    else if (H.eq(0, "conectix", 8) || tailIs(*s, "conectix")) fmt = "VHD", d = openVhd(s, path, err, depth);
    else if (H.u32(64) == 0xBEDA107F) fmt = "VDI", d = openVdi(s, err);
    else if (H.eq(0, "KDMV", 4)) fmt = "VMDK", d = openVmdkSparse(s, err);
    else if (H.eq(0, "QFI\xFB", 4)) fmt = "QCOW2", d = openQcow(s, path, err, depth);
    else if (H.u32(0) == 0xED26FF3A) fmt = "Sparse", d = openSparse(s, err);
    else if (H.eq(0, "# Disk DescriptorFile", 21)) fmt = "VMDK", d = openVmdkDescriptor(s, path, err, depth);
    return d;
}

const Handler hVhd = {"VHD",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          return Span(h).eq(0, "conectix", 8) || tailIs(s, "conectix");
                      },
                      openImage};
const Handler hVhdx = {"VHDX", [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(0, "vhdxfile", 8); },
                       openImage};
const Handler hVdi = {"VDI", [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).u32(64) == 0xBEDA107F; },
                      openImage};
const Handler hVmdk = {"VMDK",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                           return Span(h).eq(0, "KDMV", 4) || Span(h).eq(0, "# Disk DescriptorFile", 21);
                       },
                       openImage};
const Handler hQcow = {"QCOW", [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).eq(0, "QFI\xFB", 4); },
                       openImage};
const Handler hSparse = {"Sparse",
                         [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return Span(h).u32(0) == 0xED26FF3A; },
                         openImage};

const Handler hGpt = {"GPT",
                      [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                          if (Span(h).eq(512, "EFI PART", 8)) return true;
                          return Span(h).eq(4096, "EFI PART", 8) || (s.size() > 4104 && s.getUpTo(4096, 8) ==
                                                                                             std::vector<uint8_t>{'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'});
                      },
                      openGpt};

const Handler hMbr = {"MBR",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          Span H(h);
                          if (H.u16(510) != 0xAA55) return false;
                          int valid = 0;
                          for (int i = 0; i < 4; ++i) {
                              size_t o = 446 + i * 16;
                              uint32_t status = H.u8(o), type = H.u8(o + 4);
                              if (!type) continue;
                              if ((status != 0 && status != 0x80) || !H.u32(o + 8) || !H.u32(o + 12)) return false;
                              ++valid;
                          }
                          return valid > 0;
                      },
                      openMbr};

const Handler hApm = {"APM",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          return Span(h).eq(0, "ER", 2) && Span(h).eq(512, "PM", 2);
                      },
                      openApm};

}  // namespace kz::native
