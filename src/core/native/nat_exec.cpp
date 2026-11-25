// Programs: Windows PE (sections, resources turned into ordinary files, signature, overlay), UEFI TE,
// ELF, Mach-O and macOS universal binaries, COFF objects.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>

#include "../kz_sniff.hpp"
#include "nat.hpp"

namespace kz::native {

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

struct Section {
    std::string name;
    uint32_t va = 0, vsize = 0, raw = 0, ptr = 0;
};

struct Res {
    uint32_t type = 0, id = 0, lang = 0;  // type/id 0 when named
    std::string typeName, name;
    uint32_t rva = 0, size = 0;
};

const char* resTypeLabel(uint32_t t) {
    switch (t) {
        case 1: return "CURSOR";
        case 2: return "BITMAP";
        case 3: return "ICON";
        case 4: return "MENU";
        case 5: return "DIALOG";
        case 6: return "STRING";
        case 7: return "FONTDIR";
        case 8: return "FONT";
        case 9: return "ACCELERATOR";
        case 10: return "RCDATA";
        case 11: return "MESSAGETABLE";
        case 12: return "GROUP_CURSOR";
        case 14: return "GROUP_ICON";
        case 16: return "VERSION";
        case 17: return "DLGINCLUDE";
        case 19: return "PLUGPLAY";
        case 20: return "VXD";
        case 21: return "ANICURSOR";
        case 22: return "ANIICON";
        case 23: return "HTML";
        case 24: return "MANIFEST";
        default: return nullptr;
    }
}

class Pe {
public:
    Pe(StreamPtr sp) : sp_(sp), b_(*sp) {}

