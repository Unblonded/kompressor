// Reading side: listing, exploring, extracting and testing - libarchive for every
// third-party format, the native KMP reader for Kompressor archives.
#include "kz_archive.hpp"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>

#include "kmp.hpp"

namespace kz {

namespace {

}  // namespace

uint64_t sourceSize(const Source& src) {
    if (src.inMemory()) return src.mem->size();
    uint64_t total = 0;
    auto vols = volumeSet(src.path);
    if (vols.empty()) vols.push_back(src.path);
    for (auto& v : vols) {
        std::error_code ec;
        auto s = fs::file_size(u8path(v), ec);
        if (!ec) total += s;
    }
    return total;
}

int openReadSource(archive* r, const Source& src) {
    if (src.inMemory()) return archive_read_open_memory(r, src.mem->data(), src.mem->size());
    auto vols = volumeSet(src.path);
    if (!vols.empty()) {
#ifdef _WIN32
        std::vector<std::wstring> w;
        for (auto& v : vols) w.push_back(u8path(v).wstring());
        std::vector<const wchar_t*> pw;
        for (auto& v : w) pw.push_back(v.c_str());
        pw.push_back(nullptr);
        return archive_read_open_filenames_w(r, pw.data(), 1 << 20);
#else
        std::vector<const char*> pv;
        for (auto& v : vols) pv.push_back(v.c_str());
        pv.push_back(nullptr);
        return archive_read_open_filenames(r, pv.data(), 1 << 20);
#endif
    }
#ifdef _WIN32
    return archive_read_open_filename_w(r, u8path(src.path).wstring().c_str(), 1 << 20);
#else
    return archive_read_open_filename(r, src.path.c_str(), 1 << 20);
#endif
}

namespace {

const char* laError(archive* a) {
    const char* e = archive_error_string(a);
    return e ? e : "unknown error";
}

bool mentionsPassword(const std::string& e) {
    std::string l = e;
    for (auto& c : l) c = char(std::tolower((unsigned char)c));
    return l.find("passphrase") != std::string::npos || l.find("password") != std::string::npos ||
           l.find("encrypt") != std::string::npos || l.find("decrypt") != std::string::npos;
}

// RAII wrapper around a libarchive reader that knows whether it fell back to "raw" mode
// (a single compressed stream such as foo.txt.gz).
struct LaReader {
    archive* a = nullptr;
    bool raw = false;
    std::string rawName;
    ~LaReader() {
        if (a) archive_read_free(a);
    }

    bool open(const Source& src, const std::string& password, std::string& err) {
        std::string firstErr;
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool tryRaw = attempt == 1;
            archive* r = archive_read_new();
            archive_read_support_filter_all(r);
            if (tryRaw)
                archive_read_support_format_raw(r);
            else {
                archive_read_support_format_all(r);
                // CPIO and ustar/GNU TAR names carry no charset marker; libarchive would decode
                // them with the local code page (OEM/ANSI on Windows). Treat them as UTF-8, which
                // is what Linux/macOS tools and our own writer produce. PAX/ZIP/7Z are unaffected.
                archive_read_set_options(r, "cpio:hdrcharset=UTF-8,tar:hdrcharset=UTF-8");
            }
            if (!password.empty()) archive_read_add_passphrase(r, password.c_str());
            int rc = openReadSource(r, src);
            if (rc == ARCHIVE_OK && tryRaw && archive_filter_count(r) <= 1) {
                // Raw mode accepts anything; only keep it if a decompression filter matched.
                rc = ARCHIVE_FATAL;
            }
            if (rc == ARCHIVE_OK) {
                a = r;
                raw = tryRaw;
                if (raw) {
                    rawName = stripArchiveExt(src.name());
                    if (rawName.empty() || rawName == src.name()) rawName = src.name() + ".out";
                }
                return true;
            }
            if (!tryRaw) firstErr = laError(r);
            archive_read_free(r);
        }
        err = firstErr.empty() ? "unrecognized archive format" : firstErr;
        if (err.find("Unrecognized archive format") != std::string::npos)
            err = "Not a supported archive (unrecognized format)";
        return false;
    }

