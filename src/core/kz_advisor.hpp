// Recommends an output format for a set of files and a goal ("smallest", "share with anyone", ...).
//
// The files are profiled first: what kinds of data they hold (by extension), how much of it is already
// compressed (a quick zstd probe over a sample of every file kind), and how much looks duplicated.
// The recommendation picks a format and level from that profile, with a rough size and time estimate
// calibrated on a mixed 553 MB corpus.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kz_archive.hpp"

namespace kz {

struct ContentProfile {
    uint64_t totalBytes = 0, files = 0, dirs = 0, largestFile = 0;
    // bytes by kind (by extension)
    uint64_t textBytes = 0;   // text, code, markup, data files
    uint64_t mediaBytes = 0;  // already compressed: photos, audio, video, archives, office docs, PDF
    uint64_t exeBytes = 0;    // programs and libraries
    uint64_t otherBytes = 0;
    uint64_t dupBytes = 0;    // extra copies of files that look identical (same size and sample)
    // compressibility, from the zstd probe (or guessed from an archive's listing)
    double incompressible = 0.0;  // share of the bytes that did not shrink
    double ratio = 0.5;           // zstd -3 output / input over the bytes that did
    bool measured = false;        // the ratio comes from real data
};

// Profiles files and folders on disk. Reads a few MB of samples at most.
bool profileInputs(const std::vector<std::string>& inputs, ContentProfile& out, Progress& prog, std::string& err);
// Profiles an archive's contents from its listing alone (no data read): kinds by extension,
// compressibility from the archive's current size.
ContentProfile profileListing(const ArchiveInfo& info);

enum class Goal { Balanced, Smallest, Fastest, Share, Protect };
const char* goalName(Goal g);

struct Recommendation {
    OutFormat format = OutFormat::Zip;
    int level = -1;              // format level, or the KMP mode (0..5) for KMP
    std::string title;           // "7Z, level 5"
    std::string why;             // a sentence or two
    std::vector<std::string> notes;
    uint64_t estBytes = 0;       // rough output size
    double estSeconds = 0;       // rough time on this machine
    bool wantsPassword = false;  // goal is protection: fill in a password
};

Recommendation recommend(const ContentProfile& p, Goal g, unsigned threads = 0);

// Rough output size and time of a given format/level for a profile (used for comparisons).
void estimate(const ContentProfile& p, OutFormat f, int level, unsigned threads, uint64_t& bytes, double& seconds);

}  // namespace kz