    bool open(std::vector<Item>& outItems, std::string& format, std::string& err) {
        std::vector<uint8_t> head = b_.getUpTo(0, 64 * 1024);
        Span H(head);
        size_t pe = H.u32(0x3C);
        if (H.u16(0) != 0x5A4D || !H.has(pe, 24) || std::memcmp(head.data() + pe, "PE\0\0", 4) != 0) {
            err = "Not a Windows program (no PE header)";
            return false;
        }
        uint32_t machine = H.u16(pe + 4), nsec = H.u16(pe + 6), optSize = H.u16(pe + 20), chars = H.u16(pe + 22);
        stamp_ = H.u32(pe + 8);
        // Reproducible builds put a hash here instead of the build time: a "date" in the future.
        if ((int64_t)stamp_ > (int64_t)std::time(nullptr) + 86400) stamp_ = 0;
        size_t opt = pe + 24;
        uint32_t magic = H.u16(opt);
        bool plus = magic == 0x20B;
        if (magic != 0x10B && !plus) {
            err = "Unsupported Windows program header";
            return false;
        }
        uint32_t subsystem = H.u16(opt + 68);
        size_t ddBase = opt + (plus ? 112 : 96);
        uint32_t nDd = H.u32(opt + (plus ? 108 : 92));
        auto dataDir = [&](uint32_t i, uint32_t& rva, uint32_t& size) {
            rva = size = 0;
            if (i < nDd && ddBase + i * 8 + 8 <= opt + optSize) {
                rva = H.u32(ddBase + i * 8);
                size = H.u32(ddBase + i * 8 + 4);
            }
        };
        size_t table = opt + optSize;
        if (!H.has(table, (size_t)nsec * 40)) {
            err = "Damaged Windows program (section table cut short)";
            return false;
        }
        uint64_t imageEnd = 0;
        for (uint32_t i = 0; i < nsec; ++i) {
            size_t s = table + i * 40;
            Section sec;
            size_t len = 0;
            while (len < 8 && head[s + len]) ++len;
            sec.name = std::string((const char*)head.data() + s, len);
            sec.vsize = H.u32(s + 8);
            sec.va = H.u32(s + 12);
            sec.raw = H.u32(s + 16);
            sec.ptr = H.u32(s + 20);
            if (sec.ptr >= b_.size()) sec.raw = 0;  // nothing of it in the file
            else sec.raw = (uint32_t)std::min<uint64_t>(sec.raw, b_.size() - sec.ptr);
            if (sec.raw) imageEnd = std::max<uint64_t>(imageEnd, (uint64_t)sec.ptr + sec.raw);
            secs_.push_back(sec);
        }

        // What it is, for the status line.
        std::string arch = machine == 0x8664 ? "x64" : machine == 0x14C ? "x86" : machine == 0xAA64 ? "ARM64"
                         : machine == 0x1C4 ? "ARM" : machine == 0x200 ? "Itanium" : "machine 0x" + hex(machine);
        std::string kind = (subsystem >= 10 && subsystem <= 13) ? "UEFI program"
                         : subsystem == 1                       ? "Windows driver"
                         : (chars & 0x2000)                     ? "Windows library"
                                                                : "Windows program";
        format = kind + " (" + (plus ? "PE32+" : "PE32") + ", " + arch + ")";

        // Resources, turned into files.
        uint32_t resRva, resSize;
        dataDir(2, resRva, resSize);
        int rsrcSec = -1;
        std::vector<Item> res;
        if (resRva && resSize) {
            for (size_t i = 0; i < secs_.size(); ++i)
                if (resRva >= secs_[i].va && resRva - secs_[i].va < std::max(secs_[i].vsize, secs_[i].raw)) rsrcSec = (int)i;
            if (rsrcSec >= 0) resources(secs_[(size_t)rsrcSec], resRva, res);
        }

        // Sections (the resource section as its files when it could be read).
        std::set<std::string> used;
        for (size_t i = 0; i < secs_.size(); ++i) {
            const Section& s = secs_[i];
            if (!s.raw) continue;
            if ((int)i == rsrcSec && !res.empty()) continue;
            std::string name = component(s.name.empty() ? "section" + std::to_string(i + 1) : s.name);
            std::string unique = name;
            for (int k = 2; used.count(unique); ++k) unique = name + "_" + std::to_string(k);
            used.insert(unique);
            outItems.push_back(rangeItem(unique, sp_, s.ptr, s.raw, stamp_));
        }
        for (auto& r : res) outItems.push_back(std::move(r));

        // The signature, and anything appended after the program (an installer's payload).
        uint32_t certOff, certSize;
        dataDir(4, certOff, certSize);  // a file offset here, not an address
        bool cert = certOff && certSize && certOff >= imageEnd && certOff < b_.size() &&
                    certSize <= b_.size() - certOff;
        if (cert) {
            int n = 0;
            for (uint64_t at = certOff; at + 8 <= (uint64_t)certOff + certSize;) {
                std::vector<uint8_t> h = b_.get(at, 8);
                if (h.empty()) break;
                uint32_t len = Span(h).u32(0), type = Span(h).u16(6);
                if (len < 8 || at + len > (uint64_t)certOff + certSize) break;
                std::string name = "CERTIFICATE" + (n ? "_" + std::to_string(n + 1) : std::string());
                // PKCS #7 signed data opens in Windows' certificate viewer as a .p7b.
                if (type == 2) outItems.push_back(rangeItem(name + ".p7b", sp_, at + 8, len - 8, stamp_));
                else outItems.push_back(rangeItem(name + ".bin", sp_, at, len, stamp_));
                ++n;
                at += (len + 7) & ~7ull;
            }
        }
        uint64_t overlayEnd = cert && (uint64_t)certOff + certSize >= b_.size() ? certOff : b_.size();
        if (imageEnd && overlayEnd > imageEnd) {
            std::vector<uint8_t> h = b_.getUpTo(imageEnd, kSniffBytes);
            std::string ext = extFor(h.data(), h.size());
            if (h.size() >= 20 && std::memcmp(h.data() + 4, "\xEF\xBE\xAD\xDENullsoftInst", 16) == 0) ext = ".nsis";
            outItems.push_back(rangeItem("[overlay]" + ext, sp_, imageEnd, overlayEnd - imageEnd, stamp_));
        }
        if (outItems.empty()) {
            err = "This program holds no data to show";
            return false;
        }
        return true;
    }

private:
    StreamPtr sp_;
    Stream& b_;
    std::vector<Section> secs_;
    uint32_t stamp_ = 0;

    bool rvaToOff(uint32_t rva, uint32_t len, uint64_t& off) const {
        for (auto& s : secs_) {
            uint32_t span = std::max(s.vsize, s.raw);
            if (rva < s.va || rva - s.va >= span) continue;
            uint64_t d = rva - s.va;
            if (d + len > s.raw) return false;
            off = (uint64_t)s.ptr + d;
            return true;
        }
        return false;
    }

    std::vector<uint8_t> resData(const Res& r) {
        uint64_t off;
        if (!rvaToOff(r.rva, r.size, off)) return {};
        return b_.get(off, r.size);
    }

