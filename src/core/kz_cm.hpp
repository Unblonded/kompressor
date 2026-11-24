// KCM - Kompressor's context-mixing codec (used by KMP "Extreme" mode).
//
// A bitwise predictive compressor in the PAQ family. Every bit of the input is predicted from many
// models at once, the predictions are combined by small online-trained neural mixers, refined by
// adaptive probability maps (SSE) and fed to a binary arithmetic coder:
//
//   * order-0 and order-1 direct models, order-2/3/4/6/8 hashed models (nibble buckets with checks)
//   * text profile: word, word-pair and column (byte above in the previous line) models
//   * binary/x86 profile: sparse and nibble-masked models for tables and machine code, plus an
//     E8/E9 call/jump address transform so repeated call targets become repeated bytes
//   * a long-range match model that follows the most recent repeat of the last 6+ bytes
//   * three mixers selected by different contexts, a second-layer mixer, then two SSE stages
//
// Compression and decompression run the same model, so both are slow (a few MB/s per core);
// KMP splits input into blocks and runs one codec instance per core.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace kz::cm {

enum Profile : uint8_t { Generic = 0, Text = 1, X86 = 2 };

// Appends the compressed form of in[0..n) to `out` (which is cleared first).
// level 0 = Extreme, 1 = Ultimate (bigger tables, longer contexts, more models and mixers).
// `tick`, when given, is called every 64 KiB or so with the number of bytes processed since the last call.
using Tick = std::function<void(size_t)>;
bool compress(const uint8_t* in, size_t n, Profile prof, std::vector<uint8_t>& out, int level = 0, const Tick& tick = {});
// Decodes exactly n bytes into out. Returns false on corrupt input.
bool decompress(const uint8_t* in, size_t packed, uint8_t* out, size_t n, Profile prof, int level = 0, const Tick& tick = {});
// Approximate working memory for one codec instance on a block of n bytes.
uint64_t memoryFor(size_t n, int level = 0);

}  // namespace kz::cm
