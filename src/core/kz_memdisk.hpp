// A disk held in this process's memory, served over iSCSI on 127.0.0.1 so that Windows' own iSCSI
// initiator can attach it as an ordinary local disk. This is how the Windows RAM drive keeps its files in
// RAM without a kernel driver: the initiator ships with Windows, and the disk's blocks live here.
//
// Memory is taken a megabyte at a time as data is written, and given back when Windows trims what
// files no longer use (deleting files). Only one initiator login is accepted: the target name is random
// and login needs a random CHAP secret that only this process and the initiator are told.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace kz {

class MemDisk {
public:
    MemDisk();
    ~MemDisk();  // stops serving and frees the memory
    MemDisk(const MemDisk&) = delete;
    MemDisk& operator=(const MemDisk&) = delete;

    // Starts listening on 127.0.0.1 (a free port) for a disk of `size` bytes.
    bool start(uint64_t size, std::string& err);
    void stop();

    uint16_t port() const;
    const std::string& targetName() const;  // iqn....
    const std::string& chapName() const;
    const std::string& chapSecret() const;  // 16 characters (the Windows initiator wants 12 to 16)
    uint64_t size() const;
    uint64_t resident() const;  // bytes of RAM holding data right now

    // Identifies the disk Windows shows for it (SCSI vendor and product).
    static const char* vendor();   // "KOMPRESS"
    static const char* product();  // "RAM Drive"
    const std::string& serial() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace kz
