// UEFI firmware: firmware volumes ("_FVH") holding FFS files, whose sections are PE/TE images, raw
// data, names, or compressed (EFI/Tiano, LZMA) or nested volumes. Whole flash dumps and capsules are
// searched for volumes. Each file becomes a folder named by its UI name (or GUID).
#include <algorithm>
#include <map>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

const char* const kLzmaGuid = "EE4E5898-3914-4259-9D6E-DC7BD79403CF";
const char* const kLzmaF86Guid = "D42AE6BD-1352-4BFB-909A-CA72A6EAE889";
const char* const kTianoGuid = "A31280AD-481E-41B6-95E8-127F4C984779";
const char* const kCrc32Guid = "FC1BCDB0-7D31-49AA-936A-A4600D9DD083";

struct Uefi {
    Ctx& ctx;
    int budget = 200000;  // items, against pathological images
    std::set<std::string>* used = nullptr;

    void add(Item it) {
        if (budget-- > 0) ctx.out.items.push_back(std::move(it));
    }
    std::string unique(std::map<std::string, int>& seen, const std::string& base) {
        int n = seen[base]++;
        return n ? base + "_" + std::to_string(n) : base;
    }

    bool efiDecompress(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
        if (n < 8) return false;
        uint32_t csize = Span(p, n).u32(0), osize = Span(p, n).u32(4);
        if (osize > (256u << 20) || 8 + (size_t)csize > n + 8) return false;
        size_t in = std::min<size_t>(n - 8, csize);
        return unLzhStatic(p + 8, in, out, osize, 14, 4) || unLzhStatic(p + 8, in, out, osize, 20, 5);
    }

    // The sections of one file (or of a decompressed section): their bytes in `d`.
    void sections(const std::vector<uint8_t>& d, size_t from, size_t to, const std::string& dir, std::map<std::string, int>& seen,
                  int depth) {
        Span D(d);
        for (size_t o = (from + 3) & ~size_t(3); o + 4 <= to; o = (o + 3) & ~size_t(3)) {
            uint32_t size = D.u24(o), type = D.u8(o + 3), hdr = 4;
            if (size == 0xFFFFFF) size = D.u32(o + 4), hdr = 8;
            if (size < hdr || o + size > to) break;
            size_t body = o + hdr, end = o + size;
            auto file = [&](const std::string& name, size_t a, size_t b) {
                std::vector<uint8_t> v(d.begin() + (ptrdiff_t)a, d.begin() + (ptrdiff_t)b);
                add(builtItem(dir + unique(seen, name), std::move(v), 0));
            };
            switch (type) {
                case 0x01: {  // compressed
                    if (end - body < 5) break;
                    uint32_t len = D.u32(body), kind = D.u8(body + 4);
                    std::vector<uint8_t> out;
                    bool ok = kind == 0 ? (out.assign(d.begin() + (ptrdiff_t)body + 5, d.begin() + (ptrdiff_t)end), true)
                                        : efiDecompress(d.data() + body + 5, end - body - 5, out);
                    if (ok && depth < 8) {
                        out.resize(std::min<size_t>(out.size(), len));
                        sections(out, 0, out.size(), dir, seen, depth + 1);
                    } else if (!ok) {
                        file("compressed.bin", body, end);
                    }
                    break;
                }
                case 0x02: {  // GUID-defined: LZMA, Tiano, CRC32...
                    if (end - body < 20) break;
                    std::string g = guidText(d.data() + body);
                    uint32_t dataOff = D.u16(body + 16);
                    size_t dataAt = o + dataOff;
                    if (dataAt > end) break;
                    std::vector<uint8_t> out;
                    bool ok = false;
                    if (g == kLzmaGuid || g == kLzmaF86Guid) {
                        ok = unXz(d.data() + dataAt, end - dataAt, out, 0);
                        // The x86 variant also converted call targets; those stay converted here.
                    } else if (g == kTianoGuid) {
                        ok = efiDecompress(d.data() + dataAt, end - dataAt, out);
                    } else if (g == kCrc32Guid) {
                        out.assign(d.begin() + (ptrdiff_t)dataAt, d.begin() + (ptrdiff_t)end);
                        ok = true;
                    }
                    if (ok && depth < 8) sections(out, 0, out.size(), dir, seen, depth + 1);
                    else file(g + ".bin", dataAt, end);
                    break;
                }
                case 0x10: file("body.efi", body, end); break;
                case 0x11: file("body.pic", body, end); break;
                case 0x12: file("body.te", body, end); break;
                case 0x13: file("dxe.depex", body, end); break;
                case 0x14: {  // version: a build number, then text
                    std::string v = std::to_string(D.u16(body)) + " " + utf16le(d.data() + body + 2, (end - body - 2) / 2);
                    while (!v.empty() && v.back() == 0) v.pop_back();
                    add(builtItem(dir + unique(seen, "version.txt"), textFile(v), 0));
                    break;
                }
                case 0x15: break;  // the name: used for the folder
                case 0x17:         // a nested firmware volume
                    if (depth < 8) volume(d, body, end, dir + unique(seen, "volume") + "/", depth + 1);
                    break;
                case 0x18: file("freeform" + extFor(d.data() + body + 16, end - body - 16), body + 16, end); break;
                case 0x19: file("raw" + extFor(d.data() + body, end - body), body, end); break;
                case 0x1B: file("pei.depex", body, end); break;
                case 0x1C: file("smm.depex", body, end); break;
                default: file("section_" + hex(type, 2) + ".bin", body, end); break;
            }
            o = end;
        }
    }

