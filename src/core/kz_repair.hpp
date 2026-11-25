// Getting what can still be read out of a damaged archive.
//  - ZIP: rebuilt. The whole file is scanned for entries (the central directory at the end is often
//    what a cut-off download loses); every entry whose data checks out against its CRC goes into a
//    new ZIP with a fresh directory, its compressed bytes copied as they are.
//  - Anything else (7z, TAR, RAR...): salvaged. Every file that reads back intact is extracted into a
//    folder; the rest are listed as lost.
#pragma once

#include <string>
#include <vector>

#include "kz_common.hpp"

namespace kz {

struct RepairResult {
    std::string output;                 // the rebuilt ZIP, or the folder the files went into
    bool rebuilt = false;               // true: `output` is an archive; false: a folder
    uint64_t recovered = 0, lost = 0;   // files
    uint64_t unchecked = 0;             // recovered, but couldn't be verified (encrypted, unknown method)
    std::vector<std::string> lostNames;
    std::string summary;
};

// `outPath` empty: "name (repaired).zip" / "name (recovered)" beside the archive.
bool repairArchive(const Source& src, const std::string& outPath, const std::string& password, Progress& prog,
                   RepairResult& res, std::string& err);

}  // namespace kz
