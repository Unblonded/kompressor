#include "kz_common.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <clocale>
#include <ctime>
#include <fstream>
#include <regex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utime.h>
#else
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace kz {

std::string Source::name() const {
    if (!displayName.empty()) return displayName;
    return fileNameOf(path);
}

fs::path u8path(const std::string& s) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

std::string pathToUtf8(const fs::path& p) {
#if defined(__cpp_char8_t)
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

std::string fileNameOf(const std::string& path) {
    size_t e = path.size();
    while (e > 0 && (path[e - 1] == '/' || path[e - 1] == '\\')) --e;
    size_t s = path.find_last_of("/\\", e == 0 ? 0 : e - 1);
    if (s == std::string::npos || e == 0) return path.substr(0, e);
    return path.substr(s + 1, e - s - 1);
}

std::string lowerExt(const std::string& name) {
    std::string f = fileNameOf(name);
    size_t d = f.find_last_of('.');
    if (d == std::string::npos || d == 0) return {};
    std::string e = f.substr(d + 1);
    for (auto& c : e) c = char(std::tolower((unsigned char)c));
    return e;
}

std::string stripArchiveExt(const std::string& name) {
    std::string f = fileNameOf(name);
    std::string lower = f;
    for (auto& c : lower) c = char(std::tolower((unsigned char)c));
    static const char* compound[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4",
                                     ".tar.lzma", ".tar.lz", ".tar.z", ".tar.br"};
    for (const char* c : compound) {
        size_t n = std::char_traits<char>::length(c);
        if (lower.size() > n && lower.compare(lower.size() - n, n, c) == 0) return f.substr(0, f.size() - n);
    }
    size_t d = f.find_last_of('.');
    if (d == std::string::npos || d == 0) return f;
    return f.substr(0, d);
}

std::string toNFC(const std::string& s) {
#ifdef __APPLE__
    bool ascii = true;
    for (unsigned char c : s)
        if (c & 0x80) { ascii = false; break; }
    if (ascii) return s;
    CFStringRef in = CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(), kCFStringEncodingUTF8, false);
    if (!in) return s;
    CFMutableStringRef m = CFStringCreateMutableCopy(nullptr, 0, in);
    CFRelease(in);
    CFStringNormalize(m, kCFStringNormalizationFormC);
    CFIndex len = CFStringGetLength(m), bytes = 0;
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, nullptr, 0, &bytes);
    std::string out((size_t)bytes, '\0');
    CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, (UInt8*)out.data(), bytes, nullptr);
    CFRelease(m);
    return out;
#else
    return s;
#endif
}

bool sanitizeRelativePath(const std::string& in, std::string& out) {
    std::string p = in;
    std::replace(p.begin(), p.end(), '\\', '/');
    // Drive letters ("C:/...") and absolute paths are never allowed to escape.
    if (p.size() >= 2 && std::isalpha((unsigned char)p[0]) && p[1] == ':') return false;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == ".") {
            // skip (also strips leading '/')
        } else if (seg == "..") {
            return false;
        } else {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    out.clear();
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += '/';
        out += parts[k];
    }
    return !out.empty();
}

FileStat statPath(const fs::path& p, bool follow) {
    FileStat st;
#ifdef _WIN32
    std::error_code ec;
    auto s = follow ? fs::status(p, ec) : fs::symlink_status(p, ec);
    if (ec || !fs::exists(s)) return st;
    st.exists = true;
    st.isSymlink = fs::is_symlink(s);
    st.isDir = fs::is_directory(s);
    struct _stat64 sb;
    if (_wstat64(p.wstring().c_str(), &sb) == 0) {
        st.mtime = (int64_t)sb.st_mtime;
        st.size = st.isDir ? 0 : (uint64_t)sb.st_size;
    }
    st.mode = st.isDir ? 0755 : 0644;
#else
    struct stat sb;
    int r = follow ? ::stat(p.c_str(), &sb) : ::lstat(p.c_str(), &sb);
    if (r != 0) return st;
    st.exists = true;
    st.isDir = S_ISDIR(sb.st_mode);
    st.isSymlink = S_ISLNK(sb.st_mode);
    st.size = S_ISREG(sb.st_mode) ? (uint64_t)sb.st_size : 0;
    st.mtime = (int64_t)sb.st_mtime;
    st.mode = (uint32_t)(sb.st_mode & 07777);
#endif
    return st;
}

