// Self-extracting archives (Windows): a small extractor program with the archive appended, so the
// files come out with a double-click on a PC without Kompressor. The extractor (src/sfx) is embedded
// in Kompressor's own programs; the .exe is laid out as [extractor][archive][32-byte trailer], and
// the trailer says where the archive starts. Other archive tools find the archive right after the
// program's image, like any self-extracting 7z or ZIP.
#pragma once

#include <string>

#include "kz_formats.hpp"

namespace kz {

// Whether this build can make them (Windows builds carry the extractor).
bool sfxSupported();
// Whether the extractor can read this format (7z, ZIP and the TAR family; not KMP or single files).
bool sfxFormatOk(OutFormat f);
// Turns the finished archive at `path` into a self-extracting program, in place. `encrypted` makes
// the extractor ask for a password.
bool makeSelfExtracting(const std::string& path, bool encrypted, std::string& err);
// Where a self-extracting version of an archive goes: "C:/x/photos.7z" -> "C:/x/photos.exe".
std::string sfxPathFor(const std::string& archivePath);

}  // namespace kz