    // Walks the resource tree (type / name / language) and turns what it finds into files.
    void resources(const Section& sec, uint32_t resRva, std::vector<Item>& out) {
        uint64_t base = (uint64_t)sec.ptr + (resRva - sec.va);
        if (resRva - sec.va >= sec.raw) return;
        std::vector<uint8_t> tree = b_.get(base, (size_t)std::min<uint64_t>(sec.raw - (resRva - sec.va), 256ull << 20));
        Span R(tree);
        auto entries = [&](uint32_t off) {
            std::vector<std::pair<uint32_t, uint32_t>> e;
            if (!R.has(off, 16)) return e;
            uint32_t n = R.u16(off + 12) + R.u16(off + 14);
            for (uint32_t i = 0; i < n && i < 65536; ++i) {
                size_t at = off + 16 + (size_t)i * 8;
                if (!R.has(at, 8)) break;
                e.emplace_back(R.u32(at), R.u32(at + 4));
            }
            return e;
        };
        auto nameOf = [&](uint32_t v, uint32_t& id, std::string& name) {
            if (v & 0x80000000u) {
                uint32_t o = v & 0x7FFFFFFF;
                uint32_t len = R.u16(o);
                if (R.has(o + 2, (size_t)len * 2)) name = utf16le(R.p + o + 2, len);
                if (name.empty()) name = "_";
            } else {
                id = v & 0xFFFF;
            }
        };
        std::vector<Res> all;
        for (auto& [tv, to] : entries(0)) {
            if (!(to & 0x80000000u)) continue;
            Res r;
            nameOf(tv, r.type, r.typeName);
            for (auto& [nv, no] : entries(to & 0x7FFFFFFF)) {
                if (!(no & 0x80000000u)) continue;
                r.id = 0;
                r.name.clear();
                nameOf(nv, r.id, r.name);
                for (auto& [lv, lo] : entries(no & 0x7FFFFFFF)) {
                    if (lo & 0x80000000u) continue;  // a fourth level: not valid
                    if (!R.has(lo, 16)) continue;
                    r.lang = lv & 0xFFFF;
                    r.rva = R.u32(lo);
                    r.size = R.u32(lo + 4);
                    all.push_back(r);
                    if (all.size() > 200000) return;  // damaged: endless tree
                }
            }
        }

        // Several languages of one resource get the language in their names.
        std::map<std::string, int> langs;
        auto key = [](const Res& r) {
            return (r.typeName.empty() ? std::to_string(r.type) : "n" + r.typeName) + "/" +
                   (r.name.empty() ? std::to_string(r.id) : "n" + r.name);
        };
        for (auto& r : all) langs[key(r)]++;
        auto pathOf = [&](const Res& r, const std::string& ext) {
            std::string type = r.typeName.empty() ? (resTypeLabel(r.type) ? resTypeLabel(r.type) : std::to_string(r.type))
                                                  : component(r.typeName);
            std::string name = r.name.empty() ? std::to_string(r.id) : component(r.name);
            if (langs[key(r)] > 1) name += "_" + std::to_string(r.lang);
            return ".rsrc/" + type + "/" + name + ext;
        };

        int64_t t = stamp_;
        for (auto& r : all) {
            uint64_t off;
            if (!r.size || !rvaToOff(r.rva, r.size, off)) continue;
            if (r.typeName.empty()) {
                switch (r.type) {
                    case 3:  // ICON: a single image; as an .ico of its own
                    case 1: {  // CURSOR: as a .cur
                        std::vector<uint8_t> d = resData(r);
                        std::vector<uint8_t> f = r.type == 3 ? iconFile({d}, {}) : cursorFile(d);
                        if (!f.empty()) {
                            out.push_back(builtItem(pathOf(r, r.type == 3 ? ".ico" : ".cur"), std::move(f), t));
                            continue;
                        }
                        break;
                    }
                    case 14: {  // GROUP_ICON: the whole icon (every size) as one .ico, like the program's icon
                        std::vector<uint8_t> f = groupIcon(r, all);
                        if (!f.empty()) {
                            out.push_back(builtItem(pathOf(r, ".ico"), std::move(f), t));
                            continue;
                        }
                        break;
                    }
                    case 2: {  // BITMAP: a .bmp needs the file header back
                        std::vector<uint8_t> d = resData(r);
                        std::vector<uint8_t> f = bitmapFile(d);
                        if (!f.empty()) {
                            out.push_back(builtItem(pathOf(r, ".bmp"), std::move(f), t));
                            continue;
                        }
                        break;
                    }
                    case 6: {  // STRING: a table of 16 strings
                        std::string s = stringTable(r);
                        if (!s.empty()) {
                            out.push_back(builtItem(pathOf(r, ".txt"), textFile(s), t));
                            continue;
                        }
                        break;
                    }
                    case 16: {  // VERSION: as readable text
                        std::string s = versionText(resData(r));
                        if (!s.empty()) {
                            out.push_back(builtItem(pathOf(r, ".txt"), textFile(s), t));
                            continue;
                        }
                        break;
                    }
                    case 23: out.push_back(rangeItem(pathOf(r, ".html"), sp_, off, r.size, t)); continue;
                    case 24: out.push_back(rangeItem(pathOf(r, ".xml"), sp_, off, r.size, t)); continue;
                    default: break;
                }
            }
            std::vector<uint8_t> h = b_.getUpTo(off, std::min<uint32_t>(r.size, 64));
            out.push_back(rangeItem(pathOf(r, extFor(h.data(), h.size())), sp_, off, r.size, t));
        }
    }

