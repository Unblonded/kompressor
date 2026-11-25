#include "kz_search.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>

#include "kz_archive.hpp"

namespace kz {

namespace {

inline uint8_t fold(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }

// Case folding over raw bytes, for text in UTF-8 or UTF-16LE: ASCII letters, and the accented Latin
// capitals U+00C0-U+00DE (but not U+00D7, the multiplication sign). In UTF-8 those are C3 80-9E; in
// UTF-16 they're C0-DE followed by a zero byte (which UTF-8 lead bytes never are).
void foldBytes(std::vector<uint8_t>& v) {
    for (size_t i = 0; i < v.size(); ++i) {
        uint8_t c = v[i];
        if (c >= 'A' && c <= 'Z') v[i] = (uint8_t)(c + 32);
        else if (c >= 0x80 && c <= 0x9E && c != 0x97 && i && v[i - 1] == 0xC3) v[i] = (uint8_t)(c + 0x20);
        else if (c >= 0xC0 && c <= 0xDE && c != 0xD7 && i + 1 < v.size() && v[i + 1] == 0) v[i] = (uint8_t)(c + 0x20);
    }
}

int hexDigit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// UTF-8 text as UTF-16LE bytes (invalid bytes pass through as their own code units).
std::vector<uint8_t> toUtf16(const std::string& s) {
    std::vector<uint8_t> out;
    auto unit = [&](uint32_t u) { out.push_back((uint8_t)u), out.push_back((uint8_t)(u >> 8)); };
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp = c;
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n > 1 && i + n <= s.size()) {
            cp = c & (0x7F >> n);
            for (int k = 1; k < n; ++k) cp = cp << 6 | ((uint8_t)s[i + k] & 0x3F);
        } else {
            n = 1;
        }
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            unit(0xD800 | (cp >> 10));
            unit(0xDC00 | (cp & 0x3FF));
        } else {
            unit(cp);
        }
    }
    return out;
}

// Bytes made readable on one line: valid UTF-8 kept, tabs as spaces, anything else as '.'.
std::string printable(const uint8_t* p, size_t n, bool asciiOnly = false) {
    std::string s;
    for (size_t i = 0; i < n;) {
        uint8_t c = p[i];
        if (c == '\t') {
            s += ' ', ++i;
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            s += (char)c, ++i;
            continue;
        }
        int len = asciiOnly ? 0 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len && i + len <= n;
        for (int k = 1; ok && k < len; ++k) ok = (p[i + k] & 0xC0) == 0x80;
        if (ok) {
            s.append((const char*)p + i, len);
            i += len;
        } else {
            s += '.', ++i;
        }
    }
    return s;
}

std::string printable16(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = p[i] | p[i + 1] << 8;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {
            uint32_t lo = p[i + 2] | p[i + 3] << 8;
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (u == '\t') u = ' ';
        if (u < 0x20 || (u >= 0x7F && u < 0xA0) || (u >= 0xD800 && u < 0xE000)) {
            s += '.';
            continue;
        }
        char b[4];
        int k = 0;
        if (u < 0x80) b[k++] = (char)u;
        else if (u < 0x800) b[k++] = (char)(0xC0 | u >> 6), b[k++] = (char)(0x80 | (u & 0x3F));
        else if (u < 0x10000) b[k++] = (char)(0xE0 | u >> 12), b[k++] = (char)(0x80 | ((u >> 6) & 0x3F)), b[k++] = (char)(0x80 | (u & 0x3F));
        else
            b[k++] = (char)(0xF0 | u >> 18), b[k++] = (char)(0x80 | ((u >> 12) & 0x3F)), b[k++] = (char)(0x80 | ((u >> 6) & 0x3F)),
            b[k++] = (char)(0x80 | (u & 0x3F));
        s.append(b, k);
    }
    return s;
}

bool globMatch(const char* g, const char* s) {
    const char *star = nullptr, *back = nullptr;
    while (*s) {
        if (*g == '?' || (*g && fold((uint8_t)*g) == fold((uint8_t)*s))) {
            ++g, ++s;
        } else if (*g == '*') {
            star = ++g, back = s;
        } else if (star) {
            g = star, s = ++back;
        } else {
            return false;
        }
    }
    while (*g == '*') ++g;
    return !*g;
}

// Finds a file's matches as its bytes stream past. Keeps a window of recent bytes so a match, and
// the line around it, can straddle the pieces the data arrives in.
class Matcher {
public:
    Matcher(const SearchOptions& o, const std::vector<uint8_t>& pat, FileHits& out) : o_(o), out_(out) {
        pats_.push_back({pat, false});
        if (!o.hex) {
            std::vector<uint8_t> w = toUtf16(std::string(pat.begin(), pat.end()));
            if (w.size() > 0) pats_.push_back({w, true});
        }
        if (!o.matchCase && !o.hex)
            for (auto& p : pats_) foldBytes(p.bytes);
        for (auto& p : pats_) maxPat_ = std::max(maxPat_, p.bytes.size());
        keep_ = maxPat_ + kContext;
    }

