#include "kz_advisor.hpp"

#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <unordered_set>

namespace kz {

namespace {

enum Kind { KText, KMedia, KExe, KOther };

Kind kindOf(const std::string& path) {
    static const std::unordered_set<std::string> text = {
        "txt", "md", "rst", "csv", "tsv", "json", "jsonl", "xml", "html", "htm", "xhtml", "css", "scss", "less", "js",
        "mjs", "cjs", "ts", "tsx", "jsx", "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "inl", "py", "pyi", "java",
        "kt", "kts", "cs", "go", "rs", "rb", "php", "pl", "lua", "swift", "m", "mm", "sh", "bash", "zsh", "bat", "cmd",
        "ps1", "psm1", "yaml", "yml", "toml", "ini", "cfg", "conf", "log", "sql", "svg", "tex", "bib", "rtf", "srt",
        "vtt", "gradle", "cmake", "mk", "make", "vue", "svelte", "dart", "scala", "r", "jl", "hs", "ex", "exs", "erl",
        "clj", "fs", "vb", "asm", "s", "glsl", "hlsl", "shader", "obj", "mtl", "ply", "dae", "gltf", "properties",
        "env", "lock", "sln", "vcxproj", "csproj", "proto", "graphql", "ipynb", "po", "pot", "strings", "plist", "eml"};
    static const std::unordered_set<std::string> media = {
        "jpg", "jpeg", "png", "gif", "webp", "heic", "heif", "avif", "jxl", "mp3", "aac", "m4a", "ogg", "oga", "opus",
        "flac", "wma", "mp4", "m4v", "mkv", "mov", "avi", "webm", "wmv", "flv", "3gp", "ts", "zip", "7z", "rar", "gz",
        "tgz", "bz2", "xz", "txz", "zst", "lz4", "lzma", "kmp", "cab", "jar", "apk", "aab", "ipa", "docx", "xlsx",
        "pptx", "odt", "ods", "odp", "epub", "pdf", "woff", "woff2", "dmg", "msi", "msix", "appx", "nupkg", "whl",
        "vsix", "crx", "xpi", "pak", "vpk", "unitypackage"};
    static const std::unordered_set<std::string> exe = {"exe", "dll", "sys", "so", "dylib", "o", "a", "lib", "pdb",
                                                        "bin", "elf", "wasm", "pyd", "node", "ocx", "efi", "ko"};
    std::string e = lowerExt(path);
    if (e == "ts") return KText;  // TypeScript far more often than MPEG-TS in folders people zip up
    if (text.count(e)) return KText;
    if (media.count(e)) return KMedia;
    if (exe.count(e)) return KExe;
    return KOther;
}

double defaultRatio(Kind k) {
    switch (k) {
        case KText: return 0.28;
        case KExe: return 0.45;
        case KMedia: return 1.0;
        default: return 0.55;
    }
}

// zstd -3 output/input of a buffer.
double probe(const std::vector<char>& buf) {
    if (buf.empty()) return 1.0;
    std::vector<char> out(ZSTD_compressBound(buf.size()));
    size_t n = ZSTD_compress(out.data(), out.size(), buf.data(), buf.size(), 3);
    if (ZSTD_isError(n)) return 1.0;
    return std::min(1.0, double(n) / double(buf.size()));
}

// Up to `chunks` pieces of `chunk` bytes spread over the file (start, middle, ...).
bool readSample(const fs::path& p, uint64_t size, size_t chunk, int chunks, std::vector<char>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.clear();
    for (int c = 0; c < chunks; ++c) {
        uint64_t at = chunks == 1 || size <= chunk ? 0 : (size - chunk) * c / (uint64_t)(chunks - 1);
        in.clear();
        in.seekg((std::streamoff)at);
        size_t old = out.size();
        out.resize(old + chunk);
        in.read(out.data() + old, (std::streamsize)chunk);
        out.resize(old + (size_t)in.gcount());
        if (size <= chunk) break;
    }
    return true;
}

uint64_t fnv(const std::vector<char>& b) {
    uint64_t h = 1469598103934665603ull;
    for (char c : b) h = (h ^ (uint8_t)c) * 1099511628211ull;
    return h;
}

// ---------------------------------------------------------------- estimates
// Calibrated on a 553 MB corpus (64% programs, 36% text and code) on a 24-thread machine:
//   tar.zst-3 221 MB 1.6 s | zip-6 232 MB 20.6 s | zip-1 260 MB 7.8 s | 7z-5 171 MB 29.7 s | 7z-9 168.5 MB 54 s
//   tar.xz-6 170 MB 32 s   | tar.gz 232 MB 1.9 s | tar.zst-19 173 MB 39 s
//   kmp fast 221 MB 1.6 s | balanced 174 MB 28 s | ultra 162 MB 36 s | max 161 MB 63 s | extreme 139 MB 96 s
//   ultimate 133 MB 900 s
// sizeMul: output relative to zstd -3 on the compressible part. mbps: MB/s (24 threads for parallel ones).
struct Model {
    double sizeMul;
    double mbps;
    bool parallel;
    bool stores;  // no compression at all
    bool dedup;   // stores repeated files once
};

Model modelFor(OutFormat f, int level, double textShare) {
    auto lv = [&](int def) { return level < 0 ? def : level; };
    switch (f) {
        case OutFormat::Zip:  // deflate runs on one core
            return lv(6) <= 2 ? Model{1.21, 71, false, false, false} : Model{1.06, lv(6) >= 8 ? 18.0 : 27.0, false, false, false};
        case OutFormat::SevenZip: return lv(5) >= 8 ? Model{0.73, 10.2, true, false, false} : lv(5) <= 2 ? Model{0.86, 60, true, false, false}
                                                                                           : Model{0.76, 18.6, true, false, false};
        case OutFormat::TarZst:
        case OutFormat::Zst: return lv(3) >= 17 ? Model{0.735, 14.3, true, false, false} : lv(3) >= 9 ? Model{0.85, 80, true, false, false}
                                                                                       : Model{1.0, 345, true, false, false};
        case OutFormat::TarXz:
        case OutFormat::Xz: return Model{0.72, 17.3, true, false, false};
        case OutFormat::TarGz:
        case OutFormat::Gz: return Model{1.06, 290, true, false, false};
        case OutFormat::TarBz2:
        case OutFormat::Bz2: return Model{0.90, 60, true, false, false};
        case OutFormat::TarLz4:
        case OutFormat::Lz4: return Model{1.45, 900, true, false, false};
        case OutFormat::Kmp:
            switch (std::clamp(lv(2), 0, 5)) {
                case 0: return Model{1.0, 345, true, false, true};
                case 1: return Model{0.745, 20, true, false, true};
                case 2: return Model{0.68, 15.2, true, false, true};
                case 3: return Model{0.67, 8.8, true, false, true};
                case 4: return Model{0.58 - 0.08 * textShare, 5.8, true, false, true};  // context mixing gains most on text
                default: return Model{0.545 - 0.08 * textShare, 0.6, false, false, true};
            }
        default: return Model{1.0, 900, false, true, false};  // TAR, RAR (stored), ISO, CPIO
    }
}

std::string pct(double frac) {
    char b[16];
    std::snprintf(b, sizeof b, "%.0f%%", std::max(0.0, frac * 100.0));
    return b;
}

}  // namespace

const char* goalName(Goal g) {
    switch (g) {
        case Goal::Balanced: return "Best overall";
        case Goal::Smallest: return "Smallest";
        case Goal::Fastest: return "Fastest";
        case Goal::Share: return "Share";
        case Goal::Protect: return "Password";
    }
    return "";
}

void estimate(const ContentProfile& p, OutFormat f, int level, unsigned threads, uint64_t& bytes, double& seconds) {
    double total = (double)p.totalBytes;
    double text = total > 0 ? double(p.textBytes) / total : 0.0;
    Model m = modelFor(f, level, text);
    double data = m.dedup ? std::max(0.0, total - (double)p.dupBytes) : total;
    double inc = data * p.incompressible, comp = data - inc;
    double out = m.stores ? data : inc * 1.002 + comp * std::min(1.0, p.ratio * m.sizeMul);
    bytes = (uint64_t)out;
    unsigned t = threads ? threads : hardwareThreads();
    double speed = m.mbps * 1024 * 1024;
    if (m.parallel) speed *= std::clamp(double(t) / 24.0, 0.08, 2.0);
    // Already-compressed data goes faster: KMP spots it and stores it, the others just churn through it.
    double incSpeed = speed * (m.dedup ? 6.0 : 1.3);
    seconds = comp / speed + inc / incSpeed + (m.dedup ? (total - data) / (speed * 8) : 0.0) + 0.2;
}

bool profileInputs(const std::vector<std::string>& inputs, ContentProfile& out, Progress& prog, std::string& err) {
    out = ContentProfile();
    prog.setStatus("Looking at your files...");
    std::vector<InputItem> items;
    if (!gatherInputs(inputs, items, prog, err)) return false;
    struct F {
        const InputItem* it;
        Kind kind;
        double ratio = -1;  // probed
    };
    std::vector<F> files;
    for (auto& it : items) {
        if (it.isDir) {
            ++out.dirs;
            continue;
        }
        if (it.isSymlink) continue;
        Kind k = kindOf(it.arcPath);
        files.push_back({&it, k});
        ++out.files;
        out.totalBytes += it.size;
        out.largestFile = std::max(out.largestFile, it.size);
        (k == KText ? out.textBytes : k == KMedia ? out.mediaBytes : k == KExe ? out.exeBytes : out.otherBytes) += it.size;
    }
    if (prog.cancelled()) {
        err = "Cancelled";
        return false;
    }

    // Probe: the biggest files (they decide the result) plus a spread of the rest.
    std::vector<size_t> order(files.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return files[a].it->size > files[b].it->size; });
    std::vector<size_t> pick;
    for (size_t i = 0; i < order.size() && i < 24; ++i) pick.push_back(order[i]);
    if (order.size() > 24) {
        size_t rest = order.size() - 24, want = std::min<size_t>(48, rest);
        for (size_t k = 0; k < want; ++k) pick.push_back(order[24 + k * rest / want]);
    }
    std::vector<char> buf;
    for (size_t n = 0; n < pick.size(); ++n) {
        if (prog.cancelled()) break;
        F& f = files[pick[n]];
        if (f.it->size == 0) continue;
        bool big = n < 24;
        if (readSample(f.it->source, f.it->size, big ? 128 * 1024 : 64 * 1024, big ? 3 : 1, buf) && !buf.empty())
            f.ratio = probe(buf);
    }
    // Per-kind averages fill in for files that were not probed.
    double kindSum[4] = {}, kindW[4] = {};
    for (auto& f : files)
        if (f.ratio >= 0) {
            kindSum[f.kind] += f.ratio * double(f.it->size + 1);
            kindW[f.kind] += double(f.it->size + 1);
        }
    double incBytes = 0, compIn = 0, compOut = 0;
    for (auto& f : files) {
        double r = f.ratio >= 0 ? f.ratio : kindW[f.kind] > 0 ? kindSum[f.kind] / kindW[f.kind] : defaultRatio(f.kind);
        double s = double(f.it->size);
        if (r > 0.92) {
            incBytes += s;
            if (f.ratio >= 0 && f.kind != KMedia) {  // whatever its name says, it's compressed (or random) data
                uint64_t& from = f.kind == KText ? out.textBytes : f.kind == KExe ? out.exeBytes : out.otherBytes;
                from -= std::min(from, f.it->size);
                out.mediaBytes += f.it->size;
            }
        } else {
            compIn += s;
            compOut += s * r;
        }
    }
    double total = double(out.totalBytes);
    out.incompressible = total > 0 ? incBytes / total : 0.0;
    // Samples of a few hundred KB see less repetition than the whole file does: zstd on the real thing
    // packs 7-20% tighter than on the samples (measured on four sets of programs and text).
    out.ratio = compIn > 0 ? std::min(1.0, compOut / compIn * 0.87) : 1.0;
    out.measured = true;

