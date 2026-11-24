// Graphics-card help for deflate (ZIP).
//
// The card finds the repeated strings, which is most of deflate's work. Each 32 KiB block gets a
// group of 32 GPU threads that builds zlib-style hash chains over the block and the 32 KiB before
// it, and every thread walks its position's chain as deep as the level asks for the longest match.
// One thread of the group then picks matches the way zlib's lazy parser does. The CPU writes those
// matches out as ordinary deflate blocks with their own Huffman codes, so the result is a normal ZIP
// that every unzip tool opens, about the size zlib makes at the same level.
//
// It runs through Direct3D 11 compute shaders, which ship with Windows (nothing to install). On
// other systems supported() is false and nothing changes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kz::gpu {

// The user's setting ("use the graphics card too").
void setEnabled(bool on);
bool enabled();

// True when this build can use a graphics card at all.
bool supported();
// Starts the card on first use (later calls are free). False if there is no usable one, or it
// failed earlier in this run.
bool available();
// Starts the card on a background thread (opening it takes Windows a few hundred milliseconds), so
// work can begin on the CPU meanwhile.
void warmUp();
// Whether the card is ready (true), or still starting or not usable (false); never waits.
bool ready();
// True once it is known the card can't be used in this run.
bool unusable();
// The card in use, e.g. "NVIDIA GeForce RTX 4070 Ti" (empty until available() succeeds).
std::string adapterName();

// One piece of a raw deflate stream.
struct DeflateJob {
    const uint8_t* dict = nullptr;  // up to 32 KiB of data that came just before, as history
    size_t dictLen = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
    bool final = false;  // ends the stream; otherwise it ends on a byte boundary (like Z_SYNC_FLUSH)
    // What the card found (filled by finish()).
    std::vector<uint32_t> tokens;
    std::vector<uint32_t> blockTokens;  // tokens per 32 KiB block
    // The compressed piece (filled by encode()).
    std::vector<uint8_t> out;
};

// How much data one batch should carry to keep the card busy.
size_t batchBytes();

// A batch on its way through the card. Two can be in flight at once, so the card works on one while
// the CPU uploads or reads back the other.
class Batch;
struct BatchDeleter {
    void operator()(Batch* b) const;
};
using BatchPtr = std::unique_ptr<Batch, BatchDeleter>;

// Uploads the jobs and starts the card on them (deflate level 1..9), returning at once. Returns
// null with `busy` set when both batch slots are taken (finish one, or wait for another writer's),
// or null with an error when the card failed. The jobs must stay alive until finish().
BatchPtr begin(const std::vector<DeflateJob*>& jobs, int level, bool& busy, std::string& err);
// Waits for the card and fills each job's tokens. On false (the card failed or was reset) the jobs
// need compressing some other way, and the card is not used again in this run.
bool finish(Batch& b, std::string& err);
// True once the card is done with the batch (finish() then returns without waiting).
bool ready(Batch& b);
// Turns a finished job's tokens into deflate data (CPU work; any thread).
void encode(DeflateJob& job);

}  // namespace kz::gpu
