// kompressor-cli - scriptable front end for the Kompressor core.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>

#include "kmp.hpp"
#include "kz_archive.hpp"

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace kz;

static void usage() {
    std::printf(
        "Kompressor %s - extract anything, compress everything, super-compress to .kmp\n\n"
        "Usage:\n"
        "  kompressor-cli list    <archive> [-p password]\n"
        "  kompressor-cli extract <archive> [-o outdir] [-p password] [--skip-existing] [entry...]\n"
        "  kompressor-cli test    <archive> [-p password]\n"
        "  kompressor-cli cat     <archive> <entry> [-p password]      (write one member to stdout)\n"
        "  kompressor-cli create  <out.zip|.7z|.tar.xz|...> <files/folders...> [-l level] [-p password] [-t threads]\n"
        "  kompressor-cli super   <out.kmp> <files/folders...> [-m fast|balanced|ultra|max] [-t threads] [--no-dedup]\n"
        "  kompressor-cli convert <archive> <out.zip|.7z|.tar.zst|.kmp|...> [-l level] [-m mode] [-p password]\n"
        "                         [-e new-password]   (re-pack into another format; -e encrypts a ZIP output)\n"
        "  kompressor-cli formats\n",
        KZ_VERSION);
}

// Runs `work` on a thread and draws a progress line on stderr while it runs.
static bool run(Progress& prog, const std::function<bool()>& work) {
    std::atomic<bool> finished{false};
    bool result = false;
    std::thread t([&] {
        result = work();
        finished = true;
    });
    const bool tty = isatty(fileno(stderr));
    auto start = std::chrono::steady_clock::now();
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!tty || finished) continue;
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::string st = prog.status();
        if (st.size() > 50) st = "..." + st.substr(st.size() - 47);
        std::fprintf(stderr, "\r  %5.1f%%  %-8s %-52s", prog.fraction() * 100.0, formatDuration(secs).c_str(), st.c_str());
        std::fflush(stderr);
    }
    t.join();
    if (tty) std::fprintf(stderr, "\r%90s\r", "");
    return result;
}

static void printWarnings(const Progress& prog) {
    for (auto& w : prog.warnings()) std::fprintf(stderr, "warning: %s\n", w.c_str());
}

static kmp::Mode parseMode(const std::string& s) {
    if (s == "fast") return kmp::Mode::Fast;
    if (s == "balanced") return kmp::Mode::Balanced;
    if (s == "max") return kmp::Mode::Max;
    return kmp::Mode::Ultra;
}

