// A fast scratch drive that disappears when it stops, made with what each system has built in:
//  - Windows: a local NTFS drive (under "Devices and drives") whose blocks are held in RAM by an
//    elevated copy of this program (kz_memdisk) and attached through Windows' own iSCSI initiator
//    over 127.0.0.1. That copy holds it and detaches it when the drive stops or this program exits,
//    however it exits; the memory goes with it.
//  - macOS: a real RAM disk (hdiutil ram://), shown in Finder as a volume.
//  - Linux: a folder in /dev/shm, which is memory.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kz {

class RamDrive {
public:
    RamDrive();
    ~RamDrive();  // stops it
    RamDrive(const RamDrive&) = delete;
    RamDrive& operator=(const RamDrive&) = delete;

    // Makes the drive, holding up to `capacity` bytes. `letter` ('R') is used on Windows only.
    bool start(uint64_t capacity, char letter, std::string& err);
    void stop();  // everything on it is deleted

    bool running() const { return !where_.empty(); }
    const std::string& where() const { return where_; }  // "R:\", "/Volumes/Kompressor RAM", a folder
    uint64_t capacity() const;
    uint64_t used() const;
    bool hasFiles() const;  // anything the user put there

    // What the drive is, for the UI ("a local drive held in memory" ...).
    static const char* kind();
    static bool needsAdmin();

    // Windows: the elevated helper. Returns -1 when `args` isn't a helper command line, else the
    // exit code. Called first thing by the programs' main().
    static int helperMain(const std::vector<std::string>& args);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string where_;
};

}  // namespace kz
