// NSIS installers (Nullsoft): a "first header" after the setup program, then either one compressed
// stream holding the script header and every file (solid), or a compressed header followed by files
// compressed one by one. Paths come from the script's File and SetOutPath commands, with NSIS
// variables written out ($PLUGINSDIR\System.dll); files for $INSTDIR are listed at the top level,
// as 7-Zip does.
#include <zlib.h>

#include <lzma.h>

#include <algorithm>
#include <map>
#include <set>

#include "nat.hpp"

namespace kz::native {

namespace {

enum Method { kCopy, kDeflate, kLzma, kBzip2 };

bool isLzmaProps(const uint8_t* p) {
    uint32_t dict = Span(p + 1, 4).u32(0);
    return p[0] == 0x5D && dict >= (1u << 12) && dict <= (1u << 30);
}

// Decompresses front to back; going backwards starts it over.
class Unpack {
public:
    Unpack(StreamPtr src, uint64_t off, uint64_t len, Method m) : src_(std::move(src)), off_(off), len_(len), m_(m) {}
    ~Unpack() { close(); }

    uint64_t pos() const { return out_; }
    bool seek(uint64_t to, std::string& err) {
        if (to < out_ || !open_) {
            close();
            if (!start(err)) return false;
        }
        uint8_t junk[65536];
        while (out_ < to) {
            size_t n = (size_t)std::min<uint64_t>(sizeof junk, to - out_);
            if (read(junk, n, err) != n) {
                if (err.empty()) err = "Damaged installer data";
                return false;
            }
        }
        return true;
    }
    // Up to n bytes (fewer only at the end of the data).
    size_t read(uint8_t* dst, size_t n, std::string& err) {
        if (!open_ && !start(err)) return 0;
        size_t got = 0;
        while (got < n && !ended_) {
            if (inLeft_ == 0 && inPos_ < len_) {
                size_t k = (size_t)std::min<uint64_t>(in_.size(), len_ - inPos_);
                if (!src_->readAt(off_ + inPos_, in_.data(), k)) {
                    err = "Can't read the installer";
                    return got;
                }
                inPos_ += k;
                inNext_ = in_.data();
                inLeft_ = k;
            }
            bool last = inPos_ >= len_;
            size_t before = got;
            if (m_ == kCopy) {
                size_t k = std::min(n - got, inLeft_);
                std::memcpy(dst + got, inNext_, k);
                inNext_ += k, inLeft_ -= k, got += k;
                if (last && !inLeft_) ended_ = true;
            } else if (m_ == kDeflate) {
                z_.next_in = const_cast<Bytef*>(inNext_);
                z_.avail_in = (uInt)inLeft_;
                z_.next_out = dst + got;
                z_.avail_out = (uInt)(n - got);
                int r = inflate(&z_, Z_NO_FLUSH);
                got = n - z_.avail_out;
                inLeft_ = z_.avail_in;
                inNext_ = z_.next_in;
                if (r == Z_STREAM_END || (r != Z_OK && r != Z_BUF_ERROR)) {
                    ended_ = true;
                    if (r != Z_STREAM_END && got == before) err = "Damaged installer data";
                }
            } else {
                lz_.next_in = inNext_;
                lz_.avail_in = inLeft_;
                lz_.next_out = dst + got;
                lz_.avail_out = n - got;
                lzma_ret r = lzma_code(&lz_, LZMA_RUN);
                got = n - lz_.avail_out;
                inLeft_ = lz_.avail_in;
                inNext_ = lz_.next_in;
                if (r != LZMA_OK) {
                    ended_ = true;
                    if (r != LZMA_STREAM_END && got == before) err = "Damaged installer data";
                }
            }
            if (got == before && last && !inLeft_) ended_ = true;  // nothing more to come
        }
        out_ += got;
        return got;
    }

private:
    bool start(std::string& err) {
        in_.resize(1 << 16);
        inPos_ = inLeft_ = 0;
        out_ = 0;
        ended_ = false;
        if (m_ == kDeflate) {
            z_ = z_stream{};
            if (inflateInit2(&z_, -15) != Z_OK) return fail(err);
        } else if (m_ == kLzma) {
            // An optional filter byte (1 = x86 calls were made absolute), then the 5 LZMA properties.
            uint8_t h[6];
            if (len_ < 6 || !src_->readAt(off_, h, 6)) return fail(err);
            bool x86 = false;
            size_t skip = 0;
            if (h[0] <= 1 && h[1] == 0x5D) x86 = h[0] == 1, skip = 1;
            lzma_filter f[3] = {{LZMA_VLI_UNKNOWN, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}, {LZMA_VLI_UNKNOWN, nullptr}};
            int k = 0;
            if (x86) f[k++].id = LZMA_FILTER_X86;
            f[k].id = LZMA_FILTER_LZMA1;
            if (lzma_properties_decode(&f[k], nullptr, h + skip, 5) != LZMA_OK) return fail(err);
            lz_ = LZMA_STREAM_INIT;
            lzma_ret r = lzma_raw_decoder(&lz_, f);
            free(f[k].options);
            if (r != LZMA_OK) return fail(err);
            inPos_ = skip + 5;
        } else if (m_ == kBzip2) {
            err = "This installer uses NSIS's own bzip2 variant, which isn't supported yet";
            return false;
        }
        open_ = true;
        return true;
    }
    bool fail(std::string& err) {
        err = "Damaged installer data";
        return false;
    }
    void close() {
        if (!open_) return;
        if (m_ == kDeflate) inflateEnd(&z_);
        if (m_ == kLzma) lzma_end(&lz_);
        open_ = false;
    }

