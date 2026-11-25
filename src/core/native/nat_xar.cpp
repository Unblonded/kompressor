// XAR (eXtensible ARchiver): macOS installer packages (.pkg) and .xar files. A big-endian header,
// a zlib-compressed XML table of contents, then the heap of file data, each piece stored, gzip/zlib,
// bzip2, xz or LZMA. libarchive needs an XML library for this format, so it's read here instead.
#include <zlib.h>
#define LZMA_API_STATIC
#include <lzma.h>
#ifdef KZ_HAVE_BZIP2
#include <bzlib.h>
#endif

#include <algorithm>
#include <map>

#include "../kz_hash.hpp"
#include "nat.hpp"

namespace kz::native {

namespace {

// ------------------------------------------------------------------------------------------------ XML
// Just enough XML for a table of contents: elements, attributes, text and the standard entities.
struct XNode {
    std::string tag, text;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> kids;
    const XNode* child(const char* t) const {
        for (auto& k : kids)
            if (k.tag == t) return &k;
        return nullptr;
    }
    std::string attr(const char* a) const {
        for (auto& x : attrs)
            if (x.first == a) return x.second;
        return {};
    }
    std::string textOf(const char* t) const {
        const XNode* c = child(t);
        return c ? c->text : std::string();
    }
};

std::string unescape(const char* p, size_t n) {
    std::string s;
    s.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (p[i] != '&') {
            s += p[i];
            continue;
        }
        size_t semi = i + 1;
        while (semi < n && semi - i < 12 && p[semi] != ';') ++semi;
        if (semi >= n || p[semi] != ';') {
            s += '&';
            continue;
        }
        std::string e(p + i + 1, semi - i - 1);
        if (e == "amp") s += '&';
        else if (e == "lt") s += '<';
        else if (e == "gt") s += '>';
        else if (e == "quot") s += '"';
        else if (e == "apos") s += '\'';
        else if (!e.empty() && e[0] == '#') {
            uint32_t c = e.size() > 1 && (e[1] == 'x' || e[1] == 'X') ? (uint32_t)std::strtoul(e.c_str() + 2, nullptr, 16)
                                                                       : (uint32_t)std::strtoul(e.c_str() + 1, nullptr, 10);
            putUtf8(s, c);
        } else {
            s.append(p + i, semi - i + 1);
        }
        i = semi;
    }
    return s;
}

class XmlParser {
public:
    XmlParser(const char* p, size_t n) : p_(p), n_(n) {}
    bool parse(XNode& root) {
        root.tag = "#document";
        std::vector<XNode*> stack{&root};
        while (i_ < n_) {
            if (p_[i_] != '<') {
                size_t lt = find('<');
                stack.back()->text += unescape(p_ + i_, lt - i_);
                i_ = lt;
                continue;
            }
            if (starts("<?")) { i_ = after("?>"); continue; }
            if (starts("<!--")) { i_ = after("-->"); continue; }
            if (starts("<![CDATA[")) {
                size_t end = findStr("]]>");
                stack.back()->text.append(p_ + i_ + 9, end - i_ - 9);
                i_ = std::min(n_, end + 3);
                continue;
            }
            if (starts("<!")) { i_ = after(">"); continue; }
            if (starts("</")) {
                if (stack.size() < 2) return false;
                stack.pop_back();
                i_ = after(">");
                continue;
            }
            // An element: <tag a="v" ...> or <tag .../>
            ++i_;
            XNode node;
            node.tag = name();
            if (node.tag.empty()) return false;
            bool selfClose = false;
            while (i_ < n_) {
                skipSpace();
                if (i_ >= n_) return false;
                if (p_[i_] == '>') { ++i_; break; }
                if (p_[i_] == '/') { selfClose = true; i_ = after(">"); break; }
                std::string a = name();
                if (a.empty()) return false;
                skipSpace();
                std::string v;
                if (i_ < n_ && p_[i_] == '=') {
                    ++i_;
                    skipSpace();
                    if (i_ >= n_ || (p_[i_] != '"' && p_[i_] != '\'')) return false;
                    char q = p_[i_++];
                    size_t end = find(q);
                    v = unescape(p_ + i_, end - i_);
                    i_ = std::min(n_, end + 1);
                }
                node.attrs.emplace_back(std::move(a), std::move(v));
            }
            if (stack.size() > 64) return false;
            stack.back()->kids.push_back(std::move(node));
            if (!selfClose) stack.push_back(&stack.back()->kids.back());
        }
        return true;
    }

private:
    bool starts(const char* s) const {
        size_t k = std::strlen(s);
        return i_ + k <= n_ && std::memcmp(p_ + i_, s, k) == 0;
    }
    size_t find(char c) const {
        const void* q = std::memchr(p_ + i_, c, n_ - i_);
        return q ? (size_t)((const char*)q - p_) : n_;
    }
    size_t findStr(const char* s) const {
        size_t k = std::strlen(s);
        for (size_t j = i_; j + k <= n_; ++j)
            if (std::memcmp(p_ + j, s, k) == 0) return j;
        return n_;
    }
    size_t after(const char* s) const { return std::min(n_, findStr(s) + std::strlen(s)); }
    void skipSpace() {
        while (i_ < n_ && (p_[i_] == ' ' || p_[i_] == '\t' || p_[i_] == '\r' || p_[i_] == '\n')) ++i_;
    }
    std::string name() {
        size_t b = i_;
        while (i_ < n_ && !std::strchr(" \t\r\n/>=", p_[i_])) ++i_;
        return std::string(p_ + b, i_ - b);
    }
    const char* p_;
    size_t n_, i_ = 0;
};

// -------------------------------------------------------------------------------------------- helpers
std::string unBase64(const std::string& s) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// "2024-03-01T12:34:56Z" (fractions and offsets ignored) as Unix time.
int64_t isoTime(const std::string& s) {
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3 || mo < 1 || mo > 12) return 0;
    int yy = y - (mo <= 2), era = yy / 400, yoe = yy - era * 400;
    int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + mi * 60 + se;
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// One member's data: `len` bytes at `off`, decoded by its encoding, handed to the sink in pieces.
// `sum` (style, lowercase hex) is checked when it's a kind Kompressor computes.
bool readData(Stream& s, uint64_t off, uint64_t len, const std::string& style, uint64_t size,
              const std::pair<std::string, std::string>& sum, const Sink& sink, std::string& err) {
    std::unique_ptr<Digest> dg;
    if (sum.first == "md5") dg = std::make_unique<Digest>(kHashMd5);
    else if (sum.first == "sha1") dg = std::make_unique<Digest>(kHashSha1);
    else if (sum.first == "sha256") dg = std::make_unique<Digest>(kHashSha256);
    uint64_t produced = 0;
    bool stopped = false;
    auto emit = [&](const uint8_t* p, size_t n) {
        if (!n || stopped) return true;
        produced += n;
        if (dg) dg->update(p, n);
        if (!sink(p, n)) stopped = true;
        return !stopped;
    };

    std::string st = lowerStr(style);
    enum { Raw, Zlib, Bzip2, Xz } kind = Raw;
    if (st.find("gzip") != std::string::npos || st.find("zlib") != std::string::npos) kind = Zlib;
    else if (st.find("bzip2") != std::string::npos) kind = Bzip2;
    else if (st.find("lzma") != std::string::npos || st.find("xz") != std::string::npos) kind = Xz;
    else if (!st.empty() && st != "application/octet-stream") {
        err = "Unsupported XAR encoding: " + style;
        return false;
    }

    constexpr size_t kIn = 256 * 1024, kOut = 512 * 1024;
    std::vector<uint8_t> in(kIn), out(kOut);
    uint64_t pos = 0;
    auto next = [&](size_t& got) {
        got = (size_t)std::min<uint64_t>(kIn, len - pos);
        if (got && !s.readAt(off + pos, in.data(), got)) return false;
        pos += got;
        return true;
    };
    bool ok = true;
    if (kind == Raw) {
        size_t got;
        while (pos < len && !stopped) {
            if (!next(got)) { ok = false; break; }
            emit(in.data(), got);
        }
    } else if (kind == Zlib) {
        z_stream z{};
        if (inflateInit2(&z, 15 + 32) != Z_OK) return err = "zlib failed to start", false;
        int r = Z_OK;
        while (r != Z_STREAM_END && !stopped) {
            size_t got = 0;
            if (z.avail_in == 0) {
                if (pos >= len || !next(got)) { ok = false; break; }
                z.next_in = in.data();
                z.avail_in = (uInt)got;
            }
            z.next_out = out.data();
            z.avail_out = (uInt)kOut;
            r = inflate(&z, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) { ok = false; break; }
            emit(out.data(), kOut - z.avail_out);
        }
        inflateEnd(&z);
    } else if (kind == Xz) {
        lzma_stream z = LZMA_STREAM_INIT;
        if (lzma_auto_decoder(&z, UINT64_MAX, 0) != LZMA_OK) return err = "LZMA failed to start", false;
        lzma_ret r = LZMA_OK;
        while (r != LZMA_STREAM_END && !stopped) {
            size_t got = 0;
            lzma_action act = LZMA_RUN;
            if (z.avail_in == 0) {
                if (!next(got)) { ok = false; break; }
                z.next_in = in.data();
                z.avail_in = got;
                if (pos >= len) act = LZMA_FINISH;
            }
            z.next_out = out.data();
            z.avail_out = kOut;
            r = lzma_code(&z, pos >= len ? LZMA_FINISH : act);
            if (r != LZMA_OK && r != LZMA_STREAM_END) { ok = false; break; }
            emit(out.data(), kOut - z.avail_out);
            if (r == LZMA_OK && pos >= len && z.avail_in == 0 && z.avail_out != 0) { ok = false; break; }
        }
        lzma_end(&z);
    } else {
#ifdef KZ_HAVE_BZIP2
        bz_stream z{};
        if (BZ2_bzDecompressInit(&z, 0, 0) != BZ_OK) return err = "bzip2 failed to start", false;
        int r = BZ_OK;
        while (r != BZ_STREAM_END && !stopped) {
            size_t got = 0;
            if (z.avail_in == 0) {
                if (pos >= len || !next(got)) { ok = false; break; }
                z.next_in = (char*)in.data();
                z.avail_in = (unsigned)got;
            }
            z.next_out = (char*)out.data();
            z.avail_out = (unsigned)kOut;
            r = BZ2_bzDecompress(&z);
            if (r != BZ_OK && r != BZ_STREAM_END) { ok = false; break; }
            emit(out.data(), kOut - z.avail_out);
        }
        BZ2_bzDecompressEnd(&z);
#else
        err = "bzip2 is not available in this build";
        return false;
#endif
    }
    if (stopped) return true;
    if (!ok) {
        err = "Damaged data";
        return false;
    }
    if (size && produced != size) {
        err = "Wrong size after decoding";
        return false;
    }
    if (dg && dg->hex() != sum.second) {
        err = "Checksum mismatch (" + sum.first + ")";
        return false;
    }
    return true;
}

struct Data {
    bool has = false;
    uint64_t off = 0, len = 0, size = 0;
    std::string style;
    std::pair<std::string, std::string> sum;
};

Data dataOf(const XNode& f) {
    Data d;
    const XNode* x = f.child("data");
    if (!x) return d;
    d.has = true;
    d.off = std::strtoull(x->textOf("offset").c_str(), nullptr, 10);
    d.len = std::strtoull(x->textOf("length").c_str(), nullptr, 10);
    d.size = std::strtoull(x->textOf("size").c_str(), nullptr, 10);
    if (const XNode* e = x->child("encoding")) d.style = e->attr("style");
    if (const XNode* c = x->child("extracted-checksum")) {
        std::string h;
        for (char ch : c->text)
            if (std::isxdigit((unsigned char)ch)) h += (char)std::tolower((unsigned char)ch);
        d.sum = {lowerStr(c->attr("style")), h};
    }
    return d;
}

struct Walk {
    Ctx& ctx;
    StreamPtr s;
    uint64_t heap;
    std::map<std::string, Data> byId;  // hard-link originals
    std::vector<std::pair<std::string, const XNode*>> links;  // hard links, resolved after the walk
    int files = 0;

