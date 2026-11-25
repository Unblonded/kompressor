#include "kz_ramdrive.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <initguid.h>  // defines VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT here
#include <virtdisk.h>
#include <winioctl.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "kz_common.hpp"

namespace kz {

namespace fs = std::filesystem;

namespace {

const char* kLabel = "Kompressor RAM";

// Runs a program with arguments, hidden, and waits. Returns its exit code (-1 when it couldn't
// start); `out` gets what it printed.
int runTool(const std::vector<std::string>& args, std::string& out) {
    out.clear();
#ifdef _WIN32
    std::wstring cmd;
    for (auto& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"' + u8path(a).wstring() + L'"';
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    char b[4096];
    DWORD n;
    while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
#else
    int fd[2];
    if (pipe(fd) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fd[1], 2);
    posix_spawn_file_actions_addclose(&fa, fd[0]);
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fd[1]);
    if (rc != 0) {
        close(fd[0]);
        return -1;
    }
    char b[4096];
    ssize_t n;
    while ((n = read(fd[0], b, sizeof b)) > 0) out.append(b, (size_t)n);
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string firstLine(const std::string& s) {
    std::string t = trim(s);
    size_t e = t.find_first_of("\r\n");
    return e == std::string::npos ? t : t.substr(0, e);
}

// The last non-empty line, which is where diskpart and hdiutil put the reason something failed.
std::string lastLine(const std::string& s) {
    std::string t = trim(s);
    size_t e = t.find_last_of("\r\n");
    return e == std::string::npos ? t : trim(t.substr(e + 1));
}

#ifdef _WIN32
std::string winError(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (LPWSTR)&msg, 0, nullptr);
    std::string s = msg ? trim(pathToUtf8(fs::path(msg))) : "error " + std::to_string(code);
    if (msg) LocalFree(msg);
    return s;
}

std::string readStatus(const std::wstring& log) {
    std::string s;
    if (FILE* f = _wfopen(log.c_str(), L"rb")) {
        char b[2048] = {};
        size_t got = std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        s = firstLine(std::string(b, got));
    }
    return s;
}
#endif

}  // namespace

struct RamDrive::Impl {
    uint64_t cap = 0;
#ifdef _WIN32
    HANDLE helper = nullptr, stopEvent = nullptr;
    std::wstring vhdx;
#elif defined(__APPLE__)
    std::string dev;
#else
    std::string dir;
    mutable std::mutex mu;
    mutable uint64_t usedCache = 0;
    mutable std::chrono::steady_clock::time_point usedAt{};
#endif
};

RamDrive::RamDrive() = default;
RamDrive::~RamDrive() { stop(); }

const char* RamDrive::kind() {
#ifdef _WIN32
    return "a local drive on a temporary virtual disk";
#elif defined(__APPLE__)
    return "a RAM disk";
#else
    return "a folder in memory (/dev/shm)";
#endif
}

bool RamDrive::needsAdmin() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

// ============================================================================================ Windows
#ifdef _WIN32

int RamDrive::helperMain(const std::vector<std::string>& args) {
    // <exe> --ramdrive-helper <vhdx> <bytes> <letter> <parent pid> <stop event> <log>
    if (args.size() < 8 || args[1] != "--ramdrive-helper") return -1;
    std::wstring vhdx = u8path(args[2]).wstring(), log = u8path(args[7]).wstring();
    uint64_t size = std::strtoull(args[3].c_str(), nullptr, 10);
    char letter = (char)std::toupper((unsigned char)args[4][0]);
    DWORD ppid = (DWORD)std::strtoul(args[5].c_str(), nullptr, 10);
    std::wstring eventName = u8path(args[6]).wstring();
    auto say = [&](const std::string& line) {
        if (FILE* f = _wfopen(log.c_str(), L"ab")) {
            std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }
    };
    auto fail = [&](const std::string& why) {
        say("error: " + why);
        return 1;
    };
    if (letter < 'D' || letter > 'Z') return fail("bad drive letter");
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, ppid);
    HANDLE stopEvent = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    if (!parent || !stopEvent) return fail("Kompressor stopped before the drive was made");