    StreamPtr src_;
    uint64_t off_, len_;
    Method m_;
    bool open_ = false, ended_ = false;
    std::vector<uint8_t> in_;
    const uint8_t* inNext_ = nullptr;
    size_t inLeft_ = 0;
    uint64_t inPos_ = 0, out_ = 0;
    z_stream z_{};
    lzma_stream lz_ = LZMA_STREAM_INIT;
};

// A whole compressed piece, decompressed (non-solid installers compress each file on its own).
bool unpackAll(StreamPtr s, uint64_t off, uint64_t len, Method m, const Sink* sink, uint64_t* size, std::string& err) {
    Unpack u(s, off, len, m);
    std::vector<uint8_t> buf(1 << 16);
    uint64_t total = 0;
    for (;;) {
        size_t n = u.read(buf.data(), buf.size(), err);
        if (!err.empty()) return false;
        if (!n) break;
        total += n;
        if (sink && !(*sink)(buf.data(), n)) break;
    }
    if (size) *size = total;
    return true;
}

const char* const kShell[] = {
    "DESKTOP", "INTERNET", "SMPROGRAMS", "CONTROLS", "PRINTERS", "DOCUMENTS", "FAVORITES", "SMSTARTUP", "RECENT",
    "SENDTO", "BITBUCKET", "STARTMENU", nullptr, "MUSIC", "VIDEOS", nullptr, "DESKTOP", "DRIVES", "NETWORK",
    "NETHOOD", "FONTS", "TEMPLATES", "STARTMENU", "SMPROGRAMS", "SMSTARTUP", "DESKTOP", "APPDATA", "PRINTHOOD",
    "LOCALAPPDATA", "ALTSTARTUP", "ALTSTARTUP", "FAVORITES", "INTERNET_CACHE", "COOKIES", "HISTORY", "APPDATA",
    "WINDIR", "SYSDIR", "PROGRAMFILES", "PICTURES", "PROFILE", "SYSTEMX86", "PROGRAMFILESX86", "COMMONFILES",
    "COMMONFILESX86", "TEMPLATES", "DOCUMENTS", "ADMINTOOLS", "ADMINTOOLS", "CONNECTIONS", nullptr, nullptr, nullptr,
    "MUSIC", "PICTURES", "VIDEOS", "RESOURCES", "RESOURCES_LOCALIZED", nullptr, "CDBURN_AREA"};

const char* const kVars3[] = {"CMDLINE", "INSTDIR", "OUTDIR", "EXEDIR", "LANGUAGE", "TEMP",
                              "PLUGINSDIR", "EXEPATH", "EXEFILE", "HWNDPARENT", "_CLICK", "_OUTDIR"};
const char* const kVars2[] = {"CMDLINE", "INSTDIR", "OUTDIR", "EXEDIR", "LANGUAGE",
                              "TEMP", "PLUGINSDIR", "HWNDPARENT", "_CLICK", "_OUTDIR"};

struct Strings {
    const uint8_t* p = nullptr;
    size_t n = 0;  // bytes
    bool unicode = false;