    std::string entryPath(archive_entry* e) const {
        if (raw) return rawName;
        const char* p = archive_entry_pathname_utf8(e);
        if (!p) p = archive_entry_pathname(e);
        if (!p) return {};
        std::string s = toNFC(p);
        std::replace(s.begin(), s.end(), '\\', '/');
        while (!s.empty() && s.back() == '/') s.pop_back();
        while (s.size() >= 2 && s[0] == '.' && s[1] == '/') s.erase(0, 2);
        return s;
    }
};

bool selected(const std::string& path, const ExtractOptions& opt) {
    if (opt.only.empty()) return true;
    for (auto& s : opt.only) {
        if (path == s) return true;
        if (path.size() > s.size() && path.compare(0, s.size(), s) == 0 && path[s.size()] == '/') return true;
    }
    return false;
}

// Maps an archive path to the safe relative output path; false = skip this entry.
bool outputRel(const std::string& path, const ExtractOptions& opt, std::string& rel, Progress& prog) {
    std::string clean;
    if (!sanitizeRelativePath(path, clean)) {
        if (!path.empty()) prog.warn("Skipped unsafe path: " + path);
        return false;
    }
    if (!selected(clean, opt)) return false;
    if (!opt.stripPrefix.empty()) {
        const std::string& p = opt.stripPrefix;
        if (clean == p) return false;  // the folder itself: its contents land in destDir
        if (clean.size() > p.size() && clean.compare(0, p.size(), p) == 0 && clean[p.size()] == '/')
            clean = clean.substr(p.size() + 1);
    }
    rel = clean;
    return true;
}

fs::path canonicalDest(const std::string& dest) {
    std::error_code ec;
    fs::path d = u8path(dest);
    fs::create_directories(d, ec);
    fs::path c = fs::weakly_canonical(d, ec);
    return ec ? fs::absolute(d) : c;
}

// ---------------------------------------------------------------- KMP backends
bool kmpList(const Source& src, ArchiveInfo& out, std::string& err) {
    kmp::Reader r;
    if (!r.open(src, err)) return false;
    out.isKmp = true;
    out.format = std::string("Kompressor KMP (") + kmp::modeName(r.mode()) + ")";
    out.filter = "dedup + LZMA2/Zstd";
    out.archiveSize = r.archiveSize();
    out.extra = r.stats().summary();
    for (auto& e : r.entries()) {
        Entry x;
        x.path = e.path;
        x.size = e.size;
        x.mtime = e.mtime;
        x.mode = e.mode;
        x.isDir = e.type == kmp::EntryType::Dir;
        x.isSymlink = e.type == kmp::EntryType::Symlink;
        x.linkTarget = e.link;
        out.totalSize += e.size;
        out.entries.push_back(std::move(x));
    }
    return true;
}

bool kmpExtract(const Source& src, const ExtractOptions& opt, Progress& prog, std::string& err) {
    kmp::Reader r;
    if (!r.open(src, err)) return false;
    if (opt.only.empty()) r.setSequential(&prog);  // whole archive: decode blocks ahead on every core
    fs::path dest = canonicalDest(opt.destDir);
    const auto& ents = r.entries();

    struct Job { size_t idx; std::string rel; };
    std::vector<Job> dirs, files, links;
    uint64_t total = 0;
    for (size_t i = 0; i < ents.size(); ++i) {
        std::string rel;
        if (!outputRel(ents[i].path, opt, rel, prog)) continue;
        if (ents[i].type == kmp::EntryType::Dir) dirs.push_back({i, rel});
        else if (ents[i].type == kmp::EntryType::Symlink) links.push_back({i, rel});
        else {
            files.push_back({i, rel});
            total += ents[i].size;
        }
    }
    prog.total = total;
    prog.done = 0;
    // Read files in storage order so consecutive files come from the same decompressed block.
    std::sort(files.begin(), files.end(), [&](const Job& a, const Job& b) {
        uint32_t ca = ents[a.idx].chunks.empty() ? 0 : ents[a.idx].chunks.front();
        uint32_t cb = ents[b.idx].chunks.empty() ? 0 : ents[b.idx].chunks.front();
        return ca < cb;
    });

    std::error_code ec;
    for (auto& j : dirs) fs::create_directories(dest / u8path(j.rel), ec);

    int failures = 0;
    for (auto& j : files) {
        if (prog.cancelled()) {
            err = "Cancelled";
            return false;
        }
        const auto& e = ents[j.idx];
        fs::path target = dest / u8path(j.rel);
        prog.setStatus(j.rel);
        if (opt.skipExisting && fs::exists(target, ec)) {
            prog.done += e.size;
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream o(target, std::ios::binary | std::ios::trunc);
        if (!o) {
            prog.warn("Cannot create " + pathToUtf8(target));
            ++failures;
            continue;
        }
        std::string ferr;
        bool ok = r.readFile(j.idx, [&](const uint8_t* p, size_t n) {
            o.write((const char*)p, (std::streamsize)n);
            prog.done += n;
            return bool(o);
        }, &prog, ferr);
        o.close();
        if (prog.cancelled()) {
            fs::remove(target, ec);
            err = "Cancelled";
            return false;
        }
        if (!ok || !o) {
            prog.warn(ferr.empty() ? "Write failed: " + j.rel : ferr);
            ++failures;
            continue;
        }
        setFileMode(target, e.mode);
        setFileMTime(target, e.mtime);
    }
    // Symlinks last, so no later file can be written through one.
    for (auto& j : links) {
        fs::path target = dest / u8path(j.rel);
        fs::create_directories(target.parent_path(), ec);
        fs::remove(target, ec);
        fs::create_symlink(u8path(ents[j.idx].link), target, ec);
        if (ec) prog.warn("Could not create symlink " + j.rel + ": " + ec.message());
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        setFileMode(dest / u8path(it->rel), ents[it->idx].mode);
        setFileMTime(dest / u8path(it->rel), ents[it->idx].mtime);
    }
    if (failures) {
        err = std::to_string(failures) + " file(s) failed to extract";
        return false;
    }
    return true;
}

bool kmpRead(const Source& src, const std::string& path, uint64_t maxBytes, std::vector<uint8_t>& out, bool& truncated,
             Progress& prog, std::string& err) {
    kmp::Reader r;
    if (!r.open(src, err)) return false;
    int idx = r.findEntry(path);
    if (idx < 0) {
        err = "Entry not found: " + path;
        return false;
    }
    out.clear();
    truncated = false;
    const auto& e = r.entries()[(size_t)idx];
    prog.total = std::min<uint64_t>(e.size, maxBytes);
    return r.readFile((size_t)idx, [&](const uint8_t* p, size_t n) {
        size_t room = (size_t)(maxBytes - out.size());
        if (n > room) {
            out.insert(out.end(), p, p + room);
            truncated = true;
            return false;
        }
        out.insert(out.end(), p, p + n);
        prog.done = out.size();
        return true;
    }, &prog, err);
}

bool kmpTest(const Source& src, Progress& prog, std::string& err) {
    kmp::Reader r;
    if (!r.open(src, err)) return false;
    r.setSequential(&prog);
    uint64_t total = 0;
    for (auto& e : r.entries()) total += e.size;
    prog.total = total;
    int bad = 0;
    for (size_t i = 0; i < r.entries().size(); ++i) {
        const auto& e = r.entries()[i];
        if (e.type != kmp::EntryType::File) continue;
        prog.setStatus(e.path);
        std::string ferr;
        if (!r.readFile(i, [&](const uint8_t*, size_t n) { prog.done += n; return true; }, &prog, ferr)) {
            if (prog.cancelled()) { err = "Cancelled"; return false; }
            prog.warn(ferr);
            ++bad;
        }
    }
    if (bad) {
        err = std::to_string(bad) + " file(s) are damaged";
        return false;
    }
    return true;
}

}  // namespace

// ============================================================================ public API
bool probeArchive(const Source& src, std::string& formatName) {
    if (kmp::isKmp(src)) {
        formatName = "KMP";
        return true;
    }
    LaReader r;
    std::string err;
    if (!r.open(src, "", err)) return false;
    archive_entry* e;
    int rc = archive_read_next_header(r.a, &e);
    if (rc != ARCHIVE_OK && rc != ARCHIVE_WARN && rc != ARCHIVE_EOF) {
        // Encrypted 7z headers etc. still count as a recognised archive.
        if (!mentionsPassword(laError(r.a))) return false;
    }
    formatName = archive_format_name(r.a) ? archive_format_name(r.a) : "archive";
    return true;
}

bool listArchive(const Source& src, ArchiveInfo& out, const std::string& password, Progress& prog, std::string& err) {
    out = ArchiveInfo();
    if (kmp::isKmp(src)) return kmpList(src, out, err);

    LaReader r;
    if (!r.open(src, password, err)) {
        if (mentionsPassword(err)) prog.needPassword = true;
        return false;
    }
    out.archiveSize = sourceSize(src);
    prog.total = out.archiveSize;
    archive_entry* e;
    int rc;
    while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (prog.cancelled()) {
            err = "Cancelled";
            return false;
        }
        Entry x;
        x.path = r.entryPath(e);
        if (x.path.empty()) continue;
        x.size = archive_entry_size_is_set(e) ? (uint64_t)archive_entry_size(e) : 0;
        x.mtime = archive_entry_mtime_is_set(e) ? (int64_t)archive_entry_mtime(e) : 0;
        x.mode = (uint32_t)archive_entry_perm(e);
        auto ft = archive_entry_filetype(e);
        x.isDir = ft == AE_IFDIR;
        x.isSymlink = ft == AE_IFLNK;
        if (x.isSymlink) {
            const char* t = archive_entry_symlink_utf8(e);
            if (!t) t = archive_entry_symlink(e);
            if (t) x.linkTarget = toNFC(t);
        }
        x.encrypted = archive_entry_is_encrypted(e);
        if (x.encrypted) out.hasEncrypted = true;
        if (r.raw && x.size == 0) {
            // Raw streams have no size header: measure by decompressing.
            const void* buf;
            size_t n;
            la_int64_t off;
            while (archive_read_data_block(r.a, &buf, &n, &off) == ARCHIVE_OK) {
                x.size += n;
                if (prog.cancelled()) break;
            }
        }
        out.totalSize += x.size;
        out.entries.push_back(std::move(x));
        prog.done = (uint64_t)archive_filter_bytes(r.a, -1);
        if (out.entries.size() % 256 == 0) prog.setStatus(out.entries.back().path);
    }
    const char* fmt = archive_format_name(r.a);
    out.format = fmt ? fmt : "unknown";
    const char* flt = archive_filter_name(r.a, 0);
    out.filter = flt ? flt : "none";
    if (rc != ARCHIVE_EOF) {
        err = laError(r.a);
        if (mentionsPassword(err)) prog.needPassword = true;
        // Keep what was listed; report partial success if anything was read.
        if (out.entries.empty()) return false;
        out.extra = "Listing stopped early: " + err;
    }
    return true;
}