void setFileMTime(const fs::path& p, int64_t mtime) {
    if (mtime <= 0) return;
#ifdef _WIN32
    struct __utimbuf64 t;
    t.actime = mtime;
    t.modtime = mtime;
    _wutime64(p.wstring().c_str(), &t);
#else
    struct timeval tv[2];
    tv[0].tv_sec = (time_t)mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    ::lutimes(p.c_str(), tv);
#endif
}

void setFileMode(const fs::path& p, uint32_t mode) {
#ifndef _WIN32
    if (mode) ::chmod(p.c_str(), (mode_t)(mode & 07777));
#else
    (void)p;
    (void)mode;
#endif
}

std::string readSymlink(const fs::path& p) {
    std::error_code ec;
    auto t = fs::read_symlink(p, ec);
    if (ec) return {};
    return pathToUtf8(t);
}

std::string humanSize(uint64_t b) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double v = double(b);
    int u = 0;
    while (v >= 1024.0 && u < 5) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    if (u == 0)
        std::snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
    else
        std::snprintf(buf, sizeof buf, v < 10 ? "%.2f %s" : (v < 100 ? "%.1f %s" : "%.0f %s"), v, units[u]);
    return buf;
}

std::string formatTime(int64_t t) {
    if (t <= 0) return "";
    std::time_t tt = (std::time_t)t;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

std::string formatDuration(double s) {
    char buf[64];
    if (s < 1.0)
        std::snprintf(buf, sizeof buf, "%.0f ms", s * 1000.0);
    else if (s < 60.0)
        std::snprintf(buf, sizeof buf, "%.1f s", s);
    else
        std::snprintf(buf, sizeof buf, "%dm %02ds", int(s) / 60, int(s) % 60);
    return buf;
}

void initLocale() {
#ifdef _WIN32
    std::setlocale(LC_ALL, ".UTF-8");
#else
    const char* l = std::setlocale(LC_CTYPE, "");
    if (!l || std::string(l) == "C" || std::string(l) == "POSIX") {
        if (!std::setlocale(LC_CTYPE, "C.UTF-8")) std::setlocale(LC_CTYPE, "en_US.UTF-8");
    }
#endif
}

std::vector<std::string> utf8Args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 0; i < n; ++i) out.push_back(pathToUtf8(fs::path(w[i])));
        LocalFree(w);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
}

unsigned hardwareThreads() {
    unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4 : n;
}

// ---------------------------------------------------------------- memory / threads
uint64_t availableMemory() {
#ifdef _WIN32
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return ms.ullAvailPhys;
    return 0;
#elif defined(__APPLE__)
    uint64_t total = 0;
    size_t len = sizeof total;
    if (sysctlbyname("hw.memsize", &total, &len, nullptr, 0) == 0) return total / 2;  // no cheap "available" figure
    return 0;
#else
    // MemAvailable counts reclaimable cache too, which is what we want.
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        unsigned long long kb = 0;
        while (std::fgets(line, sizeof line, f))
            if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
        std::fclose(f);
        if (kb) return kb * 1024ull;
    }
    long pages = sysconf(_SC_AVPHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    return pages > 0 && page > 0 ? (uint64_t)pages * (uint64_t)page : 0;
#endif
}

unsigned threadsFor(uint64_t bytesPerThread, int requested) {
    if (requested > 0) return (unsigned)requested;
    unsigned hw = hardwareThreads();
    uint64_t avail = availableMemory();
    // Use most of the free memory (leave a quarter for the rest of the system), at least 1 GiB.
    uint64_t budget = avail ? std::max<uint64_t>(avail / 4 * 3, 1ull << 30) : (4ull << 30);
    uint64_t byMem = bytesPerThread ? budget / bytesPerThread : hw;
    return (unsigned)std::max<uint64_t>(1, std::min<uint64_t>(hw, byMem));
}

// ---------------------------------------------------------------- multi-volume sets
namespace {
bool isFile(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(u8path(p), ec);
}
}  // namespace

