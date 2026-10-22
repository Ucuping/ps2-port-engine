/*
 * Fibers (platform.h).
 *
 * Windows: the system's fibers, made as the host always made them
 * (CreateFiber, ConvertThreadToFiber). Apple on arm64 - macOS, iOS: a switch
 * of our own, which keeps what AAPCS64 has a callee keep - x19 to x29, the
 * link register, d8 to d15 - on each fiber's own stack, and a stack mapped
 * for each fiber with a guard below it. x18 is left alone (Apple's platform
 * register). The build is arm64, not arm64e: no return address is signed.
 *
 * The float control register is not the switch's: SwitchFiber keeps it per
 * fiber, the same way on both.
 */

#include "platform.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "ps2rt/fp_mode.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__) && defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#else
#error "ps2host: no fibers here yet - Windows, and Apple's systems on arm64"
#endif

namespace ps2host::platform {

struct Fiber {
#if defined(_WIN32)
    LPVOID native = nullptr;
#else
    void* sp = nullptr;                 // its registers, saved on its stack, while it is not running
    void* mapping = nullptr;            // its stack with the guard below
    std::size_t mapped = 0;
#endif
    void (*entry)(void*) = nullptr;
    void* argument = nullptr;
    ps2rt::FloatControl control{};      // its float control register, while it is not running
};

namespace {

thread_local Fiber* t_current = nullptr;

// A fiber's first turn: the float control its creator had, then its entry.
[[noreturn]] void Begin(Fiber* fiber) {
    ps2rt::WriteFloatControl(fiber->control);
    fiber->entry(fiber->argument);
    std::abort();                       // an entry returned: there is nowhere to go
}

}  // namespace

#if defined(_WIN32)

namespace {

VOID CALLBACK NativeStart(LPVOID parameter) { Begin(static_cast<Fiber*>(parameter)); }

void Switch(Fiber* /*from*/, Fiber* to) { ::SwitchToFiber(to->native); }

}  // namespace

Fiber* ThreadFiber() {
    if (t_current) return t_current;
    auto* fiber = new Fiber;
    fiber->native = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
    if (!fiber->native) {
        delete fiber;
        throw std::bad_alloc();
    }
    t_current = fiber;
    return fiber;
}

Fiber* NewFiber(std::size_t stack_bytes, void (*entry)(void*), void* argument) {
    auto* fiber = new Fiber;
    fiber->entry = entry;
    fiber->argument = argument;
    fiber->control = ps2rt::ReadFloatControl();
    // The stack as CreateFiber always gave it: `stack_bytes` committed, the
    // executable's default reserved, the system's own guard page below.
    fiber->native = CreateFiber(stack_bytes, NativeStart, fiber);
    if (!fiber->native) {
        delete fiber;
        throw std::bad_alloc();
    }
    return fiber;
}

void FreeFiber(Fiber* fiber) {
    if (!fiber) return;
    if (fiber->native) DeleteFiber(fiber->native);
    delete fiber;
}

#else  // Apple, arm64

extern "C" void ps2_fiber_switch(void** save_sp, void* load_sp);
extern "C" void ps2_fiber_start();

// ps2_fiber_switch(x0: where this fiber's stack pointer goes, x1: the next
// fiber's): the callee-saved registers pushed (20 of 8 bytes - 160, a
// multiple of 16, so the stack pointer stays aligned), the stack pointers
// exchanged, the next fiber's registers popped, and a return to where it
// left off - or, a fiber's first time, to ps2_fiber_start.
//
// ps2_fiber_start: x19 is Begin and x20 its fiber (NewFiber's frame). x29 is
// zero there, and the return address is marked undefined, so a debugger's or
// an unwinder's walk up the stack ends at it.
asm(R"(
    .text
    .p2align 2
    .private_extern _ps2_fiber_switch
    .globl _ps2_fiber_switch
_ps2_fiber_switch:
    sub sp, sp, #160
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mov x2, sp
    str x2, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    ret

    .p2align 2
    .private_extern _ps2_fiber_start
    .globl _ps2_fiber_start
_ps2_fiber_start:
    .cfi_startproc
    .cfi_undefined x30
    mov x0, x20
    blr x19
    brk #0
    .cfi_endproc
)");

namespace {

void Switch(Fiber* from, Fiber* to) { ps2_fiber_switch(&from->sp, to->sp); }

std::size_t PageSize() {
    static const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    return page;
}

std::size_t RoundUp(std::size_t value, std::size_t unit) { return (value + unit - 1) / unit * unit; }

// The registers ps2_fiber_switch pops, in its order.
enum Slot { kX19, kX20, kX21, kX22, kX23, kX24, kX25, kX26, kX27, kX28, kX29, kX30, kD8, kSlots = kD8 + 8 };
static_assert(kSlots * 8 == 160, "ps2_fiber_switch's frame");

}  // namespace

Fiber* ThreadFiber() {
    if (!t_current) t_current = new Fiber;
    return t_current;
}

Fiber* NewFiber(std::size_t stack_bytes, void (*entry)(void*), void* argument) {
    // The page size asked of the system, never taken as 4 KB: 16 KB on Apple
    // silicon. The guard is 16 KB at least wherever pages are smaller, so a
    // frame larger than one small page still lands in it.
    const std::size_t page = PageSize();
    const std::size_t guard = RoundUp(std::max<std::size_t>(16 * 1024, page), page);
    const std::size_t stack = RoundUp(stack_bytes, page);
    void* mapping = mmap(nullptr, guard + stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) throw std::bad_alloc();
    if (mprotect(mapping, guard, PROT_NONE) != 0) {
        munmap(mapping, guard + stack);
        throw std::bad_alloc();
    }
    auto* fiber = new Fiber;
    fiber->mapping = mapping;
    fiber->mapped = guard + stack;
    fiber->entry = entry;
    fiber->argument = argument;
    fiber->control = ps2rt::ReadFloatControl();
    // The first frame ps2_fiber_switch pops, at the top of the stack: the top
    // is a page boundary, the frame 160 bytes, so the stack pointer the fiber
    // starts with is 16-byte aligned, as AAPCS64 has it everywhere.
    const auto top = reinterpret_cast<std::uintptr_t>(mapping) + guard + stack;
    auto* frame = reinterpret_cast<std::uint64_t*>(top - kSlots * 8);
    std::memset(frame, 0, kSlots * 8);
    frame[kX19] = reinterpret_cast<std::uint64_t>(&Begin);
    frame[kX20] = reinterpret_cast<std::uint64_t>(fiber);
    frame[kX29] = 0;
    frame[kX30] = reinterpret_cast<std::uint64_t>(&ps2_fiber_start);
    fiber->sp = frame;
    return fiber;
}

void FreeFiber(Fiber* fiber) {
    if (!fiber) return;
    if (fiber->mapping) munmap(fiber->mapping, fiber->mapped);
    delete fiber;
}

#endif

void SwitchFiber(Fiber* to) {
    Fiber* self = t_current;
    self->control = ps2rt::ReadFloatControl();
    t_current = to;
    Switch(self, to);
    // Back on this fiber: its own float control again.
    t_current = self;
    ps2rt::WriteFloatControl(self->control);
}

}  // namespace ps2host::platform