    std::string nameOf(const XNode& f) {
        const XNode* n = f.child("name");
        if (!n) return {};
        std::string v = n->attr("enctype") == "base64" ? unBase64(n->text) : n->text;
        return component(v);
    }

    Item fileItem(const std::string& path, const Data& d, int64_t mtime) {
        Item it;
        it.path = path;
        it.size = d.size;
        it.mtime = mtime;
        StreamPtr st = s;
        uint64_t at = heap + d.off;
        it.read = [st, at, d](const Sink& sink, std::string& err) {
            if (d.len > st->size() || at > st->size() - d.len) return err = "Data lies past the end of the file", false;
            return readData(*st, at, d.len, d.style, d.size, d.sum, sink, err);
        };
        return it;
    }

    void walk(const XNode& parent, const std::string& dir, int depth) {
        if (depth > 128) return;
        for (auto& f : parent.kids) {
            if (f.tag != "file") continue;
            std::string name = nameOf(f);
            if (name.empty()) name = "file" + f.attr("id");
            std::string path = dir + name;
            const XNode* t = f.child("type");
            std::string type = t ? t->text : "file";
            int64_t mtime = isoTime(f.textOf("mtime"));
            Data d = dataOf(f);
            if (type == "directory") {
                ctx.out.items.push_back(dirItem(ctx.prefix + path, mtime));
                walk(f, path + "/", depth + 1);
            } else if (type == "symlink") {
                // Listed as a small file holding the target, as the other readers do.
                std::string target = f.textOf("link");
                std::vector<uint8_t> b(target.begin(), target.end());
                ctx.out.items.push_back(builtItem(ctx.prefix + path, std::move(b), mtime));
                ++files;
            } else if (type == "hardlink" && t && t->attr("link") != "original" && !d.has) {
                links.push_back({ctx.prefix + path, &f});
            } else {
                if (d.has) byId[f.attr("id")] = d;
                ctx.out.items.push_back(d.has ? fileItem(ctx.prefix + path, d, mtime)
                                              : builtItem(ctx.prefix + path, {}, mtime));
                ++files;
            }
        }
    }
};

bool openXar(Ctx& ctx, StreamPtr s) {
    std::vector<uint8_t> h = s->getUpTo(0, 64);
    Span H(h);
    uint32_t hsize = H.b16(4);
    uint64_t tocLen = H.b64(8), tocRaw = H.b64(16);
    if (hsize < 28 || tocLen == 0 || tocLen > s->size() - hsize || tocRaw > (256u << 20)) {
        ctx.err = "Damaged XAR header";
        return false;
    }
    std::vector<uint8_t> z = s->get(hsize, (size_t)tocLen), toc;
    if (z.empty() || !inflateZlib(z.data(), z.size(), toc, (size_t)tocRaw)) {
        ctx.err = "Damaged XAR table of contents";
        return false;
    }
    XNode doc;
    if (!XmlParser((const char*)toc.data(), toc.size()).parse(doc)) {
        ctx.err = "Unreadable XAR table of contents";
        return false;
    }
    const XNode* xar = doc.child("xar");
    const XNode* t = xar ? xar->child("toc") : nullptr;
    if (!t) {
        ctx.err = "XAR table of contents has no files";
        return false;
    }
    Walk w{ctx, s, hsize + tocLen};
    w.walk(*t, "", 0);
    for (auto& l : w.links) {
        const XNode* ty = l.second->child("type");
        auto it = w.byId.find(ty ? ty->attr("link") : std::string());
        if (it == w.byId.end()) continue;
        ctx.out.items.push_back(w.fileItem(l.first, it->second, isoTime(l.second->textOf("mtime"))));
        ++w.files;
    }
    ctx.formats.push_back("XAR");
    return true;
}

}  // namespace

const Handler hXar = {"XAR",
                      [](Stream&, const std::vector<uint8_t>& h, const std::string&) {
                          Span H(h);
                          return H.eq(0, "xar!", 4) && H.b16(4) >= 28;
                      },
                      openXar};

}  // namespace kz::native