    // Text of the string at `off` (characters for Unicode installers). `startsWithVar` tells whether it
    // begins with a variable or shell folder (so it's a full path, not one relative to $OUTDIR).
    std::string get(uint32_t off, bool* startsWithVar = nullptr, const std::string& outdir = "") const {
        std::string s;
        size_t i = unicode ? (size_t)off * 2 : off;
        bool first = true;
        if (startsWithVar) *startsWithVar = false;
        while (i < n) {
            uint32_t c;
            if (unicode) {
                if (i + 1 >= n) break;
                c = p[i] | p[i + 1] << 8;
                i += 2;
            } else {
                c = p[i++];
            }
            if (c == 0) break;
            // Codes: ANSI 252 skip, 253 variable, 254 shell folder, 255 language string;
            // Unicode 4 skip, 3 variable, 2 shell folder, 1 language string.
            bool code = unicode ? (c >= 1 && c <= 4) : c >= 252;
            if (!code) {
                if (unicode && c >= 0xD800 && c < 0xDC00 && i + 1 < n) {  // surrogate pair
                    uint32_t lo = p[i] | p[i + 1] << 8;
                    if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 2;
                }
                if (unicode) putUtf8(s, c);
                else s += c < 0x80 ? (char)c : '?';
                first = false;
                continue;
            }
            uint32_t kind = unicode ? 4 - c : c - 252;  // 0 skip, 1 variable, 2 shell folder, 3 language string
            uint32_t a, b, num;
            if (unicode) {
                if (i + 1 >= n) break;
                uint32_t w = p[i] | p[i + 1] << 8;
                i += 2;
                a = w & 0xFF, b = w >> 8, num = (a & 0x7F) | (b & 0x7F) << 7;
                if (kind == 0) {
                    putUtf8(s, w);
                    first = false;
                    continue;
                }
            } else {
                if (kind == 0) {  // the next byte is literal
                    if (i < n) s += p[i] < 0x80 ? (char)p[i] : '?', ++i;
                    first = false;
                    continue;
                }
                if (i + 1 >= n) break;
                a = p[i], b = p[i + 1];
                i += 2;
                num = (a & 0x7F) | (b & 0x7F) << 7;
            }
            if (first && startsWithVar && kind != 3) *startsWithVar = true;
            first = false;
            if (kind == 1) s += var(num, outdir);
            else if (kind == 2) s += shell(a, b);
            else s += "$(LSTR_" + std::to_string(num) + ")";
        }
        return s;
    }

    std::string var(uint32_t v, const std::string& outdir) const {
        if (v < 10) return "$" + std::to_string(v);
        if (v < 20) return "$R" + std::to_string(v - 10);
        const char* const* names = unicode ? kVars3 : kVars2;
        uint32_t count = unicode ? 12 : 10;
        if (v - 20 < count) {
            std::string name = names[v - 20];
            if (name == "OUTDIR" && !outdir.empty()) return outdir;
            return "$" + name;
        }
        return "$_" + std::to_string(v - 20 - count) + "_";
    }

