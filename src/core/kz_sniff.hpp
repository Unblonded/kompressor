// Recognises archive and compressed-stream formats from their content (magic numbers), so a file is
// handled as what it is, whatever its name says. Header-only and dependency-free: KompShell.dll (the
// Explorer menu) uses it too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace kz {

struct Sniffed {
    const char* name = nullptr;  // "7-Zip", "ZIP", "gzip"...; null = no archive signature found
    const char* ext = nullptr;   // its usual extension, without the dot
    explicit operator bool() const { return name != nullptr; }
};

// How many leading bytes sniff() wants: ISO 9660 keeps its signature at offset 32769.
constexpr size_t kSniffBytes = 32774;

inline Sniffed sniff(const uint8_t* p, size_t n) {
    auto at = [&](size_t off, const char* sig, size_t len) { return n >= off + len && std::memcmp(p + off, sig, len) == 0; };
    auto is = [&](const char* sig, size_t len) { return at(0, sig, len); };
    if (is("KMPR\x1A\r\n\0", 8)) return {"KMP", "kmp"};
    if (is("7z\xBC\xAF\x27\x1C", 6)) return {"7-Zip", "7z"};
    if (is("Rar!\x1A\x07\x01\x00", 8)) return {"RAR5", "rar"};
    if (is("Rar!\x1A\x07\x00", 7)) return {"RAR", "rar"};
    if (is("PK\x03\x04", 4) || is("PK\x05\x06", 4) || is("PK\x07\x08", 4)) return {"ZIP", "zip"};
    if (is("\x1F\x8B", 2)) return {"gzip", "gz"};
    if (is("BZh", 3) && n > 3 && p[3] >= '1' && p[3] <= '9') return {"bzip2", "bz2"};
    if (is("\xFD" "7zXZ\0", 6)) return {"xz", "xz"};
    if (is("\x28\xB5\x2F\xFD", 4)) return {"Zstandard", "zst"};
    if (is("\x04\x22\x4D\x18", 4)) return {"LZ4", "lz4"};
    if (is("LZIP", 4)) return {"lzip", "lz"};
    if (is("\x89LZO\0\r\n\x1A\n", 9)) return {"lzop", "lzo"};
    if (is("\x1F\x9D", 2)) return {"compress (.Z)", "z"};
    if (at(257, "ustar", 5)) return {"TAR", "tar"};
    if (is("070701", 6) || is("070702", 6) || is("070707", 6) || is("\xC7\x71", 2) || is("\x71\xC7", 2))
        return {"CPIO", "cpio"};
    if (is("!<arch>\n", 8)) return {"ar / Debian package", "deb"};
    if (is("\xED\xAB\xEE\xDB", 4)) return {"RPM", "rpm"};
    if (is("xar!", 4)) return {"XAR", "xar"};
    if (is("MSCF\0\0\0\0", 8)) return {"Cabinet", "cab"};
    if (is("WARC/", 5)) return {"WARC", "warc"};
    if (n >= 7 && p[2] == '-' && p[3] == 'l' && (p[4] == 'h' || p[4] == 'z') && p[6] == '-') return {"LHA", "lzh"};
    if (at(32769, "CD001", 5)) return {"ISO 9660", "iso"};
    // Read by Kompressor itself.
    if (is("MSWIM\0\0\0", 8)) return {"WIM", "wim"};
    if (is("vhdxfile", 8)) return {"VHDX", "vhdx"};
    if (is("QFI\xFB", 4)) return {"QCOW2", "qcow2"};
    if (is("KDMV", 4)) return {"VMDK", "vmdk"};
    if (at(64, "\x7F\x10\xDA\xBE", 4) && is("<<< ", 4)) return {"VDI", "vdi"};
    if (is("hsqs", 4)) return {"SquashFS", "squashfs"};
    if (is("\x45\x3D\xCD\x28", 4)) return {"CramFS", "cramfs"};
    // ARJ: its signature is only two bytes, so the first header must also look right (an archive header).
    if (is("\x60\xEA", 2) && n > 10 && (p[2] | p[3] << 8) >= 30 && (p[2] | p[3] << 8) <= 2600 && p[10] == 2)
        return {"ARJ", "arj"};
    return {};
}

