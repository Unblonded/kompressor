# Kompressor

A cross-platform (Windows · macOS · Linux) file compressor written in C++17 with a Dear ImGui interface.

* **Extracts every common archive format itself**, with no 7-Zip, unrar or tar needed: ZIP/ZIPX/JAR/APK, 7Z, RAR & RAR5 (including solid and `.partN.rar` volumes), TAR, GZ, BZ2, XZ, LZMA, ZST, LZ4, LZ, Z, ISO 9660, CAB, LHA, XAR/PKG, CPIO, RPM, DEB/AR, WARC and split `.7z.001` sets.
* **Compresses into standard formats**: ZIP (optionally AES-256), 7Z, TAR, TAR.GZ/BZ2/XZ/ZST/LZ4, single-file GZ/BZ2/XZ/ZST/LZ4, ISO and CPIO.
* **Super Compresses** into its own `.kmp` format (see below).
* **Lets you browse an archive without extracting it**: a folder tree, search, a text/hex preview, extraction of selected items, and nested archives (a `.tar.gz` inside a `.zip` inside a `.deb`…) that open in memory.
* Integrity **Test**, password prompts, background jobs you can cancel, drag & drop.
* A scriptable `kompressor-cli` built from the same core.

## The KMP super format

| Technique | What it buys |
|---|---|
| FastCDC content-defined chunking + XXH3-128 dedup | Repeated data is stored once, even when it is shifted inside different files. |
| Content-aware streams | Text, x86 code, ARM64 code, already-compressed media and generic binary data go into separate solid blocks, so similar data shares one dictionary. |
| Per-stream codecs | LZMA2 with BCJ x86/ARM64 branch filters for executables, text-tuned LZMA2 (lc=4, pb=0), and media is stored or fast-zstd'd instead of wasting CPU. |
| Max mode | Each block is compressed with every candidate codec, and the smallest result is kept. |
| Parallel blocks | Blocks are compressed on multiple threads, with the thread count capped by available memory. |
| Index at the end + per-block checksums | Listing is instant, one file can be read without decompressing the rest, and corruption is detected. |

Modes: **Fast** (zstd‑3), **Balanced** (zstd‑19 long‑range), **Ultra** (LZMA2, the default) and **Max**.

Benchmark on Apple Silicon with two Python installs (141 MB, 7,569 files):

| Output | Size | Time |
|---|---|---|
| ZIP (deflate‑6) | 43.2 MB | 3.5 s |
| TAR.ZST (‑19) | 30.4 MB | 13.2 s |
| TAR.XZ (‑9) | 28.0 MB | 36.4 s |
| 7Z (LZMA2‑9) | 27.8 MB | 39.1 s |
| **KMP Fast** | 23.5 MB | **1.8 s** |
| **KMP Ultra** | **16.9 MB** | 17.2 s |
| **KMP Max** | **16.7 MB** | 74.7 s |

The gain is largest on data with redundancy across files: multiple versions, builds, game assets or VM images. Media that is already compressed (JPEG, MP4…) cannot shrink much in any format.

## Building

The GUI dependencies (GLFW 3.4, Dear ImGui 1.91.9b) and xxHash are fetched automatically by CMake. The archive libraries (libarchive, liblzma, zstd) come from your package manager or from vcpkg.

### macOS (Homebrew)
```bash
brew install cmake ninja libarchive xz zstd
```
```bash
cmake --preset release && cmake --build --preset release
```
```bash
open build/release/Kompressor.app
```

### macOS Universal 2 (arm64 + x86_64, self-contained)
```bash
export VCPKG_ROOT=~/vcpkg   # a clone of github.com/microsoft/vcpkg, bootstrapped
```
```bash
cmake --preset macos-universal && cmake --build --preset macos-universal
```

### Windows (MSVC + vcpkg, one static .exe)
From a "Developer PowerShell for VS" with `VCPKG_ROOT` set:
```bash
cmake --preset windows && cmake --build --preset windows
```
The output is `build/windows/Kompressor.exe`, which has no DLL dependencies. The `windows-arm64` preset builds for ARM64.

### Linux (Debian/Ubuntu)
```bash
sudo apt install cmake ninja-build g++ libarchive-dev liblzma-dev libzstd-dev libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
```
```bash
cmake --preset release && cmake --build --preset release
```
Fedora: `libarchive-devel xz-devel libzstd-devel mesa-libGL-devel libX*-devel`. Arch: `libarchive xz zstd`. To build GLFW with native Wayland support, add `-DKOMPRESSOR_GLFW_WAYLAND=ON`; otherwise it runs under XWayland.

`.github/workflows/build.yml` builds all three platforms and runs a smoke test on each.

## CLI

```
kompressor-cli list    <archive>
kompressor-cli extract <archive> [-o dir] [-p password] [entry...]
kompressor-cli test    <archive>
kompressor-cli cat     <archive> <entry>            # one member to stdout
kompressor-cli create  out.7z|.zip|.tar.xz|... <files/folders> [-l level] [-p pw]
kompressor-cli super   out.kmp <files/folders> [-m fast|balanced|ultra|max] [--no-dedup]
kompressor-cli convert <any-archive> out.kmp [-m mode]   # re-pack without temp files
kompressor-cli formats
```

## Layout

```
src/core/   kz_archive (read/explore/extract/test), kz_writer (create/convert), kmp (super format), kz_formats, kz_common
src/cli/    kompressor-cli
src/gui/    main (GLFW/GL/fonts/HiDPI), app (explorer, preview, dialogs, jobs), file_dialog (built-in browser), theme
```

## Notes & limits

* RAR files can be read but not created, because the RAR format is proprietary. 7-Zip archives with *encrypted headers* are not supported by libarchive. Encrypted ZIPs work both ways.
* Extraction is hardened against path traversal: `..` entries, absolute paths and paths that resolve through symlinks are refused.
* Debug aid: `Kompressor --snapshot out.ppm [--select entry/path] archive` renders the UI, saves a frame and exits.