bool extractArchive(const Source& src, const ExtractOptions& opt, Progress& prog, std::string& err) {
    if (kmp::isKmp(src)) return kmpExtract(src, opt, prog, err);

    LaReader r;
    if (!r.open(src, opt.password, err)) {
        if (mentionsPassword(err)) prog.needPassword = true;
        return false;
    }
    fs::path dest = canonicalDest(opt.destDir);
    prog.total = sourceSize(src);
    prog.done = 0;

    archive* w = archive_write_disk_new();
    int flags = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                ARCHIVE_EXTRACT_SECURE_NODOTDOT;
    if (opt.skipExisting) flags |= ARCHIVE_EXTRACT_NO_OVERWRITE;
    archive_write_disk_set_options(w, flags);
    archive_write_disk_set_standard_lookup(w);

    archive_entry* e;
    int rc;
    int failures = 0;
    bool ok = true;
    while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (prog.cancelled()) {
            err = "Cancelled";
            ok = false;
            break;
        }
        std::string path = r.entryPath(e);
        std::string rel;
        if (!outputRel(path, opt, rel, prog)) continue;
        prog.setStatus(rel);
        fs::path full = dest / u8path(rel);
#ifdef _WIN32
        archive_entry_copy_pathname_w(e, full.wstring().c_str());
#else
        archive_entry_set_pathname(e, full.c_str());
#endif
        if (r.raw) {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0644);
        }
        if (const char* hl = archive_entry_hardlink(e)) {
            std::string hrel;
            std::string hs = hl;
            if (outputRel(hs, ExtractOptions{"", {}, opt.stripPrefix, "", false}, hrel, prog)) {
                fs::path hfull = dest / u8path(hrel);
#ifdef _WIN32
                archive_entry_copy_hardlink_w(e, hfull.wstring().c_str());
#else
                archive_entry_set_hardlink(e, hfull.c_str());
#endif
            } else {
                prog.warn("Skipped unsafe hard link: " + rel);
                continue;
            }
        }
        int wr = archive_write_header(w, e);
        if (wr < ARCHIVE_WARN) {
            std::string m = laError(w);
            if (!(opt.skipExisting && m.find("exists") != std::string::npos)) {
                prog.warn(rel + ": " + m);
                ++failures;
            }
            continue;
        }
        if (archive_entry_size(e) > 0 || r.raw) {
            const void* buf;
            size_t n;
            la_int64_t off;
            int dr;
            while ((dr = archive_read_data_block(r.a, &buf, &n, &off)) == ARCHIVE_OK) {
                if (archive_write_data_block(w, buf, n, off) < ARCHIVE_WARN) {
                    prog.warn(rel + ": " + laError(w));
                    ++failures;
                    break;
                }
                prog.done = (uint64_t)archive_filter_bytes(r.a, -1);
                if (prog.cancelled()) break;
            }
            if (dr < ARCHIVE_WARN) {
                std::string m = laError(r.a);
                if (mentionsPassword(m) || archive_entry_is_encrypted(e)) {
                    prog.needPassword = true;
                    err = opt.password.empty() ? "This archive is encrypted - a password is required"
                                               : "Wrong password (" + m + ")";
                    archive_write_finish_entry(w);
                    ok = false;
                    break;
                }
                prog.warn(rel + ": " + m);
                ++failures;
            }
        }
        archive_write_finish_entry(w);
        prog.done = (uint64_t)archive_filter_bytes(r.a, -1);
    }
    if (ok && rc != ARCHIVE_EOF && rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
        err = laError(r.a);
        if (mentionsPassword(err)) prog.needPassword = true;
        ok = false;
    }
    archive_write_close(w);
    archive_write_free(w);
    if (ok && failures) {
        err = std::to_string(failures) + " item(s) could not be extracted";
        ok = false;
    }
    if (ok) prog.done = prog.total.load();
    return ok;
}

