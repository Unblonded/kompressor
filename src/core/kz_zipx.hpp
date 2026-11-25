// ZIP archives with entries libarchive can't decode: Deflate64 (method 9), which Windows Explorer
// writes for big files and 7-Zip on request. Read here from the ZIP specification: the central
// directory (with ZIP64), and Store, Deflate, Deflate64, BZip2 and LZMA entries. Encrypted ones stay
// with libarchive.
#pragma once

#include <string>

#include "kz_common.hpp"
#include "kz_native.hpp"

namespace kz::zipx {

// A ZIP with a Deflate64 entry and nothing encrypted.
bool handles(const Source& src);
bool open(const Source& src, native::Container& out, std::string& err);

}  // namespace kz::zipx
