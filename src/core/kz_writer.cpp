// Writing side: gathering inputs, creating standard archives with libarchive,
// and super-compressing into KMP (from files or straight from another archive).
#include <archive.h>
#include <archive_entry.h>
#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <fstream>

#include "kmp.hpp"
#include "kz_archive.hpp"

namespace kz {

namespace {

constexpr size_t kIoBuf = 4 << 20;

std::string tmpPathFor(const std::string& out) { return out + ".kztmp"; }

bool commitTemp(const std::string& tmp, const std::string& out, std::string& err) {
    std::error_code ec;
    fs::path t = u8path(tmp), o = u8path(out);
    fs::remove(o, ec);
    fs::rename(t, o, ec);
    if (ec) {
        err = "Could not move archive into place: " + ec.message();
        fs::remove(t, ec);
        return false;
    }
    return true;
}

void discardTemp(const std::string& tmp) {
    std::error_code ec;
    fs::remove(u8path(tmp), ec);
}

uint64_t fileSize(const std::string& p) {
    std::error_code ec;
    auto s = fs::file_size(u8path(p), ec);
    return ec ? 0 : (uint64_t)s;
}

void fillResult(CompressResult& res, const std::vector<InputItem>& items, const std::string& out) {
    for (auto& i : items)
        if (!i.isDir && !i.isSymlink) {
            res.inputBytes += i.size;
            res.files++;
        }
    res.outputBytes = fileSize(out);
}

std::string ratioText(uint64_t in, uint64_t out) {
    char buf[128];
    double r = in ? 100.0 * double(out) / double(in) : 0.0;
    std::snprintf(buf, sizeof buf, "%s -> %s (%.1f%% of original)", humanSize(in).c_str(), humanSize(out).c_str(), r);
    return buf;
}

// ---------------------------------------------------------------- KMP from disk
bool createKmp(const std::vector<InputItem>& items, const std::string& outPath, const CompressOptions& opt,
               Progress& prog, CompressResult& res, std::string& err) {
    kmp::Options ko;
    ko.mode = (kmp::Mode)std::clamp(opt.level < 0 ? 2 : opt.level, 0, 3);
    ko.threads = opt.threads;
    ko.dedup = opt.dedup;

    std::string tmp = tmpPathFor(outPath);
    kmp::Writer w;
    w.setProgress(&prog);
    if (!w.open(u8path(tmp), ko, err)) return false;

    // Directories and links first, then files grouped by extension (similar data -> same block).
    std::vector<const InputItem*> files;
    for (auto& i : items) {
        if (i.isDir) w.addDir(i.arcPath, i.mode, i.mtime);
        else if (i.isSymlink) w.addSymlink(i.arcPath, i.linkTarget, i.mode, i.mtime);
        else files.push_back(&i);
    }
    std::stable_sort(files.begin(), files.end(), [](const InputItem* a, const InputItem* b) {
        std::string ea = lowerExt(a->arcPath), eb = lowerExt(b->arcPath);
        if (ea != eb) return ea < eb;
        std::string na = fileNameOf(a->arcPath), nb = fileNameOf(b->arcPath);
        if (na != nb) return na < nb;
        return a->arcPath < b->arcPath;
    });

    uint64_t total = 0;
    for (auto* f : files) total += f->size;
    prog.total = total;
    prog.done = 0;

    std::vector<char> buf(kIoBuf);
    for (auto* f : files) {
        if (prog.cancelled()) {
            w.abort();
            err = "Cancelled";
            return false;
        }
        prog.setStatus(f->arcPath);
        std::ifstream in(f->source, std::ios::binary);
        if (!in) {
            prog.warn("Cannot read " + pathToUtf8(f->source) + " (skipped)");
            prog.done += f->size;
            continue;
        }
        w.beginFile(f->arcPath, f->mode, f->mtime);
        while (in) {
            in.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            if (!w.write(buf.data(), (size_t)n)) break;
            prog.done += (uint64_t)n;
            if (prog.cancelled()) break;
        }
        if (!w.endFile()) {
            err = w.error();
            w.abort();
            return false;
        }
    }
    if (prog.cancelled()) {
        w.abort();
        err = "Cancelled";
        return false;
    }
    if (!w.finish(err)) return false;
    if (!commitTemp(tmp, outPath, err)) return false;
    fillResult(res, items, outPath);
    res.summary = w.stats().summary();
    return true;
}

// ---------------------------------------------------------------- libarchive writers
bool setOpt(archive* a, const std::string& o, std::string& err, bool required = true) {
    int r = archive_write_set_options(a, o.c_str());
    if (r < ARCHIVE_WARN && required) {
        const char* e = archive_error_string(a);
        err = "Option '" + o + "' rejected: " + (e ? e : "unsupported");
        return false;
    }
    return true;
}

bool configureWriter(archive* a, const CompressOptions& opt, std::string& err) {
    const FormatDesc& fd = formatDesc(opt.format);
    int level = opt.level < 0 ? fd.defLevel : std::clamp(opt.level, fd.minLevel, fd.maxLevel);
    std::string lv = std::to_string(level);
    std::string th = std::to_string(opt.threads > 0 ? opt.threads : (int)hardwareThreads());

    auto addFilter = [&](OutFormat f) -> bool {
        switch (f) {
            case OutFormat::TarGz: case OutFormat::Gz:
                archive_write_add_filter_gzip(a);
                return setOpt(a, "gzip:compression-level=" + lv, err);
            case OutFormat::TarBz2: case OutFormat::Bz2:
                archive_write_add_filter_bzip2(a);
                return setOpt(a, "bzip2:compression-level=" + lv, err);
            case OutFormat::TarXz: case OutFormat::Xz:
                archive_write_add_filter_xz(a);
                setOpt(a, "xz:threads=" + th, err, false);
                return setOpt(a, "xz:compression-level=" + lv, err);
            case OutFormat::TarZst: case OutFormat::Zst:
                if (archive_write_add_filter_zstd(a) != ARCHIVE_OK) {
                    err = "This libarchive build has no zstd support";
                    return false;
                }
                setOpt(a, "zstd:threads=" + th, err, false);
                return setOpt(a, "zstd:compression-level=" + lv, err);
            case OutFormat::TarLz4: case OutFormat::Lz4:
                if (archive_write_add_filter_lz4(a) != ARCHIVE_OK) {
                    err = "This libarchive build has no lz4 support";
                    return false;
                }
                return setOpt(a, "lz4:compression-level=" + lv, err);
            default:
                return true;
        }
    };

    switch (opt.format) {
        case OutFormat::Zip:
            archive_write_set_format_zip(a);
            if (level == 0) {
                if (!setOpt(a, "zip:compression=store", err)) return false;
            } else {
                if (!setOpt(a, "zip:compression=deflate", err)) return false;
                if (!setOpt(a, "zip:compression-level=" + lv, err)) return false;
            }
            if (!opt.password.empty()) {
                if (archive_write_set_options(a, "zip:encryption=aes256") < ARCHIVE_WARN) {
                    err = "AES-256 ZIP encryption is not available in this build";
                    return false;
                }
                archive_write_set_passphrase(a, opt.password.c_str());
            }
            return true;
        case OutFormat::SevenZip:
            archive_write_set_format_7zip(a);
            if (level == 0) return setOpt(a, "7zip:compression=store", err);
            if (!setOpt(a, "7zip:compression=lzma2", err)) return false;
            return setOpt(a, "7zip:compression-level=" + lv, err);
        case OutFormat::Tar:
            archive_write_set_format_pax_restricted(a);
            return true;
        case OutFormat::TarGz: case OutFormat::TarBz2: case OutFormat::TarXz: case OutFormat::TarZst:
        case OutFormat::TarLz4:
            archive_write_set_format_pax_restricted(a);
            return addFilter(opt.format);
        case OutFormat::Gz: case OutFormat::Bz2: case OutFormat::Xz: case OutFormat::Zst: case OutFormat::Lz4:
            archive_write_set_format_raw(a);
            return addFilter(opt.format);
        case OutFormat::Iso:
            archive_write_set_format_iso9660(a);
            setOpt(a, "iso9660:rockridge=rockridge", err, false);  // keep real permissions
            setOpt(a, "iso9660:joliet=long", err, false);         // long Unicode names for Windows
            return true;
        case OutFormat::Cpio:
            archive_write_set_format_cpio_newc(a);
            return true;
        case OutFormat::Kmp:
            break;
    }
    err = "unsupported output format";
    return false;
}

bool createLibarchive(const std::vector<InputItem>& items, const std::string& outPath, const CompressOptions& opt,
                      Progress& prog, CompressResult& res, std::string& err) {
    const FormatDesc& fd = formatDesc(opt.format);
    if (fd.singleFile) {
        size_t nFiles = 0;
        for (auto& i : items) nFiles += (!i.isDir && !i.isSymlink) ? 1 : 0;
        if (nFiles != 1 || items.size() != 1) {
            err = std::string(fd.name) + " compresses exactly one file. Use TAR." + fd.name +
                  " (or ZIP/7Z/KMP) for folders or several files.";
            return false;
        }
    }
    if (!opt.password.empty() && !fd.password) {
        err = std::string(fd.name) + " does not support passwords - use ZIP for encryption";
        return false;
    }

    archive* a = archive_write_new();
    if (!configureWriter(a, opt, err)) {
        archive_write_free(a);
        return false;
    }
    std::string tmp = tmpPathFor(outPath);
#ifdef _WIN32
    int rc = archive_write_open_filename_w(a, u8path(tmp).wstring().c_str());
#else
    int rc = archive_write_open_filename(a, tmp.c_str());
#endif
    if (rc != ARCHIVE_OK) {
        err = archive_error_string(a) ? archive_error_string(a) : "cannot create output";
        archive_write_free(a);
        return false;
    }

    uint64_t total = 0;
    for (auto& i : items) total += i.size;
    prog.total = total;
    prog.done = 0;

    std::vector<char> buf(kIoBuf);
    bool ok = true;
    for (auto& it : items) {
        if (prog.cancelled()) {
            err = "Cancelled";
            ok = false;
            break;
        }
        prog.setStatus(it.arcPath);
        archive_entry* e = archive_entry_new();
#ifdef _WIN32
        archive_entry_set_pathname_utf8(e, it.arcPath.c_str());
#else
        // The locale is UTF-8 (initLocale), so set the native string directly: on macOS the
        // UTF-8 -> native conversion would decompose names (NFD) and archives would carry them.
        archive_entry_copy_pathname(e, it.arcPath.c_str());
#endif
        archive_entry_set_mtime(e, (time_t)it.mtime, 0);
        if (it.isDir) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, it.mode ? it.mode : 0755);
            archive_entry_set_size(e, 0);
        } else if (it.isSymlink) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_perm(e, 0777);
            archive_entry_set_size(e, 0);
#ifdef _WIN32
            archive_entry_set_symlink_utf8(e, it.linkTarget.c_str());
#else
            archive_entry_copy_symlink(e, it.linkTarget.c_str());
#endif
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, it.mode ? it.mode : 0644);
            archive_entry_set_size(e, (la_int64_t)it.size);
        }
        std::ifstream in;
        if (!it.isDir && !it.isSymlink) {
            in.open(it.source, std::ios::binary);
            if (!in) {
                prog.warn("Cannot read " + pathToUtf8(it.source) + " (skipped)");
                archive_entry_free(e);
                prog.done += it.size;
                continue;
            }
        }
        if (archive_write_header(a, e) < ARCHIVE_WARN) {
            const char* m = archive_error_string(a);
            prog.warn(it.arcPath + ": " + (m ? m : "header error"));
            archive_entry_free(e);
            if (archive_errno(a) != 0 && fd.singleFile) {
                err = m ? m : "write failed";
                ok = false;
                break;
            }
            continue;
        }
        archive_entry_free(e);
        if (in.is_open()) {
            uint64_t written = 0;
            while (in && written < it.size) {
                in.read(buf.data(), (std::streamsize)std::min<uint64_t>(buf.size(), it.size - written));
                std::streamsize n = in.gcount();
                if (n <= 0) break;
                if (archive_write_data(a, buf.data(), (size_t)n) < 0) {
                    err = archive_error_string(a) ? archive_error_string(a) : "write failed";
                    ok = false;
                    break;
                }
                written += (uint64_t)n;
                prog.done += (uint64_t)n;
                if (prog.cancelled()) break;
            }
            if (!ok) break;
            if (written < it.size && !prog.cancelled())
                prog.warn(it.arcPath + ": file shrank while reading (padded)");
        }
    }
    if (ok && !prog.cancelled()) {
        if (opt.format == OutFormat::SevenZip || opt.format == OutFormat::Iso)
            prog.setStatus("Finalizing (compressing solid stream)...");
        if (archive_write_close(a) != ARCHIVE_OK) {
            err = archive_error_string(a) ? archive_error_string(a) : "close failed";
            ok = false;
        }
    } else if (prog.cancelled()) {
        err = "Cancelled";
        ok = false;
    }
    archive_write_free(a);
    if (!ok) {
        discardTemp(tmp);
        return false;
    }
    if (!commitTemp(tmp, outPath, err)) return false;
    fillResult(res, items, outPath);
    res.summary = ratioText(res.inputBytes, res.outputBytes);
    return true;
}

}  // namespace

