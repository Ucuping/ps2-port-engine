/*
 * What the host asks of the operating system, behind one face: fibers,
 * waiting on a word, sleeping to a deadline, thread priorities, a read that
 * may fail, and the crash report.
 *
 * Windows keeps the calls the host made before (fiber.cpp, system.cpp,
 * fault.cpp); Apple's systems - macOS, and iOS with the same calls - have
 * theirs beside them. Other systems are not built yet: the plan for Android
 * is in the Tekken 5 repository's FINDINGS.md.
 */

#ifndef PS2HOST_PLATFORM_H
#define PS2HOST_PLATFORM_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ps2host::platform {

// ------------------------------------------------------------------ fibers
// A guest thread's stack and registers, run in turns on one host thread.
//
// Each fiber has its own float control register (ps2rt/fp_mode.h): it is
// saved when the fiber switches away and put back when it runs again, and a
// new fiber starts with the one its creator had when it made it. So on every
// platform alike - Windows' fibers are made without FIBER_FLAG_FLOAT_SWITCH,
// and what their own switch does with MXCSR is not what decides.
struct Fiber;

// The calling thread as a fiber, to switch back to; the first call makes it one.
Fiber* ThreadFiber();
// A fiber that runs entry(argument), when first switched to, on a stack of
// `stack_bytes` (rounded up to pages) with a guard of max(16 KB, a page)
// below it. entry never returns: it switches away for the last time instead.
Fiber* NewFiber(std::size_t stack_bytes, void (*entry)(void*), void* argument);
// Runs `to`; returns when a fiber switches back to the caller.
void SwitchFiber(Fiber* to);
// A fiber that is not running.
void FreeFiber(Fiber* fiber);

// ------------------------------------------------------------------ waiting
// Asleep while `word` holds `seen` - until woken, or `timeout_ms` passes.
void WaitOnU32(const std::atomic<std::uint32_t>& word, std::uint32_t seen, unsigned timeout_ms);
// Every thread asleep on `word`, woken.
void WakeAllU32(std::atomic<std::uint32_t>& word);
// Asleep until `when`, to a tenth of a millisecond or so.
void SleepUntil(std::chrono::steady_clock::time_point when);

// ------------------------------------------------------------------ threads
enum class ThreadRole {
    Worker,         // keeps the game's pace: never slowed to save power
    FastWorker,     // ...and ahead of the system's other threads
    Emulation,      // the EE's thread, above the threads it feeds
    Timer,          // the vertical blank's timer: wakes on time with every core busy
};
// The calling thread's priority for `role`.
void SetThreadRole(ThreadRole role);
// The system's timers kept precise while the game runs (Windows: 1 ms periods
// and no power throttling of the process; elsewhere nothing to do).
void HoldTimerResolution();
// Processors the process may run on.
int ProcessCores();
// The process held to the processors in `mask` (bit n: logical processor n),
// where the system lets a process choose them; false where it does not.
bool HoldProcessToCores(std::uint64_t mask);

// ------------------------------------------------------------------ files
// The running executable's full path (empty if the system will not say) - a
// path, not a string: Windows' wide one kept whole.
std::filesystem::path ExecutablePath();

// ------------------------------------------------------------------ faults
// `bytes` from `source` into `target`; false, and no fault, where the source
// cannot be read.
bool SafeCopy(void* target, const void* source, std::size_t bytes);

#if !defined(_WIN32)
// A crash - a fault, an illegal instruction, an abort - reported on stderr
// (the signal, the address, the native frames), then given to the handler
// that was there before, or to the default action. Windows' report is
// profile.cpp's, through its unhandled-exception filter.
void InstallCrashHandler();
// The crash report written to `console` in stderr's place and to `file` too
// (-1: none) - the run log's file, so that a report reaches it while the
// process dies, and where stderr went before the log took it.
void SetCrashReportFile(int file, int console);
// SIGHUP, SIGINT, SIGTERM and SIGQUIT said where the crash report goes before
// they end the process as before; SIGPIPE ignored (a failed write, not an end).
void NoteEndingSignals();

// ------------------------------------------------------------------ the run log
// Everything the run writes to stdout and stderr copied into `path` too, as it
// is written - each stream through a pipe and a thread that hands it on to
// wherever it went before and to the file. 1 when started; 0 when another
// process holds the file (an advisory lock), -1 when it cannot be opened.
int StartRunLog(const std::string& path);
// The streams drained into the file and put back as they were.
void FinishRunLog();
#endif

}  // namespace ps2host::platform

#endif  // PS2HOST_PLATFORM_H
