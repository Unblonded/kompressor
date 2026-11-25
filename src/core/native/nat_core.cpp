#include <algorithm>
#include <cstdio>
#include <ctime>

#include "../kz_sniff.hpp"
#include "nat.hpp"

namespace kz::native {

// ------------------------------------------------------------------------------------------- streams
namespace {

class FileStream : public Stream {
public:
    explicit FileStream(const std::string& path) {
#ifdef _WIN32
        f_ = _wfopen(u8path(path).wstring().c_str(), L"rb");
#else
        f_ = std::fopen(path.c_str(), "rb");
#endif
        std::error_code ec;
        if (f_) size_ = fs::file_size(u8path(path), ec);
        if (ec) size_ = 0;
    }
    ~FileStream() override {
        if (f_) std::fclose(f_);
    }
    bool ok() const { return f_ != nullptr; }
    uint64_t size() const override { return size_; }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off > size_ || n > size_ - off) return false;
        if (!n) return true;
        if (off != pos_) {
#ifdef _WIN32
            if (_fseeki64(f_, (__int64)off, SEEK_SET) != 0) return false;
#else
            if (fseeko(f_, (off_t)off, SEEK_SET) != 0) return false;
#endif
        }
        size_t got = std::fread(dst, 1, n, f_);
        pos_ = off + got;
        return got == n;
    }

private:
    std::FILE* f_ = nullptr;
    uint64_t size_ = 0, pos_ = UINT64_MAX;
};

class MemStream : public Stream {
public:
    explicit MemStream(std::shared_ptr<const std::vector<uint8_t>> d) : d_(std::move(d)) {}
    uint64_t size() const override { return d_->size(); }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off > d_->size() || n > d_->size() - off) return false;
        if (n) std::memcpy(dst, d_->data() + off, n);
        return true;
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> d_;
};

class RangeStream : public Stream {
public:
    RangeStream(StreamPtr p, uint64_t off, uint64_t len) : p_(std::move(p)), off_(off), len_(len) {}
    uint64_t size() const override { return len_; }
    bool readAt(uint64_t off, void* dst, size_t n) override {
        if (off > len_ || n > len_ - off) return false;
        return p_->readAt(off_ + off, dst, n);
    }

private:
    StreamPtr p_;
    uint64_t off_, len_;
};

}  // namespace

StreamPtr fileStream(const std::string& path) {
    auto s = std::make_shared<FileStream>(path);
    return s->ok() ? s : nullptr;
}
StreamPtr memStream(std::shared_ptr<const std::vector<uint8_t>> d) { return std::make_shared<MemStream>(std::move(d)); }
StreamPtr memStream(std::vector<uint8_t> d) {
    return std::make_shared<MemStream>(std::make_shared<const std::vector<uint8_t>>(std::move(d)));
}
StreamPtr rangeStream(StreamPtr p, uint64_t off, uint64_t len) {
    if (off > p->size()) off = p->size();
    len = std::min(len, p->size() - off);
    return std::make_shared<RangeStream>(std::move(p), off, len);
}

bool BlockStream::readAt(uint64_t off, void* dst, size_t n) {
    if (off > size_ || n > size_ - off) return false;
    auto* out = static_cast<uint8_t*>(dst);
    while (n) {
        uint64_t idx = off / bs_;
        size_t within = (size_t)(off % bs_), take = std::min<size_t>(n, bs_ - within);
        Cached* c = nullptr;
        for (auto& e : cache_)
            if (e.index == idx) c = &e;
        if (!c) {
            c = &cache_[0];
            for (auto& e : cache_)
                if (e.used < c->used) c = &e;
            c->data.resize(bs_);
            c->index = UINT64_MAX;
            if (!readBlock(idx, c->data.data())) return false;
            c->index = idx;
        }
        c->used = ++tick_;
        std::memcpy(out, c->data.data() + within, take);
        out += take;
        off += take;
        n -= take;
    }
    return true;
}

ExtentStream::ExtentStream(uint64_t size, std::vector<Extent> ext) : size_(size), ext_(std::move(ext)) {
    std::sort(ext_.begin(), ext_.end(), [](const Extent& a, const Extent& b) { return a.at < b.at; });
}