bool readEntry(const Source& src, const std::string& entryPath, const std::string& password, uint64_t maxBytes,
               std::vector<uint8_t>& out, bool& truncated, Progress& prog, std::string& err) {
    if (kmp::isKmp(src)) return kmpRead(src, entryPath, maxBytes, out, truncated, prog, err);

    out.clear();
    truncated = false;
    LaReader r;
    if (!r.open(src, password, err)) {
        if (mentionsPassword(err)) prog.needPassword = true;
        return false;
    }
    prog.total = sourceSize(src);
    archive_entry* e;
    int rc;
    while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (prog.cancelled()) {
            err = "Cancelled";
            return false;
        }
        prog.done = (uint64_t)archive_filter_bytes(r.a, -1);
        if (r.entryPath(e) != entryPath) continue;
        uint64_t sz = archive_entry_size_is_set(e) ? (uint64_t)archive_entry_size(e) : 0;
        if (sz) out.reserve((size_t)std::min(sz, maxBytes));
        std::vector<uint8_t> buf(1 << 16);
        for (;;) {
            if (prog.cancelled()) {
                err = "Cancelled";
                return false;
            }
            la_ssize_t n = archive_read_data(r.a, buf.data(), buf.size());
            if (n < 0) {
                err = laError(r.a);
                if (mentionsPassword(err) || archive_entry_is_encrypted(e)) {
                    prog.needPassword = true;
                    if (password.empty()) err = "This entry is encrypted - a password is required";
                }
                return false;
            }
            if (n == 0) break;
            size_t room = (size_t)(maxBytes - out.size());
            if ((size_t)n > room) {
                out.insert(out.end(), buf.begin(), buf.begin() + room);
                truncated = true;
                break;
            }
            out.insert(out.end(), buf.begin(), buf.begin() + n);
        }
        return true;
    }
    err = rc == ARCHIVE_EOF ? "Entry not found: " + entryPath : laError(r.a);
    return false;
}

