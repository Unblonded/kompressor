// Output formats Kompressor can create.
#pragma once

#include <string>
#include <vector>

namespace kz {

enum class OutFormat {
    Zip,
    SevenZip,
    Tar,
    TarGz,
    TarBz2,
    TarXz,
    TarZst,
    TarLz4,
    Gz,
    Bz2,
    Xz,
    Zst,
    Lz4,
    Iso,
    Cpio,
    Kmp,  // Kompressor super-compression format
};

struct FormatDesc {
    OutFormat id;
    const char* name;         // "ZIP"
    const char* ext;          // ".zip"
    const char* description;  // one line
    int minLevel, maxLevel, defLevel;  // equal => no level setting
    bool password;            // supports encryption
    bool singleFile;          // compresses exactly one file (raw stream)
};

const std::vector<FormatDesc>& outputFormats();
const FormatDesc& formatDesc(OutFormat f);
// Detects the output format from a file name ("x.tar.zst" -> TarZst). Returns false if unknown.
bool formatFromFileName(const std::string& name, OutFormat& out);
// Replaces any known archive extension of `path` with the extension of `f`.
std::string withFormatExtension(const std::string& path, OutFormat f);

// Human readable list of formats that can be opened/extracted.
const std::vector<std::pair<std::string, std::string>>& readableFormats();
// True when a file name looks like something Kompressor can open.
bool looksLikeArchive(const std::string& name);

}  // namespace kz