    // Width, height and bit depth of an icon image (a DIB, or a PNG for big ones).
    static void imageInfo(const std::vector<uint8_t>& d, uint32_t& w, uint32_t& h, uint32_t& bpp) {
        w = h = bpp = 0;
        if (d.size() >= 24 && std::memcmp(d.data(), "\x89PNG", 4) == 0) {
            w = be32(d.data() + 16);
            h = be32(d.data() + 20);
            bpp = 32;
        } else if (d.size() >= 16) {
            Span s(d);
            w = s.u32(4);
            h = s.u32(8) / 2;  // a DIB icon's height counts its mask too
            bpp = s.u16(14);
        }
    }

    // An .ico (or .cur with hotspots) from images.
    static std::vector<uint8_t> iconFile(const std::vector<std::vector<uint8_t>>& imgs,
                                         const std::vector<std::pair<uint32_t, uint32_t>>& hotspots) {
        bool cur = !hotspots.empty();
        std::vector<uint8_t> f;
        put16(f, 0);
        put16(f, cur ? 2 : 1);
        put16(f, (uint32_t)imgs.size());
        uint32_t at = 6 + 16 * (uint32_t)imgs.size();
        for (size_t i = 0; i < imgs.size(); ++i) {
            uint32_t w, h, bpp;
            imageInfo(imgs[i], w, h, bpp);
            if (!w || !h) return {};
            f.push_back(uint8_t(w >= 256 ? 0 : w));
            f.push_back(uint8_t(h >= 256 ? 0 : h));
            f.push_back(uint8_t(bpp && bpp < 8 ? 1u << bpp : 0));
            f.push_back(0);
            put16(f, cur ? hotspots[i].first : 1);
            put16(f, cur ? hotspots[i].second : bpp);
            put32(f, (uint32_t)imgs[i].size());
            put32(f, at);
            at += (uint32_t)imgs[i].size();
        }
        for (auto& d : imgs) f.insert(f.end(), d.begin(), d.end());
        return f;
    }

    // A cursor resource is its hotspot followed by the image.
    static std::vector<uint8_t> cursorFile(const std::vector<uint8_t>& d) {
        if (d.size() < 4 + 16) return {};
        Span s(d);
        return iconFile({std::vector<uint8_t>(d.begin() + 4, d.end())}, {{s.u16(0), s.u16(2)}});
    }

    // GROUP_ICON lists the ICON resources (by id) that make up one icon.
    std::vector<uint8_t> groupIcon(const Res& g, const std::vector<Res>& all) {
        std::vector<uint8_t> d = resData(g);
        Span s(d);
        uint32_t n = s.u16(4);
        if (s.u16(2) != 1 || !n || !s.has(6, (size_t)n * 14)) return {};
        std::vector<std::vector<uint8_t>> imgs;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t id = s.u16(6 + i * 14 + 12);
            const Res* best = nullptr;
            for (auto& r : all)
                if (r.typeName.empty() && r.type == 3 && r.name.empty() && r.id == id && (!best || r.lang == g.lang)) best = &r;
            if (!best) continue;
            std::vector<uint8_t> img = resData(*best);
            if (!img.empty()) imgs.push_back(std::move(img));
        }
        if (imgs.empty()) return {};
        return iconFile(imgs, {});
    }

    static std::vector<uint8_t> bitmapFile(const std::vector<uint8_t>& d) {
        Span s(d);
        uint32_t hs = s.u32(0);
        if (hs < 12 || hs > d.size()) return {};
        uint32_t bpp, palette, entry = 4, masks = 0;
        if (hs == 12) {  // OS/2 BITMAPCOREHEADER
            bpp = s.u16(10);
            palette = bpp <= 8 ? 1u << bpp : 0;
            entry = 3;
        } else {
            bpp = s.u16(14);
            uint32_t comp = s.u32(16), used = s.u32(32);
            palette = used ? used : (bpp <= 8 ? 1u << bpp : 0);
            if (hs == 40 && comp == 3) masks = 12;  // BI_BITFIELDS: colour masks follow the header
            if (hs == 40 && comp == 6) masks = 16;  // BI_ALPHABITFIELDS
        }
        if (palette > 65536) return {};
        uint32_t offBits = 14 + hs + masks + palette * entry;
        std::vector<uint8_t> f;
        f.reserve(14 + d.size());
        f.push_back('B');
        f.push_back('M');
        put32(f, (uint32_t)(14 + d.size()));
        put32(f, 0);
        put32(f, offBits);
        f.insert(f.end(), d.begin(), d.end());
        return f;
    }

