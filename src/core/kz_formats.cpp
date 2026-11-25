#include "kz_formats.hpp"

#include <algorithm>
#include <cctype>

#include "kz_common.hpp"
#include "kz_sniff.hpp"

#include <filesystem>
#include <fstream>

namespace kz {

const std::vector<FormatDesc>& outputFormats() {
    static const std::vector<FormatDesc> f = {
        {OutFormat::Kmp, "KMP (Super)", ".kmp", "Kompressor super compression: dedup + content-aware LZMA2/Zstd", 0, 5, 2, false, false},
        {OutFormat::SevenZip, "7Z", ".7z", "7-Zip archive, LZMA2", 0, 9, 9, false, false},
        {OutFormat::Zip, "ZIP", ".zip", "Universal ZIP (Deflate), optional AES-256", 0, 9, 6, true, false},
        {OutFormat::Rar, "RAR", ".rar", "RAR5 archive, stored (RAR compression is proprietary)", 0, 0, 0, false, false},
        {OutFormat::TarXz, "TAR.XZ", ".tar.xz", "Tarball compressed with XZ/LZMA2", 0, 9, 9, false, false},
        {OutFormat::TarZst, "TAR.ZST", ".tar.zst", "Tarball compressed with Zstandard", 1, 22, 19, false, false},
        {OutFormat::TarGz, "TAR.GZ", ".tar.gz", "Tarball compressed with gzip", 1, 9, 9, false, false},
        {OutFormat::TarBz2, "TAR.BZ2", ".tar.bz2", "Tarball compressed with bzip2", 1, 9, 9, false, false},
        {OutFormat::TarLz4, "TAR.LZ4", ".tar.lz4", "Tarball compressed with LZ4 (very fast)", 1, 9, 1, false, false},
        {OutFormat::Tar, "TAR", ".tar", "Uncompressed tarball", 0, 0, 0, false, false},
        {OutFormat::Gz, "GZ", ".gz", "Single file, gzip", 1, 9, 9, false, true},
        {OutFormat::Xz, "XZ", ".xz", "Single file, XZ/LZMA2", 0, 9, 9, false, true},
        {OutFormat::Zst, "ZST", ".zst", "Single file, Zstandard", 1, 22, 19, false, true},
        {OutFormat::Bz2, "BZ2", ".bz2", "Single file, bzip2", 1, 9, 9, false, true},
        {OutFormat::Lz4, "LZ4", ".lz4", "Single file, LZ4", 1, 9, 1, false, true},
        {OutFormat::Iso, "ISO", ".iso", "ISO 9660 disc image (Rock Ridge + Joliet)", 0, 0, 0, false, false},
        {OutFormat::Cpio, "CPIO", ".cpio", "cpio (newc) archive", 0, 0, 0, false, false},
    };
    return f;
}

const FormatDesc& formatDesc(OutFormat id) {
    for (auto& f : outputFormats())
        if (f.id == id) return f;
    return outputFormats().front();
}

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool formatFromFileName(const std::string& name, OutFormat& out) {
    std::string n = lower(fileNameOf(name));
    // Aliases first.
    struct Alias { const char* ext; OutFormat f; };
    static const Alias aliases[] = {{".tgz", OutFormat::TarGz},  {".tbz2", OutFormat::TarBz2}, {".tbz", OutFormat::TarBz2},
                                    {".txz", OutFormat::TarXz},  {".tzst", OutFormat::TarZst}, {".tlz4", OutFormat::TarLz4}};
    for (auto& a : aliases)
        if (endsWith(n, a.ext)) { out = a.f; return true; }
    // Longest matching extension wins (".tar.gz" before ".gz").
    const FormatDesc* best = nullptr;
    for (auto& f : outputFormats()) {
        std::string e = f.ext;
        if (endsWith(n, e) && (!best || e.size() > std::char_traits<char>::length(best->ext))) best = &f;
    }
    if (!best) return false;
    out = best->id;
    return true;
}

std::string withFormatExtension(const std::string& path, OutFormat f) {
    std::string base = path;
    std::string n = lower(base);
    size_t cut = std::string::npos;
    for (auto& d : outputFormats()) {
        std::string e = d.ext;
        if (endsWith(n, e) && (cut == std::string::npos || base.size() - e.size() < cut)) cut = base.size() - e.size();
    }
    for (const char* e : {".tgz", ".tbz2", ".txz", ".tzst"})
        if (endsWith(n, e)) cut = base.size() - std::char_traits<char>::length(e);
    if (cut != std::string::npos) base = base.substr(0, cut);
    return base + formatDesc(f).ext;
}

const std::vector<std::pair<std::string, std::string>>& readableFormats() {
    static const std::vector<std::pair<std::string, std::string>> r = {
        {"KMP", "Kompressor super-compressed archives"},
        {"ZIP / ZIPX / JAR / APK / IPA / WAR / XPI / NUPKG / WHL", "incl. ZIP64, Deflate64, BZip2, LZMA, XZ, Zstd, AES & ZipCrypto"},
        {"7Z", "LZMA, LZMA2, PPMd, BZip2, Deflate, BCJ/BCJ2/ARM64 filters; split .7z.001"},
        {"RAR / RAR5", "incl. solid and multi-volume (.partN.rar) archives"},
        {"TAR", "ustar, pax, GNU, star; sparse files"},
        {"GZ / BZ2 / XZ / LZMA / ZST / LZ4 / LZ / LZO / Z / UU", "single-file streams and compressed tarballs"},
        {"ISO 9660", "CD/DVD images incl. Rock Ridge & Joliet"},
        {"CAB", "Microsoft Cabinet"},
        {"LHA / LZH", "LHarc archives"},
        {"XAR / PKG", "macOS installer packages"},
        {"CPIO / RPM", "including RPM payloads"},
        {"AR / DEB / LIB / A", "Unix archives and Debian packages (explore nested tarballs)"},
        {"WARC / MTREE / SHAR", "web archives and manifests"},
        {"EXE (self-extracting)", "7-Zip, RAR, ZIP and Cabinet archives inside programs"},
        {"EXE / DLL / SYS / EFI", "Windows programs: sections, resources (icons, bitmaps, version, manifest), overlay"},
        {"MSI / MSP / DOC / XLS / PPT / MSG", "Compound Files: installer and document streams, embedded cabinets"},
        {"NSIS installers", "Nullsoft setup programs: LZMA (solid), deflate and stored, with $INSTDIR/$PLUGINSDIR paths"},
        {"WIM / SWM / ESD", "Windows images: XPRESS, LZX, LZMS, solid ESD, split images"},
        {"CHM / HXS", "compiled HTML help"},
        {"ARJ", "methods 0-4"},
        {"VHD / VHDX / VMDK / VDI / QCOW2 / IMG", "virtual and raw disks, straight through to their partitions"},
        {"DMG", "Apple disk images: zlib, bzip2, ADC, LZFSE, LZMA"},
        {"MBR / GPT / APM / LVM / Android super", "partition maps and volume managers"},
        {"NTFS / FAT / exFAT / ext2-4", "file systems, incl. NTFS and Windows compact-file compression"},
        {"HFS+ / HFSX / APFS", "Mac file systems, incl. compressed files"},
        {"UDF / SquashFS / CramFS", "disc and read-only file systems"},
        {"ELF / Mach-O / TE / COFF", "program sections; universal (fat) Mach-O binaries"},
        {"UEFI firmware", "firmware volumes, capsules and flash images: PE, TE, raw, LZMA and Tiano sections"},
        {"SWF / FLV", "Flash: the movie inside compressed SWFs, pictures and sounds, video and audio streams"},
        {"MsLZ (.ex_) / Base64 / Intel HEX", "COMPRESS.EXE files, base64 text, firmware HEX images"},
    };
    return r;
}

bool looksLikeArchive(const std::string& name) {
    static const char* exts[] = {"kmp", "zip", "zipx", "jar", "apk", "ipa", "war", "ear", "xpi", "nupkg", "whl", "epub",
                                 "docx", "xlsx", "pptx", "odt", "ods", "odp", "7z", "rar", "tar", "gz", "tgz", "bz2", "tbz",
                                 "tbz2", "xz", "txz", "lzma", "zst", "tzst", "lz4", "lz", "lzo", "z", "uu", "iso", "cab",
                                 "lha", "lzh", "xar", "pkg", "cpio", "rpm", "ar", "deb", "a", "warc", "mtree", "shar",
                                 "crx", "vsix", "appx", "msix", "cbz", "cbr", "cb7", "001",
                                 // read by Kompressor itself: images, disks and other archivers
                                 "wim", "swm", "esd", "arj", "dmg", "vhd", "vhdx", "avhdx", "vmdk", "vdi", "qcow", "qcow2",
                                 "img", "ima", "simg", "squashfs", "sfs", "cramfs", "udf", "hfs", "hfsx", "apfs", "ext2",
                                 "ext3", "ext4", "fat", "ntfs", "mbr", "gpt"};
    if (looksLikeVolume(name)) return true;
    std::string e = lowerExt(name);
    if (e.empty()) return false;
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

Sniffed sniffFile(const std::string& path) {
    std::ifstream f(u8path(path), std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> buf(kSniffBytes);
    f.read((char*)buf.data(), (std::streamsize)buf.size());
    size_t got = (size_t)f.gcount();
    if (Sniffed s = sniff(buf.data(), got)) return s;
    // A self-extracting archive is a program with the archive after it.
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(u8path(path), ec);
    if (ec) return {};
    f.clear();
    return sniffSfx(buf.data(), got, size, [&](uint64_t off, uint8_t* dst, size_t n) -> size_t {
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)dst, (std::streamsize)n);
        return (size_t)f.gcount();
    });
}

bool opensAsContainer(const std::string& name) {
    static const char* exts[] = {"exe", "dll", "sys", "ocx", "cpl", "scr", "efi", "mui", "ax",  "drv",
                                 "msi", "msp", "msm", "mst", "doc", "xls", "ppt", "msg", "pub", "vsd",
                                 // files that are something else first, but hold files too
                                 "chm", "chi", "chw", "hxs", "swf", "flv", "hex", "ihx", "b64", "base64", "so", "dylib",
                                 "o", "obj", "elf", "ko", "axf", "te", "fd", "rom", "cap", "scap", "lpimg"};
    std::string e = lowerExt(name);
    for (const char* x : exts)
        if (e == x) return true;
    // MS-DOS "COMPRESS" files: the last letter of the extension became "_" (setup.ex_, readme.tx_).
    return e.size() == 3 && e[2] == '_';
}

bool isArchiveFile(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(u8path(path), ec)) return false;
    // A self-extracting program still counts as a program here: dropped on the window it gets
    // compressed. Open (or "Open archive") shows the archive inside it.
    Sniffed s = sniffFile(path);
    if (s && std::strcmp(s.ext, "exe") != 0) return true;
    return looksLikeArchive(path);
}

}  // namespace kz