// ============================================================================ public API
bool gatherInputs(const std::vector<std::string>& inputs, std::vector<InputItem>& out, Progress& prog, std::string& err) {
    out.clear();
    prog.setStatus("Scanning files...");
    for (const auto& raw : inputs) {
        std::string s = raw;
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
        fs::path p = u8path(s);
        FileStat st = statPath(p);
        if (!st.exists) {
            err = "Not found: " + raw;
            return false;
        }
        std::string top = pathToUtf8(p.filename());
        if (top.empty() || top == "." || top == "..") top = pathToUtf8(fs::absolute(p).filename());
        if (top.empty()) top = "root";

        InputItem root;
        root.source = p;
        root.arcPath = top;
        root.isDir = st.isDir;
        root.isSymlink = st.isSymlink;
        root.size = st.size;
        root.mtime = st.mtime;
        root.mode = st.mode;
        if (st.isSymlink) root.linkTarget = readSymlink(p);
        out.push_back(root);
        if (!st.isDir) continue;

        std::error_code ec;
        auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            prog.warn("Cannot read folder " + raw + ": " + ec.message());
            continue;
        }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                prog.warn("Scan error: " + ec.message());
                ec.clear();
                continue;
            }
            if (prog.cancelled()) {
                err = "Cancelled";
                return false;
            }
            const fs::path& ep = it->path();
            FileStat es = statPath(ep);
            if (!es.exists) continue;
            InputItem item;
            item.source = ep;
            std::string rel = pathToUtf8(ep.lexically_relative(p));
            std::replace(rel.begin(), rel.end(), '\\', '/');
            item.arcPath = top + "/" + rel;
            item.isDir = es.isDir;
            item.isSymlink = es.isSymlink;
            item.size = es.size;
            item.mtime = es.mtime;
            item.mode = es.mode;
            if (es.isSymlink) item.linkTarget = readSymlink(ep);
            out.push_back(std::move(item));
            if (out.size() % 1000 == 0) prog.setStatus("Scanning... " + std::to_string(out.size()) + " items");
        }
    }
    return true;
}