    std::string uiName(const std::vector<uint8_t>& d, size_t from, size_t to) {
        Span D(d);
        for (size_t o = (from + 3) & ~size_t(3); o + 4 <= to; o = (o + 3) & ~size_t(3)) {
            uint32_t size = D.u24(o), type = D.u8(o + 3);
            if (size < 4 || o + size > to) break;
            if (type == 0x15) {
                std::string s = utf16le(d.data() + o + 4, (size - 4) / 2);
                while (!s.empty() && s.back() == 0) s.pop_back();
                return s;
            }
            o += size;
        }
        return "";
    }

    // One firmware volume at [at, end) of `d`; its files go under `dir`.
    void volume(const std::vector<uint8_t>& d, size_t at, size_t end, const std::string& dir, int depth) {
        Span D(d);
        if (!D.has(at, 64) || !D.eq(at + 40, "_FVH", 4)) {
            // Not a volume after all: keep the bytes.
            std::vector<uint8_t> v(d.begin() + (ptrdiff_t)at, d.begin() + (ptrdiff_t)end);
            add(builtItem(dir.substr(0, dir.size() - 1) + ".bin", std::move(v), 0));
            return;
        }
        uint64_t len = D.u64(at + 32);
        end = (size_t)std::min<uint64_t>(end, at + len);
        size_t o = at + D.u16(at + 48);
        if (uint32_t ext = D.u16(at + 52)) o = std::max<size_t>(o, at + ext + D.u32(at + ext + 16));
        std::map<std::string, int> seen;
        if (!dir.empty()) add(dirItem(dir.substr(0, dir.size() - 1), 0));
        for (o = (o + 7) & ~size_t(7); o + 24 <= end; o = (o + 7) & ~size_t(7)) {
            bool empty = true;
            for (size_t k = 0; k < 24; ++k)
                if (d[o + k] != 0xFF) empty = false;
            if (empty) break;  // free space
            uint32_t type = D.u8(o + 18), attr = D.u8(o + 19);
            uint64_t size = D.u24(o + 20);
            size_t hdr = 24;
            if (attr & 0x01) size = D.u64(o + 24), hdr = 32;  // a large file
            if (size < hdr || o + size > end) break;
            size_t body = o + hdr, fend = (size_t)(o + size);
            std::string guid = guidText(d.data() + o);
            if (type != 0xF0) {  // not padding
                std::string name = uiName(d, body, fend);
                std::string folder = dir + unique(seen, name.empty() ? guid : component(name)) + "/";
                add(dirItem(folder.substr(0, folder.size() - 1), 0));
                std::map<std::string, int> fs;
                if (type == 0x01) {  // raw file: no sections
                    std::vector<uint8_t> v(d.begin() + (ptrdiff_t)body, d.begin() + (ptrdiff_t)fend);
                    std::string ext = extFor(v.data(), v.size());
                    add(builtItem(folder + "raw" + ext, std::move(v), 0));
                } else {
                    sections(d, body, fend, folder, fs, depth);
                }
            }
            o = fend;
        }
    }
};

// Where firmware volumes start: their signature 40 bytes in, on an 8-byte boundary (16 for most).
std::vector<std::pair<size_t, size_t>> findVolumes(const std::vector<uint8_t>& d) {
    std::vector<std::pair<size_t, size_t>> out;
    Span D(d);
    for (size_t o = 0; o + 64 <= d.size();) {
        if (D.eq(o + 40, "_FVH", 4)) {
            uint64_t len = D.u64(o + 32);
            uint32_t hl = D.u16(o + 48);
            if (len >= 64 && hl >= 56 && o + len <= d.size()) {
                out.push_back({o, (size_t)(o + len)});
                o += (size_t)len;
                continue;
            }
        }
        o += 8;
    }
    return out;
}

bool openUefi(Ctx& ctx, StreamPtr s) {
    if (s->size() > (256u << 20)) return false;
    std::vector<uint8_t> d = s->get(0, (size_t)s->size());
    auto vols = findVolumes(d);
    if (vols.empty()) return false;
    ctx.formats.push_back("UEFI");
    Uefi u{ctx};
    if (vols.size() == 1 && vols[0].first == 0) {
        u.volume(d, 0, vols[0].second, ctx.prefix, 0);
        return true;
    }
    for (size_t i = 0; i < vols.size(); ++i) u.volume(d, vols[i].first, vols[i].second, ctx.prefix + std::to_string(i) + "/", 0);
    return true;
}

}  // namespace

const Handler hUefi = {"UEFI",
                       [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                           Span H(h);
                           if (H.eq(40, "_FVH", 4)) return true;  // a volume
                           if (H.u32(16) == 0x0FF0A55A) return true;  // an Intel flash image (descriptor)
                           // Capsules: EFI, UEFI and AMI Aptio capsule GUIDs.
                           std::string g = h.size() >= 16 ? guidText(h.data()) : "";
                           return g == "3B6686BD-0D76-4030-B70E-B5519E2FC5A0" || g == "539182B9-ABB5-4391-B69A-E3A943F72FCC" ||
                                  g == "4A3CA68B-7723-48FB-803D-578CC1FEC44D" || g == "14EEBB0A-2E7A-4B12-A2E7-B6B4A1D2F0D8";
                       },
                       openUefi};

}  // namespace kz::native
