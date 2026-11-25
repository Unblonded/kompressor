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
                                           "cpio", "rpm", "deb", "jar", "apk", "whl", "nupkg", "vsix", "cbz", "cbr", "cb7", "001",
                                           // images, disks and other archivers Kompressor reads itself
                                           "wim", "swm", "esd", "arj", "dmg", "vhd", "vhdx", "avhdx", "vmdk", "vdi", "qcow",
                                           "qcow2", "img", "ima", "simg", "squashfs", "sfs", "cramfs", "udf", "hfs", "hfsx",
                                           "apfs", "ext2", "ext3", "ext4", "fat", "ntfs", "mbr", "gpt"};

// Programs, installers and Compound File documents: they get "Open archive" (to look inside, as in
// 7-Zip) but not the extract entries. A program with an archive appended (self-extracting) is found by
// its content and gets everything.
inline const char* const kContainerExts[] = {"exe", "dll", "sys", "ocx", "cpl", "scr", "efi", "mui", "ax",  "drv",
                                             "msi", "msp", "msm", "mst", "doc", "xls", "ppt", "msg", "pub", "vsd",
                                             "chm", "chi", "chw", "hxs", "swf", "flv", "hex", "ihx", "b64", "base64", "so",
                                             "dylib", "o", "obj", "elf", "ko", "axf", "te", "fd", "rom", "cap", "scap", "lpimg",
                                             "ex_", "dl_", "sy_", "tx_", "in_", "hl_", "cn_", "ch_", "ov_", "38_", "co_"};

}  // namespace kompshell
