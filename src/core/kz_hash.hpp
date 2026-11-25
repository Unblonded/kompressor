// Checksums of archive entries (CRC32, MD5, SHA-1, SHA-256), computed while streaming each entry,
// and checking them against hash lists (sha256sum / md5sum style, or BSD "SHA256 (name) = ...").
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kz_common.hpp"

namespace kz {

enum HashKind : unsigned { kHashCrc32 = 1, kHashMd5 = 2, kHashSha1 = 4, kHashSha256 = 8 };

// One running digest. update() any number of times, then hex().
class Digest {
public:
    explicit Digest(HashKind k);
    ~Digest();
    Digest(const Digest&) = delete;
    Digest& operator=(const Digest&) = delete;
    void update(const uint8_t* p, size_t n);
    std::string hex();  // lowercase; finishes the digest

private:
    struct State;
    HashKind kind_;
    State* st_;
};

struct EntryHash {
    std::string path;
    uint64_t size = 0;
    std::string crc32, md5, sha1, sha256;  // filled for the kinds asked for
    std::string error;                     // when the entry couldn't be read
    const std::string& get(HashKind k) const {
        return k == kHashCrc32 ? crc32 : k == kHashMd5 ? md5 : k == kHashSha1 ? sha1 : sha256;
    }
};

// Hashes the files of an archive (those equal to or below `only`, or all when empty) with the kinds
// in `which` (a mask of HashKind). Entries that fail to read are listed with their error; the call
// itself fails only when the archive can't be opened or the work is cancelled.
bool hashArchive(const Source& src, const std::vector<std::string>& only, const std::string& password, unsigned which,
                 std::vector<EntryHash>& out, Progress& prog, std::string& err);

// Hashes files on disk: the files given, and every file inside the folders given (recursively).
// Paths in `out` are '/' separated and relative to `base` (set here): the folder itself when one
// folder is given, otherwise the folder holding the inputs - so a list saved in `base` can be
// checked there with sha256sum -c. Files that can't be read are listed with their error.
bool hashFiles(const std::vector<std::string>& inputs, unsigned which, std::vector<EntryHash>& out, std::string& base,
               Progress& prog, std::string& err);

// A hash list: one "<hex>  <name>" (or "<hex> *<name>") per line, as sha256sum writes.
std::string formatHashList(const std::vector<EntryHash>& hashes, HashKind kind);

struct ListedHash {
    std::string name;  // '/' separated
    std::string hex;   // lowercase
};
// Reads sha256sum/md5sum/sha1sum style lines, BSD-style "SHA256 (name) = hex" lines and .sfv
// ("name crc32"). Lines that are neither are skipped.
std::vector<ListedHash> parseHashList(const std::string& text);
// Which kind a hex digest is, by its length (8 CRC32, 32 MD5, 40 SHA-1, 64 SHA-256); 0 when none.
unsigned hashKindOfHex(const std::string& hex);
// The kinds a list's digests need (a mask of HashKind).
unsigned hashKindsOfList(const std::vector<ListedHash>& list);

struct HashCheck {
    enum State { Ok, Mismatch, Missing, Unreadable } state = Ok;
    std::string name;   // as the list gives it
    std::string entry;  // the archive entry it was matched with (empty when Missing)
    std::string expected, actual;
    HashKind kind = kHashSha256;
};
// Matches each listed name with an archive entry (exactly, then ignoring case, then by a unique
// path ending, so a list made from an extracted folder still matches) and compares the digests.
// `notListed` gets the entries that no line of the list names.
std::vector<HashCheck> checkHashes(const std::vector<EntryHash>& hashes, const std::vector<ListedHash>& list,
                                   std::vector<std::string>* notListed = nullptr);

const char* hashName(HashKind k);  // "CRC32", "MD5", "SHA-1", "SHA-256"
const char* hashListExt(HashKind k);  // ".sfv", ".md5", ".sha1", ".sha256"

}  // namespace kz