bool createArchive(const std::vector<std::string>& inputs, const std::string& outPath, const CompressOptions& opt,
                   Progress& prog, CompressResult& res, std::string& err) {
    res = CompressResult();
    if (inputs.empty()) {
        err = "Nothing to compress";
        return false;
    }
    std::vector<InputItem> items;
    if (!gatherInputs(inputs, items, prog, err)) return false;
    // Never pack the archive into itself.
    std::error_code ec;
    fs::path outAbs = fs::weakly_canonical(u8path(outPath), ec);
    items.erase(std::remove_if(items.begin(), items.end(), [&](const InputItem& i) {
                    std::error_code e2;
                    return fs::weakly_canonical(i.source, e2) == outAbs;
                }),
                items.end());
    if (opt.format == OutFormat::Kmp) return createKmp(items, outPath, opt, prog, res, err);
    return createLibarchive(items, outPath, opt, prog, res, err);
}

bool convertToKmp(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                  Progress& prog, CompressResult& res, std::string& err) {
    res = CompressResult();
    kmp::Options ko;
    ko.mode = (kmp::Mode)std::clamp(opt.level < 0 ? 2 : opt.level, 0, 3);
    ko.threads = opt.threads;
    ko.dedup = opt.dedup;
    std::string tmp = tmpPathFor(outPath);
    kmp::Writer w;
    w.setProgress(&prog);
    if (!w.open(u8path(tmp), ko, err)) return false;

    // KMP -> KMP (re-pack with a different mode).
    if (kmp::isKmp(src)) {
        kmp::Reader r;
        if (!r.open(src, err)) {
            w.abort();
            return false;
        }
        uint64_t total = 0;
        for (auto& e : r.entries()) total += e.size;
        prog.total = total;
        for (size_t i = 0; i < r.entries().size(); ++i) {
            const auto& e = r.entries()[i];
            if (e.type == kmp::EntryType::Dir) w.addDir(e.path, e.mode, e.mtime);
            else if (e.type == kmp::EntryType::Symlink) w.addSymlink(e.path, e.link, e.mode, e.mtime);
            else {
                prog.setStatus(e.path);
                w.beginFile(e.path, e.mode, e.mtime);
                std::string ferr;
                bool ok = r.readFile(i, [&](const uint8_t* p, size_t n) {
                    prog.done += n;
                    return w.write(p, n);
                }, &prog, ferr);
                if (!ok || !w.endFile()) {
                    w.abort();
                    err = ferr.empty() ? w.error() : ferr;
                    return false;
                }
            }
        }
        res.inputBytes = total;
    } else {
        // Any libarchive-readable archive: stream entries straight into the KMP writer.
        ArchiveInfo info;
        archive* a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
        int rc;
        if (src.inMemory())
            rc = archive_read_open_memory(a, src.mem->data(), src.mem->size());
        else
#ifdef _WIN32
            rc = archive_read_open_filename_w(a, u8path(src.path).wstring().c_str(), 1 << 20);
#else
            rc = archive_read_open_filename(a, src.path.c_str(), 1 << 20);
#endif
        if (rc != ARCHIVE_OK) {
            err = archive_error_string(a) ? archive_error_string(a) : "cannot open source archive";
            archive_read_free(a);
            w.abort();
            return false;
        }
        prog.total = src.inMemory() ? src.mem->size() : fileSize(src.path);
        archive_entry* e;
        std::vector<char> buf(1 << 20);
        while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
            if (prog.cancelled()) break;
            const char* pn = archive_entry_pathname_utf8(e);
            if (!pn) pn = archive_entry_pathname(e);
            std::string path;
            if (!pn || !sanitizeRelativePath(toNFC(pn), path)) {
                if (pn) prog.warn("Skipped unsafe path: " + std::string(pn));
                continue;
            }
            uint32_t mode = (uint32_t)archive_entry_perm(e);
            int64_t mt = archive_entry_mtime(e);
            auto ft = archive_entry_filetype(e);
            if (ft == AE_IFDIR) {
                w.addDir(path, mode, mt);
            } else if (ft == AE_IFLNK) {
                const char* t = archive_entry_symlink_utf8(e);
                w.addSymlink(path, t ? toNFC(t) : "", mode, mt);
            } else if (ft == AE_IFREG || ft == 0) {
                prog.setStatus(path);
                w.beginFile(path, mode, mt);
                la_ssize_t n;
                while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
                    res.inputBytes += (uint64_t)n;
                    if (!w.write(buf.data(), (size_t)n)) break;
                    prog.done = (uint64_t)archive_filter_bytes(a, -1);
                    if (prog.cancelled()) break;
                }
                if (n < 0) {
                    err = archive_error_string(a) ? archive_error_string(a) : "read error";
                    if (archive_entry_is_encrypted(e)) prog.needPassword = true;
                    archive_read_free(a);
                    w.abort();
                    return false;
                }
                if (!w.endFile()) break;
            }
        }
        bool readOk = rc == ARCHIVE_EOF;
        if (!readOk && !prog.cancelled()) err = archive_error_string(a) ? archive_error_string(a) : "read error";
        archive_read_free(a);
        if (!readOk || prog.cancelled()) {
            if (prog.cancelled()) err = "Cancelled";
            if (err.empty()) err = w.error();
            w.abort();
            return false;
        }
    }
    if (!w.finish(err)) return false;
    if (!commitTemp(tmp, outPath, err)) return false;
    res.files = w.stats().files;
    res.outputBytes = fileSize(outPath);
    res.summary = w.stats().summary();
    return true;
}

}  // namespace kz
