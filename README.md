# Kompressor

A cross-platform (Windows · macOS · Linux) file compressor written in C++17 with a Dear ImGui interface.

* **Extracts every common archive format itself**, with no 7-Zip, unrar or tar needed: ZIP/ZIPX/JAR/APK, 7Z, RAR & RAR5 (including solid and `.partN.rar` volumes), TAR, GZ, BZ2, XZ, LZMA, ZST, LZ4, LZ, Z, ISO 9660, CAB, LHA, XAR/PKG, CPIO, RPM, DEB/AR, WARC and split `.7z.001` sets.
* **Compresses into standard formats**: ZIP (optionally AES-256), 7Z, TAR, TAR.GZ/BZ2/XZ/ZST/LZ4, single-file GZ/BZ2/XZ/ZST/LZ4, ISO and CPIO.
* **Super Compresses** into its own `.kmp` format (see below).
* **Recommends a format** for what you need (best overall, smallest, fastest, share with anyone, password-protected). Before compressing, it takes a quick look at the files: what kinds they are, how much is already compressed (a zstd probe over samples), and what's repeated. It shows an estimated size and time, and one click applies the recommendation. Recompressing uses the archive's own listing instead.
* **Lets you browse an archive without extracting it**: a folder tree, search, a text/hex preview, extraction of selected items, and nested archives (a `.tar.gz` inside a `.zip` inside a `.deb`…) that open in memory.
* **Works with files inside archives like a folder**: double-click a file to open it in its usual app, drag files out to the desktop or Explorer (or Ctrl+C them), and drop files onto an open archive (or Ctrl+V, or Add) to add them to the folder you're in. Delete removes entries. Edited archives are rewritten in their own format (KMP keeps its mode, encrypted ZIPs keep their password).
* **Recognises archives by their content**, not their name: a 7z called `.zip`, a ZIP called `.png` or an archive with no extension at all opens, extracts and gets the right-click entries as what it really is (also inside other archives), and Kompressor says what it really is.
* Integrity **Test**, password prompts, background jobs you can **pause**, resume or cancel, drag & drop.
* **Maximum performance** mode (Settings, or `--max-cpu` in the CLI): while a job runs it gets every core at high priority, Windows power throttling off, the fastest power plan (yours comes back afterwards) and no sleep.
* **Graphics card help** for ZIP (Settings, or `--gpu` in the CLI; Windows): the graphics card searches for repeated data alongside the CPU through Direct3D 11 compute shaders, which come with Windows. It takes only the pieces it will finish before the CPU would get to them, so it never slows a job down. The result is an ordinary ZIP about the same size.
* **Checksums** (CRC32, MD5, SHA-1, SHA-256) of the files inside an archive without extracting them, or of files and folders on disk (File > Checksums of Files, or **Checksums** in the right-click menu). Lists can be saved or copied in sha256sum / md5sum / .sfv form, and checked against such a list.
* **RAM Drive** (File > RAM Drive, or `kompressor-cli ramdrive`): a fast scratch drive that cleans itself up. On Windows it's a local NTFS drive (e.g. R:, under "Devices and drives") on a temporary virtual disk made with Windows' own VHDX support; making it asks for administrator approval once, and the disk is removed and its file deleted when the drive stops or Kompressor exits, even if Kompressor crashes. On macOS it's a real RAM disk (`hdiutil ram://`) in Finder; on Linux a folder in `/dev/shm`, which is memory. No driver or other software is installed.
* A scriptable `kompressor-cli` built from the same core.

## The KMP super format

| Technique | What it buys |
|---|---|
| FastCDC content-defined chunking + XXH3-128 dedup | Repeated data is stored once, even when it is shifted inside different files. |
| Content-aware streams | Text, x86 code, ARM64 code, already-compressed media and generic binary data go into separate solid blocks, so similar data shares one dictionary. |
| Per-stream codecs | LZMA2 with BCJ x86/ARM64 branch filters for executables, text-tuned LZMA2 (lc=4, pb=0), and media is stored or fast-zstd'd instead of wasting CPU. |
| Max mode | Each block is compressed with every candidate codec, and the smallest result is kept. |
| Extreme mode | KCM, a built-in context-mixing coder: order-0 to order-8, word, column, sparse, record, indirect and two match models feed five neural mixers and four SSE stages, per bit. Roughly 15% smaller than Max, at a few hundred KB/s per core in both directions. |
| Ultimate mode | A heavier KCM (bit-history states, extra contexts, eight mixers, six SSE stages, up to 1 GiB of tables) over blocks of up to 256 MiB, so the model keeps learning across whole data sets. For archiving: slow, several GiB of RAM. |
| Parallel blocks | Blocks are compressed on multiple threads, with the thread count capped by available memory. |
| Index at the end + per-block checksums | Listing is instant, one file can be read without decompressing the rest, and corruption is detected. |

Modes: **Fast** (zstd‑3), **Balanced** (zstd‑19 long‑range), **Ultra** (LZMA2, the default), **Max**, **Extreme** and **Ultimate** (context mixing).

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
The output is `build/windows/Kompressor.exe`, which has no DLL dependencies, plus `KompShell.dll`, the small Explorer extension that puts the **Komp** entry at the bottom of the right-click menu's first group (without it, the entry falls back to the bottom of the menu). The `windows-arm64` preset builds for ARM64.