// Where a Windows program's (PE) own image ends in its file, which is where anything appended to it
// starts (a self-extracting archive's data, an installer's payload), or 0 when `p` is not the start of
// a PE file. `p` must hold the headers (the first 4 KiB nearly always do).
inline uint64_t peImageEnd(const uint8_t* p, size_t n) {
    auto rd16 = [&](size_t o) { return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8; };
    auto rd32 = [&](size_t o) { return rd16(o) | rd16(o + 2) << 16; };
    if (n < 0x40 || p[0] != 'M' || p[1] != 'Z') return 0;
    size_t pe = rd32(0x3C);
    if (pe > n || n - pe < 24 || std::memcmp(p + pe, "PE\0\0", 4) != 0) return 0;
    size_t nsec = rd16(pe + 6), table = pe + 24 + rd16(pe + 20);
    if (table > n || (n - table) / 40 < nsec) return 0;
    uint64_t end = 0;
    for (size_t i = 0; i < nsec; ++i) {
        size_t s = table + i * 40;
        uint64_t raw = rd32(s + 16), at = rd32(s + 20);
        if (raw && at + raw > end) end = at + raw;
    }
    return end;
}

// A self-extracting archive: a Windows program with a 7z, RAR, ZIP or Cabinet archive appended. Looks
// in the first 64 KiB after the program's image (7-Zip's installers put a short config text first).
// `readAt(offset, dst, n)` returns how many bytes it read. Returns where the archive starts in `at`.
template <class ReadAt>
Sniffed sniffSfx(const uint8_t* head, size_t n, uint64_t fileSize, ReadAt&& readAt, uint64_t* at = nullptr) {
    uint64_t start = peImageEnd(head, n);
    if (!start || start >= fileSize) return {};
    constexpr size_t kLook = 64 * 1024;
    uint8_t* buf = new uint8_t[kLook + 8];
    size_t got = readAt(start, buf, kLook);
    Sniffed r;
    for (size_t i = 0; i + 8 <= got && !r; ++i) {
        const uint8_t* q = buf + i;
        if (std::memcmp(q, "7z\xBC\xAF\x27\x1C", 6) == 0) r = {"7-Zip (self-extracting)", "exe"};
        else if (std::memcmp(q, "Rar!\x1A\x07", 6) == 0) r = {"RAR (self-extracting)", "exe"};
        else if (std::memcmp(q, "PK\x03\x04", 4) == 0) r = {"ZIP (self-extracting)", "exe"};
        else if (std::memcmp(q, "MSCF\0\0\0\0", 8) == 0) r = {"Cabinet (self-extracting)", "exe"};
        if (r && at) *at = start + i;
    }
    delete[] buf;
    return r;
}

// Whether a file name's extension already says this format (zip-based names like .docx or .jar count
// as ZIP, .tgz as gzip, ...). Used to point out archives with a misleading name.
inline bool nameFits(const char* fileName, const Sniffed& s) {
    if (!s) return true;
    const char* dot = std::strrchr(fileName, '.');
    if (!dot || !dot[1]) return false;
    char e[16] = {};
    size_t i = 0;
    for (const char* c = dot + 1; *c && i < sizeof e - 1; ++c) e[i++] = (char)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c);
    auto any = [&](std::initializer_list<const char*> l) {
        for (const char* x : l)
            if (std::strcmp(e, x) == 0) return true;
        return false;
    };
    if (any({"001", "002", "003"})) return true;  // split parts: only the first part has a header anyway
    std::string_view x = s.ext;
    if (x == "zip") return any({"zip", "zipx", "jar", "war", "ear", "apk", "ipa", "xpi", "nupkg", "whl", "epub", "docx", "xlsx",
                                "pptx", "odt", "ods", "odp", "vsix", "appx", "msix", "cbz", "crx", "aar", "kmz"});
    if (x == "7z") return any({"7z", "cb7"});
    if (x == "rar") return any({"rar", "cbr"}) || (e[0] == 'r' && e[1] >= '0' && e[1] <= '9');  // .r00 volumes
    if (x == "gz") return any({"gz", "tgz", "gzip", "z"});
    if (x == "bz2") return any({"bz2", "tbz", "tbz2", "bz"});
    if (x == "xz") return any({"xz", "txz"});
    if (x == "zst") return any({"zst", "tzst", "zstd"});
    if (x == "deb") return any({"deb", "ar", "a", "udeb"});
    if (x == "xar") return any({"xar", "pkg"});
    if (x == "lzh") return any({"lzh", "lha"});
    if (x == "iso") return any({"iso", "img"});
    if (x == "wim") return any({"wim", "swm", "esd"});
    if (x == "vhdx") return any({"vhdx", "avhdx"});
    if (x == "qcow2") return any({"qcow2", "qcow", "img"});
    if (x == "squashfs") return any({"squashfs", "sfs", "sqsh", "img", "snap"});
    if (x == "cramfs") return any({"cramfs", "img"});
    if (x == "arj") return any({"arj", "a01"}) || (e[0] == 'a' && e[1] >= '0' && e[1] <= '9');  // .a01 volumes
    return std::strcmp(e, s.ext) == 0;
}

}  // namespace kz