    // Duplicates: same size and same first 4 KB (only checked among files of equal size).
    std::map<uint64_t, std::vector<const InputItem*>> bySize;
    for (auto& f : files)
        if (f.it->size >= 16 * 1024) bySize[f.it->size].push_back(f.it);
    size_t reads = 0;
    for (auto& [size, group] : bySize) {
        if (group.size() < 2 || reads > 20000 || prog.cancelled()) continue;
        std::map<uint64_t, int> seen;
        for (auto* it : group) {
            ++reads;
            if (readSample(it->source, it->size, 4096, 1, buf) && ++seen[fnv(buf)] > 1) out.dupBytes += size;
        }
    }
    if (prog.cancelled()) {
        err = "Cancelled";
        return false;
    }
    return true;
}

ContentProfile profileListing(const ArchiveInfo& info) {
    ContentProfile p;
    double packedKnownIn = 0, packedKnownOut = 0, incKnown = 0;
    double kindBytes[4] = {};
    for (auto& e : info.entries) {
        if (e.isDir) {
            ++p.dirs;
            continue;
        }
        if (e.isSymlink) continue;
        ++p.files;
        p.totalBytes += e.size;
        p.largestFile = std::max(p.largestFile, e.size);
        Kind k = kindOf(e.path);
        kindBytes[k] += double(e.size);
        (k == KText ? p.textBytes : k == KMedia ? p.mediaBytes : k == KExe ? p.exeBytes : p.otherBytes) += e.size;
        if (e.packed > 0 && e.size > 0) {  // per-entry sizes (ZIP, some others)
            double r = double(e.packed) / double(e.size);
            if (r > 0.92) incKnown += double(e.size);
            else {
                packedKnownIn += double(e.size);
                packedKnownOut += double(e.packed);
            }
        }
    }
    double total = double(p.totalBytes);
    if (total <= 0) return p;
    // How the current archive packs, relative to zstd -3 (to turn its ratio into ours).
    std::string fmt = info.format + " " + info.filter;
    for (auto& c : fmt) c = (char)std::tolower((unsigned char)c);
    double curMul = 0;
    if (fmt.find("kompressor") != std::string::npos || fmt.find("kmp") != std::string::npos) curMul = 0.65;
    else if (fmt.find("7-zip") != std::string::npos || fmt.find("7z") != std::string::npos) curMul = 0.73;
    else if (fmt.find("rar") != std::string::npos) curMul = 0.76;
    else if (fmt.find("zip") != std::string::npos) curMul = 1.06;
    else if (fmt.find("xz") != std::string::npos || fmt.find("lzma") != std::string::npos) curMul = 0.72;
    else if (fmt.find("zstd") != std::string::npos) curMul = 1.0;
    else if (fmt.find("gzip") != std::string::npos) curMul = 1.06;
    else if (fmt.find("bzip2") != std::string::npos) curMul = 0.90;
    else if (fmt.find("lz4") != std::string::npos) curMul = 1.45;

    if (packedKnownIn + incKnown > total * 0.5) {
        p.incompressible = incKnown / total;
        p.ratio = packedKnownIn > 0 ? packedKnownOut / packedKnownIn / (curMul > 0 ? curMul : 1.06) : 1.0;
        p.measured = true;
    } else {
        p.incompressible = kindBytes[KMedia] / total;
        double ct = total - kindBytes[KMedia];
        double guess = ct > 0 ? (kindBytes[KText] * defaultRatio(KText) + kindBytes[KExe] * defaultRatio(KExe) +
                                 kindBytes[KOther] * defaultRatio(KOther)) / ct
                              : 1.0;
        p.ratio = guess;
        if (curMul > 0 && info.archiveSize > 0) {  // the archive's real size tells us how compressible it is
            double rAll = double(info.archiveSize) / total;
            if (p.incompressible < 0.98) {
                double rc = (rAll - p.incompressible) / (1.0 - p.incompressible);
                if (rc > 0.02) {
                    p.ratio = rc / curMul;
                    p.measured = true;
                } else {  // smaller than our guess even allows: more of it compresses than the names say
                    p.incompressible = std::max(0.0, rAll - 0.05);
                    p.ratio = 0.05 / curMul / std::max(0.05, 1.0 - p.incompressible);
                }
            }
        }
    }
    p.ratio = std::clamp(p.ratio, 0.02, 1.0);
    p.incompressible = std::clamp(p.incompressible, 0.0, 1.0);
    return p;
}