bool testArchive(const Source& src, const std::string& password, Progress& prog, std::string& err) {
    if (kmp::isKmp(src)) return kmpTest(src, prog, err);

    LaReader r;
    if (!r.open(src, password, err)) {
        if (mentionsPassword(err)) prog.needPassword = true;
        return false;
    }
    prog.total = sourceSize(src);
    archive_entry* e;
    int rc, bad = 0;
    while ((rc = archive_read_next_header(r.a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        std::string path = r.entryPath(e);
        prog.setStatus(path);
        const void* buf;
        size_t n;
        la_int64_t off;
        int dr;
        while ((dr = archive_read_data_block(r.a, &buf, &n, &off)) == ARCHIVE_OK) {
            prog.done = (uint64_t)archive_filter_bytes(r.a, -1);
            if (prog.cancelled()) {
                err = "Cancelled";
                return false;
            }
        }
        if (dr < ARCHIVE_WARN) {
            std::string m = laError(r.a);
            if (mentionsPassword(m) || archive_entry_is_encrypted(e)) prog.needPassword = true;
            prog.warn(path + ": " + m);
            ++bad;
            if (dr == ARCHIVE_FATAL) break;
        }
    }
    if (rc != ARCHIVE_EOF && rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
        err = laError(r.a);
        return false;
    }
    if (bad) {
        err = std::to_string(bad) + " item(s) failed the integrity test";
        return false;
    }
    prog.done = prog.total.load();
    return true;
}

std::string libraryVersions() {
    std::string s = archive_version_details();
    return s;
}

}  // namespace kz
