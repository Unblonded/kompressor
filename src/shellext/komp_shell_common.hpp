// Shared by the Explorer context-menu extension (KompShell.dll) and the app code that registers it.
#pragma once

#define KOMP_SHELL_CLSID_STR L"{4FF8516A-3A6F-4DBB-8CA9-E67FD21BF5D7}"

namespace kompshell {

// What the submenu is called in Explorer.
inline constexpr wchar_t kMenuName[] = L"Komp";

// Extensions that get the Open / Extract entries. Office documents, e-books and the like are ZIPs too,
// but people rarely want to unpack them, so they only get the compress entries.
inline const char* const kArchiveExts[] = {"kmp", "zip", "zipx", "7z", "rar", "tar", "gz", "tgz", "bz2", "tbz", "tbz2", "xz",
                                           "txz", "lzma", "zst", "tzst", "lz4", "lz", "lzo", "iso", "cab", "lha", "lzh", "xar",
                                           "cpio", "rpm", "deb", "jar", "apk", "whl", "nupkg", "vsix", "cbz", "cbr", "cb7", "001"};

}  // namespace kompshell