    // A string table resource: 16 strings, each a length and UTF-16 text; ids are (block - 1) * 16 + i.
    std::string stringTable(const Res& r) {
        if (!r.name.empty() || r.id == 0) return {};
        std::vector<uint8_t> d = resData(r);
        Span s(d);
        std::string out;
        size_t at = 0;
        for (uint32_t i = 0; i < 16 && s.has(at, 2); ++i) {
            uint32_t len = s.u16(at);
            at += 2;
            if (!s.has(at, (size_t)len * 2)) break;
            if (len) {
                std::string text = utf16le(d.data() + at, len);
                std::string esc;
                for (char c : text) {
                    if (c == '\n') esc += "\\n";
                    else if (c == '\r') esc += "\\r";
                    else if (c == '\t') esc += "\\t";
                    else esc += c;
                }
                out += std::to_string((r.id - 1) * 16 + i) + "\t" + esc + "\r\n";
            }
            at += (size_t)len * 2;
        }
        return out;
    }

    // VS_VERSIONINFO as "Key: value" lines (the fixed version numbers and every string table).
    static std::string versionText(const std::vector<uint8_t>& d) {
        Span v(d);
        std::string out;
        auto align = [](size_t x) { return (x + 3) & ~size_t(3); };
        auto ver = [](uint32_t ms, uint32_t ls) {
            return std::to_string(ms >> 16) + "." + std::to_string(ms & 0xFFFF) + "." + std::to_string(ls >> 16) + "." +
                   std::to_string(ls & 0xFFFF);
        };
        // Each node: length, value length, type, key, value, children. Only the parts that hold text
        // are walked: VS_VERSION_INFO > StringFileInfo > (one table per language) > strings.
        std::vector<std::string> path;
        std::function<void(size_t, size_t)> nodes = [&](size_t off, size_t lim) {
            while (off + 6 <= lim && path.size() < 4) {
                size_t len = v.u16(off), vlen = v.u16(off + 2), type = v.u16(off + 4);
                if (len < 6 || off + len > lim) return;
                size_t k = off + 6, kc = 0;
                while (k + kc * 2 + 2 <= off + len && v.u16(k + kc * 2)) ++kc;
                std::string key = utf16le(d.data() + k, kc);
                size_t val = align(k + (kc + 1) * 2);
                size_t vbytes = type == 1 ? vlen * 2 : vlen;
                size_t depth = path.size();
                if (depth == 0 && vlen >= 52 && v.u32(val) == 0xFEEF04BD) {
                    out += "FileVersion (fixed): " + ver(v.u32(val + 8), v.u32(val + 12)) + "\r\n";
                    out += "ProductVersion (fixed): " + ver(v.u32(val + 16), v.u32(val + 20)) + "\r\n";
                } else if (depth == 2 && path[1] == "StringFileInfo") {
                    out += "\r\n[" + key + "]\r\n";
                } else if (depth == 3 && path[1] == "StringFileInfo") {
                    size_t chars = std::min(vlen, (off + len > val ? (off + len - val) / 2 : 0));
                    std::string text = v.has(val, chars * 2) ? utf16le(d.data() + val, chars) : "";
                    while (!text.empty() && text.back() == '\0') text.pop_back();
                    out += key + ": " + text + "\r\n";
                }
                bool descend = depth == 0 || (depth == 1 && key == "StringFileInfo") || (depth == 2 && path[1] == "StringFileInfo");
                if (descend) {
                    path.push_back(key);
                    nodes(align(val + vbytes), off + len);
                    path.pop_back();
                }
                off = align(off + len);
            }
        };
        nodes(0, d.size());
        return out;
    }
};


// Adds a handler's items under ctx.prefix.
void emit(Ctx& ctx, std::vector<Item>& items) {
    for (auto& it : items) {
        it.path = ctx.prefix + it.path;
        ctx.out.items.push_back(std::move(it));
    }
}

// A unique name among those already used ("name", "name_2"...).
std::string unique(std::set<std::string>& used, std::string name) {
    std::string u = name;
    for (int k = 2; used.count(u); ++k) u = name + "_" + std::to_string(k);
    used.insert(u);
    return u;
}

