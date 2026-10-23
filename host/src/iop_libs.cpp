/*
 * The IOP libraries a module imports, implemented by the host (iop_vm.h).
 *
 * Each function is bound by library and export number; the numbers and names
 * are the SDK's (engine/data/iop_exports.csv, from ps2sdk's export tables), and
 * the error codes are the kernel's (ps2sdk kerr.h). Arguments arrive as the
 * IOP's calling convention passes them: $a0-$a3, then the stack.
 *
 *   sysmem, sysclib, stdio   memory, C library, console output (logged as [iop])
 *   intrman                  interrupts: the host has no interrupt masking
 *   thbase, thsemap, thevent the IOP kernel's threads, semaphores, event flags,
 *                            clock and alarms, on the interpreter's scheduler
 *   sifman, sifcmd           SIF DMA and commands, to and from the EE (iop.cpp);
 *                            RPC served, and made to the EE's servers
 *   thvpool, thfpool, modload  memory pools, the modules loaded
 *   cdvdman, ioman           the disc: sectors, and its files (read-only; no
 *                            other device, as on a console without a host)
 *   libsd                    the sound processor (SPU2): state is kept, sound
 *                            data is copied into SPU RAM, and transfer interrupts
 *                            arrive at the rate the hardware's would - nothing
 *                            is played yet.
 *
 * A function the host lacks stops the run with its name (iop_vm.cpp).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <cctype>
#include <map>
#include <memory>
#include <sstream>

#include "iop_vm_internal.h"
#include "spu2.h"
#include "ps2host/disc.h"
#include "ps2host/memory.h"
#include "ps2host/run.h"
#include "ps2host/profile.h"

namespace ps2host {
namespace iop {

namespace {

constexpr s32 KE_OK = 0, KE_ILLEGAL_CONTEXT = -100, KE_UNKNOWN_THID = -407;
constexpr s32 KE_NOT_DORMANT = -414, KE_NOT_WAIT = -416, KE_CAN_NOT_WAIT = -417;
constexpr s32 KE_RELEASE_WAIT = -418, KE_SEMA_ZERO = -419, KE_EVF_COND = -421, KE_UNKNOWN_SEMID = -408;
constexpr u32 WEF_OR = 1, WEF_CLEAR = 0x10;
constexpr u64 kClockHz = 36864000;              // the IOP's system clock

using Map = LibraryTable;

u32 A(Vm& vm, Context& c, int i) { return Arg(vm, c, i); }

Outcome Ret(Context& c, u32 value) {
    Return(c, value);
    return Outcome::Return;
}

u64 UsToClock(u64 us) { return us * kClockHz / 1000000; }
u64 ClockToUs(u64 clock) { return clock * 1000000 / kClockHz; }

void Log(const std::string& text) {
    std::string line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (!line.empty()) std::fprintf(stderr, "[iop] %s\n", line.c_str());
}

// ---------------------------------------------------------------- threads

Outcome CreateThread(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 0);           // iop_thread_t: attr, option, thread, stacksize, priority
    Thread t;
    t.id = vm.next_id++;
    t.entry = vm.Read(p + 8, 4);
    t.stack_size = (vm.Read(p + 12, 4) + 15) & ~15u;
    t.priority = s32(vm.Read(p + 16, 4));
    t.attr = vm.Read(p, 4);
    t.option = vm.Read(p + 4, 4);
    t.initial_priority = t.priority;
    t.stack = vm.Allocate(t.stack_size);
    if (!t.stack) return Ret(c, u32(-400));          // KE_NO_MEMORY
    t.wait = Wait::Dormant;
    vm.threads[t.id] = t;
    return Ret(c, t.id);
}

Outcome Start(Vm& vm, Context& c, u32 id, u32 arg) {
    auto found = vm.threads.find(id);
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    Thread& t = found->second;
    if (t.wait != Wait::Dormant) return Ret(c, u32(KE_NOT_DORMANT));
    t.c = Context{};
    t.c.r[4] = arg;
    t.c.r[28] = c.r[28];
    t.c.r[29] = t.stack + t.stack_size - 16;
    t.c.r[31] = kThreadExit;
    t.c.pc = t.entry;
    t.c.npc = t.entry + 4;
    vm.MakeReady(t);
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

Outcome StartThread(Vm& vm, Context& c) { return Start(vm, c, A(vm, c, 0), A(vm, c, 1)); }
Outcome StartThreadArgs(Vm& vm, Context& c) { return Start(vm, c, A(vm, c, 0), A(vm, c, 2)); }

Outcome ExitThread(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_ILLEGAL_CONTEXT));
    t->ready = false;
    t->wait = Wait::Dormant;
    return Outcome::Block;
}

Outcome DeleteThread(Vm& vm, Context& c) {
    auto found = vm.threads.find(A(vm, c, 0));
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    if (found->second.wait != Wait::Dormant || found->first == vm.current) return Ret(c, u32(KE_NOT_DORMANT));
    vm.Free(found->second.stack);
    vm.threads.erase(found);
    return Ret(c, KE_OK);
}

Outcome TerminateThread(Vm& vm, Context& c) {
    auto found = vm.threads.find(A(vm, c, 0));
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    found->second.ready = false;
    found->second.wait = Wait::Dormant;
    return Ret(c, KE_OK);
}

Outcome GetThreadId(Vm& vm, Context& c) {
    return Ret(c, vm.in_interrupt ? u32(KE_ILLEGAL_CONTEXT) : vm.current);
}

Outcome ChangeThreadPriority(Vm& vm, Context& c) {
    u32 id = A(vm, c, 0);
    if (id == 0) id = vm.current;
    auto found = vm.threads.find(id);
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    found->second.priority = s32(A(vm, c, 1));
    Return(c, KE_OK);
    return vm.in_interrupt ? Outcome::Return : Outcome::Yield;
}

Outcome RotateThreadReadyQueue(Vm& vm, Context& c) {
    Return(c, KE_OK);
    return vm.in_interrupt ? Outcome::Return : Outcome::Yield;
}

Outcome GetThreadCurrentPriority(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    return Ret(c, t ? u32(t->priority) : u32(KE_ILLEGAL_CONTEXT));
}

Outcome SleepThread(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_CAN_NOT_WAIT));
    Return(c, KE_OK);
    if (t->wakeups > 0) {
        --t->wakeups;
        return Outcome::Return;
    }
    vm.Block(c, Wait::Sleep, 0);
    return Outcome::Block;
}

Outcome WakeupThread(Vm& vm, Context& c) {
    auto found = vm.threads.find(A(vm, c, 0));
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    Thread& t = found->second;
    if (!t.ready && t.wait == Wait::Sleep) {
        t.c.r[2] = KE_OK;
        vm.MakeReady(t);
    } else {
        ++t.wakeups;
    }
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

// (i)ReleaseWaitThread(id): a waiting thread's wait ends in KE_RELEASE_WAIT.
Outcome ReleaseWaitThread(Vm& vm, Context& c) {
    auto found = vm.threads.find(A(vm, c, 0));
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    Thread& t = found->second;
    if (t.ready || t.wait == Wait::Dormant || t.wait == Wait::None) return Ret(c, u32(KE_NOT_WAIT));
    if (t.wait == Wait::Sema) {
        auto& waiting = vm.semaphores[t.object].waiting;
        waiting.erase(std::remove(waiting.begin(), waiting.end(), t.id), waiting.end());
    }
    t.c.r[2] = u32(KE_RELEASE_WAIT);
    vm.MakeReady(t);
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

Outcome CancelWakeupThread(Vm& vm, Context& c) {
    u32 id = A(vm, c, 0);
    if (id == 0) id = vm.current;
    auto found = vm.threads.find(id);
    if (found == vm.threads.end()) return Ret(c, u32(KE_UNKNOWN_THID));
    const s32 count = found->second.wakeups;
    found->second.wakeups = 0;
    return Ret(c, u32(count));
}

// WaitVblankStart / WaitVblankEnd (vblank): the thread waits until the next
// field starts or ends - here, until the next 1/59.94 s boundary of the IOP's
// clock, which is when the host delivers the vertical blank.
Outcome WaitVblank(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_CAN_NOT_WAIT));
    constexpr u64 kFieldUs = 16683;
    t->until_us = (vm.NowUs() / kFieldUs + 1) * kFieldUs;
    Return(c, KE_OK);
    vm.Block(c, Wait::Delay, 0);
    return Outcome::Block;
}

Outcome DelayThread(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_CAN_NOT_WAIT));
    t->until_us = vm.NowUs() + A(vm, c, 0);
    Return(c, KE_OK);
    vm.Block(c, Wait::Delay, 0);
    return Outcome::Block;
}

void PutClock(Vm& vm, u32 p, u64 clock) {
    vm.Write(p, u32(clock), 4);
    vm.Write(p + 4, u32(clock >> 32), 4);
}

u64 GetClock(Vm& vm, u32 p) { return u64(vm.Read(p, 4)) | (u64(vm.Read(p + 4, 4)) << 32); }

Outcome GetSystemTime(Vm& vm, Context& c) {
    PutClock(vm, A(vm, c, 0), UsToClock(vm.NowUs()));
    return Ret(c, KE_OK);
}

Outcome GetSystemTimeLow(Vm& vm, Context& c) { return Ret(c, u32(UsToClock(vm.NowUs()))); }

Outcome USec2SysClock(Vm& vm, Context& c) {
    PutClock(vm, A(vm, c, 1), UsToClock(A(vm, c, 0)));
    return Ret(c, KE_OK);
}

Outcome SysClock2USec(Vm& vm, Context& c) {
    const u64 us = ClockToUs(GetClock(vm, A(vm, c, 0)));
    vm.Write(A(vm, c, 1), u32(us / 1000000), 4);
    vm.Write(A(vm, c, 2), u32(us % 1000000), 4);
    return Ret(c, KE_OK);
}

Outcome SetAlarm(Vm& vm, Context& c) {
    const u64 clock = GetClock(vm, A(vm, c, 0));
    const u32 handler = A(vm, c, 1), arg = A(vm, c, 2);
    vm.alarms[handler] = {vm.NowUs() + ClockToUs(clock), handler, arg};
    return Ret(c, KE_OK);
}

Outcome CancelAlarm(Vm& vm, Context& c) {
    vm.alarms.erase(A(vm, c, 0));
    return Ret(c, KE_OK);
}

// look_ctype_table() (sysclib): the character classes, a byte a character as
// the C library's ctype macros index them: upper 0x01, lower 0x02, digit 0x04,
// space 0x08, punctuation 0x10, control 0x20, hex digit 0x40, blank 0x80. Kept
// in IOP memory, made the first time it is asked for.
Outcome LookCtypeTable(Vm& vm, Context& c) {
    static u32 table = 0;
    if (!table) {
        table = vm.Allocate(256);
        if (!table) return Ret(c, 0);
        for (u32 ch = 0; ch < 256; ++ch) {
            u32 flags = 0;
            if (ch >= 'A' && ch <= 'Z') flags |= 0x01;
            if (ch >= 'a' && ch <= 'z') flags |= 0x02;
            if (ch >= '0' && ch <= '9') flags |= 0x04;
            if (ch == ' ' || (ch >= '\t' && ch <= '\r')) flags |= 0x08;
            if (ch > ' ' && ch < 0x7F && !(flags & 0x07)) flags |= 0x10;
            if (ch < ' ' || ch == 0x7F) flags |= 0x20;
            if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
                flags |= 0x40;
            if (ch == ' ') flags |= 0x80;
            vm.Write(table + ch, flags, 1);
        }
    }
    return Ret(c, table);
}

// ---------------------------------------------------------------- message boxes

constexpr s32 KE_UNKNOWN_MBXID = -410, KE_MBOX_NOMSG = -424, KE_WAIT_DELETE = -425;

Outcome CreateMbx(Vm& vm, Context& c) {
    MessageBox box;
    box.attr = vm.Read(A(vm, c, 0), 4);          // iop_mbx_t: attr, option
    const u32 id = vm.next_id++;
    vm.mailboxes[id] = box;
    return Ret(c, id);
}

Outcome DeleteMbx(Vm& vm, Context& c) {
    auto found = vm.mailboxes.find(A(vm, c, 0));
    if (found == vm.mailboxes.end()) return Ret(c, u32(KE_UNKNOWN_MBXID));
    for (const auto& waiter : found->second.waiting) {
        Thread& t = vm.threads[waiter.first];
        t.c.r[2] = u32(KE_WAIT_DELETE);
        vm.MakeReady(t);
    }
    vm.mailboxes.erase(found);
    return Ret(c, KE_OK);
}

// SendMbx / iSendMbx(id, message): to the first thread waiting, or queued.
Outcome SendMbx(Vm& vm, Context& c) {
    auto found = vm.mailboxes.find(A(vm, c, 0));
    if (found == vm.mailboxes.end()) return Ret(c, u32(KE_UNKNOWN_MBXID));
    MessageBox& box = found->second;
    const u32 message = A(vm, c, 1);
    if (!box.waiting.empty()) {
        const std::pair<u32, u32> waiter = box.waiting.front();
        box.waiting.pop_front();
        Thread& t = vm.threads[waiter.first];
        vm.Write(waiter.second, message, 4);
        t.c.r[2] = KE_OK;
        vm.MakeReady(t);
    } else {
        box.messages.push_back(message);
    }
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

// ReceiveMbx(&message, id): waits for one; PollMbx: KE_MBOX_NOMSG if none.
Outcome ReceiveMbx(Vm& vm, Context& c) {
    const u32 where = A(vm, c, 0);
    auto found = vm.mailboxes.find(A(vm, c, 1));
    if (found == vm.mailboxes.end()) return Ret(c, u32(KE_UNKNOWN_MBXID));
    MessageBox& box = found->second;
    if (!box.messages.empty()) {
        vm.Write(where, box.messages.front(), 4);
        box.messages.pop_front();
        return Ret(c, KE_OK);
    }
    if (!vm.Block(c, Wait::Mbx, found->first)) return Ret(c, u32(KE_CAN_NOT_WAIT));
    box.waiting.push_back({vm.current, where});
    return Outcome::Block;
}

Outcome PollMbx(Vm& vm, Context& c) {
    const u32 where = A(vm, c, 0);
    auto found = vm.mailboxes.find(A(vm, c, 1));
    if (found == vm.mailboxes.end()) return Ret(c, u32(KE_UNKNOWN_MBXID));
    MessageBox& box = found->second;
    if (box.messages.empty()) return Ret(c, u32(KE_MBOX_NOMSG));
    vm.Write(where, box.messages.front(), 4);
    box.messages.pop_front();
    return Ret(c, KE_OK);
}

// ---------------------------------------------------------------- hardware timers

constexpr s32 KE_ILLEGAL_TIMERID = -150, KE_NO_TIMER = -151;

// AllocHardTimer(source, size, prescale): an id for a timer of that clock.
Outcome AllocHardTimer(Vm& vm, Context& c) {
    if (vm.hard_timers.size() >= 4) return Ret(c, u32(KE_NO_TIMER));
    HardTimer t;
    t.source = A(vm, c, 0);
    t.bits = A(vm, c, 1) == 16 ? 16 : 32;
    t.prescale = std::max(1u, A(vm, c, 2));
    const u32 id = 0x100 + u32(vm.hard_timers.size());
    vm.hard_timers[id] = t;
    return Ret(c, id);
}

HardTimer* TimerOf(Vm& vm, Context& c) {
    auto found = vm.hard_timers.find(A(vm, c, 0));
    return found == vm.hard_timers.end() ? nullptr : &found->second;
}

Outcome FreeHardTimer(Vm& vm, Context& c) {
    return Ret(c, vm.hard_timers.erase(A(vm, c, 0)) ? u32(KE_OK) : u32(KE_ILLEGAL_TIMERID));
}

// SetTimerHandler(id, compare, handler, common)
Outcome SetTimerHandler(Vm& vm, Context& c) {
    HardTimer* t = TimerOf(vm, c);
    if (!t) return Ret(c, u32(KE_ILLEGAL_TIMERID));
    t->compare = A(vm, c, 1);
    t->handler = A(vm, c, 2);
    t->common = A(vm, c, 3);
    return Ret(c, KE_OK);
}

// SetupHardTimer(id, source, mode, prescale)
Outcome SetupHardTimer(Vm& vm, Context& c) {
    HardTimer* t = TimerOf(vm, c);
    if (!t) return Ret(c, u32(KE_ILLEGAL_TIMERID));
    t->source = A(vm, c, 1);
    t->prescale = std::max(1u, A(vm, c, 3));
    return Ret(c, KE_OK);
}

// A timer's counter: the counts of its clock, prescaled, since it started -
// since its last interrupt when a handler restarts it at its compare value -
// as wide as the timer (16 or 32 bits).
u32 Counter(Vm& vm, const HardTimer& t) {
    if (!t.running) return t.count;
    u64 counts = u64(double(vm.NowUs() - t.started_us) * HardTimerHz(t) / 1e6 / t.prescale);
    if (t.handler && t.compare) counts %= t.compare;
    return t.bits == 16 ? u32(counts & 0xFFFF) : u32(counts);
}

Outcome StartHardTimer(Vm& vm, Context& c) {
    HardTimer* t = TimerOf(vm, c);
    if (!t) return Ret(c, u32(KE_ILLEGAL_TIMERID));
    t->running = true;
    t->started_us = vm.NowUs();
    t->due_us = vm.NowUs() + HardTimerPeriodUs(*t);
    return Ret(c, KE_OK);
}

Outcome StopHardTimer(Vm& vm, Context& c) {
    HardTimer* t = TimerOf(vm, c);
    if (!t) return Ret(c, u32(KE_ILLEGAL_TIMERID));
    t->count = Counter(vm, *t);
    t->running = false;
    return Ret(c, KE_OK);
}

// GetTimerCounter(id)
Outcome GetTimerCounter(Vm& vm, Context& c) {
    HardTimer* t = TimerOf(vm, c);
    return Ret(c, t ? Counter(vm, *t) : u32(KE_ILLEGAL_TIMERID));
}

// sceSifGetOtherData(receive data, EE source, IOP destination, size, mode):
// EE memory read into IOP memory; done at once.
Outcome SifGetOtherData(Vm& vm, Context& c) {
    const u32 source = A(vm, c, 1), dest = A(vm, c, 2), size = A(vm, c, 3);
    Memory& ee = GuestMemory();
    for (u32 i = 0; i < size; ++i) vm.Write(dest + i, u32(ee.Read(source + i, 1)), 1);
    return Ret(c, 0);
}

// ---------------------------------------------------------------- semaphores

Outcome CreateSema(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 0);           // iop_sema_t: attr, option, initial, max
    Semaphore s;
    s.attr = vm.Read(p, 4);
    s.count = s32(vm.Read(p + 8, 4));
    s.max = s32(vm.Read(p + 12, 4));
    const u32 id = vm.next_id++;
    vm.semaphores[id] = s;
    return Ret(c, id);
}

Outcome DeleteSema(Vm& vm, Context& c) {
    auto found = vm.semaphores.find(A(vm, c, 0));
    if (found == vm.semaphores.end()) return Ret(c, u32(KE_UNKNOWN_SEMID));
    for (u32 id : found->second.waiting) {
        Thread& t = vm.threads[id];
        t.c.r[2] = u32(-425);                        // KE_WAIT_DELETE
        vm.MakeReady(t);
    }
    vm.semaphores.erase(found);
    return Ret(c, KE_OK);
}

Outcome SignalSema(Vm& vm, Context& c) {
    auto found = vm.semaphores.find(A(vm, c, 0));
    if (found == vm.semaphores.end()) return Ret(c, u32(KE_UNKNOWN_SEMID));
    Semaphore& s = found->second;
    if (!s.waiting.empty()) {
        Thread& t = vm.threads[s.waiting.front()];
        s.waiting.pop_front();
        t.c.r[2] = KE_OK;
        vm.MakeReady(t);
    } else if (s.count < s.max || s.max <= 0) {
        ++s.count;
    }
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

Outcome WaitSema(Vm& vm, Context& c) {
    auto found = vm.semaphores.find(A(vm, c, 0));
    if (found == vm.semaphores.end()) return Ret(c, u32(KE_UNKNOWN_SEMID));
    Semaphore& s = found->second;
    if (s.count > 0) {
        --s.count;
        return Ret(c, KE_OK);
    }
    if (!vm.Block(c, Wait::Sema, found->first)) return Ret(c, u32(KE_CAN_NOT_WAIT));
    s.waiting.push_back(vm.current);
    return Outcome::Block;
}

Outcome PollSema(Vm& vm, Context& c) {
    auto found = vm.semaphores.find(A(vm, c, 0));
    if (found == vm.semaphores.end()) return Ret(c, u32(KE_UNKNOWN_SEMID));
    if (found->second.count <= 0) return Ret(c, u32(KE_SEMA_ZERO));
    --found->second.count;
    return Ret(c, KE_OK);
}

Outcome ReferSemaStatus(Vm& vm, Context& c) {
    auto found = vm.semaphores.find(A(vm, c, 0));
    if (found == vm.semaphores.end()) return Ret(c, u32(KE_UNKNOWN_SEMID));
    const u32 p = A(vm, c, 1);           // attr, option, initial, max, current, waiting, ...
    const Semaphore& s = found->second;
    vm.Write(p, s.attr, 4);
    vm.Write(p + 4, 0, 4);
    vm.Write(p + 8, 0, 4);
    vm.Write(p + 12, u32(s.max), 4);
    vm.Write(p + 16, u32(s.count), 4);
    vm.Write(p + 20, u32(s.waiting.size()), 4);
    return Ret(c, KE_OK);
}

// ---------------------------------------------------------------- event flags

bool Satisfied(u32 bits, u32 want, u32 mode) {
    return (mode & WEF_OR) ? (bits & want) != 0 : (bits & want) == want;
}

// Wakes the threads whose wait an event flag's new bits satisfy.
void CheckWaiters(Vm& vm, u32 id) {
    EventFlag& e = vm.events[id];
    for (auto& [tid, t] : vm.threads) {
        if (t.ready || t.wait != Wait::Event || t.object != id) continue;
        if (!Satisfied(e.bits, t.event_bits, t.event_mode)) continue;
        if (t.event_result) vm.Write(t.event_result, e.bits, 4);
        if (t.event_mode & WEF_CLEAR) e.bits &= ~t.event_bits;
        t.c.r[2] = KE_OK;
        vm.MakeReady(t);
    }
}

Outcome CreateEventFlag(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 0);           // iop_event_t: attr, option, bits
    EventFlag e;
    e.attr = vm.Read(p, 4);
    e.bits = vm.Read(p + 8, 4);
    const u32 id = vm.next_id++;
    vm.events[id] = e;
    return Ret(c, id);
}

Outcome DeleteEventFlag(Vm& vm, Context& c) {
    vm.events.erase(A(vm, c, 0));
    return Ret(c, KE_OK);
}

Outcome SetEventFlag(Vm& vm, Context& c) {
    const u32 id = A(vm, c, 0);
    if (!vm.events.count(id)) return Ret(c, u32(-409));     // KE_UNKNOWN_EVFID
    vm.events[id].bits |= A(vm, c, 1);
    CheckWaiters(vm, id);
    Return(c, KE_OK);
    return vm.reschedule && !vm.in_interrupt ? Outcome::Yield : Outcome::Return;
}

Outcome ClearEventFlag(Vm& vm, Context& c) {
    const u32 id = A(vm, c, 0);
    if (!vm.events.count(id)) return Ret(c, u32(-409));
    vm.events[id].bits &= A(vm, c, 1);
    return Ret(c, KE_OK);
}

Outcome WaitEventFlag(Vm& vm, Context& c) {
    const u32 id = A(vm, c, 0), want = A(vm, c, 1), mode = A(vm, c, 2), result = A(vm, c, 3);
    if (!vm.events.count(id)) return Ret(c, u32(-409));
    EventFlag& e = vm.events[id];
    if (Satisfied(e.bits, want, mode)) {
        if (result) vm.Write(result, e.bits, 4);
        if (mode & WEF_CLEAR) e.bits &= ~want;
        return Ret(c, KE_OK);
    }
    if (!vm.Block(c, Wait::Event, id)) return Ret(c, u32(KE_CAN_NOT_WAIT));
    Thread* t = vm.Current();
    t->event_bits = want;
    t->event_mode = mode;
    t->event_result = result;
    return Outcome::Block;
}

Outcome PollEventFlag(Vm& vm, Context& c) {
    const u32 id = A(vm, c, 0), want = A(vm, c, 1), mode = A(vm, c, 2), result = A(vm, c, 3);
    if (!vm.events.count(id)) return Ret(c, u32(-409));
    EventFlag& e = vm.events[id];
    if (!Satisfied(e.bits, want, mode)) return Ret(c, u32(KE_EVF_COND));
    if (result) vm.Write(result, e.bits, 4);
    if (mode & WEF_CLEAR) e.bits &= ~want;
    return Ret(c, KE_OK);
}

Outcome ReferEventFlagStatus(Vm& vm, Context& c) {
    const u32 id = A(vm, c, 0), p = A(vm, c, 1);    // attr, option, initBits, currBits, numThreads
    if (!vm.events.count(id)) return Ret(c, u32(-409));
    const EventFlag& e = vm.events[id];
    vm.Write(p, e.attr, 4);
    vm.Write(p + 4, 0, 4);
    vm.Write(p + 8, 0, 4);
    vm.Write(p + 12, e.bits, 4);
    vm.Write(p + 16, 0, 4);
    return Ret(c, KE_OK);
}

// ---------------------------------------------------------------- memory, C library

Outcome AllocSysMemory(Vm& vm, Context& c) { return Ret(c, vm.Allocate(A(vm, c, 1))); }
Outcome FreeSysMemory(Vm& vm, Context& c) { vm.Free(A(vm, c, 0)); return Ret(c, KE_OK); }
Outcome QueryMemSize(Vm&, Context& c) { return Ret(c, kRamSize); }
Outcome QueryMaxFreeMemSize(Vm& vm, Context& c) { return Ret(c, vm.LargestFree()); }

Outcome Memcpy(Vm& vm, Context& c) {
    const u32 d = A(vm, c, 0), s = A(vm, c, 1), n = A(vm, c, 2);
    if (n) std::memmove(vm.At(d, n), vm.At(s, n), n);
    return Ret(c, d);
}

Outcome Bcopy(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0), d = A(vm, c, 1), n = A(vm, c, 2);
    if (n) std::memmove(vm.At(d, n), vm.At(s, n), n);
    return Ret(c, 0);
}

Outcome Memset(Vm& vm, Context& c) {
    const u32 d = A(vm, c, 0), n = A(vm, c, 2);
    if (n) std::memset(vm.At(d, n), int(A(vm, c, 1) & 0xFF), n);
    return Ret(c, d);
}

Outcome Bzero(Vm& vm, Context& c) {
    const u32 d = A(vm, c, 0), n = A(vm, c, 1);
    if (n) std::memset(vm.At(d, n), 0, n);
    return Ret(c, 0);
}

Outcome Memcmp(Vm& vm, Context& c) {
    const u32 n = A(vm, c, 2);
    return Ret(c, n ? u32(std::memcmp(vm.At(A(vm, c, 0), n), vm.At(A(vm, c, 1), n), n)) : 0);
}

Outcome Strlen(Vm& vm, Context& c) { return Ret(c, u32(vm.String(A(vm, c, 0), 1u << 20).size())); }

Outcome Strcpy(Vm& vm, Context& c) {
    const std::string s = vm.String(A(vm, c, 1), 1u << 20);
    std::memcpy(vm.At(A(vm, c, 0), u32(s.size()) + 1), s.c_str(), s.size() + 1);
    return Ret(c, A(vm, c, 0));
}

Outcome Strncpy(Vm& vm, Context& c) {
    const u32 d = A(vm, c, 0), n = A(vm, c, 2);
    const std::string s = vm.String(A(vm, c, 1), n);
    for (u32 i = 0; i < n; ++i) vm.Write(d + i, i < s.size() ? u8(s[i]) : 0, 1);
    return Ret(c, d);
}

Outcome Strcmp(Vm& vm, Context& c) {
    return Ret(c, u32(std::strcmp(vm.String(A(vm, c, 0), 1u << 20).c_str(),
                                  vm.String(A(vm, c, 1), 1u << 20).c_str())));
}

Outcome Strncmp(Vm& vm, Context& c) {
    const u32 n = A(vm, c, 2);
    return Ret(c, u32(std::strncmp(vm.String(A(vm, c, 0), n).c_str(),
                                   vm.String(A(vm, c, 1), n).c_str(), n)));
}

Outcome Strcat(Vm& vm, Context& c) {
    const u32 d = A(vm, c, 0);
    const std::string joined = vm.String(d, 1u << 20) + vm.String(A(vm, c, 1), 1u << 20);
    std::memcpy(vm.At(d, u32(joined.size()) + 1), joined.c_str(), joined.size() + 1);
    return Ret(c, d);
}

Outcome Strchr(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0);
    const char want = char(A(vm, c, 1));
    const std::string text = vm.String(s, 1u << 20);
    if (!want) return Ret(c, s + u32(text.size()));
    const std::size_t at = text.find(want);
    return Ret(c, at == std::string::npos ? 0 : s + u32(at));
}

Outcome Strrchr(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0);
    const std::string text = vm.String(s, 1u << 20);
    const std::size_t at = text.rfind(char(A(vm, c, 1)));
    return Ret(c, at == std::string::npos ? 0 : s + u32(at));
}

Outcome Tolower(Vm& vm, Context& c) { (void)vm; return Ret(c, u32(std::tolower(int(c.r[4] & 0xFF)))); }
Outcome Toupper(Vm& vm, Context& c) { (void)vm; return Ret(c, u32(std::toupper(int(c.r[4] & 0xFF)))); }

Outcome Strtol(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0), end = A(vm, c, 1);
    const std::string text = vm.String(s, 256);
    char* stop = nullptr;
    const long value = std::strtol(text.c_str(), &stop, int(A(vm, c, 2)));
    if (end) vm.Write(end, s + u32(stop - text.c_str()), 4);
    return Ret(c, u32(value));
}

Outcome Strtoul(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0), end = A(vm, c, 1);
    const std::string text = vm.String(s, 256);
    char* stop = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &stop, int(A(vm, c, 2)));
    if (end) vm.Write(end, s + u32(stop - text.c_str()), 4);
    return Ret(c, u32(value));
}

Outcome Strstr(Vm& vm, Context& c) {
    const u32 s = A(vm, c, 0);
    const std::string text = vm.String(s, 1u << 20);
    const std::size_t at = text.find(vm.String(A(vm, c, 1), 1u << 20));
    return Ret(c, at == std::string::npos ? 0 : s + u32(at));
}

// Arguments of a variadic call: from argument `first` on.
std::function<u32()> Varargs(Vm& vm, Context& c, int first) {
    auto index = std::make_shared<int>(first);
    return [&vm, &c, index]() { return Arg(vm, c, (*index)++); };
}

// Arguments from a va_list: on the IOP, a pointer to consecutive words.
std::function<u32()> VaList(Vm& vm, u32 list) {
    auto at = std::make_shared<u32>(list);
    return [&vm, at]() { const u32 v = vm.Read(*at, 4); *at += 4; return v; };
}

Outcome Sprintf(Vm& vm, Context& c) {
    const std::string text = Format(vm, A(vm, c, 1), Varargs(vm, c, 2));
    std::memcpy(vm.At(A(vm, c, 0), u32(text.size()) + 1), text.c_str(), text.size() + 1);
    return Ret(c, u32(text.size()));
}

Outcome Vsprintf(Vm& vm, Context& c) {
    const std::string text = Format(vm, A(vm, c, 1), VaList(vm, A(vm, c, 2)));
    std::memcpy(vm.At(A(vm, c, 0), u32(text.size()) + 1), text.c_str(), text.size() + 1);
    return Ret(c, u32(text.size()));
}

Outcome Printf(Vm& vm, Context& c) {
    const std::string text = Format(vm, A(vm, c, 0), Varargs(vm, c, 1));
    Log(text);
    return Ret(c, u32(text.size()));
}

Outcome Puts(Vm& vm, Context& c) {
    Log(vm.String(A(vm, c, 0)));
    return Ret(c, 0);
}

// ---------------------------------------------------------------- interrupts

Outcome CpuSuspendIntr(Vm& vm, Context& c) {
    if (const u32 state = A(vm, c, 0)) vm.Write(state, 0, 4);
    return Ret(c, KE_OK);
}

Outcome Succeed(Vm&, Context& c) { return Ret(c, KE_OK); }

Outcome QueryIntrContext(Vm& vm, Context& c) { return Ret(c, vm.in_interrupt ? 1 : 0); }

// ---------------------------------------------------------------- SIF

// sceSifSetCmdBuffer(table, entries): where a module's own command handlers are
// listed. The host keeps handlers itself (command_handlers), so only the
// previous table is answered.
Outcome SetCmdBuffer(Vm& vm, Context& c) {
    const u32 previous = vm.handler_table;
    vm.handler_table = A(vm, c, 0);
    return Ret(c, previous);
}

Outcome AddCmdHandler(Vm& vm, Context& c) {
    vm.command_handlers[A(vm, c, 0)] = {A(vm, c, 1), A(vm, c, 2)};
    return Ret(c, KE_OK);
}

Outcome RemoveCmdHandler(Vm& vm, Context& c) {
    vm.command_handlers.erase(A(vm, c, 0));
    return Ret(c, KE_OK);
}

// A command to the EE: `size` bytes from IOP `src` to EE `dest`, then the
// packet - its header written - to the EE's receive buffer.
void SendToEe(Vm& vm, u32 command, u32 packet, u32 psize, u32 src, u32 dest, u32 size) {
    Memory& ee = GuestMemory();
    for (u32 i = 0; i < size; ++i) ee.Write(dest + i, vm.Read(src + i, 1), 1);
    vm.Write(packet, (psize & 0xFF) | (size << 8), 4);
    vm.Write(packet + 4, dest, 4);
    vm.Write(packet + 8, command, 4);
    std::vector<u8> bytes(psize);
    std::memcpy(bytes.data(), vm.At(packet, psize), psize);
    if (vm.trace) {
        std::string words;
        for (u32 i = 16; i + 4 <= psize && i < 48; i += 4) {
            char w[12];
            std::snprintf(w, sizeof w, " %08X", unsigned(vm.Read(packet + i, 4)));
            words += w;
        }
        std::fprintf(stderr, "[iop] to the EE: command %u,%s\n", unsigned(command), words.c_str());
    }
    QueueIopPacket(std::move(bytes));
    // From an interrupt handler (sceSifSendCmd in one: Burnout 3's B3ROUTE
    // sends each capture of the player's engine so, every 256 samples): the
    // EE's handler runs now, as on the console. Its answer - the processed
    // sound, DMA'd back - is there for the handler 128 samples on, not one
    // frame on (the input ring played stale pieces: crackle).
    if (vm.current == 0 && !(command & 0x80000000u)) ps2host::DeliverIopPacketNow();
}

// sceSifSendCmd(command, packet, packet_size, src, dest, size): the packet goes
// to the EE's receive buffer and `size` bytes from IOP `src` to EE `dest`.
Outcome SendCmd(Vm& vm, Context& c) {
    SendToEe(vm, A(vm, c, 0), A(vm, c, 1), A(vm, c, 2), A(vm, c, 3), A(vm, c, 4), A(vm, c, 5));
    return Ret(c, 1);
}

// sceSifSetDma(transfers, count): IOP memory to EE memory, each
// { src, dest, size, attr }; done at once.
Outcome SetDma(Vm& vm, Context& c) {
    const u32 list = A(vm, c, 0), count = A(vm, c, 1);
    Memory& ee = GuestMemory();
    for (u32 k = 0; k < count; ++k) {
        const u32 src = vm.Read(list + 16 * k, 4), dest = vm.Read(list + 16 * k + 4, 4);
        const u32 size = vm.Read(list + 16 * k + 8, 4);
        if (std::getenv("PS2HOST_SIF_LOG"))
            std::fprintf(stderr, "[sif] IOP 0x%08X -> EE 0x%08X, %u bytes: %08X %08X %08X %08X\n",
                         unsigned(src), unsigned(dest), unsigned(size), unsigned(vm.Read(src, 4)),
                         unsigned(vm.Read(src + 4, 4)), unsigned(vm.Read(src + 8, 4)), unsigned(vm.Read(src + 12, 4)));
        for (u32 i = 0; i < size; ++i) ee.Write(dest + i, vm.Read(src + i, 1), 1);
    }
    return Ret(c, 1);
}

// sceSifSetRpcQueue(queue, thread): the thread that serves the queue.
Outcome SetRpcQueue(Vm& vm, Context& c) {
    vm.rpc_queues[A(vm, c, 0)] = A(vm, c, 1);
    return Ret(c, KE_OK);
}

// sceSifRegisterRpc(server, sid, function, buffer, cfunction, cbuffer, queue)
Outcome RegisterRpc(Vm& vm, Context& c) {
    RpcServerEntry e;
    e.server = A(vm, c, 0);
    e.sid = A(vm, c, 1);
    e.function = A(vm, c, 2);
    e.buffer = A(vm, c, 3);
    e.cfunction = A(vm, c, 4);
    e.cbuffer = A(vm, c, 5);
    e.queue = A(vm, c, 6);
    vm.rpc_servers[e.server] = e;
    char text[128];
    std::snprintf(text, sizeof text, "IOP: a module serves RPC 0x%08X (function 0x%08X)",
                  unsigned(e.sid), unsigned(e.function));
    Note("iop rpc " + std::to_string(e.sid), text);
    return Ret(c, KE_OK);
}

// sceSifRpcLoop(queue): the thread serves requests for ever. It waits here;
// each request runs the server's function in it (ServeNext), and returning
// from that sends the answer and waits for the next (iop_vm.cpp).
Outcome RpcLoop(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_CAN_NOT_WAIT));
    t->rpc_queue = A(vm, c, 0);
    vm.rpc_queues[t->rpc_queue] = t->id;
    vm.Block(c, Wait::Rpc, t->rpc_queue);
    return Outcome::Block;          // the scheduler starts serving (ServeNext)
}

// sceSifSetDmaIntr(transfers, count, callback, data): sceSifSetDma, then the
// callback once the transfer is done - as an interrupt, after this returns.
Outcome SetDmaIntr(Vm& vm, Context& c) {
    const u32 callback = A(vm, c, 2), data = A(vm, c, 3);
    SetDma(vm, c);
    if (callback) vm.completions.push_back({callback, data});
    return Ret(c, 1);
}

Outcome DmaStat(Vm&, Context& c) { return Ret(c, u32(-1)); }    // < 0: finished

// ---------------------------------------------------------------- cdvdman
// The disc, read from the user's image. Reads finish at once.

// sceCdRead(lsn, sectors, buffer, mode): mode points at sceCdRMode
// { u8 trycount, spindlctrl, datapattern, pad }; the pattern picks 2048, 2328
// or 2340 bytes a sector.
Outcome CdRead(Vm& vm, Context& c) {
    const u32 lsn = A(vm, c, 0), sectors = A(vm, c, 1), buffer = A(vm, c, 2), mode = A(vm, c, 3);
    const u32 pattern = mode ? *vm.At(mode + 2, 1) : 0;
    std::vector<u8> data(std::size_t(sectors) * Disc::PatternSize(pattern));
    if (!GuestDisc().IsOpen() || !GuestDisc().ReadPattern(lsn, sectors, pattern, data.data()))
        return Ret(c, 0);
    std::memcpy(vm.At(buffer, u32(data.size())), data.data(), data.size());
    return Ret(c, 1);
}

Outcome CdSync(Vm&, Context& c) { return Ret(c, 0); }              // done
Outcome CdGetError(Vm&, Context& c) { return Ret(c, 0); }          // SCECdErNO
Outcome CdGetDiskType(Vm&, Context& c) {                          // SCECdPS2CD, SCECdPS2DVD
    return Ret(c, GuestDisc().IsCd() ? 0x12 : 0x14);
}
Outcome CdDiskReady(Vm&, Context& c) { return Ret(c, 2); }         // SCECdComplete

Outcome FlushDcache(Vm&, Context& c) { return Ret(c, 0); }
// RegisterLibraryEntries(table): a module offers its functions to the modules
// loaded after it (iop_vm.cpp links their imports to them).
Outcome RegisterLibraryEntries(Vm& vm, Context& c) {
    const u32 table = Arg(vm, c, 0);
    const std::string name = vm.String(table + 12, 8);
    vm.exports[name] = table;
    Note("iop exports " + name, "IOP: a module exports the library " + name);
    return Ret(c, 0);
}
Outcome CheckInit(Vm&, Context& c) { return Ret(c, 1); }
Outcome GetSreg(Vm&, Context& c) { return Ret(c, 0); }

// ---------------------------------------------------------------- libsd

struct Sound {
    Spu2 spu;
    struct Handler { u32 function = 0, arg = 0; };
    Handler transfer[2], spu2;
    // The older API (libsd 2.x): a callback per transfer channel and one for
    // the SPU2's interrupt, each called with 0 - when no handler of the newer
    // kind (sceSdSetTransIntrHandler, sceSdSetSpu2IntrHandler) is set.
    u32 transfer_callback[2] = {0, 0}, irq_callback = 0;
    struct Block {
        bool active = false;
        u32 address = 0, size = 0, half = 0;
        u64 period_us = 0, due_us = 0;
    } block[2];
    std::vector<u32> pending;       // channels whose transfer interrupt is due now
    // The IOP's instructions at the last SoundInterrupts: while the IOP
    // catches up, the sound goes no faster than they.
    u64 sound_instructions = 0;
};

Sound& S() {
    static Sound sound;
    return sound;
}

Outcome SdQuit(Vm&, Context& c) { return Ret(c, KE_OK); }

// sceSdInit: libsd leaves every voice of both cores mixed, dry and into the
// effect (VMIXL, VMIXR, VMIXEL, VMIXER all 24 bits; PCSX2's SPU2 resets so).
// A driver that only clears the effect mix and never sets the dry one -
// Mister Mosquito's SNDN2DRV - relies on it: at 0 its narration and music
// voice went nowhere.
Outcome SdInit(Vm&, Context& c) {
    for (u32 core = 0; core < 2; ++core)
        for (u32 which : {0x18u, 0x19u, 0x1Au, 0x1Bu}) S().spu.SetSwitch((which << 8) | core, 0xFFFFFF);
    return Ret(c, KE_OK);
}

Outcome SdSetParam(Vm&, Context& c) { S().spu.SetParam(c.r[4] & 0xFFFF, c.r[5] & 0xFFFF); return Ret(c, 0); }
Outcome SdGetParam(Vm&, Context& c) { return Ret(c, S().spu.GetParam(c.r[4] & 0xFFFF)); }
Outcome SdSetSwitch(Vm&, Context& c) { S().spu.SetSwitch(c.r[4] & 0xFFFF, c.r[5]); return Ret(c, 0); }
Outcome SdGetSwitch(Vm&, Context& c) { return Ret(c, S().spu.GetSwitch(c.r[4] & 0xFFFF)); }
Outcome SdSetAddr(Vm&, Context& c) { S().spu.SetAddr(c.r[4] & 0xFFFF, c.r[5]); return Ret(c, 0); }
Outcome SdGetAddr(Vm&, Context& c) { return Ret(c, S().spu.GetAddr(c.r[4] & 0xFFFF)); }
Outcome SdSetCoreAttr(Vm&, Context& c) { S().spu.SetCoreAttr(c.r[4] & 0xFFFF, c.r[5] & 0xFFFF); return Ret(c, 0); }
Outcome SdGetCoreAttr(Vm&, Context& c) { return Ret(c, S().spu.GetCoreAttr(c.r[4] & 0xFFFF)); }

// Pitch: 0x1000 plays a sample at its recorded rate; a semitone is 2^(1/12).
Outcome SdNote2Pitch(Vm&, Context& c) {
    const double semitones = (double(s32(c.r[6] & 0xFFFF)) - double(c.r[4] & 0xFFFF)) +
                             (double(s16(c.r[7] & 0xFFFF)) - double(c.r[5] & 0xFFFF)) / 128.0;
    const double pitch = 4096.0 * std::pow(2.0, semitones / 12.0);
    return Ret(c, u32(pitch > 0x3FFF ? 0x3FFF : pitch));
}

Outcome SdPitch2Note(Vm&, Context& c) {
    const double pitch = double(c.r[6] & 0xFFFF);
    if (pitch <= 0) return Ret(c, 0);
    const double semitones = 12.0 * std::log2(pitch / 4096.0) + double(c.r[4] & 0xFFFF);
    const s32 note = s32(std::floor(semitones));
    const s32 fine = s32((semitones - note) * 128.0);
    return Ret(c, u32((note << 8) | (fine & 0xFF)));
}

u32 Batch(u32 func, u32 entry, u32 value) {
    Spu2& s = S().spu;
    switch (func) {
        case 0x01: s.SetParam(entry, value & 0xFFFF); return 0;
        case 0x02: s.SetSwitch(entry, value); return 0;
        case 0x03: s.SetAddr(entry, value); return 0;
        case 0x04: s.SetCoreAttr(entry, value & 0xFFFF); return 0;
        case 0x11: return s.GetParam(entry);
        case 0x12: return s.GetSwitch(entry);
        case 0x13: return s.GetAddr(entry);
        case 0x14: return s.GetCoreAttr(entry);
        default: throw Unimplemented{"IOP: sceSdProcBatch function " + std::to_string(func)};
    }
}

Outcome SdProcBatch(Vm& vm, Context& c) {
    const u32 batch = A(vm, c, 0), rets = A(vm, c, 1), count = A(vm, c, 2);
    for (u32 i = 0; i < count; ++i) {
        const u32 head = vm.Read(batch + 8 * i, 4), value = vm.Read(batch + 8 * i + 4, 4);
        const u32 result = Batch(head & 0xFFFF, head >> 16, value);
        if (rets) vm.Write(rets + 4 * i, result, 4);
    }
    return Ret(c, count);
}

Outcome SdProcBatchEx(Vm& vm, Context& c) {
    const u32 batch = A(vm, c, 0), rets = A(vm, c, 1), count = A(vm, c, 2), voices = A(vm, c, 3);
    u32 done = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 head = vm.Read(batch + 8 * i, 4), value = vm.Read(batch + 8 * i + 4, 4);
        for (u32 v = 0; v < 24; ++v) {
            if (!(voices & (1u << v))) continue;
            // The entry's voice field is the low bits of the parameter number.
            const u32 result = Batch(head & 0xFFFF, (head >> 16) | (v << 1), value);
            if (rets) vm.Write(rets + 4 * done, result, 4);
            ++done;
        }
    }
    return Ret(c, done);
}

// sceSdVoiceTrans(channel, mode, iop_address, spu_address, size): into SPU RAM
// at once; the channel's transfer interrupt follows, as the DMA's would.
Outcome SdVoiceTrans(Vm& vm, Context& c) {
    const u32 channel = c.r[4] & 1, mode = c.r[5] & 0xFFFF, iop = A(vm, c, 2), spu = A(vm, c, 3);
    const u32 size = A(vm, c, 4);
    Sound& s = S();
    if ((mode & 3) == 0) {
        if (size) s.spu.Write(spu, vm.At(iop, size), size);
    } else if ((mode & 3) == 1) {
        if (size) s.spu.Read(spu, vm.At(iop, size), size);
    }
    // Only a DMA transfer interrupts when done; SD_TRANS_MODE_IO (8) is the CPU
    // writing through the SPU2's registers, finished when the call returns.
    if (!(mode & 8)) s.pending.push_back(channel);
    // As libsd answers (ps2sdk voice.c): DMA the size, IO-mode writes 0.
    return Ret(c, (mode & 8) ? 0 : size);
}

Outcome SdVoiceTransStatus(Vm&, Context& c) { return Ret(c, 1); }     // complete

// sceSdBlockTrans(channel, mode, iop_address, size, ...): with SD_TRANS_LOOP,
// the hardware streams the buffer's two halves in turn and interrupts after
// each, at the output rate (48 kHz, 16-bit stereo: 192,000 bytes a second).
Outcome SdBlockTrans(Vm& vm, Context& c) {
    const u32 channel = c.r[4] & 1, mode = c.r[5] & 0xFFFF, address = A(vm, c, 2), size = A(vm, c, 3);
    Sound::Block& b = S().block[channel];
    if ((mode & 3) == 2) {                   // SD_TRANS_STOP
        b.active = false;
        S().spu.SetInput(channel, nullptr, 0);
        return Ret(c, 0);
    }
    b.active = true;
    // Written to the core (SD_TRANS_WRITE): its sound-data input plays the
    // buffer, from IOP memory, as the half-buffer interrupts below pace it.
    if ((mode & 3) == 0 && address < kRamSize && size <= kRamSize - address)
        S().spu.SetInput(channel, vm.ram + address, size);
    b.address = address;
    b.size = size;
    b.half = 0;
    const u32 bytes = (mode & 0x10) ? size / 2 : size;
    b.period_us = u64(bytes) * 1000000 / 192000;
    b.due_us = vm.NowUs() + b.period_us;
    return Ret(c, 0);
}

Outcome SdBlockTransStatus(Vm&, Context& c) {
    const u32 channel = c.r[4] & 1;
    const Sound::Block& b = S().block[channel];
    // A buffer the SPU2 plays: the half it is playing.
    const u32 half = S().spu.InputActive(channel) ? S().spu.InputHalf(channel) : b.half;
    return Ret(c, (half << 24) | ((b.address + half * (b.size / 2)) & 0xFFFFFF));
}

Outcome SdStopTrans(Vm&, Context& c) {
    S().block[c.r[4] & 1].active = false;
    S().spu.SetInput(c.r[4] & 1, nullptr, 0);
    return Ret(c, 0);
}

Outcome SdSetTransIntrHandler(Vm&, Context& c) {
    Sound::Handler& h = S().transfer[c.r[4] & 1];
    const u32 previous = h.function;
    h = {c.r[5], c.r[6]};
    return Ret(c, previous);
}

// sceSdSetTransCallback(channel, function), sceSdSetIRQCallback(function):
// the previous one back.
Outcome SdSetTransCallback(Vm&, Context& c) {
    u32& f = S().transfer_callback[c.r[4] & 1];
    const u32 previous = f;
    f = u32(c.r[5]);
    return Ret(c, previous);
}

Outcome SdSetIrqCallback(Vm&, Context& c) {
    u32& f = S().irq_callback;
    const u32 previous = f;
    f = u32(c.r[4]);
    return Ret(c, previous);
}

Outcome SdSetSpu2IntrHandler(Vm&, Context& c) {
    const u32 previous = S().spu2.function;
    S().spu2 = {c.r[4], c.r[5]};
    return Ret(c, previous);
}

Outcome SdGetTransIntrHandlerArgument(Vm&, Context& c) { return Ret(c, S().transfer[c.r[4] & 1].arg); }
Outcome SdGetSpu2IntrHandlerArgument(Vm&, Context& c) { return Ret(c, S().spu2.arg); }
Outcome SdReturnsPrevious(Vm&, Context& c) { return Ret(c, 0); }

// sceSdEffectAttr: int core, int mode, short depth_L, short depth_R, int delay, int feedback.
Outcome SdSetEffectAttr(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 1);
    const s32 depth_left = s32(std::int16_t(vm.Read(p + 8, 2))), depth_right = s32(std::int16_t(vm.Read(p + 10, 2)));
    return Ret(c, u32(S().spu.SetEffectAttr(c.r[4] & 1, vm.Read(p + 4, 4), depth_left, depth_right,
                                            vm.Read(p + 12, 4), vm.Read(p + 16, 4))));
}

Outcome SdGetEffectAttr(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 1);
    u32 mode = 0, delay = 0, feedback = 0;
    s32 depth_left = 0, depth_right = 0;
    S().spu.GetEffectAttr(c.r[4] & 1, &mode, &depth_left, &depth_right, &delay, &feedback);
    vm.Write(p, c.r[4] & 1, 4);
    vm.Write(p + 4, mode, 4);
    vm.Write(p + 8, u32(depth_left) & 0xFFFF, 2);
    vm.Write(p + 10, u32(depth_right) & 0xFFFF, 2);
    vm.Write(p + 12, delay, 4);
    vm.Write(p + 16, feedback, 4);
    return Ret(c, 0);
}

// ---------------------------------------------------------------- thread status

constexpr s32 KE_ILLEGAL_TYPE = -431, KE_ILLEGAL_SIZE = -432;
constexpr u32 THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04, THS_DORMANT = 0x10;

Thread* ThreadOf(Vm& vm, u32 id) {
    if (id == 0) return vm.Current();                // TH_SELF
    auto found = vm.threads.find(id);
    return found == vm.threads.end() ? nullptr : &found->second;
}

// A thread's status, and what it waits on as thbase names it: TSW_SLEEP 1,
// DELAY 2, SEMA 3, EVENTFLAG 4, MBX 5. A wait inside the SIF library (its RPC
// loop, an RPC to the EE) is a sleep, as the library's own is.
void StatusOf(const Vm& vm, const Thread& t, u32* status, u32* type, u32* object) {
    *type = *object = 0;
    if (t.id == vm.current) *status = THS_RUN;
    else if (t.ready) *status = THS_READY;
    else if (t.wait == Wait::Dormant) *status = THS_DORMANT;
    else *status = THS_WAIT;
    if (*status != THS_WAIT) return;
    switch (t.wait) {
        case Wait::Sleep: case Wait::Rpc: case Wait::Client: *type = 1; break;
        case Wait::Delay: *type = 2; break;
        case Wait::Sema: *type = 3; *object = t.object; break;
        case Wait::Event: *type = 4; *object = t.object; break;
        case Wait::Mbx: *type = 5; *object = t.object; break;
        default: break;
    }
}

// ReferThreadStatus(id, info): iop_thread_info_t - attr, option, status,
// entry, stack, stackSize, gpReg, initPriority, currentPriority, waitType,
// waitId, wakeupCount, regContext, reserved[4].
Outcome ReferThreadStatus(Vm& vm, Context& c) {
    Thread* t = ThreadOf(vm, A(vm, c, 0));
    if (!t) return Ret(c, u32(KE_UNKNOWN_THID));
    u32 status, type, object;
    StatusOf(vm, *t, &status, &type, &object);
    const u32 words[17] = {t->attr, t->option, status, t->entry, t->stack, t->stack_size, t->c.r[28],
                           u32(t->initial_priority), u32(t->priority), type, object, u32(t->wakeups)};
    const u32 p = A(vm, c, 1);
    for (u32 i = 0; i < 17; ++i) vm.Write(p + 4 * i, words[i], 4);
    return Ret(c, KE_OK);
}

// ReferThreadRunStatus(id, status, size): iop_thread_run_status_t - status,
// currentPriority, waitType, waitId, wakeupCount, regContext, runClocks (8
// bytes), and three counts the host does not keep; 44 bytes, the rest zeroed.
Outcome ReferThreadRunStatus(Vm& vm, Context& c) {
    if (vm.in_interrupt) return Ret(c, u32(KE_ILLEGAL_CONTEXT));
    Thread* t = ThreadOf(vm, A(vm, c, 0));
    if (!t) return Ret(c, u32(KE_UNKNOWN_THID));
    const u32 p = A(vm, c, 1), size = A(vm, c, 2);
    if (size < 44) return Ret(c, u32(KE_ILLEGAL_SIZE));
    for (u32 i = 0; i < size; ++i) vm.Write(p + i, 0, 1);
    u32 status, type, object;
    StatusOf(vm, *t, &status, &type, &object);
    const u32 words[5] = {status, u32(t->priority), type, object, u32(t->wakeups)};
    for (u32 i = 0; i < 5; ++i) vm.Write(p + 4 * i, words[i], 4);
    return Ret(c, KE_OK);
}

// GetThreadmanIdList(type, buffer, length, &count): the ids of one kind of
// object - TMID_Thread 1, Semaphore 2, EventFlag 3, Mbox 4, Vpl 5, Fpl 6,
// SleepThread 7, DelayThread 8, DormantThread 9 - as many as the buffer holds;
// returns how many it wrote, and sets the count to how many there are.
Outcome GetThreadmanIdList(Vm& vm, Context& c) {
    if (vm.in_interrupt) return Ret(c, u32(KE_ILLEGAL_CONTEXT));
    const u32 type = A(vm, c, 0), buffer = A(vm, c, 1), length = A(vm, c, 2), count = A(vm, c, 3);
    std::vector<u32> ids;
    auto threads = [&](auto keep) { for (const auto& [id, t] : vm.threads) if (keep(t)) ids.push_back(id); };
    switch (type) {
        case 1: threads([](const Thread&) { return true; }); break;
        case 2: for (const auto& entry : vm.semaphores) ids.push_back(entry.first); break;
        case 3: for (const auto& entry : vm.events) ids.push_back(entry.first); break;
        case 4: for (const auto& entry : vm.mailboxes) ids.push_back(entry.first); break;
        case 5: case 6:
            for (const auto& [id, pool] : vm.pools) if (pool.fixed == (type == 6)) ids.push_back(id);
            break;
        case 7: threads([](const Thread& t) { return !t.ready && t.wait == Wait::Sleep; }); break;
        case 8: threads([](const Thread& t) { return !t.ready && t.wait == Wait::Delay; }); break;
        case 9: threads([](const Thread& t) { return !t.ready && t.wait == Wait::Dormant; }); break;
        default: return Ret(c, u32(KE_ILLEGAL_TYPE));
    }
    const u32 written = std::min<u32>(u32(ids.size()), s32(length) > 0 ? length : 0);
    for (u32 i = 0; i < written; ++i) vm.Write(buffer + 4 * i, ids[i], 4);
    if (count) vm.Write(count, u32(ids.size()), 4);
    return Ret(c, written);
}

// ExitDeleteThread(): the thread ends, and is deleted once it has stopped.
Outcome ExitDeleteThread(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t) return Ret(c, u32(KE_ILLEGAL_CONTEXT));
    t->ready = false;
    t->wait = Wait::Dormant;
    t->exit_delete = true;
    return Outcome::Block;
}

// ---------------------------------------------------------------- setjmp

// setjmp(env), longjmp(env, value) (sysclib): $s0-$s7, $sp, $fp and $ra in the
// buffer's first eleven words, as ps2sdk's sysclib keeps them (newlib's MIPS
// layout). longjmp returns from that setjmp again, with `value` (1 for 0).
Outcome Setjmp(Vm& vm, Context& c) {
    const u32 env = A(vm, c, 0);
    for (u32 i = 0; i < 8; ++i) vm.Write(env + 4 * i, c.r[16 + i], 4);
    vm.Write(env + 32, c.r[29], 4);
    vm.Write(env + 36, c.r[30], 4);
    vm.Write(env + 40, c.r[31], 4);
    return Ret(c, 0);
}

Outcome Longjmp(Vm& vm, Context& c) {
    const u32 env = A(vm, c, 0), value = A(vm, c, 1);
    for (u32 i = 0; i < 8; ++i) c.r[16 + i] = vm.Read(env + 4 * i, 4);
    c.r[29] = vm.Read(env + 32, 4);
    c.r[30] = vm.Read(env + 36, 4);
    c.r[31] = vm.Read(env + 40, 4);                  // where the call returns: setjmp's caller
    return Ret(c, value ? value : 1);
}

// ---------------------------------------------------------------- memory pools

constexpr s32 KE_NO_MEMORY = -400, KE_UNKNOWN_VPLID = -411, KE_UNKNOWN_FPLID = -412;
constexpr s32 KE_ILLEGAL_MEMBLOCK = -426, KE_ILLEGAL_MEMSIZE = -427;

Pool* PoolOf(Vm& vm, u32 id, bool fixed) {
    auto found = vm.pools.find(id);
    return found == vm.pools.end() || found->second.fixed != fixed ? nullptr : &found->second;
}

// A piece of a pool, first fit, in units of 8 bytes; 0 if none is free.
u32 PoolTake(Pool& pool, u32 size) {
    size = (size + 7) & ~7u;
    for (auto it = pool.free_blocks.begin(); it != pool.free_blocks.end(); ++it) {
        if (it->second < size) continue;
        const u32 address = it->first, left = it->second - size;
        pool.free_blocks.erase(it);
        if (left) pool.free_blocks[address + size] = left;
        pool.used_blocks[address] = size;
        return address;
    }
    return 0;
}

bool PoolGive(Pool& pool, u32 address) {
    auto used = pool.used_blocks.find(address);
    if (used == pool.used_blocks.end()) return false;
    u32 start = address, size = used->second;
    pool.used_blocks.erase(used);
    auto next = pool.free_blocks.find(start + size);
    if (next != pool.free_blocks.end()) { size += next->second; pool.free_blocks.erase(next); }
    auto before = pool.free_blocks.lower_bound(start);
    if (before != pool.free_blocks.begin() && (--before)->first + before->second == start) {
        start = before->first;
        size += before->second;
        pool.free_blocks.erase(before);
    }
    pool.free_blocks[start] = size;
    return true;
}

u32 NewPool(Vm& vm, Pool pool) {
    pool.base = vm.Allocate(pool.size);
    if (!pool.base) return u32(KE_NO_MEMORY);
    pool.free_blocks[pool.base] = pool.size;
    const u32 id = vm.next_id++;
    vm.pools[id] = std::move(pool);
    return id;
}

// CreateVpl(param): iop_vpl_param - attr, option, size.
Outcome CreateVpl(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 0);
    const s32 size = s32(vm.Read(p + 8, 4));
    if (size <= 0) return Ret(c, u32(KE_ILLEGAL_MEMSIZE));
    Pool pool;
    pool.attr = vm.Read(p, 4);
    pool.option = vm.Read(p + 4, 4);
    pool.size = (u32(size) + 7) & ~7u;
    return Ret(c, NewPool(vm, std::move(pool)));
}

// CreateFpl(param): iop_fpl_param - attr, option, block_size, blocks.
Outcome CreateFpl(Vm& vm, Context& c) {
    const u32 p = A(vm, c, 0);
    const s32 block_size = s32(vm.Read(p + 8, 4)), blocks = s32(vm.Read(p + 12, 4));
    if (block_size <= 0 || blocks <= 0) return Ret(c, u32(KE_ILLEGAL_MEMSIZE));
    Pool pool;
    pool.fixed = true;
    pool.attr = vm.Read(p, 4);
    pool.option = vm.Read(p + 4, 4);
    pool.block_size = (u32(block_size) + 7) & ~7u;
    pool.blocks = u32(blocks);
    pool.size = pool.block_size * pool.blocks;
    return Ret(c, NewPool(vm, std::move(pool)));
}

Outcome DeletePool(Vm& vm, Context& c, bool fixed) {
    Pool* pool = PoolOf(vm, A(vm, c, 0), fixed);
    if (!pool) return Ret(c, u32(fixed ? KE_UNKNOWN_FPLID : KE_UNKNOWN_VPLID));
    vm.Free(pool->base);
    vm.pools.erase(A(vm, c, 0));
    return Ret(c, KE_OK);
}

Outcome DeleteVpl(Vm& vm, Context& c) { return DeletePool(vm, c, false); }
Outcome DeleteFpl(Vm& vm, Context& c) { return DeletePool(vm, c, true); }

// pAllocateVpl(id, size): a piece, or KE_NO_MEMORY without waiting.
Outcome PollAllocateVpl(Vm& vm, Context& c) {
    Pool* pool = PoolOf(vm, A(vm, c, 0), false);
    if (!pool) return Ret(c, u32(KE_UNKNOWN_VPLID));
    const s32 size = s32(A(vm, c, 1));
    if (size <= 0 || u32(size) > pool->size) return Ret(c, u32(KE_ILLEGAL_MEMSIZE));
    const u32 address = PoolTake(*pool, u32(size));
    return Ret(c, address ? address : u32(KE_NO_MEMORY));
}

Outcome FreeVpl(Vm& vm, Context& c) {
    Pool* pool = PoolOf(vm, A(vm, c, 0), false);
    if (!pool) return Ret(c, u32(KE_UNKNOWN_VPLID));
    return Ret(c, PoolGive(*pool, A(vm, c, 1)) ? u32(KE_OK) : u32(KE_ILLEGAL_MEMBLOCK));
}

// ---------------------------------------------------------------- modload, loadcore

constexpr s32 KE_UNKNOWN_MODULE = -202, KE_ILLEGAL_FLAG = -220, KE_LIBRARY_NOTFOUND = -213;

// SearchModuleByName(name): the latest module of that name loaded, by its id.
// A name no module recorded has - a BIOS module's (rom0:), whose image the
// host does not have - is answered 1 and noted, as DirtySock's search was
// before modules were recorded (iop_inet.cpp): KE_UNKNOWN_MODULE would tell
// a module that one it needs is missing when it may be the BIOS's.
Outcome SearchModuleByName(Vm& vm, Context& c) {
    const std::string name = vm.String(A(vm, c, 0), 56);
    s32 found = 0;
    for (const auto& [id, module] : vm.modules)
        if (module.name == name && id > found) found = id;
    if (found) return Ret(c, u32(found));
    Note("iop module search " + name, "IOP: SearchModuleByName(\"" + name +
         "\"): no module loaded by that name is recorded; answered 1");
    return Ret(c, 1);
}

// ReferModuleStatus(id, status): ModuleStatus - name (56 bytes), version and
// flags (16 bits each), id, entry, gp, text address and size, data and BSS
// sizes, two reserved words.
Outcome ReferModuleStatus(Vm& vm, Context& c) {
    const auto found = vm.modules.find(s32(A(vm, c, 0)));
    if (found == vm.modules.end()) return Ret(c, u32(KE_UNKNOWN_MODULE));
    const ModuleRecord& m = found->second;
    const u32 p = A(vm, c, 1);
    for (u32 i = 0; i < 56; ++i) vm.Write(p + i, i < 55 && i < m.name.size() ? u8(m.name[i]) : 0, 1);
    vm.Write(p + 56, m.version, 2);
    vm.Write(p + 58, m.flags, 2);
    const u32 words[9] = {u32(found->first), m.entry, m.gp, m.text, m.text_size, m.data_size, m.bss_size, 0, 0};
    for (u32 i = 0; i < 9; ++i) vm.Write(p + 60 + 4 * i, words[i], 4);
    return Ret(c, KE_OK);
}

// SetModuleFlags(id, flag): only 0x40 (the module may be unloaded) is one.
Outcome SetModuleFlags(Vm& vm, Context& c) {
    if (vm.in_interrupt) return Ret(c, u32(KE_ILLEGAL_CONTEXT));
    const u32 flag = A(vm, c, 1);
    if (flag & ~0x40u) return Ret(c, u32(KE_ILLEGAL_FLAG));
    const auto found = vm.modules.find(s32(A(vm, c, 0)));
    if (found == vm.modules.end()) return Ret(c, u32(KE_UNKNOWN_MODULE));
    found->second.flags = u16(found->second.flags | flag);
    return Ret(c, KE_OK);
}

// ReleaseLibraryEntries(table): what RegisterLibraryEntries offered is no more.
Outcome ReleaseLibraryEntries(Vm& vm, Context& c) {
    const u32 table = A(vm, c, 0);
    const auto found = vm.exports.find(vm.String(table + 12, 8));
    if (found == vm.exports.end() || found->second != table) return Ret(c, u32(KE_LIBRARY_NOTFOUND));
    vm.exports.erase(found);
    return Ret(c, KE_OK);
}

// ---------------------------------------------------------------- cdvdman: files, status

// sceCdSearchFile(file, path): sceCdlFILE { lsn, size, name[16], date[8] } for
// a file on the disc ("\\DATA\\X.BIN;1"); 1 if it is there, else 0. The date
// as cdvdman gives it: the ISO record's flags, then seconds, minutes, hours,
// day, month, and the year in two bytes.
Outcome CdSearchFile(Vm& vm, Context& c) {
    const u32 out = A(vm, c, 0);
    const std::string path = vm.String(A(vm, c, 1), 256);
    Disc::File file;
    if (!GuestDisc().IsOpen() || !GuestDisc().Find(path, &file)) return Ret(c, 0);
    vm.Write(out, file.lsn, 4);
    vm.Write(out + 4, file.size, 4);
    const std::string name = path.substr(path.find_last_of("\\/") + 1);
    for (u32 i = 0; i < 16; ++i) vm.Write(out + 8 + i, i < 15 && i < name.size() ? u8(name[i]) : 0, 1);
    const u32 year = 1900 + file.date[0];
    const u8 date[8] = {file.flags, file.date[5], file.date[4], file.date[3], file.date[2], file.date[1],
                        u8(year), u8(year >> 8)};
    for (u32 i = 0; i < 8; ++i) vm.Write(out + 24 + i, date[i], 1);
    return Ret(c, 1);
}

Outcome CdSeek(Vm&, Context& c) { return Ret(c, GuestDisc().IsOpen() ? 1 : 0); }

// sceCdStatus(): SCECdStatPause - spinning, its reads done - with a disc in
// (reads finish at once here); SCECdStatShellOpen without one.
Outcome CdStatus(Vm&, Context& c) { return Ret(c, GuestDisc().IsOpen() ? 0x0A : 0x01); }

// ---------------------------------------------------------------- ioman
// The IOP's file system for modules, on the host: "cdrom0:" is the disc,
// read-only; there is no other device, as on a console without a development
// host ("host0:") - its calls fail as ioman's do, with the error negated.

constexpr s32 kENOENT = 2, kEBADF = 9, kENODEV = 19, kENOTDIR = 20, kEISDIR = 21, kEINVAL = 22;
constexpr s32 kEMFILE = 24, kEROFS = 30;
constexpr u32 kModeFile = 0x14, kModeDirectory = 0x25;     // FIO_SO_IFREG|IROTH, IFDIR|IROTH|IXOTH

// A path's device and the path on it: "cdrom0:\\A\\B.BIN;1" -> ("cdrom", "\\A\\B.BIN;1"),
// a path with no device taken in the current directory (chdir).
std::pair<std::string, std::string> Device(Vm& vm, std::string path) {
    if (path.find(':') == std::string::npos && !vm.current_directory.empty())
        path = vm.current_directory + (path.empty() || path[0] == '\\' || path[0] == '/' ? "" : "\\") + path;
    const std::size_t colon = path.find(':');
    if (colon == std::string::npos) return {"", path};
    std::string device = path.substr(0, colon);
    while (!device.empty() && std::isdigit(static_cast<unsigned char>(device.back()))) device.pop_back();
    for (char& ch : device) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return {device, path.substr(colon + 1)};
}

bool OnDisc(const std::string& device) { return device == "cdrom" && GuestDisc().IsOpen(); }

s32 NewDescriptor(Vm& vm, IoFile file) {
    for (s32 fd = 1; fd < 64; ++fd)
        if (!vm.files.count(fd)) {
            vm.files[fd] = std::move(file);
            return fd;
        }
    return -kEMFILE;
}

// open(path, flags, mode): flags 1 read, 2 write, 3 both, 0x200 create...
Outcome IoOpen(Vm& vm, Context& c) {
    const auto [device, path] = Device(vm, vm.String(A(vm, c, 0), 1024));
    if (!OnDisc(device)) return Ret(c, u32(-kENODEV));
    if (A(vm, c, 1) & 0x0F02u) return Ret(c, u32(-kEROFS));    // write, append, create, truncate, exclusive
    IoFile file;
    if (!GuestDisc().Find(path, &file.file)) return Ret(c, u32(-kENOENT));
    if (file.file.flags & 0x02) return Ret(c, u32(-kEISDIR));
    return Ret(c, u32(NewDescriptor(vm, std::move(file))));
}

// close(fd), dclose(fd)
Outcome IoClose(Vm& vm, Context& c) {
    return Ret(c, vm.files.erase(s32(A(vm, c, 0))) ? u32(KE_OK) : u32(-kEBADF));
}

IoFile* FileOf(Vm& vm, Context& c, bool directory) {
    auto found = vm.files.find(s32(A(vm, c, 0)));
    return found == vm.files.end() || found->second.directory != directory ? nullptr : &found->second;
}

// read(fd, buffer, size): from the position on, as much as the file holds.
Outcome IoRead(Vm& vm, Context& c) {
    IoFile* f = FileOf(vm, c, false);
    if (!f) return Ret(c, u32(-kEBADF));
    const u32 buffer = A(vm, c, 1);
    const s32 asked = s32(A(vm, c, 2));
    if (asked < 0) return Ret(c, u32(-kEINVAL));
    const u32 size = std::min(u32(asked), f->file.size - std::min(f->position, f->file.size));
    if (!size) return Ret(c, 0);
    const u32 first = f->position / kSectorSize;
    const u32 sectors = (f->position + size + kSectorSize - 1) / kSectorSize - first;
    std::vector<u8> data(std::size_t(sectors) * kSectorSize);
    if (!GuestDisc().Read(f->file.lsn + first, sectors, data.data())) return Ret(c, u32(-kEINVAL));
    std::memcpy(vm.At(buffer, size), &data[f->position % kSectorSize], size);
    f->position += size;
    return Ret(c, size);
}

// write(fd, buffer, size): the disc is read-only.
Outcome IoWrite(Vm& vm, Context& c) {
    return Ret(c, FileOf(vm, c, false) ? u32(-kEROFS) : u32(-kEBADF));
}

// lseek(fd, offset, whence): from the start (0), the position (1), the end (2).
Outcome IoLseek(Vm& vm, Context& c) {
    IoFile* f = FileOf(vm, c, false);
    if (!f) return Ret(c, u32(-kEBADF));
    const s64 offset = s32(A(vm, c, 1));
    const u32 whence = A(vm, c, 2);
    if (whence > 2) return Ret(c, u32(-kEINVAL));
    const s64 at = offset + (whence == 1 ? s64(f->position) : whence == 2 ? s64(f->file.size) : 0);
    if (at < 0) return Ret(c, u32(-kEINVAL));
    f->position = u32(at);
    return Ret(c, f->position);
}

// remove, mkdir, rmdir (path): the disc's are read-only, no other device is.
Outcome IoChange(Vm& vm, Context& c) {
    return Ret(c, OnDisc(Device(vm, vm.String(A(vm, c, 0), 1024)).first) ? u32(-kEROFS) : u32(-kENODEV));
}

// dopen(path): a directory on the disc, its entries for dread.
Outcome IoDopen(Vm& vm, Context& c) {
    const auto [device, path] = Device(vm, vm.String(A(vm, c, 0), 1024));
    if (!OnDisc(device)) return Ret(c, u32(-kENODEV));
    IoFile dir;
    dir.directory = true;
    if (!GuestDisc().Find(path, &dir.file)) return Ret(c, u32(-kENOENT));
    if (!(dir.file.flags & 0x02) || !GuestDisc().List(path, &dir.entries)) return Ret(c, u32(-kENOTDIR));
    return Ret(c, u32(NewDescriptor(vm, std::move(dir))));
}

// io_stat_t: mode, attr, size, ctime, atime, mtime (8 bytes each: as
// sceCdSearchFile's date), hisize.
void PutStat(Vm& vm, u32 p, const Disc::File& file) {
    const u32 year = 1900 + file.date[0];
    const u8 time[8] = {0, file.date[5], file.date[4], file.date[3], file.date[2], file.date[1], u8(year), u8(year >> 8)};
    vm.Write(p, file.flags & 0x02 ? kModeDirectory : kModeFile, 4);
    vm.Write(p + 4, 0, 4);
    vm.Write(p + 8, file.size, 4);
    for (u32 t = 0; t < 3; ++t)
        for (u32 i = 0; i < 8; ++i) vm.Write(p + 12 + 8 * t + i, time[i], 1);
    vm.Write(p + 36, 0, 4);
}

// dread(fd, entry): io_dirent_t - its io_stat_t, the name (256 bytes), a
// private word; the name's length, or 0 past the last.
Outcome IoDread(Vm& vm, Context& c) {
    IoFile* dir = FileOf(vm, c, true);
    if (!dir) return Ret(c, u32(-kEBADF));
    if (dir->next >= dir->entries.size()) return Ret(c, 0);
    const Disc::Entry& entry = dir->entries[dir->next++];
    const u32 p = A(vm, c, 1);
    PutStat(vm, p, entry.file);
    for (u32 i = 0; i < 256; ++i) vm.Write(p + 40 + i, i < 255 && i < entry.name.size() ? u8(entry.name[i]) : 0, 1);
    vm.Write(p + 296, 0, 4);
    return Ret(c, u32(entry.name.size()));
}

// getstat(path, stat)
Outcome IoGetstat(Vm& vm, Context& c) {
    const auto [device, path] = Device(vm, vm.String(A(vm, c, 0), 1024));
    if (!OnDisc(device)) return Ret(c, u32(-kENODEV));
    Disc::File file;
    if (!GuestDisc().Find(path, &file)) return Ret(c, u32(-kENOENT));
    PutStat(vm, A(vm, c, 1), file);
    return Ret(c, KE_OK);
}

// chdir(path): where a path with no device is looked for - a directory on the
// disc.
Outcome IoChdir(Vm& vm, Context& c) {
    const std::string asked = vm.String(A(vm, c, 0), 1024);
    const auto [device, path] = Device(vm, asked);
    if (!OnDisc(device)) return Ret(c, u32(-kENODEV));
    Disc::File file;
    if (!GuestDisc().Find(path, &file)) return Ret(c, u32(-kENOENT));
    if (!(file.flags & 0x02)) return Ret(c, u32(-kENOTDIR));
    vm.current_directory = asked.find(':') == std::string::npos ? "cdrom0:" + path : asked;
    return Ret(c, KE_OK);
}

// ---------------------------------------------------------------- SIF RPC: to the EE, and request by request

constexpr u32 kRpcBindCommand = 0x80000009u, kRpcCallCommand = 0x8000000Au, kRpcPacketSize = 0x40;
constexpr u32 SIF_RPC_M_NOWAIT = 0x01;
constexpr s32 E_SIF_PKT_ALLOC = -0xD001;

// An RPC packet to the EE, the client's own while its RPC is in flight:
// rec_id, pkt_addr, rpc_id and client after the command header, then the
// command's fields (`fields`, from offset 32).
u32 RpcPacket(Vm& vm, u32 client, std::initializer_list<u32> fields) {
    static u32 rpc_id = 0;
    const u32 packet = vm.Allocate(kRpcPacketSize);
    if (!packet) return 0;
    for (u32 i = 0; i < kRpcPacketSize; i += 4) vm.Write(packet + i, 0, 4);
    vm.Write(packet + 20, packet, 4);
    vm.Write(packet + 24, ++rpc_id, 4);
    vm.Write(packet + 28, client, 4);
    u32 at = 32;
    for (u32 field : fields) { vm.Write(packet + at, field, 4); at += 4; }
    vm.Write(client, packet, 4);                    // hdr.pkt_addr: in flight
    vm.Write(client + 4, rpc_id, 4);                // hdr.rpc_id
    return packet;
}

// The client's RPC goes to the EE; the calling thread waits for its end
// (iop::RpcEnd) unless the mode says not to.
Outcome Send(Vm& vm, Context& c, u32 client, u32 command, RpcClient call, u32 mode,
             u32 src, u32 dest, u32 size) {
    const bool wait = !(mode & SIF_RPC_M_NOWAIT);
    if (wait && (!vm.Current() || vm.in_interrupt)) {
        vm.Free(call.packet);
        vm.Write(client, 0, 4);                     // hdr.pkt_addr: nothing in flight after all
        return Ret(c, u32(KE_CAN_NOT_WAIT));
    }
    vm.Write(client + 12, mode, 4);                 // hdr.mode
    call.thread = wait ? vm.current : 0;
    vm.rpc_clients[client] = call;
    SendToEe(vm, command, call.packet, kRpcPacketSize, src, dest, size);
    Return(c, 0);
    if (!wait) return Outcome::Return;
    vm.Block(c, Wait::Client, client);
    return Outcome::Block;
}

// sceSifBindRpc(client, sid, mode): a server of the EE's, by its id; its
// address and buffers are the client's once the EE answers (server 0: the EE
// has no such server yet).
Outcome BindRpc(Vm& vm, Context& c) {
    const u32 client = A(vm, c, 0), sid = A(vm, c, 1), mode = A(vm, c, 2);
    vm.Write(client + 16, 0, 4);                    // command
    vm.Write(client + 36, 0, 4);                    // server
    RpcClient call;
    call.bind = true;
    call.packet = RpcPacket(vm, client, {sid});
    if (!call.packet) return Ret(c, u32(E_SIF_PKT_ALLOC));
    return Send(vm, c, client, kRpcBindCommand, call, mode, 0, 0, 0);
}

// sceSifCallRpc(client, procedure, mode, send, send_size, receive,
// receive_size, end_function, end_param): `send` goes to the server's buffer
// on the EE, the server's answer comes back to `receive`; the end function
// runs once it has (as an interrupt, the SIF library's way). A call that waits
// for nothing - no wait, no end function - asks for no end (rmode 0).
Outcome CallRpc(Vm& vm, Context& c) {
    const u32 client = A(vm, c, 0), procedure = A(vm, c, 1), mode = A(vm, c, 2);
    const u32 send = A(vm, c, 3), send_size = A(vm, c, 4), receive = A(vm, c, 5), receive_size = A(vm, c, 6);
    RpcClient call;
    call.end_function = A(vm, c, 7);
    call.end_param = A(vm, c, 8);
    vm.Write(client + 28, call.end_function, 4);
    vm.Write(client + 32, call.end_param, 4);
    const u32 server = vm.Read(client + 36, 4), buffer = vm.Read(client + 20, 4);
    const u32 rmode = (mode & SIF_RPC_M_NOWAIT) && !call.end_function ? 0 : 1;
    call.packet = RpcPacket(vm, client, {procedure, send_size, receive, receive_size, rmode, server});
    if (!call.packet) return Ret(c, u32(E_SIF_PKT_ALLOC));
    if (!rmode) {
        // Nothing waits for its end, and the EE sends none.
        SendToEe(vm, kRpcCallCommand, call.packet, kRpcPacketSize, send, buffer, send_size);
        vm.Free(call.packet);
        vm.Write(client, 0, 4);
        return Ret(c, 0);
    }
    return Send(vm, c, client, kRpcCallCommand, call, mode, send, buffer, send_size);
}

// sceSifCheckStatRpc(client): 1 while its RPC is in flight, else 0.
Outcome CheckStatRpc(Vm& vm, Context& c) { return Ret(c, vm.rpc_clients.count(A(vm, c, 0)) ? 1 : 0); }

// sceSifGetNextRequest(queue): the next request waiting in a queue, taken
// from it for sceSifExecRequest: its server, the request's fields written into
// the server's data (size, client, pkt_addr, rpc_number, receive, rsize,
// rmode, rid); 0 if none waits.
Outcome GetNextRequest(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    auto& pending = vm.rpc_pending[A(vm, c, 0)];
    if (!t || pending.empty()) return Ret(c, 0);
    t->serving = pending.front();
    pending.pop_front();
    const RpcRequest& r = t->serving;
    auto word = [&](std::size_t at) { u32 v = 0; if (at + 4 <= r.packet.size()) std::memcpy(&v, &r.packet[at], 4); return v; };
    vm.Write(r.server + 12, r.size, 4);
    vm.Write(r.server + 28, word(28), 4);
    vm.Write(r.server + 32, word(20), 4);
    vm.Write(r.server + 36, r.procedure, 4);
    vm.Write(r.server + 40, r.receive, 4);
    vm.Write(r.server + 44, r.receive_size, 4);
    vm.Write(r.server + 48, word(48), 4);
    vm.Write(r.server + 52, word(16), 4);
    return Ret(c, r.server);
}

// sceSifExecRequest(server): the server's function - function(rpc_number,
// buffer, size) - on the request sceSifGetNextRequest took, in this thread;
// then its answer goes back and the call ends (iop_vm.cpp, at kExecReturn),
// and this returns.
Outcome ExecRequest(Vm& vm, Context& c) {
    Thread* t = vm.Current();
    if (!t || !t->serving.server || t->serving.server != A(vm, c, 0)) return Ret(c, 0);
    const RpcServerEntry& entry = vm.rpc_servers[t->serving.server];
    t->exec_return = c.r[31];
    c.r[4] = t->serving.procedure;
    c.r[5] = entry.buffer;
    c.r[6] = t->serving.size;
    vm.call_into = entry.function;
    return Outcome::Return;
}

// sceSifRemoveRpc(server, queue): the server serves no more; returns it.
Outcome RemoveRpc(Vm& vm, Context& c) {
    const u32 server = A(vm, c, 0);
    vm.rpc_servers.erase(server);
    return Ret(c, server);
}

// sceSifRemoveRpcQueue(queue): no more requests come to the queue; returns it.
Outcome RemoveRpcQueue(Vm& vm, Context& c) {
    const u32 queue = A(vm, c, 0);
    vm.rpc_queues.erase(queue);
    vm.rpc_pending.erase(queue);
    return Ret(c, queue);
}

const Map& Table() {
    static const Map table = {
        {{"sysmem", 4}, AllocSysMemory}, {{"sysmem", 5}, FreeSysMemory},
        {{"sysmem", 6}, QueryMemSize}, {{"sysmem", 7}, QueryMaxFreeMemSize},
        {{"sysmem", 8}, QueryMaxFreeMemSize},
        {{"sysmem", 14}, Printf},                                    // Kprintf
        {{"stdio", 4}, Printf}, {{"stdio", 7}, Puts},
        {{"sysclib", 7}, Tolower}, {{"sysclib", 6}, Toupper},
        {{"sysclib", 11}, Memcmp}, {{"sysclib", 12}, Memcpy}, {{"sysclib", 13}, Memcpy},
        {{"sysclib", 14}, Memset}, {{"sysclib", 15}, Memcmp}, {{"sysclib", 16}, Bcopy},
        {{"sysclib", 17}, Bzero}, {{"sysclib", 19}, Sprintf}, {{"sysclib", 20}, Strcat},
        {{"sysclib", 21}, Strchr}, {{"sysclib", 22}, Strcmp}, {{"sysclib", 23}, Strcpy},
        {{"sysclib", 27}, Strlen}, {{"sysclib", 29}, Strncmp}, {{"sysclib", 30}, Strncpy},
        {{"sysclib", 32}, Strrchr}, {{"sysclib", 34}, Strstr}, {{"sysclib", 36}, Strtol},
        {{"sysclib", 38}, Strtoul}, {{"sysclib", 42}, Vsprintf},
        {{"intrman", 4}, Succeed}, {{"intrman", 5}, Succeed}, {{"intrman", 6}, Succeed},
        {{"intrman", 7}, Succeed}, {{"intrman", 8}, Succeed}, {{"intrman", 9}, Succeed},
        {{"intrman", 17}, CpuSuspendIntr}, {{"intrman", 18}, Succeed},
        {{"intrman", 23}, QueryIntrContext},
        {{"thbase", 4}, CreateThread}, {{"thbase", 5}, DeleteThread},
        {{"thbase", 6}, StartThread}, {{"thbase", 7}, StartThreadArgs},
        {{"thbase", 8}, ExitThread}, {{"thbase", 10}, TerminateThread},
        {{"thbase", 11}, TerminateThread}, {{"thbase", 14}, ChangeThreadPriority},
        {{"thbase", 15}, ChangeThreadPriority}, {{"thbase", 16}, RotateThreadReadyQueue},
        {{"thbase", 18}, ReleaseWaitThread}, {{"thbase", 19}, ReleaseWaitThread},
        {{"thbase", 20}, GetThreadId}, {{"thbase", 24}, SleepThread},
        {{"thbase", 25}, WakeupThread}, {{"thbase", 26}, WakeupThread},
        {{"thbase", 27}, CancelWakeupThread}, {{"thbase", 28}, CancelWakeupThread},
        {{"thbase", 33}, DelayThread}, {{"thbase", 34}, GetSystemTime},
        {{"thbase", 35}, SetAlarm}, {{"thbase", 36}, SetAlarm},
        {{"thbase", 37}, CancelAlarm}, {{"thbase", 38}, CancelAlarm},
        {{"thbase", 39}, USec2SysClock}, {{"thbase", 40}, SysClock2USec},
        {{"thbase", 42}, GetThreadCurrentPriority}, {{"thbase", 43}, GetSystemTimeLow},
        {{"thsemap", 4}, CreateSema}, {{"thsemap", 5}, DeleteSema},
        {{"thsemap", 6}, SignalSema}, {{"thsemap", 7}, SignalSema},
        {{"thsemap", 8}, WaitSema}, {{"thsemap", 9}, PollSema},
        {{"thsemap", 11}, ReferSemaStatus}, {{"thsemap", 12}, ReferSemaStatus},
        {{"thevent", 4}, CreateEventFlag}, {{"thevent", 5}, DeleteEventFlag},
        {{"thevent", 6}, SetEventFlag}, {{"thevent", 7}, SetEventFlag},
        {{"thevent", 8}, ClearEventFlag}, {{"thevent", 9}, ClearEventFlag},
        {{"thevent", 10}, WaitEventFlag}, {{"thevent", 11}, PollEventFlag},
        {{"thevent", 13}, ReferEventFlagStatus}, {{"thevent", 14}, ReferEventFlagStatus},
        {{"sifman", 5}, Succeed}, {{"sifman", 7}, SetDma}, {{"sifman", 8}, DmaStat},
        {{"sifman", 29}, CheckInit},
        {{"sifcmd", 4}, Succeed}, {{"sifcmd", 5}, Succeed}, {{"sifcmd", 6}, GetSreg},
        {{"sifcmd", 7}, Succeed}, {{"sifcmd", 8}, SetCmdBuffer}, {{"sifcmd", 9}, Succeed},
        {{"sifcmd", 10}, AddCmdHandler}, {{"sifcmd", 11}, RemoveCmdHandler},
        {{"sifcmd", 12}, SendCmd}, {{"sifcmd", 13}, SendCmd}, {{"sifcmd", 14}, Succeed},
        {{"sifcmd", 17}, RegisterRpc}, {{"sifcmd", 19}, SetRpcQueue}, {{"sifcmd", 22}, RpcLoop},
        {{"sifman", 32}, SetDmaIntr},
        {{"cdvdman", 4}, Succeed}, {{"cdvdman", 6}, CdRead}, {{"cdvdman", 8}, CdGetError},
        {{"cdvdman", 11}, CdSync}, {{"cdvdman", 12}, CdGetDiskType}, {{"cdvdman", 13}, CdDiskReady},
        {{"loadcore", 5}, FlushDcache}, {{"loadcore", 6}, RegisterLibraryEntries},
        {{"thmsgbx", 4}, CreateMbx}, {{"thmsgbx", 5}, DeleteMbx},
        {{"thmsgbx", 6}, SendMbx}, {{"thmsgbx", 7}, SendMbx},
        {{"thmsgbx", 8}, ReceiveMbx}, {{"thmsgbx", 9}, PollMbx},
        {{"timrman", 4}, AllocHardTimer}, {{"timrman", 6}, FreeHardTimer},
        {{"timrman", 20}, SetTimerHandler}, {{"timrman", 22}, SetupHardTimer},
        {{"timrman", 23}, StartHardTimer}, {{"timrman", 24}, StopHardTimer},
        {{"sifcmd", 23}, SifGetOtherData},
        {{"sysclib", 8}, LookCtypeTable},
        {{"vblank", 4}, WaitVblank}, {{"vblank", 5}, WaitVblank},
        {{"libsd", 2}, SdQuit}, {{"libsd", 4}, SdInit},
        {{"libsd", 5}, SdSetParam}, {{"libsd", 6}, SdGetParam},
        {{"libsd", 7}, SdSetSwitch}, {{"libsd", 8}, SdGetSwitch},
        {{"libsd", 9}, SdSetAddr}, {{"libsd", 10}, SdGetAddr},
        {{"libsd", 11}, SdSetCoreAttr}, {{"libsd", 12}, SdGetCoreAttr},
        {{"libsd", 13}, SdNote2Pitch}, {{"libsd", 14}, SdPitch2Note},
        {{"libsd", 15}, SdProcBatch}, {{"libsd", 16}, SdProcBatchEx},
        {{"libsd", 17}, SdVoiceTrans}, {{"libsd", 18}, SdBlockTrans},
        {{"libsd", 19}, SdVoiceTransStatus}, {{"libsd", 20}, SdBlockTransStatus},
        {{"libsd", 21}, SdSetTransCallback}, {{"libsd", 22}, SdSetIrqCallback},
        {{"libsd", 23}, SdSetEffectAttr}, {{"libsd", 24}, SdGetEffectAttr},
        {{"libsd", 25}, Succeed}, {{"libsd", 26}, SdSetTransIntrHandler},
        {{"libsd", 27}, SdSetSpu2IntrHandler}, {{"libsd", 28}, SdGetTransIntrHandlerArgument},
        {{"libsd", 29}, SdGetSpu2IntrHandlerArgument}, {{"libsd", 30}, SdStopTrans},
        {{"libsd", 31}, Succeed}, {{"libsd", 32}, Succeed}, {{"libsd", 33}, Succeed},
        {{"sysclib", 4}, Setjmp}, {{"sysclib", 5}, Longjmp},
        {{"thbase", 9}, ExitDeleteThread}, {{"thbase", 22}, ReferThreadStatus},
        {{"thbase", 45}, ReferThreadRunStatus}, {{"thbase", 47}, GetThreadmanIdList},
        {{"thvpool", 4}, CreateVpl}, {{"thvpool", 5}, DeleteVpl},
        {{"thvpool", 7}, PollAllocateVpl}, {{"thvpool", 9}, FreeVpl},
        {{"thfpool", 4}, CreateFpl}, {{"thfpool", 5}, DeleteFpl},
        {{"timrman", 10}, GetTimerCounter},
        {{"modload", 17}, ReferModuleStatus}, {{"modload", 22}, SearchModuleByName},
        {{"modload", 30}, SetModuleFlags}, {{"loadcore", 7}, ReleaseLibraryEntries},
        {{"cdvdman", 7}, CdSeek}, {{"cdvdman", 10}, CdSearchFile}, {{"cdvdman", 28}, CdStatus},
        // ioman (1.4, the export numbers iomanX keeps for the old library's name)
        {{"ioman", 4}, IoOpen}, {{"ioman", 5}, IoClose}, {{"ioman", 6}, IoRead},
        {{"ioman", 7}, IoWrite}, {{"ioman", 8}, IoLseek}, {{"ioman", 10}, IoChange},
        {{"ioman", 11}, IoChange}, {{"ioman", 12}, IoChange}, {{"ioman", 13}, IoDopen},
        {{"ioman", 14}, IoClose}, {{"ioman", 15}, IoDread}, {{"ioman", 16}, IoGetstat},
        {{"ioman", 26}, IoChdir},
        {{"sifcmd", 15}, BindRpc}, {{"sifcmd", 16}, CallRpc}, {{"sifcmd", 18}, CheckStatRpc},
        {{"sifcmd", 20}, GetNextRequest}, {{"sifcmd", 21}, ExecRequest},
        {{"sifcmd", 24}, RemoveRpc}, {{"sifcmd", 25}, RemoveRpcQueue},
    };
    return table;
}

std::map<std::pair<std::string, u32>, std::string> LoadNames() {
    std::map<std::pair<std::string, u32>, std::string> names;
    // engine/data/iop_exports.csv, built in (engine/host/CMakeLists.txt).
    static const char kExports[] =
#include "iop_exports.inc"
        ;
    std::istringstream in(kExports);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#' || line.rfind("library,", 0) == 0) continue;
        std::stringstream fields(line);
        std::string library, index, name;
        std::getline(fields, library, ',');
        std::getline(fields, index, ',');
        std::getline(fields, name, ',');
        names[{library, u32(std::stoul(index))}] = name;
    }
    return names;
}

}  // namespace

Library FindLibrary(const std::string& library, u32 index) {
    const auto found = Table().find({library, index});
    if (found != Table().end()) return found->second;
    const auto network = NetworkLibraries().find({library, index});
    return network == NetworkLibraries().end() ? Library() : network->second;
}

std::string ExportName(const std::string& library, u32 index) {
    static const auto names = LoadNames();
    const auto found = names.find({library, index});
    return found == names.end() ? "?" : found->second;
}

void SetPcmInputVolume(u32 core, u32 percent) { S().spu.SetInputVolume(core, percent); }

void EnableSound() {
    S().spu.SetOutput([](const Spu2::s16* frames, u32 count) { AudioFrames(frames, count); });
}

// Due sound interrupts: a voice transfer's completion, a block transfer's
// half-buffer. Called by the interpreter's scheduler (iop_vm.cpp).
bool SoundInterrupts(Vm& vm, bool idle) {
    Sound& s = S();
    std::vector<u32> due;
    due.swap(s.pending);
    const u64 now = vm.NowUs();
    for (u32 ch = 0; ch < 2; ++ch) {
        Sound::Block& b = s.block[ch];
        // A buffer the SPU2 plays interrupts as it plays it (below).
        if (s.spu.InputActive(ch)) continue;
        if (b.active && b.period_us && now >= b.due_us) {
            b.half ^= 1;
            b.due_us += b.period_us;
            if (b.due_us < now) b.due_us = now + b.period_us;   // fell behind: don't burst
            due.push_back(ch);
        }
    }
    for (u32 ch : due) {
        if (s.transfer[ch].function) vm.Call(s.transfer[ch].function, {ch, s.transfer[ch].arg});
        else if (s.transfer_callback[ch]) vm.Call(s.transfer_callback[ch], {0});
    }
    // The SPU2 runs beside the IOP, and a stream's decoder (CRI's ADX) on the
    // IOP keeps a half of the buffer ahead of it. The IOP here runs in bursts
    // (between the EE's frames, or as it idles): through a burst the sound
    // goes on only as fast as the IOP's work (36.864 MHz, an instruction a
    // cycle), never more than 50 ms behind; once the IOP has nothing to do,
    // to the present. Each half the input begins stops it, its interrupt
    // delivered, so the decoder runs before the SPU2 reads on. Only while a
    // stream plays (a block transfer's input): otherwise to the present, the
    // sound's interrupts when the IOP's clock gives them (Burnout 3's
    // audio-paced movies are recorded so).
    const u64 executed = vm.instructions - s.sound_instructions;
    s.sound_instructions = vm.instructions;
    const u64 played = s.spu.PlayedUs();
    const bool streaming = s.spu.InputActive(0) || s.spu.InputActive(1);
    u64 to = idle || !played || !streaming ? now : std::min(now, played + executed * 1000 / 36864);
    if (to + 50'000 < now) to = now - 50'000;
    // The SPU2's own interrupt: playback (or a transfer) reached IRQA.
    Timed timed(Section::Spu2);
    s.spu.Advance(to, [&](u32 core) {
        if (s.spu2.function) vm.Call(s.spu2.function, {1u << core, s.spu2.arg});
        else if (s.irq_callback) vm.Call(s.irq_callback, {0});
    }, [&](u32 core) {
        Sound::Block& b = s.block[core];
        b.half = s.spu.InputHalf(core);
        if (s.transfer[core].function) vm.Call(s.transfer[core].function, {core, s.transfer[core].arg});
        else if (s.transfer_callback[core]) vm.Call(s.transfer_callback[core], {0});
    });
    return s.spu.PlayedUs() + 21 < now;     // a sample (20.8 us) or more to play
}

std::string Format(Vm& vm, u32 format, const std::function<u32()>& next) {
    const std::string f = vm.String(format, 4096);
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') {
            out += f[i];
            continue;
        }
        std::string spec = "%";
        ++i;
        while (i < f.size() && std::strchr("-+ #0", f[i])) spec += f[i++];
        if (i < f.size() && f[i] == '*') { spec += std::to_string(s32(next())); ++i; }
        while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) spec += f[i++];
        if (i < f.size() && f[i] == '.') {
            spec += f[i++];
            if (i < f.size() && f[i] == '*') { spec += std::to_string(s32(next())); ++i; }
            while (i < f.size() && std::isdigit(static_cast<unsigned char>(f[i]))) spec += f[i++];
        }
        while (i < f.size() && std::strchr("hlLqjzt", f[i])) ++i;  // all words on the IOP
        if (i >= f.size()) break;
        const char conversion = f[i];
        char buffer[512];
        switch (conversion) {
            case 'd': case 'i':
                std::snprintf(buffer, sizeof buffer, (spec + "d").c_str(), s32(next())); break;
            case 'u': case 'x': case 'X': case 'o':
                std::snprintf(buffer, sizeof buffer, (spec + conversion).c_str(), unsigned(next())); break;
            case 'p':
                std::snprintf(buffer, sizeof buffer, "0x%08X", unsigned(next())); break;
            case 'c':
                std::snprintf(buffer, sizeof buffer, (spec + "c").c_str(), int(next() & 0xFF)); break;
            case 's':
                std::snprintf(buffer, sizeof buffer, (spec + "s").c_str(), vm.String(next(), 400).c_str()); break;
            case '%': std::snprintf(buffer, sizeof buffer, "%%"); break;
            default: std::snprintf(buffer, sizeof buffer, "%%%c", conversion); break;
        }
        out += buffer;
    }
    return out;
}

}  // namespace iop
}  // namespace ps2host
