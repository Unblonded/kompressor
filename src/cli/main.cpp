// kompressor-cli - scriptable front end for the Kompressor core.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <iostream>
#include <mutex>
#include <thread>

#include "kmp.hpp"
#include "kz_cm.hpp"
#include "kz_advisor.hpp"
#include "kz_archive.hpp"
#include "kz_gpu.hpp"
#include "kz_hash.hpp"
#include "kz_search.hpp"
#include "kz_perf.hpp"
#include "kz_ramdrive.hpp"
#include <csignal>

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
        "  kompressor-cli hash    <archive> [-a crc32|md5|sha1|sha256|all] [-c list] [-p password] [entry...]\n"
        "                         (checksums of the files inside; -c checks them against a .sha256/.md5/.sfv list)\n"
        "  kompressor-cli sum     <files/folders...> [-a crc32|md5|sha1|sha256|all] [-c list]\n"
        "                         (checksums of files on disk; names are relative to the folder given, or the inputs' folder)\n"
        "  kompressor-cli grep    <archive> <text> [--case] [--hex] [--names \"*.txt;*.ini\"] [-p password] [entry...]\n"
        "                         (find text - UTF-8 or UTF-16 - or hex bytes inside the files; case-insensitive unless --case)\n"
        "  kompressor-cli cat     <archive> <entry> [-p password]      (write one member to stdout)\n"
        "  kompressor-cli create  <out.zip|.7z|.tar.xz|...> <files/folders...> [-l level] [-p password] [-t threads]\n"
        "  kompressor-cli super   <out.kmp> <files/folders...> [-m fast|balanced|ultra|max|extreme|ultimate] [-t threads] [--no-dedup]\n"
        "  kompressor-cli convert <archive> <out.zip|.7z|.tar.zst|.kmp|...> [-l level] [-m mode] [-p password]\n"
        "                         [-e new-password]   (re-pack into another format; -e encrypts a ZIP output)\n"
        "  kompressor-cli add     <archive> <files/folders...> [-d folder] [-p password]   (add or replace entries)\n"
        "  kompressor-cli delete  <archive> <entry...> [-p password]   (remove entries and folders)\n"
        "  kompressor-cli advise  <files/folders...>   (which format suits these files, for each goal)\n"
        "  kompressor-cli ramdrive [--size 2g] [--letter R]\n"
        "                         (a scratch drive until Ctrl+C; everything on it is deleted then)\n"
        "  kompressor-cli formats\n\n"
        "  create, super and convert also take -v <size> to split the result into parts (e.g. -v 100m, -v 4g)\n"
        "  --max-cpu   maximum performance: every core at high priority, no power throttling, fastest power plan\n"
        "  --gpu       let the graphics card search for matches too (ZIP; Windows)\n",
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
    const bool logProgress = std::getenv("KZ_PROGRESS_LOG") != nullptr;  // one line per second, for tests
    auto start = std::chrono::steady_clock::now();
    int ticks = 0;
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (logProgress && ++ticks % 10 == 0)
            std::fprintf(stderr, "progress %3d s %5.1f%%\n", ticks / 10, prog.fraction() * 100.0);
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

// "100m", "4g", "1.5g", "700000k" or plain bytes. Returns 0 when it cannot be read.
static uint64_t parseSize(const std::string& s) {
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (!end || end == s.c_str() || v <= 0) return 0;
    switch (std::tolower((unsigned char)*end)) {
        case 'k': v *= 1024.0; break;
        case 'm': v *= 1024.0 * 1024.0; break;
        case 'g': v *= 1024.0 * 1024.0 * 1024.0; break;
        case 't': v *= 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        default: break;
    }
    return (uint64_t)v;
}

static kmp::Mode parseMode(const std::string& s) {
    if (s == "fast") return kmp::Mode::Fast;
    if (s == "balanced") return kmp::Mode::Balanced;
    if (s == "max") return kmp::Mode::Max;
    if (s == "extreme") return kmp::Mode::Extreme;
    if (s == "ultimate") return kmp::Mode::Ultimate;
    return kmp::Mode::Ultra;
}

