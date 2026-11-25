// Formats Kompressor reads itself (no outside code), covering what 7-Zip opens beyond libarchive:
//  - programs: Windows PE (sections, resources as files, signature, overlay), TE, ELF, Mach-O and
//    universal binaries, COFF objects;
//  - installers and packs: NSIS, MSI and other Compound Files, CHM/HXS help, WIM/ESD, ARJ, DMG,
//    SZDD (MS-LZ), compressed SWF, FLV streams, PPMd, Base64, Intel HEX, UEFI firmware;
//  - disks: VHD, VHDX, VDI, VMDK, QCOW2, Android sparse/super images, MBR, GPT, Apple partition maps,
//    LVM, and the file systems in them: NTFS, FAT, exFAT, ext2/3/4, HFS+, APFS, UDF, SquashFS, CramFS.
// A disk image is listed straight through to its files: "VHDX > GPT > NTFS" shows the NTFS files under
// a folder per partition.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "kz_common.hpp"

namespace kz::native {

struct Item {
    std::string path;  // '/' separated
    bool isDir = false;
    uint64_t size = 0;
    int64_t mtime = 0;
    // Hands the item's bytes to the sink in pieces; a false return from the sink stops (not an error).
    std::function<bool(const std::function<bool(const uint8_t*, size_t)>&, std::string&)> read;
};

struct Container {
    std::string format;  // "VHDX > GPT > NTFS", "Windows program (PE32+, x64)"...
    std::vector<Item> items;
    std::string warning;  // something was skipped or couldn't be read
};

// Reads the source's layout. False when no format here recognises it.
bool open(const Source& src, Container& out, std::string& err);
// True for files that must be read here even though libarchive would take them: an NSIS installer
// can carry a stored 7z or ZIP that libarchive would open instead of the installer itself.
bool claimsFirst(const Source& src);

bool readItem(const Source& src, const Item& item, const std::function<bool(const uint8_t*, size_t)>& sink,
              std::string& err);

}  // namespace kz::native