// =============================================================================================== TE
// Terse Executable (UEFI): a PE with most of its header stripped.
bool openTe(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 40 + 40 * 96);
    Span H(h);
    uint32_t nsec = H.u8(4), stripped = H.u16(6);
    if (!H.has(40, (size_t)nsec * 40) || nsec == 0) return false;
    std::vector<Item> items;
    std::set<std::string> used;
    for (uint32_t i = 0; i < nsec; ++i) {
        size_t o = 40 + i * 40;
        uint32_t raw = H.u32(o + 16), ptr = H.u32(o + 20);
        if (!raw || ptr < stripped) continue;
        uint64_t at = (uint64_t)ptr - stripped + 40;
        if (at >= s->size()) continue;
        raw = (uint32_t)std::min<uint64_t>(raw, s->size() - at);
        std::string name = component(cstr(h.data() + o, 8));
        items.push_back(rangeItem(unique(used, name), s, at, raw, 0));
    }
    if (items.empty()) return false;
    emit(ctx, items);
    ctx.formats.push_back("TE");
    return true;
}

// ============================================================================================== ELF
const char* elfMachine(uint32_t m) {
    switch (m) {
        case 3: return "x86";
        case 62: return "x86-64";
        case 40: return "ARM";
        case 183: return "ARM64";
        case 243: return "RISC-V";
        case 8: return "MIPS";
        case 20: return "PowerPC";
        case 21: return "PowerPC64";
        case 2: return "SPARC";
        case 43: return "SPARC64";
        case 22: return "S390";
        case 258: return "LoongArch";
        default: return nullptr;
    }
}

bool openElf(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 64);
    bool is64 = h.size() > 5 && h[4] == 2, be = h.size() > 5 && h[5] == 2;
    Span H(h);
    auto u16 = [&](const Span& x, size_t o) { return be ? x.b16(o) : x.u16(o); };
    auto u32 = [&](const Span& x, size_t o) { return be ? x.b32(o) : x.u32(o); };
    auto u64 = [&](const Span& x, size_t o) { return be ? x.b64(o) : x.u64(o); };
    auto word = [&](const Span& x, size_t o) { return is64 ? u64(x, o) : (uint64_t)u32(x, o); };
    uint32_t type = u16(H, 16), machine = u16(H, 18);
    uint64_t shoff = word(H, is64 ? 40 : 32), phoff = word(H, is64 ? 32 : 28);
    uint32_t shentsize = u16(H, is64 ? 58 : 46), shnum = u16(H, is64 ? 60 : 48), shstrndx = u16(H, is64 ? 62 : 50);
    uint32_t phentsize = u16(H, is64 ? 54 : 42), phnum = u16(H, is64 ? 56 : 44);
    std::vector<Item> items;
    std::set<std::string> used;
    uint64_t size = s->size();
    if (shoff && shnum && shentsize >= (is64 ? 64u : 40u) && shnum < 65536) {
        std::vector<uint8_t> sh = s->getUpTo(shoff, (size_t)shnum * shentsize);
        Span S(sh);
        std::vector<uint8_t> names;
        if (shstrndx < shnum) {
            size_t o = (size_t)shstrndx * shentsize;
            uint64_t noff = word(S, o + (is64 ? 24 : 16)), nsz = word(S, o + (is64 ? 32 : 20));
            if (noff < size && nsz < (64u << 20)) names = s->getUpTo(noff, (size_t)nsz);
        }
        for (uint32_t i = 1; i < shnum; ++i) {
            size_t o = (size_t)i * shentsize;
            if (!S.has(o, shentsize)) break;
            uint32_t nm = u32(S, o), stype = u32(S, o + 4);
            uint64_t off = word(S, o + (is64 ? 24 : 16)), sz = word(S, o + (is64 ? 32 : 20));
            if (stype == 8 /* NOBITS */ || !sz || off >= size) continue;
            sz = std::min(sz, size - off);
            std::string name = nm < names.size() ? cstr(names.data() + nm, names.size() - nm) : "";
            if (name.empty()) name = "section" + std::to_string(i);
            items.push_back(rangeItem(unique(used, component(name)), s, off, sz, 0));
        }
    }
    if (items.empty() && phoff && phnum && phentsize >= (is64 ? 56u : 32u)) {  // no section table: the segments
        std::vector<uint8_t> ph = s->getUpTo(phoff, (size_t)phnum * phentsize);
        Span P(ph);
        for (uint32_t i = 0; i < phnum; ++i) {
            size_t o = (size_t)i * phentsize;
            if (!P.has(o, phentsize)) break;
            uint64_t off = is64 ? u64(P, o + 8) : u32(P, o + 4), sz = is64 ? u64(P, o + 32) : u32(P, o + 16);
            if (!sz || off >= size) continue;
            items.push_back(rangeItem("segment" + std::to_string(i), s, off, std::min(sz, size - off), 0));
        }
    }
    if (items.empty()) return false;
    emit(ctx, items);
    const char* m = elfMachine(machine);
    const char* kind = type == 1 ? "object" : type == 2 ? "program" : type == 3 ? "shared library" : type == 4 ? "core dump" : "file";
    ctx.formats.push_back(std::string("ELF ") + kind + " (" + (m ? m : "machine " + std::to_string(machine)) + ", " +
                          (is64 ? "64-bit" : "32-bit") + ")");
    return true;
}

