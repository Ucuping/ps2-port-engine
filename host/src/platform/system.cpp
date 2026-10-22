/*
 * Waiting, sleeping and thread priorities (platform.h).
 *
 * Windows: the calls threads.cpp made before - WaitOnAddress, a
 * high-resolution waitable timer, SetThreadPriority, power throttling off,
 * 1 ms timer periods. Apple's systems: os_sync_wait_on_address (public since
 * macOS 14.4 and iOS 17.4, below the deployment target), mach_wait_until,
 * and quality-of-service classes in place of priorities.
 */

#include "platform.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "synchronization.lib")
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread/qos.h>
#else
#error "ps2host: no waits or priorities here yet - Windows, and Apple's systems"
#endif

namespace ps2host::platform {

static_assert(sizeof(std::atomic<std::uint32_t>) == 4 && std::atomic<std::uint32_t>::is_always_lock_free,
              "a 32-bit atomic is the word the system waits on");

#if defined(_WIN32)

void WaitOnU32(const std::atomic<std::uint32_t>& word, std::uint32_t seen, unsigned timeout_ms) {
    WaitOnAddress(const_cast<std::atomic<std::uint32_t>*>(&word), &seen, sizeof seen, timeout_ms);
}

void WakeAllU32(std::atomic<std::uint32_t>& word) { WakeByAddressAll(&word); }

/* A high-resolution waitable timer (Windows 10 1803 on), not Sleep's
   millisecond steps that can overshoot by more; sleep_until where there is
   none. */
void SleepUntil(std::chrono::steady_clock::time_point when) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                       TIMER_ALL_ACCESS);
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    if (!timer) {
        std::this_thread::sleep_until(when);
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    else std::this_thread::sleep_until(when);
}

void SetThreadRole(ThreadRole role) {
    switch (role) {
        case ThreadRole::Worker: {
            THREAD_POWER_THROTTLING_STATE throttling{};
            throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
            throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
            throttling.StateMask = 0;               // never throttled
            SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &throttling, sizeof throttling);
            break;
        }
        case ThreadRole::FastWorker: SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); break;
        case ThreadRole::Emulation: SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); break;
        case ThreadRole::Timer: SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); break;
    }
}

void HoldTimerResolution() {
    timeBeginPeriod(1);             // sleeps to the millisecond, not Windows' 15.6 ms
    // ...even with the window hidden behind others: Windows 11 drops a
    // process's timer resolution then (a 200 us sleep took 15 ms, a blank
    // seen 14 ms late), unless told not to; and the process not throttled.
    PROCESS_POWER_THROTTLING_STATE process{};
    process.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    process.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    process.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &process, sizeof process);
}

bool HoldProcessToCores(std::uint64_t mask) {
    return mask && SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask));
}

std::filesystem::path ExecutablePath() {
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    return std::filesystem::path(exe);
}

int ProcessCores() {
    DWORD_PTR process = 0, system = 0;
    int cores = int(std::thread::hardware_concurrency());
    if (GetProcessAffinityMask(GetCurrentProcess(), &process, &system) && process) {
        cores = 0;
        for (DWORD_PTR m = process; m; m &= m - 1) ++cores;
    }
    return cores;
}

#else  // Apple

void WaitOnU32(const std::atomic<std::uint32_t>& word, std::uint32_t seen, unsigned timeout_ms) {
    if (!timeout_ms) return;            // (a timeout of zero is an error there)
    // A wake, a timeout, a value already changed or a spurious return alike
    // end the wait: every caller looks at the word again.
    os_sync_wait_on_address_with_timeout(const_cast<std::atomic<std::uint32_t>*>(&word), seen, sizeof seen,
                                         OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME,
                                         std::uint64_t(timeout_ms) * 1'000'000);
}

void WakeAllU32(std::atomic<std::uint32_t>& word) {
    os_sync_wake_by_address_all(&word, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

// mach_wait_until: to the tick of the system's absolute clock.
void SleepUntil(std::chrono::steady_clock::time_point when) {
    const auto left = when - std::chrono::steady_clock::now();
    if (left <= std::chrono::steady_clock::duration::zero()) return;
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const auto nanoseconds = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count());
    const std::uint64_t ticks = nanoseconds / timebase.numer * timebase.denom
                                + nanoseconds % timebase.numer * timebase.denom / timebase.numer;
    mach_wait_until(mach_absolute_time() + ticks);
}

// Quality of service in place of priorities: every role kept on the
// performance cores and never slowed for power, ordered within the class as
// Windows orders their priorities - the timer above the EE above the workers
// it feeds (a relative priority can only lower a thread within its class).
void SetThreadRole(ThreadRole role) {
    switch (role) {
        case ThreadRole::Worker: pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0); break;
        case ThreadRole::FastWorker: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, -2); break;
        case ThreadRole::Emulation: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, -1); break;
        case ThreadRole::Timer: pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0); break;
    }
}

void HoldTimerResolution() {}           // timers here are precise without asking

int ProcessCores() { return int(std::thread::hardware_concurrency()); }

// Apple's systems let a thread hint at its processors, not a process choose them.
bool HoldProcessToCores(std::uint64_t) { return false; }

std::filesystem::path ExecutablePath() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);   // the links resolved
    return error ? std::filesystem::path(path) : canonical;
}

#endif

}  // namespace ps2host::platform