bool ExtentStream::readAt(uint64_t off, void* dst, size_t n) {
    if (off > size_ || n > size_ - off) return false;
    auto* out = static_cast<uint8_t*>(dst);
    // First extent that ends after `off`.
    auto it = std::upper_bound(ext_.begin(), ext_.end(), off, [](uint64_t v, const Extent& e) { return v < e.at; });
    if (it != ext_.begin()) --it;
    while (n) {
        while (it != ext_.end() && it->at + it->len <= off) ++it;
        if (it == ext_.end() || it->at > off) {  // a gap: zeros up to the next extent
            uint64_t stop = it == ext_.end() ? off + n : std::min<uint64_t>(off + n, it->at);
            size_t z = (size_t)(stop - off);
            std::memset(out, 0, z);
            out += z, off += z, n -= z;
            continue;
        }
        size_t take = (size_t)std::min<uint64_t>(n, it->at + it->len - off);
        uint64_t rel = off - it->at;
        if (it->src) {
            if (!it->src->readAt(it->srcOff + rel, out, take)) return false;
        } else if (it->pattern.empty()) {
            std::memset(out, 0, take);
        } else {
            for (size_t i = 0; i < take; ++i) out[i] = it->pattern[(rel + i) % it->pattern.size()];
        }
        out += take, off += take, n -= take;
    }
    return true;
}

// ------------------------------------------------------------------------------------------- helpers
void put16(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    put16(v, x & 0xFFFF);
    put16(v, x >> 16);
}

void putUtf8(std::string& s, uint32_t c) {
    if (c < 0x80) {
        s += char(c);
    } else if (c < 0x800) {
        s += char(0xC0 | c >> 6);
        s += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += char(0xE0 | c >> 12);
        s += char(0x80 | (c >> 6 & 0x3F));
        s += char(0x80 | (c & 0x3F));
    } else {
        s += char(0xF0 | c >> 18);
        s += char(0x80 | (c >> 12 & 0x3F));
        s += char(0x80 | (c >> 6 & 0x3F));
        s += char(0x80 | (c & 0x3F));
    }
}

static std::string utf16(const uint8_t* p, size_t chars, bool be) {
    std::string s;
    auto unit = [&](size_t i) {
        return be ? (uint32_t)p[i * 2] << 8 | p[i * 2 + 1] : (uint32_t)p[i * 2] | (uint32_t)p[i * 2 + 1] << 8;
    };
    for (size_t i = 0; i < chars; ++i) {
        uint32_t c = unit(i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < chars) {
            uint32_t d = unit(i + 1);
            if (d >= 0xDC00 && d < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
                ++i;
            }
        }
        putUtf8(s, c);
    }
    return s;
}
std::string utf16le(const uint8_t* p, size_t chars) { return utf16(p, chars, false); }
std::string utf16be(const uint8_t* p, size_t chars) { return utf16(p, chars, true); }

std::string latin1(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i]; ++i) putUtf8(s, p[i]);
    return s;
}

std::string cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string((const char*)p, n);
}

std::string component(std::string s) {
    for (auto& c : s)
        if (c == '/' || c == '\\' || (unsigned char)c < 0x20) c = '_';
    if (s.empty() || s == "." || s == "..") s = "_";
    return s;
}