// ========================================================================================== Mach-O
std::string machCpu(uint32_t cpu, uint32_t sub) {
    switch (cpu) {
        case 7: return "x86";
        case 0x01000007: return (sub & 0xFF) == 8 ? "x86_64h" : "x86_64";
        case 12: return "arm";
        case 0x0100000C: return (sub & 0xFF) == 2 ? "arm64e" : "arm64";
        case 0x0200000C: return "arm64_32";
        case 18: return "ppc";
        case 0x01000012: return "ppc64";
        default: return "";
    }
}

bool openMachO(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 32);
    Span H(h);
    uint32_t magic = H.u32(0);
    bool be = magic == 0xCEFAEDFE || magic == 0xCFFAEDFE;
    bool is64 = magic == 0xFEEDFACF || magic == 0xCFFAEDFE;
    auto u32 = [&](const Span& x, size_t o) { return be ? x.b32(o) : x.u32(o); };
    auto u64 = [&](const Span& x, size_t o) { return be ? x.b64(o) : x.u64(o); };
    uint32_t cpu = u32(H, 4), sub = u32(H, 8), ftype = u32(H, 12), ncmds = u32(H, 16), cmdsSize = u32(H, 20);
    if (ncmds > 65536 || cmdsSize > (64u << 20)) return false;
    size_t base = is64 ? 32 : 28;
    std::vector<uint8_t> cmds = s->getUpTo(base, cmdsSize);
    Span C(cmds);
    std::vector<Item> items;
    std::set<std::string> used;
    uint64_t size = s->size();
    size_t at = 0;
    for (uint32_t i = 0; i < ncmds && C.has(at, 8); ++i) {
        uint32_t cmd = u32(C, at), len = u32(C, at + 4);
        if (len < 8 || !C.has(at, len)) break;
        if (cmd == 0x1 || cmd == 0x19) {  // LC_SEGMENT / LC_SEGMENT_64
            bool s64 = cmd == 0x19;
            std::string seg = component(cstr(cmds.data() + at + 8, 16));
            uint64_t fileoff = s64 ? u64(C, at + 40) : u32(C, at + 32), filesize = s64 ? u64(C, at + 48) : u32(C, at + 36);
            uint32_t nsects = u32(C, at + (s64 ? 64 : 48));
            size_t sectBase = at + (s64 ? 72 : 56), sectSize = s64 ? 80 : 68;
            bool any = false;
            for (uint32_t k = 0; k < nsects && k < 4096; ++k) {
                size_t o = sectBase + k * sectSize;
                if (!C.has(o, sectSize)) break;
                std::string sect = component(cstr(cmds.data() + o, 16));
                uint64_t ssz = s64 ? u64(C, o + 40) : u32(C, o + 36);
                uint32_t off = u32(C, o + (s64 ? 48 : 40)), flags = u32(C, o + (s64 ? 64 : 56));
                uint32_t stype = flags & 0xFF;
                if (stype == 1 || stype == 0xC || stype == 0x12 || !off || !ssz || off >= size) continue;  // zero-filled
                items.push_back(rangeItem(unique(used, seg + "/" + sect), s, off, std::min<uint64_t>(ssz, size - off), 0));
                any = true;
            }
            if (!any && filesize && fileoff < size)
                items.push_back(rangeItem(unique(used, seg), s, fileoff, std::min(filesize, size - fileoff), 0));
        }
        at += len;
    }
    if (items.empty()) return false;
    emit(ctx, items);
    std::string arch = machCpu(cpu, sub);
    const char* kind = ftype == 1 ? "object" : ftype == 2 ? "program" : ftype == 6 ? "library" : ftype == 8 ? "bundle" : "file";
    ctx.formats.push_back(std::string("Mach-O ") + kind + " (" + (arch.empty() ? "cpu " + std::to_string(cpu) : arch) + ")");
    return true;
}