int main(int argc, char** argv) {
    initLocale();
    std::vector<std::string> args = utf8Args(argc, argv);
    if (args.size() < 2) {
        usage();
        return 1;
    }
    std::string cmd = args[1];
    std::vector<std::string> pos;
    std::string password, outDir, mode = "ultra", encrypt;
    int level = -1, threads = 0;
    bool skipExisting = false, dedup = true;
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "-p") password = next();
        else if (a == "-o") outDir = next();
        else if (a == "-l") level = std::atoi(next().c_str());
        else if (a == "-t") threads = std::atoi(next().c_str());
        else if (a == "-m") mode = next();
        else if (a == "-e") encrypt = next();
        else if (a == "--skip-existing") skipExisting = true;
        else if (a == "--no-dedup") dedup = false;
        else pos.push_back(a);
    }

    Progress prog;
    std::string err;

    if (cmd == "formats") {
        std::printf("Read / extract:\n");
        for (auto& f : readableFormats()) std::printf("  %-55s %s\n", f.first.c_str(), f.second.c_str());
        std::printf("\nCreate:\n");
        for (auto& f : outputFormats()) std::printf("  %-12s %-9s %s\n", f.name, f.ext, f.description);
        std::printf("\n%s\n", libraryVersions().c_str());
        return 0;
    }

    if (cmd == "list" || cmd == "l") {
        if (pos.empty()) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        ArchiveInfo info;
        bool ok = run(prog, [&] { return listArchive(src, info, password, prog, err); });
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        std::printf("Format: %s   Filter: %s   Size: %s\n", info.format.c_str(), info.filter.c_str(),
                    humanSize(info.archiveSize).c_str());
        if (!info.extra.empty()) std::printf("%s\n", info.extra.c_str());
        std::printf("\n%12s  %-16s  %s\n", "Size", "Modified", "Name");
        for (auto& e : info.entries)
            std::printf("%12s  %-16s  %s%s%s\n", e.isDir ? "<DIR>" : humanSize(e.size).c_str(),
                        formatTime(e.mtime).c_str(), e.path.c_str(), e.encrypted ? "  [encrypted]" : "",
                        e.isSymlink ? (" -> " + e.linkTarget).c_str() : "");
        std::printf("\n%zu entries, %s total\n", info.entries.size(), humanSize(info.totalSize).c_str());
        return 0;
    }

    if (cmd == "extract" || cmd == "x") {
        if (pos.empty()) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        ExtractOptions o;
        o.destDir = outDir.empty() ? stripArchiveExt(pos[0]) : outDir;
        if (outDir.empty()) {
            fs::path p = u8path(pos[0]);
            o.destDir = pathToUtf8(p.parent_path() / u8path(stripArchiveExt(pathToUtf8(p.filename()))));
        }
        o.password = password;
        o.skipExisting = skipExisting;
        o.only.assign(pos.begin() + 1, pos.end());
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run(prog, [&] { return extractArchive(src, o, prog, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("Extracted to %s in %s\n", o.destDir.c_str(), formatDuration(s).c_str());
        return 0;
    }

    if (cmd == "test" || cmd == "t") {
        if (pos.empty()) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        bool ok = run(prog, [&] { return testArchive(src, password, prog, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "FAILED: %s\n", err.c_str());
            return 2;
        }
        std::printf("OK - archive is intact\n");
        return 0;
    }

    if (cmd == "cat") {
        if (pos.size() < 2) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        std::vector<uint8_t> data;
        bool trunc = false;
        if (!readEntry(src, pos[1], password, ~0ull >> 1, data, trunc, prog, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        std::fwrite(data.data(), 1, data.size(), stdout);
        return 0;
    }

    if (cmd == "create" || cmd == "c" || cmd == "super" || cmd == "s") {
        if (pos.size() < 2) return usage(), 1;
        CompressOptions o;
        std::string out = pos[0];
        if (cmd == "super" || cmd == "s") {
            o.format = OutFormat::Kmp;
            if (lowerExt(out) != "kmp") out += ".kmp";
            o.level = (int)parseMode(mode);
        } else {
            if (!formatFromFileName(out, o.format)) {
                std::fprintf(stderr, "error: cannot tell the format from '%s' (try .zip .7z .tar.xz .tar.zst .kmp ...)\n",
                             out.c_str());
                return 1;
            }
            o.level = level;
            if (o.format == OutFormat::Kmp) o.level = level >= 0 ? level : (int)parseMode(mode);
        }
        o.password = password;
        o.threads = threads;
        o.dedup = dedup;
        std::vector<std::string> inputs(pos.begin() + 1, pos.end());
        CompressResult res;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run(prog, [&] { return createArchive(inputs, out, o, prog, res, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("Created %s in %s\n  %s\n", out.c_str(), formatDuration(s).c_str(), res.summary.c_str());
        return 0;
    }

    if (cmd == "convert") {
        if (pos.size() < 2) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        CompressOptions o;
        if (!formatFromFileName(pos[1], o.format)) {
            std::fprintf(stderr, "error: cannot tell the format from '%s' (try .zip .7z .tar.xz .tar.zst .kmp ...)\n",
                         pos[1].c_str());
            return 1;
        }
        o.level = o.format == OutFormat::Kmp ? (level >= 0 ? level : (int)parseMode(mode)) : level;
        o.password = encrypt;
        o.threads = threads;
        o.dedup = dedup;
        CompressResult res;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run(prog, [&] { return convertArchive(src, pos[1], o, password, prog, res, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::error_code ec;
        uint64_t before = fs::file_size(u8path(pos[0]), ec);
        std::printf("Converted in %s: %s -> %s\n  %s\n", formatDuration(s).c_str(), humanSize(before).c_str(),
                    humanSize(res.outputBytes).c_str(), res.summary.c_str());
        return 0;
    }

    usage();
    return 1;
}