To build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php) (`winget install JRSoftware.InnoSetup`) and run:
```powershell
powershell -ExecutionPolicy Bypass -File packaging\windows\build_installer.ps1
```
It writes `build/windows/installer/KompressorSetup-<version>.exe`. The installer:
- installs per user, with no admin prompt
- adds a Start menu shortcut
- can add the Explorer right-click menu (on by default)
- can add a desktop shortcut and put `kompressor-cli` on `PATH` (both off by default)
- registers an uninstaller in Apps & Features

The right-click menu can also be switched on or off later in the app's **Settings** (gear button, or Ctrl+,). Its one-step entries ("Extract here", "Extract to folder", "Add to .7z / .zip / .kmp") run in a small window that shows only the task's progress (with Pause, Cancel and "Open in Kompressor") and closes itself when the task succeeds.

### Linux (Debian/Ubuntu)
```bash
sudo apt install cmake ninja-build g++ libarchive-dev liblzma-dev libzstd-dev libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
```
```bash
cmake --preset release && cmake --build --preset release
```
Fedora: `libarchive-devel xz-devel libzstd-devel mesa-libGL-devel libX*-devel`. Arch: `libarchive xz zstd`. To build GLFW with native Wayland support, add `-DKOMPRESSOR_GLFW_WAYLAND=ON`; otherwise it runs under XWayland.

`.github/workflows/build.yml` builds all three platforms and runs a smoke test on each. Every push to `main` then publishes a GitHub release, `v<version>-<build number>`, with the Windows installer and zip, the macOS app and the Linux binaries. The version comes from `project()` in `CMakeLists.txt`.

## CLI

```
kompressor-cli list    <archive>
kompressor-cli extract <archive> [-o dir] [-p password] [entry...]
kompressor-cli test    <archive>
kompressor-cli cat     <archive> <entry>            # one member to stdout
kompressor-cli grep    <archive> <text> [--case] [--hex] [--names "*.txt"]   # find text or bytes inside the files
kompressor-cli hash    <archive> [-a crc32|md5|sha1|sha256|all] [-c list.sha256]   # checksums, or check a list
kompressor-cli sum     <files/folders> [-a crc32|md5|sha1|sha256|all] [-c list.sha256]   # the same for files on disk
kompressor-cli create  out.7z|.zip|.tar.xz|... <files/folders> [-l level] [-p pw]
kompressor-cli super   out.kmp <files/folders> [-m fast|balanced|ultra|max|extreme|ultimate] [--no-dedup]
kompressor-cli convert <any-archive> out.7z|.zip|.tar.zst|.kmp|... [-l level] [-m mode] [-p pw] [-e new-pw]
                                                     # re-pack into another format without temp files
kompressor-cli add     <archive> <files/folders> [-d folder-in-archive] [-p pw]   # add or replace entries
kompressor-cli delete  <archive> <entry...> [-p pw]                               # remove entries / folders
kompressor-cli advise  <files/folders>               # which format suits these files, for each goal
kompressor-cli ramdrive [--size 2g] [--letter R]   # a scratch drive until Ctrl+C
kompressor-cli formats

# create, super and convert take -v <size> to split the output into .001, .002 ... parts
kompressor-cli create backup.7z ~/Photos -v 4g
# split sets (.001, .partN.rar, .r00) open from any part
```

## Regression tests

```
powershell -File tests
un.ps1 -Generate   # build the samples, then check them
powershell -File tests
un.ps1             # check again (after a code change)
```

`tests/make-samples.ps1` writes a fixed corpus of files and packs it with every writer the PC has (Python, .NET, Windows' tar, makecab, Windows disc imaging, Kompressor itself and 7-Zip when installed); missing writers are skipped. `tests/run.ps1` opens each sample with `kompressor-cli hash` and checks every file inside against Windows' own SHA-256 of the corpus, as `tests/samples.tsv` describes. The few formats with no independent reference are checked against `tests/baselines/`, which `-Update` rewrites. The exit code is the number of failures.

## Layout

```
src/core/   kz_archive (read/explore/extract/test), kz_writer (create/convert), kmp (super format), kz_formats, kz_common
src/cli/    kompressor-cli
src/gui/    main (GLFW/GL/fonts/HiDPI), app (explorer, preview, dialogs, jobs), file_dialog (built-in browser), theme
src/shellext/ KompShell.dll, the Windows Explorer context-menu extension
tests/      regression samples and runner
```

## Notes & limits

* RAR files are created as genuine RAR5 archives with stored (uncompressed) data, written from RARLAB's published format description; RAR's compression algorithm is proprietary, so it is not used. 7-Zip archives with *encrypted headers* are not supported by libarchive. Encrypted ZIPs work both ways.
* Adding to or deleting from an archive rewrites the whole archive (every entry is recompressed), so it takes about as long as recompressing it. A RAR comes back stored, single-file formats (GZ, XZ, ...), split sets and archives nested inside other archives can't be edited, and encrypted 7z/RAR archives can't be written back. Files opened from an archive are temporary copies in `%TEMP%\Kompressor` (cleaned up after a day); edits to them don't go back into the archive. Dragging and copying files out is Windows-only for now.
* The Windows RAM drive's files are kept in a temporary file in `%TEMP%\Kompressor` that Windows caches in memory, not in memory alone: a true RAM disk on Windows needs a kernel driver, which would have to be third-party or signed by Microsoft. On Linux the drive is limited by `/dev/shm` (usually half the memory), not by a size you pick.
* Extraction is hardened against path traversal: `..` entries, absolute paths and paths that resolve through symlinks are refused.
* Debug aid: `Kompressor --snapshot out.ppm [--select entry/path] archive` renders the UI, saves a frame and exits. `KZ_FPS_LOG=fps.txt` redraws continuously and logs the frame rate each second.
