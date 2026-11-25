// Kompressor - reading, exploring, extracting and creating archives.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kz_common.hpp"
#include "kz_formats.hpp"

struct archive;

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

// Opens a libarchive reader on a source: memory, a single file, or every volume of a multi-volume
// set. Returns the libarchive status code.
int openReadSource(archive* r, const Source& src);
// Total size of a source (all volumes of a set).
uint64_t sourceSize(const Source& src);

// Identifies whether a source can be opened at all (cheap: reads headers only).
bool probeArchive(const Source& src, std::string& formatName);

bool listArchive(const Source& src, ArchiveInfo& out, const std::string& password, Progress& prog, std::string& err);
bool extractArchive(const Source& src, const ExtractOptions& opt, Progress& prog, std::string& err);
// Reads one member into memory (at most maxBytes; `truncated` reports whether more data exists).
bool readEntry(const Source& src, const std::string& entryPath, const std::string& password, uint64_t maxBytes,
               std::vector<uint8_t>& out, bool& truncated, Progress& prog, std::string& err);
// Decompresses everything and validates checksums without writing to disk.
bool testArchive(const Source& src, const std::string& password, Progress& prog, std::string& err);

// Streams the contents of every file in an archive, in archive order. begin() decides whether the
// entry is wanted; data() gets its bytes in pieces (false stops everything); end() reports whether the
// entry read completely. Returns false when the archive can't be opened, or on cancel.
struct EntryVisitor {
    std::function<bool(const std::string& path, uint64_t size)> begin;
    std::function<bool(const uint8_t* p, size_t n)> data;
    std::function<void(bool ok, const std::string& err)> end;
};
bool visitEntries(const Source& src, const std::string& password, EntryVisitor& v, Progress& prog, std::string& err);

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
    uint64_t volumeSize = 0; // >0: split the finished archive into .001, .002 ... parts of this size
};

struct CompressResult {
    uint64_t inputBytes = 0;
    uint64_t outputBytes = 0;
    uint64_t files = 0;
    std::string summary;
    std::vector<std::string> parts;  // the files written: the archive itself, or its .001 ... volumes
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

// A change to an existing archive: entries to drop and files from disk to add.
struct ArchiveEdit {
    std::vector<std::string> remove;  // archive paths; each entry and everything below it is dropped
    std::vector<std::string> add;     // files / folders on disk (an entry with the same path is replaced)...
    std::string addUnder;             // ...placed in this archive folder ("" = the top level)
};

// Whether Kompressor can write `src` back (a file on disk in a format it can create, not split into
// volumes). `why` explains a refusal.
bool canEditArchive(const Source& src, std::string& why);

// Applies `edit` in place. The archive is rewritten in its own format into a temporary file that
// replaces it at the end, so every entry is recompressed (RAR comes back stored). `password` is the
// archive's password when it is encrypted; ZIP keeps it, other formats refuse.
bool editArchive(const Source& src, const ArchiveEdit& edit, const std::string& password, Progress& prog,
                 CompressResult& res, std::string& err);

std::string libraryVersions();

}  // namespace kz
