/*
 * The platform layer's tests (host/src/platform): fibers, waits, sleeps,
 * guarded reads and the crash report - ps2platform alone, so that they run on
 * a platform before the rest of the host builds there. The same checks run
 * on Windows and on Apple's systems: what they pin is the same on both.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>

#include "platform/platform.h"
#include "ps2rt/fp_mode.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define PS2_NOINLINE __declspec(noinline)
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define PS2_NOINLINE __attribute__((noinline))
#endif

namespace platform = ps2host::platform;
using Clock = std::chrono::steady_clock;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

double Milliseconds(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// The float control register's control fields: MXCSR's exception flags are
// sticky state, raised by any arithmetic between a write and a read.
ps2rt::FloatControl Controls(ps2rt::FloatControl value) {
#if PS2RT_FP_MXCSR
    return value & ~0x3Fu;
#else
    return value;
#endif
}

// ------------------------------------------------------------------ fibers

struct PingPong {
    platform::Fiber* main = nullptr;
    long count = 0;
};

void PingPongEntry(void* parameter) {
    auto& p = *static_cast<PingPong*>(parameter);
    for (;;) {
        ++p.count;
        platform::SwitchFiber(p.main);
    }
}

void FibersSwitch() {
    PingPong p;
    p.main = platform::ThreadFiber();
    platform::Fiber* other = platform::NewFiber(1 << 20, PingPongEntry, &p);
    const long kTurns = 200000;
    const auto from = Clock::now();
    for (long i = 0; i < kTurns; ++i) platform::SwitchFiber(other);
    const double ns = Milliseconds(Clock::now() - from) * 1e6 / kTurns;
    Check(p.count == kTurns, "a switch to a fiber runs it once and comes back");
    std::printf("  a switch to a fiber and back: %.0f ns\n", ns);
    platform::FreeFiber(other);
}

// Values live across the switches: the compiler keeps them in callee-saved
// registers - x19 to x28 and d8 to d15 on arm64 - or spills them, while the
// other fiber works its own. Mixed so that no closed form stands in for them.
std::uint64_t Mix(std::uint64_t v) { return (v ^ (v >> 29)) * 0x9E3779B97F4A7C15ull; }

template <bool kSwitch>
PS2_NOINLINE void Rounds(std::uint64_t seed, platform::Fiber* other, std::uint64_t* integers, double* doubles) {
    std::uint64_t a = seed + 1, b = seed * 3, d = seed * 5, e = seed * 7, f = seed * 11, g = seed * 13,
                  h = seed * 17, i = seed * 19, j = seed * 23, k = seed * 29;
    double x = double(seed % 997) + 0.5, y = x + 1.25, z = y + 2.5, w = z + 3.75, u = w + 5, v = u + 6.25,
           s = v + 7.5, t = s + 8.75;
    for (int round = 0; round < 1000; ++round) {
        if constexpr (kSwitch) platform::SwitchFiber(other);
        a = Mix(a ^ b); b = Mix(b + d); d = Mix(d ^ e); e = Mix(e + f); f = Mix(f ^ g);
        g = Mix(g + h); h = Mix(h ^ i); i = Mix(i + j); j = Mix(j ^ k); k = Mix(k + a);
        x = std::fmod(x * 1.5 + y, 997.0); y = std::fmod(y * 1.25 + z, 991.0);
        z = std::fmod(z * 1.75 + w, 983.0); w = std::fmod(w * 1.125 + u, 977.0);
        u = std::fmod(u * 1.5 + v, 971.0); v = std::fmod(v * 1.25 + s, 967.0);
        s = std::fmod(s * 1.75 + t, 953.0); t = std::fmod(t * 1.125 + x, 947.0);
    }
    const std::uint64_t ints[] = {a, b, d, e, f, g, h, i, j, k};
    const double floats[] = {x, y, z, w, u, v, s, t};
    std::memcpy(integers, ints, sizeof ints);
    std::memcpy(doubles, floats, sizeof floats);
}

struct Churn {
    platform::Fiber* main = nullptr;
    std::uint64_t seed = 0;
};

void ChurnEntry(void* parameter) {
    auto& c = *static_cast<Churn*>(parameter);
    std::uint64_t ints[10];
    double floats[8];
    for (;;) Rounds<true>(c.seed, c.main, ints, floats);         // its own values, in the same registers
}

void ValuesSurviveSwitches() {
    Churn c;
    c.main = platform::ThreadFiber();
    c.seed = std::uint64_t(std::rand()) * 7919 + 1;
    const std::uint64_t seed = std::uint64_t(std::rand()) * 104729 + 3;
    std::uint64_t expected_ints[10], ints[10];
    double expected_floats[8], floats[8];
    Rounds<false>(seed, nullptr, expected_ints, expected_floats);
    platform::Fiber* other = platform::NewFiber(1 << 20, ChurnEntry, &c);
    Rounds<true>(seed, other, ints, floats);
    Check(std::memcmp(ints, expected_ints, sizeof ints) == 0,
          "integer values live across switches come back as they were");
    Check(std::memcmp(floats, expected_floats, sizeof floats) == 0,
          "double values live across switches come back as they were");
    platform::FreeFiber(other);
}

struct Modes {
    platform::Fiber* main = nullptr;
    ps2rt::FloatControl c0 = 0, cv = 0;
    ps2rt::FloatControl first = 0, kept = 0;
};

void ModesEntry(void* parameter) {
    auto& m = *static_cast<Modes*>(parameter);
    m.first = ps2rt::ReadFloatControl();         // what it started with
    platform::SwitchFiber(m.main);
    ps2rt::WriteFloatControl(m.cv);              // its own, now
    platform::SwitchFiber(m.main);
    m.kept = ps2rt::ReadFloatControl();          // still its own?
    for (;;) platform::SwitchFiber(m.main);
}

void LateEntry(void* parameter) {
    auto& m = *static_cast<Modes*>(parameter);
    m.first = ps2rt::ReadFloatControl();
    for (;;) platform::SwitchFiber(m.main);
}

// The float control register goes with the fiber, on Windows as on arm64:
// saved when a fiber switches away, put back when it runs again, and a new
// fiber starting with its creator's.
void FloatControlGoesWithTheFiber() {
    const ps2rt::FloatControl original = ps2rt::ReadFloatControl();
    Modes m;
    m.main = platform::ThreadFiber();
    m.c0 = original;
    m.cv = ps2rt::VuFloatControl(original);
    platform::Fiber* other = platform::NewFiber(1 << 20, ModesEntry, &m);     // made under c0
    ps2rt::WriteFloatControl(m.cv);
    platform::SwitchFiber(other);
    Check(Controls(m.first) == Controls(m.c0),
          "a new fiber starts with its creator's float control, not the switcher's");
    Check(Controls(ps2rt::ReadFloatControl()) == Controls(m.cv), "a fiber has its own float control back");
    ps2rt::WriteFloatControl(m.c0);
    platform::SwitchFiber(other);                // it sets cv for itself
    Check(Controls(ps2rt::ReadFloatControl()) == Controls(m.c0),
          "what another fiber sets does not come back with the switch");
    platform::SwitchFiber(other);
    Check(Controls(m.kept) == Controls(m.cv), "a fiber keeps the float control it set");

    ps2rt::WriteFloatControl(m.cv);
    platform::Fiber* late = platform::NewFiber(1 << 20, LateEntry, &m);      // made under cv
    ps2rt::WriteFloatControl(m.c0);
    platform::SwitchFiber(late);
    Check(Controls(m.first) == Controls(m.cv), "...whatever its creator had when it made it");
    ps2rt::WriteFloatControl(original);
    platform::FreeFiber(late);
    platform::FreeFiber(other);
}

struct Alignment {
    platform::Fiber* main = nullptr;
    bool aligned = false;
};

void AlignmentEntry(void* parameter) {
    auto& a = *static_cast<Alignment*>(parameter);
    alignas(16) volatile unsigned char probe[16] = {};
    a.aligned = reinterpret_cast<std::uintptr_t>(&probe[0]) % 16 == 0;
#if defined(__aarch64__)
    std::uintptr_t sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(sp));
    a.aligned = a.aligned && sp % 16 == 0;
#endif
    for (;;) platform::SwitchFiber(a.main);
}

void StackStartsAligned() {
    Alignment a;
    a.main = platform::ThreadFiber();
    platform::Fiber* other = platform::NewFiber(1 << 20, AlignmentEntry, &a);
    platform::SwitchFiber(other);
    Check(a.aligned, "a fiber's stack pointer is 16-byte aligned");
    platform::FreeFiber(other);
}

struct Thrower {
    platform::Fiber* main = nullptr;
    bool caught_inside = false;
    std::exception_ptr carried;
};

void ThrowerEntry(void* parameter) {
    auto& t = *static_cast<Thrower*>(parameter);
    try {
        throw std::runtime_error("inside a fiber");
    } catch (const std::runtime_error&) {
        t.caught_inside = true;
    }
    // As threads.cpp does: caught at the fiber's top, thrown again by the host.
    try {
        throw std::logic_error("for the host");
    } catch (...) {
        t.carried = std::current_exception();
    }
    for (;;) platform::SwitchFiber(t.main);
}

void ExceptionsInAFiber() {
    Thrower t;
    t.main = platform::ThreadFiber();
    platform::Fiber* other = platform::NewFiber(1 << 20, ThrowerEntry, &t);
    platform::SwitchFiber(other);
    Check(t.caught_inside, "an exception thrown and caught inside a fiber");
    bool rethrown = false;
    try {
        if (t.carried) std::rethrow_exception(t.carried);
    } catch (const std::logic_error&) {
        rethrown = true;
    }
    Check(rethrown, "an exception carried out of a fiber and thrown on the host's stack");
    platform::FreeFiber(other);
}

PS2_NOINLINE int Recurse(int depth) {
    volatile char pad[1024];
    pad[0] = char(depth);
    pad[sizeof pad - 1] = char(depth);
    if (depth == 0) return pad[0];
    return Recurse(depth - 1) + pad[sizeof pad - 1];
}

struct Deep {
    platform::Fiber* main = nullptr;
    int depth = 0;
    int result = -1;
};

void DeepEntry(void* parameter) {
    auto& d = *static_cast<Deep*>(parameter);
    d.result = Recurse(d.depth);
    for (;;) platform::SwitchFiber(d.main);
}

void MostOfTheStackIsThere() {
    Deep d;
    d.main = platform::ThreadFiber();
    d.depth = 700;                      // some 750 KB of a megabyte
    platform::Fiber* other = platform::NewFiber(1 << 20, DeepEntry, &d);
    platform::SwitchFiber(other);
    int expected = 0;
    for (int depth = d.depth; depth > 0; --depth) expected += char(depth);
    Check(d.result == expected, "a fiber recurses through most of its megabyte of stack");
    platform::FreeFiber(other);
}

// ------------------------------------------------------------------ waiting

// Bounds from above only where a wait may end early: both systems allow a
// spurious return, and every caller looks at the word again.
void WaitsEnd() {
    std::atomic<std::uint32_t> word{5};
    auto from = Clock::now();
    platform::WaitOnU32(word, 5, 30);
    Check(Milliseconds(Clock::now() - from) < 1000, "a wait on an unchanged word ends by its timeout");
    from = Clock::now();
    platform::WaitOnU32(word, 6, 5000);
    Check(Milliseconds(Clock::now() - from) < 1000, "a wait on a word already changed ends at once");

    double waited = -1;
    std::thread waiter([&] {
        const auto start = Clock::now();
        while (word.load() == 5) platform::WaitOnU32(word, 5, 5000);
        waited = Milliseconds(Clock::now() - start);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    word.store(7);
    platform::WakeAllU32(word);
    waiter.join();
    Check(waited >= 0 && waited < 2500, "a wake ends a wait long before its timeout");
}

void SleepsLastToTheDeadline() {
    const auto from = Clock::now();
    platform::SleepUntil(from + std::chrono::milliseconds(20));
    const double slept = Milliseconds(Clock::now() - from);
    Check(slept >= 19.9 && slept < 1000, "a sleep to a deadline wakes no earlier than it");
}

// ------------------------------------------------------------------ files

void TheExecutableIsFound() {
    const std::string path = platform::ExecutablePath().string();
    std::FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    Check(file != nullptr, "the running executable's path names a file");
    if (file) std::fclose(file);
}

// ------------------------------------------------------------------ faults

void CopiesThatMayFail() {
    const int source[4] = {1, 2, 3, 4};
    int target[4] = {};
    Check(platform::SafeCopy(target, source, sizeof source) && std::memcmp(target, source, sizeof source) == 0,
          "a readable source is copied");
    volatile std::uintptr_t nowhere = 16;
    Check(!platform::SafeCopy(target, reinterpret_cast<const void*>(nowhere), 16),
          "an unreadable one is not, and does not fault");
}

#if !defined(_WIN32)

// What a child process wrote to stderr, and how it ended.
struct Child {
    std::string output;
    int status = 0;
};

template <typename Body>
Child RunChild(Body body) {
    int pipe_fds[2];
    Child child;
    if (pipe(pipe_fds) != 0) return child;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipe_fds[1], STDERR_FILENO);
        close(pipe_fds[0]);
        body();
        _exit(1);
    }
    close(pipe_fds[1]);
    char buffer[4096];
    for (ssize_t n; (n = read(pipe_fds[0], buffer, sizeof buffer)) > 0;) child.output.append(buffer, size_t(n));
    close(pipe_fds[0]);
    waitpid(pid, &child.status, 0);
    return child;
}

void HandlerBefore(int) {
    const char text[] = "the handler before\n";
    (void)!write(STDERR_FILENO, text, sizeof text - 1);
    _exit(42);
}

void CrashReportThenTheHandlerBefore() {
    const Child child = RunChild([] {
        signal(SIGSEGV, HandlerBefore);
        signal(SIGBUS, HandlerBefore);
        platform::InstallCrashHandler();
        volatile std::uintptr_t nowhere = 16;
        *reinterpret_cast<volatile int*>(nowhere) = 1;
    });
    Check(child.output.find("[crash] SIG") != std::string::npos, "a crash is reported");
    Check(child.output.find("the handler before") != std::string::npos && WIFEXITED(child.status) &&
              WEXITSTATUS(child.status) == 42,
          "...then given to the handler that was there before");
}

void OverflowEntry(void*) { std::exit(Recurse(100000)); }

void GuardStopsAnOverflow() {
    const Child child = RunChild([] {
        signal(SIGSEGV, HandlerBefore);
        signal(SIGBUS, HandlerBefore);
        platform::InstallCrashHandler();
        platform::ThreadFiber();
        platform::SwitchFiber(platform::NewFiber(1 << 20, OverflowEntry, nullptr));
    });
    Check(child.output.find("[crash] SIG") != std::string::npos && WIFEXITED(child.status) &&
              WEXITSTATUS(child.status) == 42,
          "a fiber's stack overflow faults on its guard, and is reported on the handler's own stack");
}

#else

// What Windows' own fibers do with MXCSR, made as threads.cpp made them
// (CreateFiber, no FIBER_FLAG_FLOAT_SWITCH) - not what the layer does: the
// layer keeps it per fiber either way. Printed, for the record.
struct Native {
    LPVOID main = nullptr;
    unsigned c0 = 0, cv = 0, seen = 0;
};

VOID CALLBACK NativeEntry(LPVOID parameter) {
    auto& n = *static_cast<Native*>(parameter);
    n.seen = _mm_getcsr();
    _mm_setcsr(n.c0);
    SwitchToFiber(n.main);
}

void NativeFibersOnRecord() {
    std::thread([] {
        Native n;
        n.c0 = _mm_getcsr();
        n.cv = ps2rt::VuFloatControl(n.c0);
        n.main = ConvertThreadToFiber(nullptr);
        LPVOID other = CreateFiber(1 << 16, NativeEntry, &n);
        _mm_setcsr(n.cv);
        SwitchToFiber(other);
        const unsigned back = _mm_getcsr();
        _mm_setcsr(n.c0);
        DeleteFiber(other);
        ConvertFiberToThread();
        const bool thread = Controls(n.seen) == Controls(n.cv) && Controls(back) == Controls(n.c0);
        const bool fiber = Controls(n.seen) != Controls(n.cv) && Controls(back) == Controls(n.cv);
        std::printf("  Windows' own fibers (CreateFiber, no FIBER_FLAG_FLOAT_SWITCH): MXCSR %s\n",
                    thread ? "follows the thread" : fiber ? "goes with the fiber" : "does neither cleanly");
    }).join();
}

#endif

}  // namespace

int main() {
    FibersSwitch();
    ValuesSurviveSwitches();
    FloatControlGoesWithTheFiber();
    StackStartsAligned();
    ExceptionsInAFiber();
    MostOfTheStackIsThere();
    WaitsEnd();
    SleepsLastToTheDeadline();
    TheExecutableIsFound();
    CopiesThatMayFail();
#if !defined(_WIN32)
    CrashReportThenTheHandlerBefore();
    GuardStopsAnOverflow();
#else
    NativeFibersOnRecord();
#endif
    std::printf(g_failures ? "platform tests: %d failed\n" : "platform tests: all passed\n", g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