Recommendation recommend(const ContentProfile& p, Goal g, unsigned threads) {
    Recommendation r;
    const double total = double(std::max<uint64_t>(p.totalBytes, 1));
    const double textShare = double(p.textBytes) / total, exeShare = double(p.exeBytes) / total;
    const double dupShare = double(p.dupBytes) / total;
    const bool media = p.incompressible > 0.7;
    const bool single = p.files == 1 && p.dirs == 0;
    auto est = [&](OutFormat f, int level, uint64_t& b, double& s) { estimate(p, f, level, threads, b, s); };

    switch (g) {
        case Goal::Share:
            r.format = OutFormat::Zip;
            r.level = media ? 1 : 6;
            r.why = "ZIP opens with a double-click on Windows, macOS, Linux, phones and anything else, no extra app needed.";
            if (media) r.why += " Your files are mostly already compressed, so the fast level loses nothing.";
            break;
        case Goal::Protect:
            r.format = OutFormat::Zip;
            r.level = media ? 1 : 6;
            r.wantsPassword = true;
            r.why = "ZIP with AES-256: strong encryption that 7-Zip, WinRAR, Kompressor and most archive apps open. "
                    "Pick a strong password.";
            r.notes.push_back("Windows' and macOS' built-in zip support can't open AES-encrypted ZIPs; the other side needs "
                              "an archive app such as 7-Zip.");
            r.notes.push_back("File names inside a ZIP stay visible without the password; only the contents are encrypted.");
            break;
        case Goal::Fastest:
            r.format = single ? OutFormat::Zst : OutFormat::TarZst;
            r.level = 3;
            r.why = "Zstandard on every core: by far the quickest to make and to open, and usually smaller than ZIP.";
            r.notes.push_back("Opens in Kompressor, 7-Zip, macOS and Linux (tar). Windows' built-in zip support can't open it.");
            break;
        case Goal::Balanced: {
            if (media) {
                r.format = OutFormat::Zip;
                r.level = 1;
                r.why = "Most of this is already compressed (photos, video, audio, archives), so heavier settings would "
                        "barely shrink it. A quick ZIP keeps it in one file that opens anywhere.";
            } else {
                r.format = OutFormat::SevenZip;
                r.level = 5;
                r.why = "7Z is a clearly smaller file than ZIP at a good speed, and opens in 7-Zip, WinRAR, Kompressor and "
                        "Windows 11's own Explorer.";
                uint64_t b;
                double s;
                est(r.format, r.level, b, s);
                if (s > 20 * 60) {  // a lot of data: keep it quick
                    r.format = single ? OutFormat::Zst : OutFormat::TarZst;
                    r.level = 9;
                    r.why = "That's a lot of data, so Zstandard (multi-core) gets close to 7Z's size in a fraction of the time.";
                    r.notes.push_back("Opens in Kompressor, 7-Zip, macOS and Linux (tar).");
                }
            }
            break;
        }
        case Goal::Smallest: {
            r.format = OutFormat::Kmp;
            // The heaviest mode that finishes in reasonable time on this machine.
            const int modes[] = {5, 4, 3, 2};
            const double budget[] = {4 * 60, 15 * 60, 30 * 60, 1e18};
            r.level = 2;
            for (int i = 0; i < 4; ++i) {
                uint64_t b;
                double s;
                est(OutFormat::Kmp, modes[i], b, s);
                if (s <= budget[i]) {
                    r.level = modes[i];
                    break;
                }
            }
            if (media) r.level = std::min(r.level, 2);  // heavy models gain nothing on compressed data
            static const char* names[] = {"Fast", "Balanced", "Ultra", "Max", "Extreme", "Ultimate"};
            r.why = std::string("Super Compress (") + names[r.level] + ") ";
            if (r.level >= 4) r.why += "predicts every bit with context mixing: the smallest files Kompressor can make";
            else if (r.level == 3) r.why += "tries every codec on every block and keeps the smallest";
            else r.why += "uses LZMA2 with code filters and dedup";
            r.why += r.level >= 4 && p.totalBytes > (64ull << 20) ? ". Heavier modes would take too long for this much data." : ".";
            if (media) r.notes.push_back("Most of this is already compressed, so even the heaviest mode can't shrink it much.");
            if (r.level >= 4) r.notes.push_back("Extracting takes about as long as compressing with this mode.");
            r.notes.push_back("Only Kompressor opens .kmp files. For a standard format, 7Z at level 9 is the smallest.");
            break;
        }
    }

    est(r.format, r.level, r.estBytes, r.estSeconds);
    const auto& fd = formatDesc(r.format);
    if (r.format == OutFormat::Kmp) {
        static const char* names[] = {"Fast", "Balanced", "Ultra", "Max", "Extreme", "Ultimate"};
        r.title = std::string("Super Compress (.kmp), ") + names[std::clamp(r.level, 0, 5)];
    } else {
        r.title = std::string(fd.name) + (fd.minLevel < fd.maxLevel ? ", level " + std::to_string(r.level) : "");
    }

    // Extra hints from the content.
    if (g != Goal::Smallest && !media) {
        uint64_t kb;
        double ks;
        est(OutFormat::Kmp, textShare > 0.4 ? 4 : 3, kb, ks);
        if (r.estBytes > 0 && kb < r.estBytes * 0.85)
            r.notes.push_back("If only you need to open it, Super Compress would make it about " +
                              pct(1.0 - double(kb) / double(r.estBytes)) + " smaller.");
    }
    if (dupShare > 0.1 && r.format != OutFormat::Kmp)
        r.notes.push_back("About " + pct(dupShare) + " of this looks like repeated copies; Super Compress stores repeats once.");
    if (exeShare > 0.3 && (r.format == OutFormat::Zip || r.format == OutFormat::TarZst))
        r.notes.push_back("It's mostly programs: 7Z and Super Compress have filters that shrink executables further.");
    if (p.totalBytes > (4000ull << 20))
        r.notes.push_back("Over 4 GB: switch on Split into parts for FAT32 drives, e-mail or upload limits.");
    return r;
}

}  // namespace kz
