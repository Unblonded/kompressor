// Kompressor - reading, exploring, extracting and creating archives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kz_common.hpp"
#include "kz_formats.hpp"

namespace kz {

struct Entry {
    std::string path;        // normalised, '/' separated, no trailing slash
    uint64_t size = 0;       // uncompressed size
    uint64_t packed = 0;     // compressed size if the format reports it, else 0
    int64_t mtime = 0;
    uint32_t mode = 0;
    bool isDir = false;
    bool isSymlink = false;
    bool encrypted = false;
    std::string linkTarget;
};

struct ArchiveInfo {
    std::string format;      // "ZIP 2.0 (deflation)", "7-Zip", "Kompressor KMP v1"...
    std::string filter;      // outer compression filter ("gzip", "xz", "none")
    std::string extra;       // extra stats line (e.g. KMP dedup/ratio summary)
    bool isKmp = false;
    bool hasEncrypted = false;
    uint64_t archiveSize = 0;
    uint64_t totalSize = 0;  // sum of entry sizes
    std::vector<Entry> entries;
};

struct ExtractOptions {
    std::string destDir;
    std::vector<std::string> only;  // when non-empty: entries equal to, or below, these paths
    std::string stripPrefix;        // removed from the front of extracted paths (extracting a sub-folder)
    std::string password;
    bool skipExisting = false;
};

// Identifies whether a source can be opened at all (cheap: reads headers only).
bool probeArchive(const Source& src, std::string& formatName);

bool listArchive(const Source& src, ArchiveInfo& out, const std::string& password, Progress& prog, std::string& err);
bool extractArchive(const Source& src, const ExtractOptions& opt, Progress& prog, std::string& err);
// Reads one member into memory (at most maxBytes; `truncated` reports whether more data exists).
bool readEntry(const Source& src, const std::string& entryPath, const std::string& password, uint64_t maxBytes,
               std::vector<uint8_t>& out, bool& truncated, Progress& prog, std::string& err);
// Decompresses everything and validates checksums without writing to disk.
bool testArchive(const Source& src, const std::string& password, Progress& prog, std::string& err);

// --- creation ------------------------------------------------------------------
struct InputItem {
    fs::path source;      // on disk
    std::string arcPath;  // name inside the archive
    bool isDir = false;
    bool isSymlink = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    uint32_t mode = 0;
    std::string linkTarget;
};

// Expands files/folders into a flat list. Folders keep their own name as the top level.
bool gatherInputs(const std::vector<std::string>& inputs, std::vector<InputItem>& out, Progress& prog, std::string& err);

struct CompressOptions {
    OutFormat format = OutFormat::Zip;
    int level = -1;          // -1 = format default; for KMP: 0 Fast, 1 Balanced, 2 Ultra, 3 Max
    std::string password;    // ZIP only
    int threads = 0;         // 0 = auto
    bool dedup = true;       // KMP only
};

struct CompressResult {
    uint64_t inputBytes = 0;
    uint64_t outputBytes = 0;
    uint64_t files = 0;
    std::string summary;
};

bool createArchive(const std::vector<std::string>& inputs, const std::string& outPath, const CompressOptions& opt,
                   Progress& prog, CompressResult& res, std::string& err);

// Re-packs any readable archive into a .kmp without extracting to disk.
bool convertToKmp(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                  Progress& prog, CompressResult& res, std::string& err);

// Re-packs any readable archive (including .kmp) into any output format without extracting to disk.
// Single-file formats (GZ, XZ, ...) need a source holding exactly one file.
bool convertArchive(const Source& src, const std::string& outPath, const CompressOptions& opt, const std::string& password,
                    Progress& prog, CompressResult& res, std::string& err);

std::string libraryVersions();

}  // namespace kz