    // A dynamic VHDX: the file only grows as data is written.
    size = std::max<uint64_t>(size, 64ull << 20);
    size = (size + (1ull << 20) - 1) & ~((1ull << 20) - 1);
    DeleteFileW(vhdx.c_str());
    VIRTUAL_STORAGE_TYPE vst{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};
    CREATE_VIRTUAL_DISK_PARAMETERS cp{};
    cp.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    cp.Version2.MaximumSize = size;
    HANDLE vd = nullptr;
    DWORD r = CreateVirtualDisk(&vst, vhdx.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr, CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &cp,
                                nullptr, &vd);
    if (r != ERROR_SUCCESS) return fail("Could not create the virtual disk: " + winError(r));
    auto drop = [&] {
        CloseHandle(vd);  // detaches it when attached (the attachment lasts only as long as this handle)
        for (int i = 0; i < 50 && !DeleteFileW(vhdx.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; ++i) Sleep(100);
    };
    ATTACH_VIRTUAL_DISK_PARAMETERS ap{};
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    r = AttachVirtualDisk(vd, nullptr, ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER, 0, &ap, nullptr);
    if (r != ERROR_SUCCESS) {
        drop();
        return fail("Could not attach the virtual disk: " + winError(r));
    }

    // Which disk number it got, then make sure that disk really is this new, empty one before
    // anything is written to it.
    wchar_t phys[MAX_PATH] = {};
    ULONG n = sizeof phys;
    r = GetVirtualDiskPhysicalPath(vd, &n, phys);
    const wchar_t* num = r == ERROR_SUCCESS ? wcsstr(phys, L"PhysicalDrive") : nullptr;
    if (!num) {
        drop();
        return fail("Could not find the virtual disk's number");
    }
    int disk = _wtoi(num + 13);
    std::string why;
    HANDLE h = CreateFileW(phys, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        why = "could not open it: " + winError(GetLastError());
    } else {
        std::vector<uint8_t> lay(4096);
        GET_LENGTH_INFORMATION len{};
        DWORD got = 0;
        // A virtual disk backed by a file (never a real disk)...
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<uint8_t> desc(1024);
        bool virt = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, desc.data(), (DWORD)desc.size(), &got, nullptr) &&
                    ((STORAGE_DEVICE_DESCRIPTOR*)desc.data())->BusType == BusTypeFileBackedVirtual;
        // ...with no partitions (Windows reports a blank disk as RAW, or as MBR with only empty slots)...
        bool empty = false;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, lay.data(), (DWORD)lay.size(), &got, nullptr)) {
            auto* L = (DRIVE_LAYOUT_INFORMATION_EX*)lay.data();
            empty = L->PartitionStyle == PARTITION_STYLE_RAW;
            if (L->PartitionStyle == PARTITION_STYLE_MBR) {
                empty = true;
                for (DWORD i = 0; i < L->PartitionCount; ++i)
                    if (L->PartitionEntry[i].PartitionLength.QuadPart != 0) empty = false;
            }
        }
        // ...of exactly the size just made.
        if (!virt)
            why = "it isn't a virtual disk";
        else if (!empty)
            why = "it already has partitions";
        else if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &len, sizeof len, &got, nullptr))
            why = "could not read its size: " + winError(GetLastError());
        else if ((uint64_t)len.Length.QuadPart != size)
            why = "its size is " + std::to_string(len.Length.QuadPart) + " bytes, not " + std::to_string(size);
        CloseHandle(h);
    }
    if (!why.empty()) {
        drop();
        return fail("The new virtual disk (" + pathToUtf8(fs::path(phys)) + ") was left alone: " + why);
    }

    // Partition, format and name it with diskpart.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring script = std::wstring(tmp) + L"kompressor-ramdrive-" + std::to_wstring(GetCurrentProcessId()) + L".txt";
    if (FILE* f = _wfopen(script.c_str(), L"wb")) {
        std::fprintf(f,
                     "select disk %d\r\n"
                     "attributes disk clear readonly noerr\r\n"
                     "online disk noerr\r\n"
                     "convert mbr\r\n"
                     "create partition primary\r\n"
                     "format fs=ntfs quick label=\"%s\"\r\n"
                     "assign letter=%c\r\n"
                     "exit\r\n",
                     disk, kLabel, letter);
        std::fclose(f);
    }
    std::string out;
    int rc = runTool({"diskpart", "/s", pathToUtf8(fs::path(script))}, out);
    DeleteFileW(script.c_str());
    wchar_t root[] = {(wchar_t)letter, L':', L'\\', 0};
    if (rc != 0 || GetDriveTypeW(root) != DRIVE_FIXED) {
        drop();
        std::string why = lastLine(out);
        return fail("Could not format the drive" + (why.empty() ? std::string() : ": " + why));
    }
    say("ready");

    // Hold it until Kompressor asks to stop it, or exits.
    HANDLE wait[2] = {stopEvent, parent};
    WaitForMultipleObjects(2, wait, FALSE, INFINITE);
    DetachVirtualDisk(vd, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
    drop();
    CloseHandle(parent);
    CloseHandle(stopEvent);
    return 0;
}

