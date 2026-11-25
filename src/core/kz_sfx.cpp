#include "kz_sfx.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include "kz_common.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace kz {

namespace {
// Must match src/sfx/sfx_main.cpp.
constexpr char kMagic[8] = {'K', 'Z', 'S', 'F', 'X', 0, 1, 0};
constexpr uint32_t kFlagEncrypted = 1;

bool stub(const uint8_t*& p, size_t& n) {
#if defined(_WIN32) && defined(KZ_HAVE_SFX)
    HMODULE m = GetModuleHandleW(nullptr);
    HRSRC r = FindResourceW(m, L"KZSFX", MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(m, r) : nullptr;
    p = g ? (const uint8_t*)LockResource(g) : nullptr;
    n = r ? SizeofResource(m, r) : 0;
    return p && n > 1024;
#else
    (void)p;
    (void)n;
    return false;
#endif
}
}  // namespace

bool sfxSupported() {
    const uint8_t* p;
    size_t n;
    return stub(p, n);
}

bool sfxFormatOk(OutFormat f) {
    switch (f) {
        case OutFormat::Zip:
        case OutFormat::SevenZip:
        case OutFormat::Tar:
        case OutFormat::TarGz:
        case OutFormat::TarBz2:
        case OutFormat::TarXz:
        case OutFormat::TarZst: return true;
        default: return false;
    }
}

std::string sfxPathFor(const std::string& archivePath) {
    fs::path p = u8path(archivePath);
    return pathToUtf8(p.parent_path() / u8path(stripArchiveExt(fileNameOf(archivePath)) + ".exe"));
}

bool makeSelfExtracting(const std::string& path, bool encrypted, std::string& err) {
    const uint8_t* sp = nullptr;
    size_t sn = 0;
    if (!stub(sp, sn)) {
        err = "This build of Kompressor can't make self-extracting archives.";
        return false;
    }
    std::string tmp = path + ".sfxtmp";
    std::ifstream in(u8path(path), std::ios::binary);
    std::ofstream out(u8path(tmp), std::ios::binary | std::ios::trunc);
    if (!in || !out) {
        err = "Can't write the self-extracting file.";
        return false;
    }
    out.write((const char*)sp, (std::streamsize)sn);
    std::vector<char> buf(4 << 20);
    uint64_t size = 0;
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        out.write(buf.data(), got);
        size += (uint64_t)got;
    }
    uint8_t t[32] = {};
    uint64_t off = sn;
    uint32_t flags = encrypted ? kFlagEncrypted : 0;
    std::memcpy(t, kMagic, 8);
    std::memcpy(t + 8, &off, 8);
    std::memcpy(t + 16, &size, 8);
    std::memcpy(t + 24, &flags, 4);
    out.write((const char*)t, 32);
    in.close();
    out.close();
    std::error_code ec;
    if (!out || size == 0) {
        fs::remove(u8path(tmp), ec);
        err = "Writing the self-extracting file failed (disk full?).";
        return false;
    }
    fs::rename(u8path(tmp), u8path(path), ec);
    if (ec) {
        fs::remove(u8path(tmp), ec);
        err = "Can't move the self-extracting file into place: " + ec.message();
        return false;
    }
    return true;
}

}  // namespace kz
