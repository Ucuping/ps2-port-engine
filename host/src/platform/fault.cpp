/*
 * Reads that may fail, and the crash report (platform.h).
 *
 * Windows: SafeCopy guards the copy with structured exception handling, as
 * threads.cpp's read of a suspended thread's guest calls did; the crash
 * report is profile.cpp's filter. Apple's systems: vm_read_overwrite, which
 * fails rather than faults on an address it cannot read; and signal handlers
 * for the report, on an alternate stack.
 *
 * Android (not built yet): process_vm_readv could serve SafeCopy, but an
 * app's seccomp policy may refuse it - to be checked there, with a read under
 * sigsetjmp and a fault handler as the fallback.
 */

#include "platform.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <execinfo.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#error "ps2host: no guarded reads or crash reports here yet - Windows, and Apple's systems"
#endif

namespace ps2host::platform {

#if defined(_WIN32)

// No C++ objects here: the copy alone is guarded.
bool SafeCopy(void* target, const void* source, std::size_t bytes) {
    __try {
        std::memcpy(target, source, bytes);
    } __except (1) {
        return false;
    }
    return true;
}

#else  // Apple

bool SafeCopy(void* target, const void* source, std::size_t bytes) {
    vm_size_t copied = 0;
    const kern_return_t result =
        vm_read_overwrite(mach_task_self(), reinterpret_cast<vm_address_t>(source), bytes,
                          reinterpret_cast<vm_address_t>(target), &copied);
    return result == KERN_SUCCESS && copied == bytes;
}

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT};
struct sigaction g_previous[NSIG];
std::atomic<bool> g_reported{false};

// Only what a signal handler may call: write(2), and formatting by hand.
void Write(const char* text) { (void)!::write(STDERR_FILENO, text, std::strlen(text)); }

void WriteNumber(std::uint64_t value, int base) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = "0123456789ABCDEF"[value % base];
        value /= base;
    } while (value);
    char text[28];
    int k = 0;
    if (base == 16) {
        text[k++] = '0';
        text[k++] = 'x';
    }
    while (n) text[k++] = digits[--n];
    text[k] = 0;
    Write(text);
}

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (a fault)";
        case SIGBUS: return "SIGBUS (a fault)";
        case SIGILL: return "SIGILL (an illegal instruction)";
        case SIGFPE: return "SIGFPE (an arithmetic fault)";
        case SIGTRAP: return "SIGTRAP (a trap)";
        case SIGABRT: return "SIGABRT (an abort)";
    }
    return "a signal";
}

// The handler that was there before gets the signal as it came; with none,
// the default action does. A fault returns to the instruction that made it,
// which faults again with the default action in place - so the system's own
// report sees the fault itself; a signal that was sent is raised again.
void Forward(int sig, siginfo_t* info, void* context) {
    const struct sigaction& previous = g_previous[sig];
    sigaction(sig, &previous, nullptr);
    if (previous.sa_flags & SA_SIGINFO) {
        if (previous.sa_sigaction) {
            previous.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN) {
        previous.sa_handler(sig);
        return;
    }
    if (previous.sa_handler == SIG_IGN) signal(sig, SIG_DFL);       // an ignored fault comes back for ever
    const bool fault = info->si_code > 0 && sig != SIGABRT;
    if (!fault) raise(sig);
}

void Report(int sig, siginfo_t* info, void* context) {
    if (!g_reported.exchange(true)) {
        Write("[crash] ");
        Write(SignalName(sig));
#if defined(__aarch64__)
        Write(" at ");
        WriteNumber(static_cast<ucontext_t*>(context)->uc_mcontext->__ss.__pc, 16);
#endif
        Write(", address ");
        WriteNumber(reinterpret_cast<std::uintptr_t>(info->si_addr), 16);
        std::uint64_t thread = 0;
        pthread_threadid_np(nullptr, &thread);
        Write(" on thread ");
        WriteNumber(thread, 10);
        Write("\n[crash] native frames, innermost first:\n");
        void* frames[32];
        const int count = backtrace(frames, 32);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
    }
    Forward(sig, info, context);
}

}  // namespace

void InstallCrashHandler() {
    // The handler's own stack on this thread - the one the EE's fibers run
    // on: a fiber's stack run into its guard leaves none to report on.
    static void* alternate = nullptr;
    if (!alternate) {
        const std::size_t size = std::max<std::size_t>(SIGSTKSZ, 128 * 1024);
        alternate = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (alternate == MAP_FAILED) {
            alternate = nullptr;
        } else {
            stack_t stack{};
            stack.ss_sp = alternate;
            stack.ss_size = size;
            sigaltstack(&stack, nullptr);
        }
    }
    struct sigaction action{};
    action.sa_sigaction = Report;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : kSignals) sigaction(sig, &action, &g_previous[sig]);
}

#endif

}  // namespace ps2host::platform