    std::string shell(uint32_t a, uint32_t b) const {
        if (a & 0x80) {  // a folder found in the registry: the value's name is a string at (a & 0x3F)
            std::string name = get(a & 0x3F);
            std::string base = name == "ProgramFilesDir" ? "$PROGRAMFILES" : name == "CommonFilesDir" ? "$COMMONFILES" : "$" + name;
            return (a & 0x40) ? base + "64" : base;
        }
        uint32_t idx = a < sizeof kShell / sizeof *kShell && kShell[a] ? a : b;
        if (idx < sizeof kShell / sizeof *kShell && kShell[idx]) return std::string("$") + kShell[idx];
        return "$SHELL_" + std::to_string(a);
    }
};

// Where the first header is: on a 512-byte boundary, after the setup program.
uint64_t findFirstHeader(Stream& s) {
    static const uint8_t sig[16] = {0xEF, 0xBE, 0xAD, 0xDE, 'N', 'u', 'l', 'l', 's', 'o', 'f', 't', 'I', 'n', 's', 't'};
    uint64_t end = std::min<uint64_t>(s.size(), 8ull << 20);  // the setup program is small
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t at = 0; at < end; at += buf.size()) {
        size_t n = (size_t)std::min<uint64_t>(buf.size(), s.size() - at);
        if (n < 32 || !s.readAt(at, buf.data(), n)) return UINT64_MAX;
        for (size_t o = 0; o + 28 <= n; o += 512)
            if (std::memcmp(buf.data() + o + 4, sig, 16) == 0) return at + o;
    }
    return UINT64_MAX;
}

struct NsisFile {
    std::string path;
    uint32_t data;  // where its data is in the data block
    int64_t mtime;
    uint64_t size = 0;
};

bool openNsis(Ctx& ctx, StreamPtr s) {
    uint64_t fh = findFirstHeader(*s);
    if (fh == UINT64_MAX) return false;
    std::vector<uint8_t> fhv = s->get(fh, 28);
    Span F(fhv);
    uint32_t headerLen = F.u32(20), allLen = F.u32(24);
    uint64_t post = fh + 28, end = std::min<uint64_t>(s->size(), fh + std::max<uint64_t>(allLen, 28));
    if (headerLen < 36 || headerLen > (256u << 20) || end <= post + 4) return false;
    std::vector<uint8_t> sig = s->get(post, 16);
    if (sig.size() < 16) return false;
    Span G(sig);
    uint32_t first = G.u32(0);
    Method m;
    bool solid = true;
    if (first == headerLen) {
        m = kCopy, solid = false;
    } else if (isLzmaProps(sig.data()) || (sig[0] <= 1 && isLzmaProps(sig.data() + 1))) {
        m = kLzma;
    } else if (isLzmaProps(sig.data() + 4) || (sig[4] <= 1 && isLzmaProps(sig.data() + 5))) {
        m = kLzma, solid = false;
    } else if (sig[3] == 0x80) {
        m = sig[4] == 0x31 && sig[5] < 14 ? kBzip2 : kDeflate, solid = false;
    } else if (sig[0] == 0x31 && sig[1] < 14) {
        m = kBzip2;
    } else {
        m = kDeflate;
    }
    // The script header.
    std::vector<uint8_t> header;
    std::shared_ptr<Unpack> sol;
    uint64_t dataStart = 0;  // solid: in the unpacked stream; otherwise: in the file
    std::string err;
    if (solid) {
        sol = std::make_shared<Unpack>(s, post, end - post, m);
        uint8_t l[4];
        if (sol->read(l, 4, err) != 4 || Span(l, 4).u32(0) != headerLen) {
            ctx.err = err.empty() ? "Damaged installer (header)" : err;
            return false;
        }
        header.resize(headerLen);
        if (sol->read(header.data(), headerLen, err) != headerLen) {
            ctx.err = err.empty() ? "Damaged installer (header)" : err;
            return false;
        }
        dataStart = 4 + headerLen;
    } else {
        uint64_t clen = first & 0x7FFFFFFF;
        if (post + 4 + clen > end) return false;
        if (first & 0x80000000) {
            Unpack u(s, post + 4, clen, m);
            header.resize(headerLen);
            if (u.read(header.data(), headerLen, err) != headerLen) {
                ctx.err = err.empty() ? "Damaged installer (header)" : err;
                return false;
            }
        } else {
            header = s->get(post + 4, (size_t)std::min<uint64_t>(clen, headerLen));
        }
        dataStart = post + 4 + clen;
    }
    Span H(header);
    uint32_t entOff = H.u32(4 + 2 * 8), entNum = H.u32(8 + 2 * 8);
    uint32_t strOff = H.u32(4 + 3 * 8), strEnd = H.u32(4 + 4 * 8);
    if (strEnd <= strOff || strEnd > header.size()) strEnd = (uint32_t)header.size();
    if (!H.has(entOff, (size_t)entNum * 28) || strOff >= header.size()) {
        ctx.err = "Damaged installer (script)";
        return false;
    }
    Strings S;
    S.p = header.data() + strOff;
    S.n = strEnd - strOff;
    S.unicode = S.n >= 4 && S.p[0] == 0 && S.p[1] == 0;
    // Walk the script for SetOutPath (EW_CREATEDIR with its flag set) and File (EW_EXTRACTFILE).
    std::vector<NsisFile> files;
    std::set<std::pair<std::string, uint32_t>> seen;
    std::string outdir = "$INSTDIR";
    for (uint32_t e = 0; e < entNum; ++e) {
        size_t o = entOff + (size_t)e * 28;
        uint32_t which = H.u32(o), a0 = H.u32(o + 4), a1 = H.u32(o + 8), a2 = H.u32(o + 12), a3 = H.u32(o + 16),
                 a4 = H.u32(o + 20);
        if (which == 11 && a1) {
            outdir = S.get(a0, nullptr, outdir);
        } else if (which == 20) {
            bool abs = false;
            std::string name = S.get(a1, &abs, outdir);
            if (name.empty()) continue;
            bool rooted = abs || (name.size() > 1 && name[1] == ':') || name.compare(0, 2, "\\\\") == 0;
            if (!rooted) name = outdir + "\\" + name;
            std::replace(name.begin(), name.end(), '\\', '/');
            if (name.compare(0, 9, "$INSTDIR/") == 0) name.erase(0, 9);
            // Tidy the path: no empty, "." or ".." parts.
            std::string clean, part;
            for (size_t i = 0; i <= name.size(); ++i) {
                if (i < name.size() && name[i] != '/') {
                    part += name[i];
                    continue;
                }
                if (!part.empty() && part != "." && part != "..") clean += (clean.empty() ? "" : "/") + component(part);
                part.clear();
            }
            if (clean.empty() || !seen.insert({clean, a2}).second) continue;
            uint64_t ft = (uint64_t)a4 << 32 | a3;
            int64_t mt = (ft == 0 || ft == UINT64_MAX || a4 == 0xFFFFFFFF) ? 0 : fromFiletime(ft);
            files.push_back({clean, a2, mt});
        }
    }
    ctx.formats.push_back("NSIS");
    std::sort(files.begin(), files.end(), [](const NsisFile& x, const NsisFile& y) { return x.data < y.data; });
    if (solid) {
        // Each file's size is at the front of its data: one pass through the stream finds them all.
        for (auto& f : files) {
            uint8_t l[4];
            if (!sol->seek(dataStart + f.data, err) || sol->read(l, 4, err) != 4) {
                ctx.out.warning = err.empty() ? "The installer's data ends early" : err;
                break;
            }
            f.size = Span(l, 4).u32(0) & 0x7FFFFFFF;
        }
        for (auto& f : files) {
            Item it;
            it.path = ctx.prefix + f.path;
            it.size = f.size;
            it.mtime = f.mtime;
            uint64_t at = dataStart + f.data + 4, len = f.size;
            it.read = [sol, at, len](const Sink& sink, std::string& err) {
                if (!sol->seek(at, err)) return false;
                std::vector<uint8_t> buf(1 << 16);
                for (uint64_t left = len; left;) {
                    size_t n = (size_t)std::min<uint64_t>(buf.size(), left);
                    if (sol->read(buf.data(), n, err) != n) {
                        if (err.empty()) err = "The installer's data ends early";
                        return false;
                    }
                    if (!sink(buf.data(), n)) return true;
                    left -= n;
                }
                return true;
            };
            ctx.out.items.push_back(std::move(it));
        }
        return true;
    }
    for (auto& f : files) {
        uint64_t at = dataStart + f.data;
        std::vector<uint8_t> l = s->getUpTo(at, 4);
        if (l.size() < 4) {
            ctx.out.warning = "The installer's data ends early";
            continue;
        }
        uint32_t v = Span(l).u32(0), len = v & 0x7FFFFFFF;
        if (at + 4 + len > s->size()) {
            ctx.out.warning = "The installer's data ends early";
            continue;
        }
        if (!(v & 0x80000000) || m == kCopy) {
            ctx.out.items.push_back(rangeItem(ctx.prefix + f.path, s, at + 4, len, f.mtime));
            continue;
        }
        Item it;
        it.path = ctx.prefix + f.path;
        it.mtime = f.mtime;
        std::string e2;
        unpackAll(s, at + 4, len, m, nullptr, &it.size, e2);  // the size is only known by unpacking
        it.read = [s, at, len, m](const Sink& sink, std::string& err) { return unpackAll(s, at + 4, len, m, &sink, nullptr, err); };
        ctx.out.items.push_back(std::move(it));
    }
    return true;
}

}  // namespace

const Handler hNsis = {"NSIS",
                       [](Stream& s, const std::vector<uint8_t>& h, const std::string&) {
                           return Span(h).eq(0, "MZ", 2) && findFirstHeader(s) != UINT64_MAX;
                       },
                       openNsis};

}  // namespace kz::native