std::string extFor(const uint8_t* p, size_t n) {
    auto is = [&](const char* sig, size_t len) { return n >= len && std::memcmp(p, sig, len) == 0; };
    if (is("\x89PNG", 4)) return ".png";
    if (is("GIF8", 4)) return ".gif";
    if (is("\xFF\xD8\xFF", 3)) return ".jpg";
    if (is("BM", 2) && n >= 14) return ".bmp";
    if (is("MZ", 2)) return ".exe";
    if (is("\x7F" "ELF", 4)) return ".elf";
    if (is("%PDF", 4)) return ".pdf";
    if (is("{\\rtf", 5)) return ".rtf";
    if (is("<?xml", 5) || is("\xEF\xBB\xBF<?xml", 8)) return ".xml";
    if (is("<!DOCTYPE html", 14) || is("<html", 5) || is("<HTML", 5)) return ".html";
    if (is("RIFF", 4) && n >= 12) {
        if (std::memcmp(p + 8, "WAVE", 4) == 0) return ".wav";
        if (std::memcmp(p + 8, "AVI ", 4) == 0) return ".avi";
        if (std::memcmp(p + 8, "ACON", 4) == 0) return ".ani";
    }
    if (is("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) return ".msi";
    if (is("ITSF", 4)) return ".chm";
    if (is("MSWIM\0\0\0", 8)) return ".wim";
    if (Sniffed s = sniff(p, n)) return std::string(".") + s.ext;
    return ".bin";
}

std::string hex(uint64_t v, int digits) {
    char b[24];
    std::snprintf(b, sizeof b, "%0*llX", digits, (unsigned long long)v);
    return b;
}

std::vector<uint8_t> guidBytes(const char* t) {
    // "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::vector<uint8_t> raw;
    for (const char* c = t; *c && c[1]; ++c) {
        if (*c == '-' || *c == '{' || *c == '}') continue;
        raw.push_back(uint8_t(nib(c[0]) << 4 | nib(c[1])));
        ++c;
    }
    if (raw.size() != 16) return std::vector<uint8_t>(16, 0);
    std::vector<uint8_t> g = {raw[3], raw[2], raw[1], raw[0], raw[5], raw[4], raw[7], raw[6]};
    g.insert(g.end(), raw.begin() + 8, raw.end());
    return g;
}

std::string guidText(const uint8_t* p) {
    char b[40];
    std::snprintf(b, sizeof b, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", p[3], p[2], p[1], p[0],
                  p[5], p[4], p[7], p[6], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    return b;
}

int64_t fromFiletime(uint64_t ft) {
    if (ft < 116444736000000000ull) return 0;
    return (int64_t)((ft - 116444736000000000ull) / 10000000ull);
}

int64_t fromDos(uint32_t date, uint32_t time) {
    if (!date) return 0;
    std::tm t{};
    t.tm_year = (int)((date >> 9) & 0x7F) + 80;
    t.tm_mon = (int)((date >> 5) & 0xF) - 1;
    t.tm_mday = (int)(date & 0x1F);
    t.tm_hour = (int)((time >> 11) & 0x1F);
    t.tm_min = (int)((time >> 5) & 0x3F);
    t.tm_sec = (int)((time & 0x1F) * 2);
    if (t.tm_mon < 0 || t.tm_mday == 0) return 0;
#ifdef _WIN32
    return (int64_t)_mkgmtime(&t);
#else
    return (int64_t)timegm(&t);
#endif
}

std::vector<uint8_t> textFile(const std::string& s) {
    std::vector<uint8_t> v = {0xEF, 0xBB, 0xBF};
    v.insert(v.end(), s.begin(), s.end());
    return v;
}

// --------------------------------------------------------------------------------------------- items
void addRun(std::vector<std::pair<uint64_t, uint64_t>>& runs, uint64_t off, uint64_t len) {
    if (!len) return;
    if (!runs.empty() && runs.back().first != kHole && off != kHole && runs.back().first + runs.back().second == off)
        runs.back().second += len;
    else if (!runs.empty() && runs.back().first == kHole && off == kHole)
        runs.back().second += len;
    else
        runs.emplace_back(off, len);
}

static bool pump(Stream& s, uint64_t off, uint64_t len, const Sink& sink, std::string& err, std::vector<uint8_t>& buf) {
    for (uint64_t done = 0; done < len;) {
        size_t n = (size_t)std::min<uint64_t>(1 << 20, len - done);
        buf.resize(n);
        if (off == kHole) std::memset(buf.data(), 0, n);
        else if (!s.readAt(off + done, buf.data(), n)) {
            err = "The data ends early or can't be read";
            return false;
        }
        if (!sink(buf.data(), n)) return true;
        done += n;
    }
    return true;
}

Item runsItem(std::string path, StreamPtr s, std::vector<std::pair<uint64_t, uint64_t>> runs, uint64_t size, int64_t mtime) {
    Item it;
    it.path = std::move(path);
    it.size = size;
    it.mtime = mtime;
    it.read = [s, runs = std::move(runs), size](const Sink& sink, std::string& err) {
        std::vector<uint8_t> buf;
        uint64_t left = size;
        bool stop = false;
        Sink guard = [&](const uint8_t* p, size_t n) {
            if (!sink(p, n)) {
                stop = true;
                return false;
            }
            return true;
        };
        for (auto& [off, len] : runs) {
            uint64_t take = std::min(len, left);
            if (!take) break;
            if (!pump(*s, off, take, guard, err, buf)) return false;
            if (stop) return true;
            left -= take;
        }
        if (left) return pump(*s, kHole, left, guard, err, buf);  // shorter than its size: the rest is zeros
        return true;
    };
    return it;
}

Item rangeItem(std::string path, StreamPtr s, uint64_t off, uint64_t len, int64_t mtime) {
    return runsItem(std::move(path), std::move(s), {{off, len}}, len, mtime);
}

Item streamItem(std::string path, StreamPtr s, int64_t mtime) {
    uint64_t n = s->size();
    return runsItem(std::move(path), std::move(s), {{0, n}}, n, mtime);
}

Item builtItem(std::string path, std::vector<uint8_t> data, int64_t mtime) {
    Item it;
    it.path = std::move(path);
    it.size = data.size();
    it.mtime = mtime;
    auto d = std::make_shared<const std::vector<uint8_t>>(std::move(data));
    it.read = [d](const Sink& sink, std::string&) {
        for (size_t at = 0; at < d->size(); at += 1 << 20)
            if (!sink(d->data() + at, std::min<size_t>(1 << 20, d->size() - at))) break;
        return true;
    };
    return it;
}

Item dirItem(std::string path, int64_t mtime) {
    Item it;
    it.path = std::move(path);
    it.isDir = true;
    it.mtime = mtime;
    it.read = [](const Sink&, std::string&) { return true; };
    return it;
}

// ------------------------------------------------------------------------------------------ dispatch
namespace {

// Order matters: specific signatures first; file systems before the MBR (their boot sectors end in
// 55 AA too); formats only known by their extension last.
const Handler* const kHandlers[] = {
    &hVhdx, &hVhd, &hVdi, &hVmdk, &hQcow, &hSparse, &hDmg, &hLp,  //
    &hXar, &hCfb, &hChm, &hWim, &hArj, &hMslz, &hSwf, &hFlv, &hPpmd, &hUefi,  //
    &hNsis, &hPe, &hTe, &hElf, &hMachO, &hMub,  //
    &hSquash, &hCramfs, &hApfs, &hHfs, &hUdf, &hExt, &hNtfs, &hExfat, &hFat, &hLvm,  //
    &hGpt, &hApm, &hMbr,  //
    &hBase64, &hIhex, &hCoff,
};

constexpr size_t kHead = 72 * 1024;

}  // namespace

bool explore(Ctx& ctx, StreamPtr s) {
    if (ctx.depth > 6) return false;
    std::vector<uint8_t> head = s->getUpTo(0, kHead);
    if (head.empty()) return false;
    for (const Handler* h : kHandlers) {
        if (!h->probe(*s, head, ctx.name)) continue;
        size_t before = ctx.out.items.size(), fmts = ctx.formats.size();
        ctx.depth++;
        bool ok = h->open(ctx, s);
        ctx.depth--;
        if (ok) return true;
        // Not really this format after all (or damaged): undo and try the next.
        ctx.out.items.resize(before);
        ctx.formats.resize(fmts);
    }
    return false;
}

std::string explorePart(Ctx& ctx, StreamPtr s, const std::string& folder, const std::string& rawName) {
    Ctx sub{ctx.out, ctx.prefix + folder + "/", "", ctx.depth, {}, {}};
    if (explore(sub, s) && !sub.formats.empty()) {
        // The folder is named after what was found in it: "2.ntfs".
        std::string fmt = sub.formats.front();
        std::string ext = fmt;
        for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
        std::string dir = ctx.prefix + folder + "." + ext + "/";
        for (size_t i = 0; i < ctx.out.items.size(); ++i) {
            auto& p = ctx.out.items[i].path;
            if (p.compare(0, sub.prefix.size(), sub.prefix) == 0) p = dir + p.substr(sub.prefix.size());
        }
        if (!sub.err.empty() && ctx.err.empty()) ctx.err = sub.err;
        for (auto& f : sub.formats)
            if (std::find(ctx.formats.begin(), ctx.formats.end(), f) == ctx.formats.end()) ctx.formats.push_back(f);
        return fmt;
    }
    ctx.out.items.push_back(streamItem(ctx.prefix + rawName, s, 0));
    return "";
}

bool open(const Source& src, Container& out, std::string& err) {
    out = Container();
    StreamPtr s = src.inMemory() ? memStream(src.mem) : fileStream(src.path);
    if (!s) {
        err = "Cannot open " + src.name();
        return false;
    }
    Ctx ctx{out, "", src.name(), 0, {}, {}};
    if (!src.inMemory()) ctx.srcPath = src.path;
    if (!explore(ctx, s)) {
        err = ctx.err.empty() ? "Not a supported archive (unrecognized format)" : ctx.err;
        return false;
    }
    if (out.format.empty()) {
        for (auto& f : ctx.formats) out.format += (out.format.empty() ? "" : " > ") + f;
    }
    if (!ctx.err.empty()) out.warning = ctx.err;
    return true;
}

bool claimsFirst(const Source& src) {
    StreamPtr s = src.inMemory() ? memStream(src.mem) : fileStream(src.path);
    if (!s) return false;
    std::vector<uint8_t> head = s->getUpTo(0, 72 * 1024);
    // libarchive reads text files as "mtree" listings, so text formats are claimed here too.
    if (hNsis.probe(*s, head, src.name()) || hIhex.probe(*s, head, src.name()) || hBase64.probe(*s, head, src.name()))
        return true;
    // A file starting with zeros reads as an empty tar to libarchive; disk images (UDF, ext...) do.
    if (head.size() >= 1024 && std::all_of(head.begin(), head.begin() + 1024, [](uint8_t b) { return b == 0; })) {
        for (const Handler* h : kHandlers)
            if (h->probe(*s, head, src.name())) return true;
    }
    return false;
}

bool readItem(const Source&, const Item& item, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err) {
    if (!item.read) return true;
    return item.read(sink, err);
}

}  // namespace kz::native
