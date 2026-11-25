// 7z archives that libarchive can't read: password-protected ones, with or without encrypted file
// names. Read here from the format's description (no outside code): the headers, AES-256 with 7-Zip's
// SHA-256 key derivation, and the usual methods (LZMA, LZMA2, BCJ/ARM/... filters and Delta through
// liblzma, Deflate through zlib, BZip2, BCJ2 and Copy here). PPMd and Deflate64 inside an encrypted
// 7z are reported as unsupported.
#pragma once

#include <string>

#include "kz_common.hpp"
#include "kz_native.hpp"

namespace kz::sevenzip {

// Whether `src` is a 7z archive to read here rather than with libarchive: its names or contents are
// encrypted (or KZ_NATIVE_7Z=1 is set, which sends every 7z here, for testing).
bool handles(const Source& src);

// Lists the archive into `out`; its items read through the decoders here. `reading`: the caller is
// about to read contents, so a missing or wrong password is reported now. `needPassword` is set
// when a password is missing or wrong.
bool open(const Source& src, const std::string& password, bool reading, native::Container& out, bool& encrypted,
          bool& needPassword, std::string& err);

}  // namespace kz::sevenzip
