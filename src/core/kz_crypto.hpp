// SHA-256 and AES-256 decryption, written here (no outside code). AES uses the CPU's AES
// instructions when it has them (x86 AES-NI) and lookup tables otherwise.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kz::crypto {

class Sha256 {
public:
    Sha256();
    void update(const uint8_t* p, size_t n);
    void final(uint8_t out[32]);

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

// AES-256 in CBC mode, decrypting. The IV carries over between calls, so a stream can be decrypted
// in pieces of any multiple of 16 bytes.
class Aes256CbcDecrypt {
public:
    Aes256CbcDecrypt(const uint8_t key[32], const uint8_t iv[16]);
    void decrypt(uint8_t* p, size_t n);  // n a multiple of 16; in place

private:
    uint32_t rk_[60];        // decryption round keys (table version)
    alignas(16) uint8_t ni_[15][16];  // decryption round keys (AES-NI version)
    uint8_t iv_[16];
    bool useNi_ = false;
};

// FIPS-197 / FIPS-180 known answers; false if this build computes either wrong.
bool selfTest();

}  // namespace kz::crypto