bool RamDrive::start(uint64_t capacity, char letter, std::string& err) {
    stop();
    letter = (char)std::toupper((unsigned char)letter);
    if (letter < 'D' || letter > 'Z') return err = "Pick a drive letter from D to Z", false;
    if (GetLogicalDrives() & (1u << (letter - 'A'))) return err = std::string(1, letter) + ": is already in use: pick another letter", false;

    static std::atomic<int> seq{0};
    std::string tag = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++seq);
    fs::path dir = fs::temp_directory_path() / "Kompressor";
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path vhdx = dir / ("ramdrive-" + tag + ".vhdx"), log = dir / ("ramdrive-" + tag + ".log");
    fs::remove(log, ec);
    std::wstring eventName = L"Local\\KompressorRamDrive-" + u8path(tag).wstring();

    auto impl = std::make_unique<Impl>();
    impl->cap = capacity;
    impl->vhdx = vhdx.wstring();
    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!impl->stopEvent) return err = "Could not start the drive: " + winError(GetLastError()), false;

    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    std::wstring params = L"--ramdrive-helper \"" + vhdx.wstring() + L"\" " + std::to_wstring(capacity) + L" " + (wchar_t)letter + L" " +
                          std::to_wstring(GetCurrentProcessId()) + L" " + eventName + L" \"" + log.wstring() + L"\"";
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof ei;
    ei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    ei.lpVerb = L"runas";
    ei.lpFile = exe;
    ei.lpParameters = params.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei) || !ei.hProcess) {
        DWORD e = GetLastError();
        CloseHandle(impl->stopEvent);
        err = e == ERROR_CANCELLED ? "Making the drive needs administrator approval, which wasn't given"
                                   : "Could not start the drive: " + winError(e);
        return false;
    }
    impl->helper = ei.hProcess;

    // Wait for the helper to say it's ready (or why not).
    std::string status;
    for (int i = 0; i < 1200 && status.empty(); ++i) {
        status = readStatus(log.wstring());
        if (status.empty() && WaitForSingleObject(impl->helper, 100) == WAIT_OBJECT_0) {
            status = readStatus(log.wstring());  // it exited: one last look at what it wrote
            if (status.empty()) status = "error: the drive helper stopped unexpectedly";
        }
    }
    fs::remove(log, ec);
    if (status != "ready") {
        SetEvent(impl->stopEvent);
        WaitForSingleObject(impl->helper, 30000);
        CloseHandle(impl->helper);
        CloseHandle(impl->stopEvent);
        err = status.compare(0, 7, "error: ") == 0 ? status.substr(7) : "The drive took too long to start";
        return false;
    }
    impl_ = std::move(impl);
    where_ = std::string(1, letter) + ":\\";
    return true;
}

