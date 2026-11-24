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
    return {};
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
    return std::strcmp(e, s.ext) == 0;
}

}  // namespace kz