// A macOS universal ("fat") binary: one Mach-O per architecture, each listed as a file.
bool openMub(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 8 + 64 * 32);
    Span H(h);
    bool fat64 = H.b32(0) == 0xCAFEBABF;
    uint32_t n = H.b32(4);
    size_t es = fat64 ? 32 : 20;
    if (n == 0 || n > 64 || !H.has(8, n * es)) return false;
    std::vector<Item> items;
    std::set<std::string> used;
    for (uint32_t i = 0; i < n; ++i) {
        size_t o = 8 + i * es;
        uint32_t cpu = H.b32(o), sub = H.b32(o + 4);
        uint64_t off = fat64 ? H.b64(o + 8) : H.b32(o + 8), sz = fat64 ? H.b64(o + 16) : H.b32(o + 12);
        std::string arch = machCpu(cpu, sub);
        // Java class files share the magic number: theirs aren't believable architecture tables.
        if (arch.empty() || !sz || off + sz > s->size()) return false;
        items.push_back(rangeItem(unique(used, arch), s, off, sz, 0));
    }
    emit(ctx, items);
    ctx.formats.push_back("Universal binary");
    return true;
}

// ============================================================================================= COFF
// An object file (.obj): sections with no PE header in front.
bool openCoff(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 20);
    Span H(h);
    uint32_t nsec = H.u16(2), symPtr = H.u32(8), nsym = H.u32(12), opt = H.u16(16);
    if (!nsec || nsec > 65279 || opt != 0) return false;
    std::vector<uint8_t> tab = s->getUpTo(20, (size_t)nsec * 40);
    Span T(tab);
    if (!T.has(0, (size_t)nsec * 40)) return false;
    std::vector<uint8_t> strtab;  // long names ("/123") live after the symbol table
    if (symPtr) {
        uint64_t st = (uint64_t)symPtr + (uint64_t)nsym * 18;
        std::vector<uint8_t> len = s->getUpTo(st, 4);
        if (len.size() == 4) strtab = s->getUpTo(st, std::min<uint32_t>(Span(len).u32(0), 64u << 20));
    }
    std::vector<Item> items;
    std::set<std::string> used;
    for (uint32_t i = 0; i < nsec; ++i) {
        size_t o = (size_t)i * 40;
        std::string name = cstr(tab.data() + o, 8);
        if (name.size() > 1 && name[0] == '/') {
            size_t at = (size_t)std::strtoul(name.c_str() + 1, nullptr, 10);
            if (at < strtab.size()) name = cstr(strtab.data() + at, strtab.size() - at);
        }
        uint32_t raw = T.u32(o + 16), ptr = T.u32(o + 20);
        if (!raw || !ptr || ptr >= s->size()) continue;
        items.push_back(rangeItem(unique(used, component(name)), s, ptr, std::min<uint64_t>(raw, s->size() - ptr), 0));
    }
    if (items.empty()) return false;
    emit(ctx, items);
    ctx.formats.push_back("COFF object");
    return true;
}

bool knownCoffMachine(uint32_t m) {
    return m == 0x14C || m == 0x8664 || m == 0xAA64 || m == 0x1C4 || m == 0x200 || m == 0xA641 || m == 0xA64E;
}

bool endsWith(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)s[s.size() - n + i]) != suf[i]) return false;
    return true;
}

}  // namespace

const Handler hPe = {
    "PE",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) { return h.size() >= 64 && h[0] == 'M' && h[1] == 'Z'; },
    [](Ctx& ctx, StreamPtr s) {
        std::vector<Item> items;
        std::string format;
        if (!Pe(s).open(items, format, ctx.err)) return false;
        emit(ctx, items);
        ctx.formats.push_back(format);
        return true;
    },
};

const Handler hTe = {
    "TE",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
        return h.size() >= 40 && h[0] == 'V' && h[1] == 'Z' && h[4] > 0 && h[4] < 96;
    },
    openTe,
};

const Handler hElf = {
    "ELF",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
        return h.size() >= 52 && std::memcmp(h.data(), "\x7F" "ELF", 4) == 0 && (h[4] == 1 || h[4] == 2) && (h[5] == 1 || h[5] == 2);
    },
    openElf,
};

const Handler hMachO = {
    "MachO",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
        uint32_t m = Span(h).u32(0);
        return m == 0xFEEDFACE || m == 0xFEEDFACF || m == 0xCEFAEDFE || m == 0xCFFAEDFE;
    },
    openMachO,
};

const Handler hMub = {
    "Mub",
    [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
        uint32_t m = Span(h).b32(0);
        return m == 0xCAFEBABE || m == 0xCAFEBABF;
    },
    openMub,
};

const Handler hCoff = {
    "COFF",
    [](Stream&, const std::vector<uint8_t>& h, const std::string& name) {
        return endsWith(name, ".obj") && h.size() >= 20 && knownCoffMachine(Span(h).u16(0));
    },
    openCoff,
};

}  // namespace kz::native