void RamDrive::stop() {
    if (!impl_) return;
    SetEvent(impl_->stopEvent);
    WaitForSingleObject(impl_->helper, 60000);
    CloseHandle(impl_->helper);
    CloseHandle(impl_->stopEvent);
    for (int i = 0; i < 20 && !DeleteFileW(impl_->vhdx.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND; ++i) Sleep(100);
    impl_.reset();
    where_.clear();
}

uint64_t RamDrive::capacity() const {
    ULARGE_INTEGER freeU{}, total{}, freeAll{};
    if (!impl_ || !GetDiskFreeSpaceExW(u8path(where_).wstring().c_str(), &freeU, &total, &freeAll)) return impl_ ? impl_->cap : 0;
    return total.QuadPart;
}

uint64_t RamDrive::used() const {
    ULARGE_INTEGER freeU{}, total{}, freeAll{};
    if (!impl_ || !GetDiskFreeSpaceExW(u8path(where_).wstring().c_str(), &freeU, &total, &freeAll)) return 0;
    return total.QuadPart - freeAll.QuadPart;
}

// ============================================================================================== macOS
#elif defined(__APPLE__)

int RamDrive::helperMain(const std::vector<std::string>&) { return -1; }

bool RamDrive::start(uint64_t capacity, char, std::string& err) {
    stop();
    std::string out;
    uint64_t sectors = (std::max<uint64_t>(capacity, 16ull << 20) + 511) / 512;
    if (runTool({"hdiutil", "attach", "-nomount", "ram://" + std::to_string(sectors)}, out) != 0)
        return err = "macOS could not make the RAM disk: " + lastLine(out), false;
    std::string dev = firstLine(out);
    size_t sp = dev.find_first_of(" \t");
    if (sp != std::string::npos) dev.resize(sp);
    if (dev.compare(0, 9, "/dev/disk") != 0) return err = "macOS could not make the RAM disk: " + firstLine(out), false;
    if (runTool({"diskutil", "erasevolume", "HFS+", kLabel, dev}, out) != 0) {
        std::string why = lastLine(out);
        runTool({"hdiutil", "detach", dev, "-force"}, out);
        return err = "Could not format the RAM disk: " + why, false;
    }
    // Where it was mounted ("/Volumes/Kompressor RAM", or "... 1" when that name is taken).
    std::string mount = std::string("/Volumes/") + kLabel;
    if (runTool({"diskutil", "info", dev}, out) == 0) {
        size_t at = out.find("Mount Point:");
        if (at != std::string::npos) {
            std::string m = firstLine(out.substr(at + 12));
            if (!m.empty()) mount = m;
        }
    }
    impl_ = std::make_unique<Impl>();
    impl_->cap = capacity;
    impl_->dev = dev;
    where_ = mount;
    return true;
}

void RamDrive::stop() {
    if (!impl_) return;
    std::string out;
    if (runTool({"hdiutil", "detach", impl_->dev}, out) != 0) runTool({"hdiutil", "detach", impl_->dev, "-force"}, out);
    impl_.reset();
    where_.clear();
}

uint64_t RamDrive::capacity() const {
    struct statvfs v {};
    if (!impl_ || statvfs(where_.c_str(), &v) != 0) return impl_ ? impl_->cap : 0;
    return (uint64_t)v.f_blocks * v.f_frsize;
}

uint64_t RamDrive::used() const {
    struct statvfs v {};
    if (!impl_ || statvfs(where_.c_str(), &v) != 0) return 0;
    return (uint64_t)(v.f_blocks - v.f_bfree) * v.f_frsize;
}

// ============================================================================================== Linux
#else

int RamDrive::helperMain(const std::vector<std::string>&) { return -1; }

bool RamDrive::start(uint64_t capacity, char, std::string& err) {
    stop();
    // /dev/shm is a memory file system every user can write to. Its own limit (usually half the
    // memory) applies; it can't be made smaller without administrator rights.
    std::string base = "/dev/shm";
    if (access(base.c_str(), W_OK) != 0) {
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        if (!rt || access(rt, W_OK) != 0) return err = "There's no memory file system (/dev/shm) to use here", false;
        base = rt;
    }
    std::string dir = base + "/KompressorRAM-" + std::to_string(getuid());
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return err = "Could not make " + dir + ": " + ec.message(), false;
    chmod(dir.c_str(), 0700);
    impl_ = std::make_unique<Impl>();
    impl_->cap = capacity;
    impl_->dir = dir;
    where_ = dir;
    return true;
}

void RamDrive::stop() {
    if (!impl_) return;
    std::error_code ec;
    fs::remove_all(impl_->dir, ec);
    impl_.reset();
    where_.clear();
}

uint64_t RamDrive::capacity() const {
    struct statvfs v {};
    if (!impl_ || statvfs(where_.c_str(), &v) != 0) return impl_ ? impl_->cap : 0;
    return (uint64_t)v.f_blocks * v.f_frsize;  // the file system's own limit
}

uint64_t RamDrive::used() const {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> l(impl_->mu);
    auto now = std::chrono::steady_clock::now();
    if (now - impl_->usedAt > std::chrono::seconds(1)) {  // walking the folder: at most once a second
        uint64_t total = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(impl_->dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) total += it->file_size(e2);
        }
        impl_->usedCache = total;
        impl_->usedAt = now;
    }
    return impl_->usedCache;
}

#endif

bool RamDrive::hasFiles() const {
    if (!impl_) return false;
    std::error_code ec;
    for (auto it = fs::directory_iterator(u8path(where_), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string low = pathToUtf8(it->path().filename());
        for (auto& c : low) c = (char)std::tolower((unsigned char)c);
        // What the system keeps there by itself.
        if (low == "system volume information" || low == "$recycle.bin" || low == ".fseventsd" || low == ".trashes" ||
            low == ".spotlight-v100" || low == ".temporaryitems" || low == ".ds_store")
            continue;
        return true;
    }
    return false;
}

}  // namespace kz