    void feed(const uint8_t* p, size_t n) {
        if (!sniffed_) {
            head_.insert(head_.end(), p, p + std::min(n, (size_t)8192 - std::min(head_.size(), (size_t)8192)));
            if (head_.size() >= 8192) sniff();
        }
        buf_.insert(buf_.end(), p, p + n);
        if (buf_.size() >= (1u << 20) + keep_) process(false);
    }
    void finish() {
        if (!sniffed_) sniff();
        process(true);
    }

private:
    static constexpr size_t kContext = 160;
    struct Pat {
        std::vector<uint8_t> bytes;
        bool utf16;
    };

    void sniff() {
        sniffed_ = true;
        size_t zeros = 0, oddZeros = 0;
        for (size_t i = 0; i < head_.size(); ++i)
            if (!head_[i]) ++zeros, oddZeros += i & 1;
        bool bom16 = head_.size() >= 2 && head_[0] == 0xFF && head_[1] == 0xFE;
        utf16File_ = bom16 || (zeros > head_.size() / 4 && oddZeros * 10 >= zeros * 9);
        out_.binary = zeros && !utf16File_;
        head_.clear();
        head_.shrink_to_fit();
    }

    void countLines(size_t upto) {
        for (; lineAt_ < upto; ++lineAt_)
            if (buf_[lineAt_] == '\n') ++line_;
    }

    void process(bool final) {
        if (!sniffed_) sniff();
        size_t limit = final ? buf_.size() : buf_.size() - keep_;
        if (limit > scanned_) {
            const std::vector<uint8_t>* hay = &buf_;
            if (!o_.matchCase && !o_.hex) {
                folded_ = buf_;
                foldBytes(folded_);
                hay = &folded_;
            }
            // Every match starting in [scanned_, limit), in file order; at one spot, the longer
            // (UTF-16) form wins.
            std::vector<std::pair<size_t, int>> found;
            for (int k = 0; k < (int)pats_.size(); ++k) {
                const auto& pb = pats_[(size_t)k].bytes;
                if (pb.empty() || pb.size() > hay->size()) continue;
                auto it = hay->begin() + (ptrdiff_t)scanned_;
                auto stop = hay->begin() + (ptrdiff_t)std::min(limit + pb.size() - 1, hay->size());
                std::boyer_moore_horspool_searcher<std::vector<uint8_t>::const_iterator> bm(pb.begin(), pb.end());
                while (true) {
                    auto r = std::search(it, stop, bm);
                    if (r == stop) break;
                    size_t at = (size_t)(r - hay->begin());
                    if (at >= limit) break;
                    if (!pats_[(size_t)k].utf16 || ((base_ + at) % 2 == 0 || !utf16File_)) found.push_back({at, k});
                    it = r + (ptrdiff_t)pb.size();
                }
            }
            std::sort(found.begin(), found.end());
            for (size_t i = 0; i < found.size(); ++i) {
                if (i + 1 < found.size() && found[i + 1].first == found[i].first) continue;  // keep the later (UTF-16) one
                if (i > 0 && found[i - 1].first == found[i].first && pats_[(size_t)found[i].second].utf16 == false) continue;
                hit(found[i].first, pats_[(size_t)found[i].second]);
            }
            scanned_ = limit;
        }
        // Drop what's behind, keeping some for the next snippet's start.
        if (!final && scanned_ > kContext) {
            size_t drop = scanned_ - kContext;
            countLines(drop);
            buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)drop);
            base_ += drop;
            scanned_ -= drop;
            lineAt_ -= drop;
        }
    }

    void hit(size_t at, const Pat& p) {
        out_.count++;
        if (out_.hits.size() >= o_.maxHitsPerFile) return;
        SearchHit h;
        h.offset = base_ + at;
        h.utf16 = p.utf16;
        size_t len = p.bytes.size();
        if (out_.binary) {
            size_t s = at >= 24 ? at - 24 : 0, e = std::min(buf_.size(), at + len + 24);
            std::string a = p.utf16 ? printable16(&buf_[s + ((at - s) & 1)], at - s - ((at - s) & 1)) : printable(&buf_[s], at - s, true);
            std::string m = p.utf16 ? printable16(&buf_[at], len) : printable(&buf_[at], len, true);
            std::string b = p.utf16 ? printable16(&buf_[at + len], (e - at - len) & ~(size_t)1) : printable(&buf_[at + len], e - at - len, true);
            h.snippet = a + m + b;
            h.matchAt = (uint32_t)a.size(), h.matchLen = (uint32_t)m.size();
        } else {
            countLines(at);
            h.line = line_ + 1;
            size_t step = p.utf16 ? 2 : 1, s = at, e = at + len;
            auto isEol = [&](size_t i) { return buf_[i] == '\n' || buf_[i] == '\r'; };
            while (s >= step && at - s < 120 && !(isEol(s - step) && (step == 1 || buf_[s - 1] == 0))) s -= step;
            while (e + step <= buf_.size() && e - at - len < 120 && !isEol(e)) e += step;
            auto text = [&](size_t a, size_t n) { return p.utf16 ? printable16(&buf_[a], n) : printable(&buf_[a], n); };
            std::string a = text(s, at - s), m = text(at, len), b = text(at + len, e - at - len);
            size_t lead = a.find_first_not_of(' ');
            a = lead == std::string::npos ? std::string() : a.substr(lead);
            h.snippet = a + m + b;
            h.matchAt = (uint32_t)a.size(), h.matchLen = (uint32_t)m.size();
        }
        out_.hits.push_back(std::move(h));
    }

    const SearchOptions& o_;
    FileHits& out_;
    std::vector<Pat> pats_;
    size_t maxPat_ = 0, keep_ = 0;
    std::vector<uint8_t> buf_, folded_, head_;
    uint64_t base_ = 0;
    size_t scanned_ = 0, lineAt_ = 0;
    uint32_t line_ = 0;
    bool sniffed_ = false, utf16File_ = false;
};

}  // namespace