int main(int argc, char** argv) {
    initLocale();
    std::vector<std::string> args = utf8Args(argc, argv);
    if (int rc = RamDrive::helperMain(args); rc >= 0) return rc;  // the elevated RAM drive helper (Windows)
    if (args.size() < 2) {
        usage();
        return 1;
    }
    std::string cmd = args[1];
    std::vector<std::string> pos;
    std::string password, outDir, mode = "ultra", encrypt, intoDir, algo = "sha256", checkList;
    int level = -1, threads = 0;
    uint64_t volumeSize = 0;
    bool skipExisting = false, dedup = true, maxCpu = false, useGpu = false, matchCase = false, hexSearch = false;
    std::string names, ramSize = "1g";
    char letter = 'R';
    for (size_t i = 2; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : std::string(); };
        if (a == "-p") password = next();
        else if (a == "-o") outDir = next();
        else if (a == "-l") level = std::atoi(next().c_str());
        else if (a == "-t") threads = std::atoi(next().c_str());
        else if (a == "-m") mode = next();
        else if (a == "-e") encrypt = next();
        else if (a == "-d") intoDir = next();
        else if (a == "-a") algo = next();
        else if (a == "-c") checkList = next();
        else if (a == "--case") matchCase = true;
        else if (a == "--hex") hexSearch = true;
        else if (a == "--names") names = next();
        else if (a == "--size") ramSize = next();
        else if (a == "--letter") { std::string l = next(); letter = l.empty() ? 'R' : l[0]; }
        else if (a == "-v") {
            std::string v = next();
            volumeSize = parseSize(v);
            if (!volumeSize) {
                std::fprintf(stderr, "error: bad part size '%s' (try -v 100m or -v 4g)\n", v.c_str());
                return 1;
            }
        }
        else if (a == "--skip-existing") skipExisting = true;
        else if (a == "--no-dedup") dedup = false;
        else if (a == "--max-cpu") maxCpu = true;
        else if (a == "--gpu") useGpu = true;
        else pos.push_back(a);
    }

    Progress prog;
    std::string err;

    if (useGpu) {
        gpu::setEnabled(true);
        // Opened before the work starts: with every core busy, the driver takes seconds to start up.
        if (gpu::available()) std::fprintf(stderr, "graphics card: %s\n", gpu::adapterName().c_str());
        else std::fprintf(stderr, "graphics card: none usable, the CPU does all the work\n");
    }
    perf::recoverPowerPlan();
    struct Boost {  // released on every way out of main()
        explicit Boost(bool on) {
            if (!on) return;
            perf::setMaxPerformance(true);
            perf::boost(true);
            std::fprintf(stderr, "maximum performance: %s\n", perf::boostSummary().c_str());
        }
        ~Boost() { perf::boost(false); }
    } boost(maxCpu);

    if (cmd == "kcm") {  // undocumented: benchmark the context-mixing codec on one file
        if (pos.size() < 2) return 1;
        std::vector<uint8_t> in;
        {
            FILE* f = std::fopen(pos[0].c_str(), "rb");
            if (!f) return 1;
            uint8_t buf[1 << 16];
            size_t r;
            while ((r = std::fread(buf, 1, sizeof buf, f)) > 0) in.insert(in.end(), buf, buf + r);
            std::fclose(f);
        }
        auto prof = (cm::Profile)std::atoi(pos[1].c_str());
        int level = pos.size() > 2 ? std::atoi(pos[2].c_str()) : 0;
        std::vector<uint8_t> out, back(in.size());
        auto t0 = std::chrono::steady_clock::now();
        cm::compress(in.data(), in.size(), prof, out, level);
        auto t1 = std::chrono::steady_clock::now();
        bool ok = cm::decompress(out.data(), out.size(), back.data(), back.size(), prof, level) && back == in;
        auto t2 = std::chrono::steady_clock::now();
        std::printf("%-28s %10zu -> %9zu (%.3f bpc)  enc %.2fs  dec %.2fs  %s\n", fileNameOf(pos[0]).c_str(), in.size(),
                    out.size(), in.empty() ? 0.0 : 8.0 * out.size() / in.size(),
                    std::chrono::duration<double>(t1 - t0).count(), std::chrono::duration<double>(t2 - t1).count(),
                    ok ? "ok" : "MISMATCH");
        return ok ? 0 : 2;
    }

    if (cmd == "ramdrive" || cmd == "ram") {
        uint64_t cap = parseSize(ramSize);
        if (!cap) {
            std::fprintf(stderr, "error: bad size '%s' (try --size 512m or --size 4g)\n", ramSize.c_str());
            return 1;
        }
        if (RamDrive::needsAdmin()) std::printf("Making %s needs administrator approval...\n", RamDrive::kind());
        std::fflush(stdout);
        RamDrive drive;
        if (!drive.start(cap, letter, err)) return std::fprintf(stderr, "error: %s\n", err.c_str()), 1;
        std::printf("RAM drive: %s, %s, at %s\n", humanSize(drive.capacity()).c_str(), RamDrive::kind(), drive.where().c_str());
        std::printf("Press Ctrl+C to stop. Everything on the drive is deleted then.\n");
        std::fflush(stdout);
        static std::atomic<bool> quit{false};
        std::signal(SIGINT, [](int) { quit = true; });
        std::signal(SIGTERM, [](int) { quit = true; });
        while (!quit) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::printf("\nstopping: %s freed\n", humanSize(drive.used()).c_str());
        drive.stop();
        return 0;
    }

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

    if (cmd == "hash" || cmd == "h" || cmd == "sum") {
        if (pos.empty()) return usage(), 1;
        const bool onDisk = cmd == "sum";  // files and folders, not the files inside an archive
        Source src{pos[0], nullptr, ""};
        std::vector<ListedHash> listed;
        unsigned which = 0;
        if (!checkList.empty()) {
            std::ifstream f(u8path(checkList), std::ios::binary);
            if (!f) {
                std::fprintf(stderr, "error: cannot read %s\n", checkList.c_str());
                return 1;
            }
            std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            listed = parseHashList(text);
            which = hashKindsOfList(listed);
            if (!which) {
                std::fprintf(stderr, "error: no checksums found in %s\n", checkList.c_str());
                return 1;
            }
        } else if (algo == "all") which = kHashCrc32 | kHashMd5 | kHashSha1 | kHashSha256;
        else if (algo == "crc32" || algo == "crc") which = kHashCrc32;
        else if (algo == "md5") which = kHashMd5;
        else if (algo == "sha1") which = kHashSha1;
        else if (algo == "sha256") which = kHashSha256;
        else return usage(), 1;
        std::vector<std::string> only(pos.begin() + 1, pos.end());
        std::vector<EntryHash> hashes;
        std::string base;
        bool ok = run(prog, [&] {
            return onDisk ? hashFiles(pos, which, hashes, base, prog, err) : hashArchive(src, only, password, which, hashes, prog, err);
        });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        if (!listed.empty()) {
            std::vector<std::string> extra;
            auto checks = checkHashes(hashes, listed, &extra);
            int bad = 0;
            static const char* word[] = {"OK", "MISMATCH", "MISSING", "UNREADABLE"};
            for (auto& c : checks) {
                std::printf("%-10s %s\n", word[c.state], c.name.c_str());
                if (c.state != HashCheck::Ok) ++bad;
            }
            for (auto& x : extra) std::printf("%-10s %s\n", "NOT LISTED", x.c_str());
            std::printf("%zu checked, %d problem(s), %zu not in the list\n", checks.size(), bad, extra.size());
            return bad ? 2 : 0;
        }
        static const HashKind kinds[] = {kHashCrc32, kHashMd5, kHashSha1, kHashSha256};
        int bad = 0;
        for (auto& h : hashes) {
            if (!h.error.empty()) {
                std::printf("%s: %s\n", h.path.c_str(), h.error.c_str());
                ++bad;
                continue;
            }
            for (HashKind k : kinds)
                if (which & k) std::printf("%s ", h.get(k).c_str());
            std::printf("*%s\n", h.path.c_str());
        }
        return bad ? 2 : 0;
    }

    if (cmd == "grep" || cmd == "g") {
        if (pos.size() < 2) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        SearchOptions so;
        so.text = pos[1];
        so.hex = hexSearch;
        so.matchCase = matchCase;
        so.names = names;
        std::vector<std::string> only(pos.begin() + 2, pos.end());
        std::vector<FileHits> found;
        std::mutex mu;
        bool ok = run(prog, [&] {
            return searchArchive(src, only, password, so, [&](FileHits&& f) {
                std::lock_guard<std::mutex> l(mu);
                found.push_back(std::move(f));
            }, prog, err);
        });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        uint64_t total = 0;
        size_t withHits = 0;
        for (auto& f : found) {
            if (!f.error.empty()) std::printf("%s: error: %s\n", f.path.c_str(), f.error.c_str());
            if (!f.count) continue;
            ++withHits;
            total += f.count;
            for (auto& h : f.hits) {
                if (h.line) std::printf("%s:%u: %s\n", f.path.c_str(), h.line, h.snippet.c_str());
                else std::printf("%s:@0x%llx: %s\n", f.path.c_str(), (unsigned long long)h.offset, h.snippet.c_str());
            }
            if (f.count > f.hits.size())
                std::printf("%s: ... %llu more\n", f.path.c_str(), (unsigned long long)(f.count - f.hits.size()));
        }
        std::printf("%llu match(es) in %zu file(s)\n", (unsigned long long)total, withHits);
        return withHits ? 0 : 1;
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
        o.volumeSize = volumeSize;
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
        std::printf("Created %s in %s\n  %s\n", res.parts.empty() ? out.c_str() : res.parts.front().c_str(), formatDuration(s).c_str(), res.summary.c_str());
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
        o.volumeSize = volumeSize;
        CompressResult res;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run(prog, [&] { return convertArchive(src, pos[1], o, password, prog, res, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        uint64_t before = sourceSize(src);  // every volume of a split archive
        std::printf("Converted in %s: %s -> %s\n  %s\n", formatDuration(s).c_str(), humanSize(before).c_str(),
                    humanSize(res.outputBytes).c_str(), res.summary.c_str());
        return 0;
    }

    if (cmd == "advise") {  // which format suits these files, for every goal
        if (pos.empty()) return usage(), 1;
        ContentProfile p;
        bool ok = run(prog, [&] { return profileInputs(pos, p, prog, err); });
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return 2;
        }
        std::printf("%s in %llu files: %.0f%% text, %.0f%% already compressed, %.0f%% programs, %.0f%% other\n",
                    humanSize(p.totalBytes).c_str(), (unsigned long long)p.files, 100.0 * p.textBytes / std::max<uint64_t>(1, p.totalBytes),
                    100.0 * p.mediaBytes / std::max<uint64_t>(1, p.totalBytes), 100.0 * p.exeBytes / std::max<uint64_t>(1, p.totalBytes),
                    100.0 * p.otherBytes / std::max<uint64_t>(1, p.totalBytes));
        std::printf("probe: %.0f%% won't shrink, the rest packs to %.0f%% with zstd -3, %s repeated\n\n", p.incompressible * 100,
                    p.ratio * 100, humanSize(p.dupBytes).c_str());
        for (Goal g : {Goal::Balanced, Goal::Smallest, Goal::Fastest, Goal::Share, Goal::Protect}) {
            Recommendation r = recommend(p, g, threads);
            std::printf("%-13s %-40s ~%s, ~%s\n  %s\n", goalName(g), r.title.c_str(), humanSize(r.estBytes).c_str(),
                        formatDuration(r.estSeconds).c_str(), r.why.c_str());
            for (auto& n : r.notes) std::printf("  - %s\n", n.c_str());
        }
        return 0;
    }

    if (cmd == "add" || cmd == "a" || cmd == "delete" || cmd == "d") {
        if (pos.size() < 2) return usage(), 1;
        Source src{pos[0], nullptr, ""};
        ArchiveEdit edit;
        bool adding = cmd == "add" || cmd == "a";
        for (size_t i = 1; i < pos.size(); ++i) (adding ? edit.add : edit.remove).push_back(pos[i]);
        edit.addUnder = intoDir;
        CompressResult res;
        auto t0 = std::chrono::steady_clock::now();
        bool ok = run(prog, [&] { return editArchive(src, edit, password, prog, res, err); });
        printWarnings(prog);
        if (!ok) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            return prog.needPassword ? 3 : 2;
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("Updated %s in %s\n  %s\n", pos[0].c_str(), formatDuration(s).c_str(), res.summary.c_str());
        return 0;
    }

    usage();
    return 1;
}
