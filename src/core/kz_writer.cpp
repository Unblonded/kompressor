// Writing side: gathering inputs, creating standard archives with libarchive,
// and super-compressing into KMP (from files or straight from another archive).
#include <archive.h>
#include <archive_entry.h>
#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <unordered_set>

#include "kmp.hpp"
#include "kz_archive.hpp"
#include "kz_parallel.hpp"
#include "kz_rar.hpp"
#include "kz_zip.hpp"

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

// An in-place edit while re-packing: source entries to leave out and files from disk to append.
struct EditPlan {
    std::vector<std::string> remove;
    std::vector<InputItem> add;  // arcPath already includes the target folder
    std::unordered_set<std::string> added;
    uint64_t addBytes = 0;
    bool drops(const std::string& p) const {
        if (added.count(p)) return true;  // replaced by a file being added
        for (auto& r : remove)
            if (p == r || (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/')) return true;
        return false;
    }
};

// Copies exactly `size` bytes of an open file into `put` (cut short, or zero-padded if the file changed
// since it was scanned) so writers that announced the size in a header stay consistent.
bool pumpFile(InFile& in, uint64_t size, std::vector<char>& buf, Progress& prog,
              const std::function<bool(const char*, size_t)>& put) {
    uint64_t left = size;
    while (left) {
        size_t n = in.read(buf.data(), (size_t)std::min<uint64_t>(left, buf.size()));
        if (n == 0) break;
        if (!put(buf.data(), (size_t)n)) return false;
        left -= (uint64_t)n;
        prog.done += (uint64_t)n;
        if (prog.cancelled()) return false;
    }
    if (left) {
        std::fill(buf.begin(), buf.end(), char(0));
        while (left) {
            size_t k = (size_t)std::min<uint64_t>(left, buf.size());
            if (!put(buf.data(), k)) return false;
            left -= k;
        }
    }
    return true;
}

// ---------------------------------------------------------------- KMP from disk
bool createKmp(const std::vector<InputItem>& items, const std::string& outPath, const CompressOptions& opt,
               Progress& prog, CompressResult& res, std::string& err) {
    kmp::Options ko;
    ko.mode = (kmp::Mode)std::clamp(opt.level < 0 ? 2 : opt.level, 0, 5);
    ko.threads = opt.threads;
    ko.dedup = opt.dedup;
    for (auto& i : items) ko.sizeHint += i.size;

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
        InFile in(f->source);
        if (!in) {
            prog.warn("Cannot read " + pathToUtf8(f->source) + " (skipped)");
            prog.done += f->size;
            continue;
        }
        w.beginFile(f->arcPath, f->mode, f->mtime);
        for (;;) {
            size_t n = in.read(buf.data(), buf.size());
            if (n == 0) break;
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

bool configureWriter(archive* a, const CompressOptions& opt, std::string& err, bool external = false) {
    const FormatDesc& fd = formatDesc(opt.format);
    int level = opt.level < 0 ? fd.defLevel : std::clamp(opt.level, fd.minLevel, fd.maxLevel);
    std::string lv = std::to_string(level);
    std::string th = std::to_string(opt.threads > 0 ? opt.threads : (int)hardwareThreads());

    auto addFilter = [&](OutFormat f) -> bool {
        if (external) return true;  // compressed by a StreamCompressor instead
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
            // Store names as UTF-8 (and set the ZIP UTF-8 flag). Without this, libarchive on
            // Windows converts names to the ANSI code page and drops characters such as CJK.
            if (!setOpt(a, "zip:hdrcharset=UTF-8", err)) return false;
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
            return setOpt(a, "cpio:hdrcharset=UTF-8", err);  // see ZIP: avoid the ANSI code page
        case OutFormat::Kmp:
            break;
    }
    err = "unsupported output format";
    return false;
}

// libarchive writes the plain tar (or raw) stream into a StreamCompressor, which compresses it on
// every core. Formats without one write straight to the file with libarchive's own filter.
la_ssize_t streamWrite(archive*, void* ctx, const void* buf, size_t n) {
    return static_cast<StreamCompressor*>(ctx)->write(buf, n) ? (la_ssize_t)n : -1;
}
int streamClose(archive*, void* ctx) { return static_cast<StreamCompressor*>(ctx)->finish() ? ARCHIVE_OK : ARCHIVE_FATAL; }

struct LaOutput {
    archive* a = nullptr;
    std::unique_ptr<StreamCompressor> comp;
    ~LaOutput() {
        if (a) archive_write_free(a);
    }
    bool open(const CompressOptions& opt, const std::string& tmp, uint64_t sizeHint, Progress& prog, std::string& err) {
        std::string cerr;
        comp = makeStreamCompressor(opt.format, opt.level, opt.threads, sizeHint, tmp, cerr);
        if (comp) comp->setProgress(&prog);
        if (!comp && !cerr.empty()) {
            err = cerr;
            return false;
        }
        a = archive_write_new();
        if (!configureWriter(a, opt, err, comp != nullptr)) return false;
        int rc;
        if (comp) {
            if (formatDesc(opt.format).singleFile) archive_write_set_bytes_in_last_block(a, 1);  // no tar padding
            rc = archive_write_open2(a, comp.get(), nullptr, streamWrite, streamClose, nullptr);
        } else {
#ifdef _WIN32
            rc = archive_write_open_filename_w(a, u8path(tmp).wstring().c_str());
#else
            rc = archive_write_open_filename(a, tmp.c_str());
#endif
        }
        if (rc != ARCHIVE_OK) {
            err = archive_error_string(a) ? archive_error_string(a) : "cannot create output";
            return false;
        }
        return true;
    }
    bool close(std::string& err) {
        if (archive_write_close(a) != ARCHIVE_OK) {
            err = comp && !comp->error().empty() ? comp->error()
                                                 : (archive_error_string(a) ? archive_error_string(a) : "close failed");
            return false;
        }
        return true;
    }
};

// Opens the writer for formats Kompressor writes itself (7z multithreaded, RAR5 stored).
std::unique_ptr<ArchiveWriter> openDirectWriter(const CompressOptions& opt, const std::string& tmp, uint64_t sizeHint,
                                                std::string& err) {
    if (opt.format == OutFormat::SevenZip) {
        auto w = std::make_unique<SevenZipWriter>();
        if (!w->open(tmp, opt.level < 0 ? formatDesc(OutFormat::SevenZip).defLevel : opt.level, opt.threads, sizeHint, err))
            return nullptr;
        return w;
    }
    if (opt.format == OutFormat::Rar) {
        auto w = std::make_unique<RarWriter>();
        if (!w->open(tmp, err)) return nullptr;
        return w;
    }
    if (opt.format == OutFormat::Zip && opt.password.empty()) {
        auto w = std::make_unique<ZipWriter>();
        if (!w->open(tmp, opt.level < 0 ? formatDesc(OutFormat::Zip).defLevel : opt.level, opt.threads, err)) return nullptr;
        return w;
    }
    return nullptr;
}
// Formats written by Kompressor itself: 7z and ZIP on every core (an encrypted ZIP goes through
// libarchive, which does the AES), RAR5 stored.
bool writesDirectly(const CompressOptions& opt) {
    return opt.format == OutFormat::SevenZip || opt.format == OutFormat::Rar ||
           (opt.format == OutFormat::Zip && opt.password.empty());
}

// 7z / RAR, reading files from disk.
bool createDirect(const std::vector<InputItem>& items, const std::string& outPath, const CompressOptions& opt,
                  Progress& prog, CompressResult& res, std::string& err) {
    uint64_t total = 0;
    for (auto& i : items) total += i.size;
    std::string tmp = tmpPathFor(outPath);
    std::unique_ptr<ArchiveWriter> wp = openDirectWriter(opt, tmp, total, err);
    if (!wp) return false;
    ArchiveWriter& w = *wp;
    w.setProgress(&prog);
    prog.total = total;
    prog.done = 0;
    std::vector<char> buf(kIoBuf);
    for (auto& it : items) {
        if (prog.cancelled()) {
            w.abort();
            err = "Cancelled";
            return false;
        }
        prog.setStatus(it.arcPath);
        if (it.isDir) {
            w.addDir(it.arcPath, it.mode, it.mtime);
            continue;
        }
        if (it.isSymlink) {
            w.addSymlink(it.arcPath, it.linkTarget, it.mtime);
            continue;
        }
        InFile in(it.source);
        if (!in) {
            prog.warn("Cannot read " + pathToUtf8(it.source) + " (skipped)");
            prog.done += it.size;
            continue;
        }
        w.expectSize(it.size);
        w.beginFile(it.arcPath, it.mode, it.mtime);
        for (;;) {
            size_t n = in.read(buf.data(), buf.size());
            if (n == 0) break;
            if (!w.write(buf.data(), (size_t)n)) {
                err = w.error();
                w.abort();
                return false;
            }
            prog.done += (uint64_t)n;
            if (prog.cancelled()) break;
        }
    }
    prog.setStatus(std::string("Finishing ") + formatDesc(opt.format).name + "...");
    if (!w.finish(err)) return false;
    if (!commitTemp(tmp, outPath, err)) return false;
    fillResult(res, items, outPath);
    res.summary = ratioText(res.inputBytes, res.outputBytes);
    return true;
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
    if (writesDirectly(opt)) return createDirect(items, outPath, opt, prog, res, err);

    uint64_t total = 0;
    for (auto& i : items) total += i.size;
    std::string tmp = tmpPathFor(outPath);
    LaOutput lo;
    if (!lo.open(opt, tmp, total, prog, err)) {
        lo.comp.reset();
        discardTemp(tmp);
        return false;
    }
    archive* a = lo.a;
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
        InFile in;
        if (!it.isDir && !it.isSymlink) {
            in.open(it.source);
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
        if (in.isOpen()) {
            uint64_t written = 0;
            while (written < it.size) {
                size_t n = in.read(buf.data(), (size_t)std::min<uint64_t>(buf.size(), it.size - written));
                if (n == 0) break;
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
        if (!lo.close(err)) ok = false;
    } else if (prog.cancelled()) {
        err = "Cancelled";
        ok = false;
    }
    archive_write_free(lo.a);
    lo.a = nullptr;
    lo.comp.reset();
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

static bool createWhole(const std::vector<std::string>& inputs, const std::string& outPath, const CompressOptions& opt,
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

static bool convertToKmpImpl(const Source& src, const std::string& outPath, const CompressOptions& opt,
                             const std::string& password, Progress& prog, CompressResult& res, std::string& err,
                             const EditPlan* plan) {
    res = CompressResult();
    const uint64_t addBytes = plan ? plan->addBytes : 0;
    kmp::Options ko;
    ko.mode = (kmp::Mode)std::clamp(opt.level < 0 ? 2 : opt.level, 0, 5);
    ko.threads = opt.threads;
    ko.dedup = opt.dedup;
    ko.sizeHint = sourceSize(src) * 2 + addBytes;  // a rough guess at the unpacked size
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
        r.setSequential();
        uint64_t total = 0;
        for (auto& e : r.entries())
            if (!(plan && plan->drops(e.path))) total += e.size;
        prog.total = total + addBytes;
        for (size_t i = 0; i < r.entries().size(); ++i) {
            const auto& e = r.entries()[i];
            if (plan && plan->drops(e.path)) continue;
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
        archive_read_set_options(a, "cpio:hdrcharset=UTF-8,tar:hdrcharset=UTF-8");  // see kz_archive
        if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
        int rc = openReadSource(a, src);
        if (rc != ARCHIVE_OK) {
            err = archive_error_string(a) ? archive_error_string(a) : "cannot open source archive";
            archive_read_free(a);
            w.abort();
            return false;
        }
        prog.total = sourceSize(src) + addBytes;
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
            if (plan && plan->drops(path)) continue;
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
        prog.done = sourceSize(src);
    }
    if (plan) {
        std::vector<char> buf(kIoBuf);
        for (auto& it : plan->add) {
            if (prog.cancelled()) {
                err = "Cancelled";
                w.abort();
                return false;
            }
            if (it.isDir) {
                w.addDir(it.arcPath, it.mode, it.mtime);
                continue;
            }
            if (it.isSymlink) {
                w.addSymlink(it.arcPath, it.linkTarget, it.mode, it.mtime);
                continue;
            }
            InFile in(it.source);
            if (!in) {
                prog.warn("Cannot read " + pathToUtf8(it.source) + " (skipped)");
                continue;
            }
            prog.setStatus(it.arcPath);
            w.beginFile(it.arcPath, it.mode, it.mtime);
            bool ok = pumpFile(in, it.size, buf, prog, [&](const char* p, size_t n) { return w.write(p, n); });
            if (!ok || !w.endFile()) {
                err = prog.cancelled() ? "Cancelled" : w.error();
                w.abort();
                return false;
            }
            res.inputBytes += it.size;
        }
    }
    if (!w.finish(err)) return false;
    if (!commitTemp(tmp, outPath, err)) return false;
    res.files = w.stats().files;
    res.outputBytes = fileSize(outPath);
    res.summary = w.stats().summary();
    return true;
}

bool convertToKmp(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                  Progress& prog, CompressResult& res, std::string& err) {
    return convertToKmpImpl(src, outPath, opt, password, prog, res, err, nullptr);
}

namespace {

// One entry of the source archive, as handed to the writer.
struct ConvEntry {
    std::string path;
    int type = 0;  // 0 file, 1 dir, 2 symlink
    uint32_t mode = 0;
    int64_t mtime = 0;
    uint64_t size = 0;
    std::string link;
};

class EntrySink {
public:
    virtual ~EntrySink() = default;
    // Starts an entry. Returns false on a fatal error (err set); `skip` is set when the entry is dropped.
    virtual bool begin(const ConvEntry& e, bool& skip, std::string& err) = 0;
    virtual bool write(const void* p, size_t n, std::string& err) = 0;
    virtual uint64_t files() const = 0;
};

class DirectSink : public EntrySink {
public:
    explicit DirectSink(ArchiveWriter& w) : w_(w) {}
    bool begin(const ConvEntry& e, bool& skip, std::string& err) override {
        skip = false;
        bool ok = true;
        if (e.type == 1) w_.addDir(e.path, e.mode, e.mtime);
        else if (e.type == 2) ok = w_.addSymlink(e.path, e.link, e.mtime);
        else {
            if (e.size) w_.expectSize(e.size);  // 0 may just mean "unknown"
            ok = w_.beginFile(e.path, e.mode, e.mtime);
            ++files_;
        }
        if (!ok) err = w_.error();
        return ok;
    }
    bool write(const void* p, size_t n, std::string& err) override {
        if (w_.write(p, n)) return true;
        err = w_.error();
        return false;
    }
    uint64_t files() const override { return files_; }

private:
    ArchiveWriter& w_;
    uint64_t files_ = 0;
};

// Writes converted entries into a libarchive writer.
class ConvWriter : public EntrySink {
public:
    ConvWriter(archive* a, const FormatDesc& fd, Progress& prog) : a_(a), fd_(fd), prog_(prog) {}

    // Starts an entry. Returns false on a fatal error (err set); `skip` is set when the entry is dropped.
    bool begin(const ConvEntry& e, bool& skip, std::string& err) override {
        skip = false;
        if (fd_.singleFile) {
            if (e.type != 0) {
                skip = true;
                return true;
            }
            if (++files_ > 1) {
                err = std::string(fd_.name) + " holds exactly one file, but this archive has several. Use TAR." +
                      fd_.name + " (or ZIP/7Z/KMP) instead.";
                return false;
            }
        } else if (e.type == 0) {
            ++files_;
        }
        archive_entry* ae = archive_entry_new();
#ifdef _WIN32
        archive_entry_set_pathname_utf8(ae, e.path.c_str());
#else
        archive_entry_copy_pathname(ae, e.path.c_str());
#endif
        archive_entry_set_mtime(ae, (time_t)e.mtime, 0);
        if (e.type == 1) {
            archive_entry_set_filetype(ae, AE_IFDIR);
            archive_entry_set_perm(ae, e.mode ? e.mode : 0755);
            archive_entry_set_size(ae, 0);
        } else if (e.type == 2) {
            archive_entry_set_filetype(ae, AE_IFLNK);
            archive_entry_set_perm(ae, 0777);
            archive_entry_set_size(ae, 0);
#ifdef _WIN32
            archive_entry_set_symlink_utf8(ae, e.link.c_str());
#else
            archive_entry_copy_symlink(ae, e.link.c_str());
#endif
        } else {
            archive_entry_set_filetype(ae, AE_IFREG);
            archive_entry_set_perm(ae, e.mode ? e.mode : 0644);
            archive_entry_set_size(ae, (la_int64_t)e.size);
        }
        int r = archive_write_header(a_, ae);
        archive_entry_free(ae);
        if (r < ARCHIVE_WARN) {
            const char* m = archive_error_string(a_);
            if (r == ARCHIVE_FATAL) {
                err = m ? m : "write failed";
                return false;
            }
            prog_.warn(e.path + ": " + (m ? m : "header error"));
            skip = true;
        }
        return true;
    }

    bool write(const void* p, size_t n, std::string& err) override {
        if (archive_write_data(a_, p, n) < 0) {
            err = archive_error_string(a_) ? archive_error_string(a_) : "write failed";
            return false;
        }
        return true;
    }

    uint64_t files() const override { return files_; }

private:
    archive* a_;
    const FormatDesc& fd_;
    Progress& prog_;
    uint64_t files_ = 0;
};

}  // namespace

static bool convertWhole(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                    Progress& prog, CompressResult& res, std::string& err, const EditPlan* plan = nullptr) {
    if (opt.format == OutFormat::Kmp) return convertToKmpImpl(src, outPath, opt, password, prog, res, err, plan);
    res = CompressResult();
    const uint64_t addBytes = plan ? plan->addBytes : 0;
    const FormatDesc& fd = formatDesc(opt.format);
    if (!opt.password.empty() && !fd.password) {
        err = std::string(fd.name) + " does not support passwords - use ZIP for encryption";
        return false;
    }
    if (!plan && !src.inMemory()) {  // never overwrite the archive we are reading from (edits replace it at the end)
        std::error_code e1, e2;
        if (fs::weakly_canonical(u8path(src.path), e1) == fs::weakly_canonical(u8path(outPath), e2)) {
            err = "The output file is the archive being converted - choose another name";
            return false;
        }
    }

    std::string tmp = tmpPathFor(outPath);
    uint64_t hint = sourceSize(src) * 2 + addBytes;  // rough unpacked size, to size the parallel work units
    const bool is7z = writesDirectly(opt);
    std::unique_ptr<ArchiveWriter> direct;
    LaOutput lo;
    std::unique_ptr<EntrySink> sinkOwner;
    if (is7z) {
        direct = openDirectWriter(opt, tmp, hint, err);
        if (!direct) return false;
        direct->setProgress(&prog);
        sinkOwner = std::make_unique<DirectSink>(*direct);
    } else {
        if (!lo.open(opt, tmp, hint, prog, err)) {
            lo.comp.reset();
            discardTemp(tmp);
            return false;
        }
        sinkOwner = std::make_unique<ConvWriter>(lo.a, fd, prog);
    }
    EntrySink& cw = *sinkOwner;
    int rc = ARCHIVE_OK;
    bool ok = true;
    auto fail = [&](const std::string& m) {
        if (err.empty()) err = m;
        ok = false;
    };

    if (kmp::isKmp(src)) {
        kmp::Reader r;
        if (!r.open(src, err)) {
            ok = false;
        } else {
            r.setSequential();
            uint64_t total = 0;
            for (auto& e : r.entries())
                if (!(plan && plan->drops(e.path))) total += e.size;
            prog.total = total + addBytes;
            prog.done = 0;
            for (size_t i = 0; ok && i < r.entries().size(); ++i) {
                if (prog.cancelled()) {
                    fail("Cancelled");
                    break;
                }
                const auto& ke = r.entries()[i];
                if (plan && plan->drops(ke.path)) continue;
                ConvEntry ce;
                ce.path = ke.path;
                ce.mode = ke.mode;
                ce.mtime = ke.mtime;
                ce.size = ke.size;
                ce.type = ke.type == kmp::EntryType::Dir ? 1 : ke.type == kmp::EntryType::Symlink ? 2 : 0;
                ce.link = ke.link;
                bool skip = false;
                if (!cw.begin(ce, skip, err)) {
                    ok = false;
                    break;
                }
                if (ce.type != 0) continue;
                prog.setStatus(ce.path);
                std::string ferr;
                bool rd = r.readFile(i, [&](const uint8_t* p, size_t n) {
                    prog.done += n;
                    res.inputBytes += n;
                    return skip || cw.write(p, n, err);
                }, &prog, ferr);
                if (!rd) fail(ferr.empty() ? "read error" : ferr);
            }
        }
    } else {
        archive* a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        archive_read_set_options(a, "cpio:hdrcharset=UTF-8,tar:hdrcharset=UTF-8");
        if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
        rc = openReadSource(a, src);
        if (rc != ARCHIVE_OK) {
            fail(archive_error_string(a) ? archive_error_string(a) : "cannot open source archive");
        } else {
            prog.total = sourceSize(src) + addBytes;
            prog.done = 0;
            archive_entry* e;
            std::vector<char> buf(kIoBuf);
            while (ok && ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN)) {
                if (prog.cancelled()) {
                    fail("Cancelled");
                    break;
                }
                const char* pn = archive_entry_pathname_utf8(e);
                if (!pn) pn = archive_entry_pathname(e);
                ConvEntry ce;
                if (!pn || !sanitizeRelativePath(toNFC(pn), ce.path)) {
                    if (pn) prog.warn("Skipped unsafe path: " + std::string(pn));
                    continue;
                }
                if (plan && plan->drops(ce.path)) continue;
                if (archive_entry_hardlink(e)) {
                    prog.warn(ce.path + ": hard link skipped");
                    continue;
                }
                ce.mode = (uint32_t)archive_entry_perm(e);
                ce.mtime = archive_entry_mtime(e);
                auto ft = archive_entry_filetype(e);
                if (ft == AE_IFDIR) {
                    ce.type = 1;
                } else if (ft == AE_IFLNK) {
                    ce.type = 2;
                    const char* t = archive_entry_symlink_utf8(e);
                    ce.link = t ? toNFC(t) : "";
                } else if (ft != AE_IFREG && ft != 0) {
                    prog.warn(ce.path + ": special file skipped");
                    continue;
                }
                if (ce.type != 0) {
                    bool skip;
                    if (!cw.begin(ce, skip, err)) ok = false;
                    continue;
                }
                prog.setStatus(ce.path);
                // Headers need the size up front; when the source does not record it, read the data first.
                std::vector<char> held;
                bool sized = archive_entry_size_is_set(e) != 0;
                la_ssize_t n = 0;
                if (sized) {
                    ce.size = (uint64_t)archive_entry_size(e);
                } else {
                    while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) held.insert(held.end(), buf.data(), buf.data() + n);
                    ce.size = held.size();
                }
                if (n < 0) {
                    fail(archive_error_string(a) ? archive_error_string(a) : "read error");
                    if (archive_entry_is_encrypted(e)) prog.needPassword = true;
                    break;
                }
                bool skip = false;
                if (!cw.begin(ce, skip, err)) {
                    ok = false;
                    break;
                }
                uint64_t written = 0;
                if (!sized) {
                    if (!skip && !held.empty() && !cw.write(held.data(), held.size(), err)) ok = false;
                    written = held.size();
                } else {
                    while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
                        // never write past the size announced in the header
                        size_t take = (size_t)std::min<uint64_t>((uint64_t)n, ce.size - std::min(ce.size, written));
                        if (!skip && take && !cw.write(buf.data(), take, err)) {
                            ok = false;
                            break;
                        }
                        written += (uint64_t)n;
                        prog.done = (uint64_t)archive_filter_bytes(a, -1);
                        if (prog.cancelled()) break;
                    }
                    if (ok && n < 0) {
                        fail(archive_error_string(a) ? archive_error_string(a) : "read error");
                        if (archive_entry_is_encrypted(e)) prog.needPassword = true;
                    }
                }
                res.inputBytes += written;
                if (ok && !skip && !prog.cancelled() && written < ce.size) {  // keep the output valid
                    std::vector<char> zeros((size_t)std::min<uint64_t>(ce.size - written, buf.size()));
                    for (uint64_t left = ce.size - written; ok && left;) {
                        size_t k = (size_t)std::min<uint64_t>(left, zeros.size());
                        if (!cw.write(zeros.data(), k, err)) ok = false;
                        left -= k;
                    }
                    prog.warn(ce.path + ": shorter than recorded (padded)");
                }
            }
            if (ok && prog.cancelled()) fail("Cancelled");
            if (ok && rc != ARCHIVE_EOF) fail(archive_error_string(a) ? archive_error_string(a) : "read error");
            prog.done = sourceSize(src);
        }
        archive_read_free(a);
    }

    if (ok && plan) {  // files being added go after the kept entries
        std::vector<char> buf(kIoBuf);
        for (auto& it : plan->add) {
            if (prog.cancelled()) {
                fail("Cancelled");
                break;
            }
            ConvEntry ce;
            ce.path = it.arcPath;
            ce.mode = it.mode;
            ce.mtime = it.mtime;
            InFile in;
            if (it.isDir) {
                ce.type = 1;
            } else if (it.isSymlink) {
                ce.type = 2;
                ce.link = it.linkTarget;
            } else {
                in.open(it.source);
                if (!in) {
                    prog.warn("Cannot read " + pathToUtf8(it.source) + " (skipped)");
                    continue;
                }
                ce.size = it.size;
            }
            bool skip = false;
            if (!cw.begin(ce, skip, err)) {
                ok = false;
                break;
            }
            if (ce.type != 0) continue;
            prog.setStatus(ce.path);
            if (!pumpFile(in, it.size, buf, prog, [&](const char* p, size_t n) { return skip || cw.write(p, n, err); })) {
                fail(prog.cancelled() ? "Cancelled" : "write failed");
                break;
            }
            res.inputBytes += it.size;
        }
    }

    if (ok && fd.singleFile && cw.files() == 0) fail(std::string(fd.name) + " needs a file to compress, but the archive has none");
    if (ok) {
        prog.setStatus("Finishing...");
        std::string cerr;
        if (is7z ? !direct->finish(cerr) : !lo.close(cerr)) fail(cerr.empty() ? "close failed" : cerr);
    } else if (is7z) {
        direct->abort();
    }
    sinkOwner.reset();
    if (lo.a) {
        archive_write_free(lo.a);
        lo.a = nullptr;
    }
    lo.comp.reset();
    if (!ok) {
        discardTemp(tmp);
        return false;
    }
    if (!commitTemp(tmp, outPath, err)) return false;
    res.files = cw.files();
    res.outputBytes = fileSize(outPath);
    res.summary = ratioText(res.inputBytes, res.outputBytes);
    return true;
}

// Splits a finished archive into volumes when asked to, and records which files were written.
static bool finishOutput(const std::string& outPath, const CompressOptions& opt, Progress& prog, CompressResult& res,
                         std::string& err) {
    res.parts.clear();
    if (!opt.volumeSize) {
        res.parts.push_back(outPath);
        return true;
    }
    prog.setStatus("Splitting into parts");
    if (!splitIntoVolumes(outPath, opt.volumeSize, res.parts, err)) return false;
    if (res.parts.size() > 1)
        res.summary += ", split into " + std::to_string(res.parts.size()) + " parts of " + humanSize(opt.volumeSize);
    return true;
}

bool createArchive(const std::vector<std::string>& inputs, const std::string& outPath, const CompressOptions& opt,
                   Progress& prog, CompressResult& res, std::string& err) {
    return createWhole(inputs, outPath, opt, prog, res, err) && finishOutput(outPath, opt, prog, res, err);
}

bool convertArchive(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                    Progress& prog, CompressResult& res, std::string& err) {
    return convertWhole(src, outPath, opt, password, prog, res, err) && finishOutput(outPath, opt, prog, res, err);
}

static std::string lowerStr(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool canEditArchive(const Source& src, std::string& why) {
    if (src.inMemory()) {
        why = "This archive is inside another archive. Extract it first to change it.";
        return false;
    }
    if (kmp::isKmp(src)) return true;
    std::string name = lowerStr(fileNameOf(src.path));
    OutFormat f;
    if (!formatFromFileName(name, f)) {
        why = "Kompressor can't write this kind of archive (or it is split into parts), so it can't be changed.";
        return false;
    }
    if (formatDesc(f).singleFile) {
        why = std::string(formatDesc(f).name) + " holds a single compressed file, so nothing can be added to it.";
        return false;
    }
    // "name.part2.rar": one volume of a multi-volume RAR set
    size_t p = name.rfind(".part");
    if (f == OutFormat::Rar && p != std::string::npos && p + 5 < name.size() && std::isdigit((unsigned char)name[p + 5])) {
        why = "This archive is split into parts, so it can't be changed.";
        return false;
    }
    return true;
}

bool editArchive(const Source& src, const ArchiveEdit& edit, const std::string& password, Progress& prog,
                 CompressResult& res, std::string& err) {
    res = CompressResult();
    if (!canEditArchive(src, err)) return false;
    EditPlan plan;
    for (auto& r : edit.remove) {
        std::string c;
        if (sanitizeRelativePath(r, c) && !c.empty()) plan.remove.push_back(c);
    }
    std::string under;
    if (!edit.addUnder.empty() && !sanitizeRelativePath(edit.addUnder, under)) {
        err = "Invalid folder: " + edit.addUnder;
        return false;
    }
    if (!edit.add.empty()) {
        if (!gatherInputs(edit.add, plan.add, prog, err)) return false;
        std::error_code ec;
        fs::path self = fs::weakly_canonical(u8path(src.path), ec);
        plan.add.erase(std::remove_if(plan.add.begin(), plan.add.end(), [&](const InputItem& i) {
                           std::error_code e2;
                           return fs::weakly_canonical(i.source, e2) == self;  // never pack the archive into itself
                       }),
                       plan.add.end());
        for (auto& it : plan.add) {
            if (!under.empty()) it.arcPath = under + "/" + it.arcPath;
            plan.added.insert(it.arcPath);
            if (!it.isDir && !it.isSymlink) plan.addBytes += it.size;
        }
    }
    CompressOptions opt;
    if (kmp::isKmp(src)) {
        kmp::Reader r;
        if (!r.open(src, err)) return false;
        opt.format = OutFormat::Kmp;
        opt.level = (int)r.mode();
    } else {
        formatFromFileName(lowerStr(fileNameOf(src.path)), opt.format);
        if (!password.empty()) {
            if (!formatDesc(opt.format).password) {
                err = std::string("Kompressor can't write encrypted ") + formatDesc(opt.format).name +
                      " archives, so this one can't be changed without losing its password.";
                return false;
            }
            opt.password = password;
        }
    }
    prog.setStatus("Updating " + src.name());
    return convertWhole(src, src.path, opt, password, prog, res, err, &plan);
}

}  // namespace kz
