// Maximum performance mode: lets compression and extraction run the CPU flat out.
//
// While a boost is held (Windows):
//   * the process runs at HIGH priority (never realtime, which can starve the mouse and keyboard),
//   * Windows power throttling (EcoQoS) is turned off for it, so its threads are not parked on
//     efficiency cores or run at reduced clocks,
//   * the Ultimate / High performance power plan is active (processor minimum state 100%), and the
//     previous plan comes back when the boost ends (or at the next start, after a crash),
//   * the PC is kept from going to sleep.
// On every platform the memory budget for worker threads grows, so more of them run in parallel.
#pragma once

#include <string>

namespace kz::perf {

// The user's setting. Read by threadsFor() and the boost below.
void setMaxPerformance(bool on);
bool maxPerformance();

// Start or stop the boost. Call from one thread (the GUI thread or the CLI's main thread):
// the keep-awake request belongs to the thread that made it.
void boost(bool on);
bool boosted();
// What the last boost managed to do, e.g. "high priority, no power throttling, High performance plan".
const std::string& boostSummary();

// Puts back a power plan a crashed run left switched. Call once at startup.
void recoverPowerPlan();

}  // namespace kz::perf
