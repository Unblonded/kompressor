// Searching the contents of every file in an archive, streamed (nothing is extracted to disk): text
// as UTF-8 and UTF-16LE, or raw bytes given in hex, with a line number and a snippet for each hit.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kz_common.hpp"

namespace kz {

struct SearchOptions {
    std::string text;          // what to find (UTF-8), or hex digits when `hex`
    bool hex = false;          // "4D 5A 90" / "4d5a90": raw bytes
    bool matchCase = false;    // text only; ASCII letters fold, others match exactly
    std::string names;         // "*.txt;*.ini": only files whose name or path matches; empty = all
    size_t maxHitsPerFile = 100;  // hits kept per file (all are counted)
};

struct SearchHit {
    uint64_t offset = 0;   // byte offset in the file
    uint32_t line = 0;     // 1-based; 0 in binary files
    std::string snippet;   // the line (or the bytes around it), printable UTF-8
    uint32_t matchAt = 0, matchLen = 0;  // the match inside `snippet`
    bool utf16 = false;    // found as UTF-16LE text
};

struct FileHits {
    std::string path;
    uint64_t size = 0;
    uint64_t count = 0;    // every match, including those not kept in `hits`
    bool binary = false;
    std::vector<SearchHit> hits;
    std::string error;     // the file couldn't be read completely
};

// Parses SearchOptions::text into the bytes to find; false (with `err`) when it's empty or bad hex.
bool searchPattern(const SearchOptions& o, std::vector<uint8_t>& bytes, std::string& err);
// Whether `path` matches a "*.txt;*.ini" list (case-insensitive, against the name and the whole path).
bool nameMatches(const std::string& path, const std::string& globs);

// Searches the files of an archive (those equal to or below `only`, or all) and hands every file
// with at least one hit (or a read error) to `onFile`, from the worker thread, as it's found.
bool searchArchive(const Source& src, const std::vector<std::string>& only, const std::string& password,
                   const SearchOptions& o, const std::function<void(FileHits&&)>& onFile, Progress& prog,
                   std::string& err);

}  // namespace kz