std::vector<std::string> volumeSet(const std::string& path) {
    std::vector<std::string> vols;
    std::string name = fileNameOf(path);
    std::string dir = path.substr(0, path.size() - name.size());
    std::smatch m;
    static const std::regex partRar(R"(^(.*)\.part(\d+)\.rar$)", std::regex::icase);
    static const std::regex numbered(R"(^(.*)\.(\d{3})$)");
    static const std::regex oldRar(R"(^(.*)\.(rar|r\d\d)$)", std::regex::icase);
    if (std::regex_match(name, m, partRar)) {  // name.part1.rar, name.part2.rar ...
        std::string stem = m[1].str();
        size_t width = m[2].str().size();
        for (int i = 1; i < 10000; ++i) {
            std::string num = std::to_string(i);
            if (num.size() < width) num = std::string(width - num.size(), '0') + num;
            std::string p = dir + stem + ".part" + num + ".rar";
            if (!isFile(p)) break;
            vols.push_back(p);
        }
    } else if (std::regex_match(name, m, numbered)) {  // name.7z.001, .002 ... (any part may be given)
        for (int i = 1; i < 1000; ++i) {
            char num[8];
            std::snprintf(num, sizeof num, "%03d", i);
            std::string p = dir + m[1].str() + "." + num;
            if (!isFile(p)) break;
            vols.push_back(p);
        }
    } else if (std::regex_match(name, m, oldRar)) {  // old style: name.rar, name.r00, name.r01 ...
        std::string ext = m[2].str();
        bool upper = std::isupper((unsigned char)ext[0]) != 0;
        std::string first = dir + m[1].str() + (upper ? ".RAR" : ".rar");
        if (!isFile(first)) return {};
        vols.push_back(first);
        for (int i = 0; i < 100; ++i) {
            char num[8];
            std::snprintf(num, sizeof num, upper ? ".R%02d" : ".r%02d", i);
            std::string p = dir + m[1].str() + num;
            if (!isFile(p)) break;
            vols.push_back(p);
        }
    }
    if (vols.size() < 2) vols.clear();
    return vols;
}

std::string firstVolume(const std::string& path) {
    auto v = volumeSet(path);
    return v.empty() ? path : v.front();
}

bool looksLikeVolume(const std::string& name) {
    static const std::regex vol(R"(.*(\.\d{3}|\.part\d+\.rar|\.r\d\d)$)", std::regex::icase);
    return std::regex_match(fileNameOf(name), vol);
}

bool splitIntoVolumes(const std::string& path, uint64_t volumeSize, std::vector<std::string>& parts, std::string& err) {
    parts.clear();
    std::error_code ec;
    fs::path p = u8path(path);
    uint64_t size = fs::file_size(p, ec);
    if (ec) {
        err = "cannot read " + path;
        return false;
    }
    auto partName = [&](int i) {
        char num[8];
        std::snprintf(num, sizeof num, ".%03d", i);
        return path + num;
    };
    // Volumes left over from an earlier, longer split would be picked up as part of this set.
    for (int i = 1; i < 1000 && isFile(partName(i)); ++i) fs::remove(u8path(partName(i)), ec);
    if (volumeSize == 0 || size <= volumeSize) {
        parts.push_back(path);
        return true;
    }
    uint64_t count = (size + volumeSize - 1) / volumeSize;
    if (count > 999) {
        err = "that would make more than 999 parts - choose a larger part size";
        return false;
    }
    {
        // Write parts 2..N from the tail, then cut the original down to part 1 and rename it.
        std::ifstream in(p, std::ios::binary);
        std::vector<char> buf(4 << 20);
        for (uint64_t i = 1; i < count; ++i) {
            in.clear();
            in.seekg((std::streamoff)(i * volumeSize));
            std::ofstream out(u8path(partName((int)i + 1)), std::ios::binary | std::ios::trunc);
            uint64_t left = std::min(volumeSize, size - i * volumeSize);
            while (left) {
                size_t n = (size_t)std::min<uint64_t>(left, buf.size());
                in.read(buf.data(), (std::streamsize)n);
                if ((size_t)in.gcount() != n) {
                    err = "read error while splitting";
                    return false;
                }
                out.write(buf.data(), (std::streamsize)n);
                left -= n;
            }
            if (!out) {
                err = "write error while splitting (disk full?)";
                return false;
            }
        }
    }
    fs::resize_file(p, volumeSize, ec);
    if (!ec) fs::rename(p, u8path(partName(1)), ec);
    if (ec) {
        err = "cannot finish the first part: " + ec.message();
        return false;
    }
    for (uint64_t i = 1; i <= count; ++i) parts.push_back(partName((int)i));
    return true;
}

}  // namespace kz
