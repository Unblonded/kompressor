#include "kz_perf.hpp"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <powrprof.h>
#endif

namespace kz::perf {

namespace {
std::atomic<bool> g_max{false};
bool g_boosted = false;
std::string g_summary;

#ifdef _WIN32
// Where the plan that was active before the boost is remembered, so a crash can't leave it switched.
const wchar_t* kKey = L"Software\\Kompressor";
const wchar_t* kRestoreValue = L"RestorePowerScheme";

// {e9a42b02-d5df-448d-aa00-03f14749eb61}: "Ultimate Performance" (present on some systems, hidden on most).
const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

// Held while a boost is active, so another Kompressor starting up doesn't "recover" a plan that a
// running job switched on purpose.
const wchar_t* kBoostMutex = L"Local\\KompressorBoost";
HANDLE g_mutex = nullptr;

bool havePrevScheme = false;
GUID prevScheme{};

std::wstring guidText(const GUID& g) {
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

bool schemeExists(const GUID& g) {
    for (ULONG i = 0;; ++i) {
        GUID s;
        DWORD size = sizeof s;
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, (UCHAR*)&s, &size) != ERROR_SUCCESS) return false;
        if (IsEqualGUID(s, g)) return true;
    }
}

void setThrottling(bool allow) {
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // ControlMask set with StateMask clear = "never throttle"; both clear = "let Windows decide" again.
    st.ControlMask = allow ? 0 : PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
}

// Switches to the fastest installed plan. Returns its name, or "" if nothing changed.
std::string raisePowerPlan() {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) return {};
    GUID cur = *active;
    LocalFree(active);
    const GUID* want = nullptr;
    const char* name = nullptr;
    if (schemeExists(kUltimate)) {
        want = &kUltimate;
        name = "Ultimate Performance plan";
    } else if (schemeExists(GUID_MIN_POWER_SAVINGS)) {
        want = &GUID_MIN_POWER_SAVINGS;
        name = "High performance plan";
    }
    if (!want) return {};
    if (IsEqualGUID(cur, *want)) return std::string(name) + " (already on)";
    // Remember the old plan first, so even a crash right after the switch gets it restored.
    std::wstring old = guidText(cur);
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(h, kRestoreValue, 0, REG_SZ, (const BYTE*)old.c_str(), (DWORD)((old.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
    if (PowerSetActiveScheme(nullptr, want) != ERROR_SUCCESS) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, kRestoreValue);
        return {};
    }
    havePrevScheme = true;
    prevScheme = cur;
    return name;
}

void restorePowerPlan() {
    if (havePrevScheme) PowerSetActiveScheme(nullptr, &prevScheme);
    havePrevScheme = false;
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, kRestoreValue);
}
#endif
}  // namespace

void setMaxPerformance(bool on) { g_max = on; }
bool maxPerformance() { return g_max; }
bool boosted() { return g_boosted; }
const std::string& boostSummary() { return g_summary; }

void boost(bool on) {
    if (on == g_boosted) return;
    g_boosted = on;
#ifdef _WIN32
    if (on) {
        g_summary.clear();
        g_mutex = CreateMutexW(nullptr, FALSE, kBoostMutex);
        auto add = [](const std::string& s) { g_summary += (g_summary.empty() ? "" : ", ") + s; };
        if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) add("high priority");
        setThrottling(false);
        add("no power throttling");
        std::string plan = raisePowerPlan();
        if (!plan.empty()) add(plan);
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        add("sleep blocked");
    } else {
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        setThrottling(true);
        restorePowerPlan();
        SetThreadExecutionState(ES_CONTINUOUS);
        if (g_mutex) CloseHandle(g_mutex);
        g_mutex = nullptr;
    }
#else
    g_summary = on ? "all cores, larger memory budget" : "";
#endif
}

void recoverPowerPlan() {
#ifdef _WIN32
    if (HANDLE other = OpenMutexW(SYNCHRONIZE, FALSE, kBoostMutex)) {  // another Kompressor is boosting right now
        CloseHandle(other);
        return;
    }
    wchar_t buf[64];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, kRestoreValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return;
    GUID g;
    if (SUCCEEDED(CLSIDFromString(buf, &g)) && schemeExists(g)) PowerSetActiveScheme(nullptr, &g);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kKey, kRestoreValue);
#endif
}

}  // namespace kz::perf