bool searchPattern(const SearchOptions& o, std::vector<uint8_t>& bytes, std::string& err) {
    bytes.clear();
    if (!o.hex) {
        bytes.assign(o.text.begin(), o.text.end());
        if (bytes.empty()) return err = "Type something to search for", false;
        return true;
    }
    std::string h;
    for (size_t i = 0; i < o.text.size(); ++i) {
        char c = o.text[i];
        if (c == '0' && i + 1 < o.text.size() && (o.text[i + 1] == 'x' || o.text[i + 1] == 'X')) {
            ++i;
            continue;
        }
        if (c == ' ' || c == ',' || c == '\t' || c == '-' || c == ':') continue;
        if (hexDigit(c) < 0) return err = std::string("Not a hex digit: '") + c + "'", false;
        h += c;
    }
    if (h.empty()) return err = "Type the bytes to search for, in hex (4D 5A 90)", false;
    if (h.size() % 2) return err = "Hex needs two digits per byte", false;
    for (size_t i = 0; i < h.size(); i += 2) bytes.push_back((uint8_t)(hexDigit(h[i]) << 4 | hexDigit(h[i + 1])));
    return true;
}

bool nameMatches(const std::string& path, const std::string& globs) {
    size_t slash = path.rfind('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    bool any = false;
    size_t at = 0;
    while (at <= globs.size()) {
        size_t end = globs.find_first_of(";,", at);
        std::string g = globs.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? globs.size() + 1 : end + 1;
        size_t a = g.find_first_not_of(' '), b = g.find_last_not_of(' ');
        if (a == std::string::npos) continue;
        g = g.substr(a, b - a + 1);
        any = true;
        if (globMatch(g.c_str(), name.c_str()) || globMatch(g.c_str(), path.c_str())) return true;
    }
    return !any;
}

bool searchArchive(const Source& src, const std::vector<std::string>& only, const std::string& password,
                   const SearchOptions& o, const std::function<void(FileHits&&)>& onFile, Progress& prog,
                   std::string& err) {
    std::vector<uint8_t> pat;
    if (!searchPattern(o, pat, err)) return false;
    auto wanted = [&](const std::string& path) {
        if (!nameMatches(path, o.names)) return false;
        if (only.empty()) return true;
        for (auto& w : only)
            if (path == w || (path.size() > w.size() && path.compare(0, w.size(), w) == 0 && path[w.size()] == '/')) return true;
        return false;
    };
    FileHits cur;
    std::unique_ptr<Matcher> m;
    EntryVisitor v;
    v.begin = [&](const std::string& path, uint64_t size) {
        if (!wanted(path)) return false;
        cur = FileHits();
        cur.path = path;
        cur.size = size;
        m = std::make_unique<Matcher>(o, pat, cur);
        return true;
    };
    v.data = [&](const uint8_t* p, size_t n) {
        m->feed(p, n);
        return !prog.cancelled();
    };
    v.end = [&](bool ok, const std::string& e) {
        m->finish();
        if (!ok) cur.error = e.empty() ? "Could not be read" : e;
        if (cur.count || !cur.error.empty()) onFile(std::move(cur));
        m.reset();
    };
    return visitEntries(src, password, v, prog, err);
}

}  // namespace kz
