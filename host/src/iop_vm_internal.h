/*
 * The IOP interpreter's state, shared by the interpreter (iop_vm.cpp) and the
 * host's implementations of the IOP libraries (iop_libs.cpp).
 */

#ifndef PS2HOST_IOP_VM_INTERNAL_H
#define PS2HOST_IOP_VM_INTERNAL_H

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ps2host/disc.h"
#include "ps2host/host.h"

namespace ps2host {
namespace iop {

constexpr u32 kRamSize = 2u << 20;
// Where host code returns to from a call into the IOP, and where a thread's
// entry returns to (ExitThread): addresses no code occupies.
constexpr u32 kReturnFromCall = 0x1FFFFFF0u;
constexpr u32 kThreadExit = 0x1FFFFFE0u;
// Where an RPC server's function returns to, in its server thread.
constexpr u32 kRpcReturn = 0x1FFFFFD0u;
// Where guest code a library function runs in the calling thread returns to
// (sceSifExecRequest's server function): the library call ends there.
constexpr u32 kExecReturn = 0x1FFFFFC0u;

struct Context {
    u32 r[32] = {};
    u32 hi = 0, lo = 0;
    u32 pc = 0, npc = 4;
};

// Client: waiting for the end of an RPC it made to the EE (sceSifBindRpc,
// sceSifCallRpc).
enum class Wait { None, Sema, Event, Sleep, Delay, Dormant, Rpc, Mbx, Client };

struct RpcRequest {
    u32 server = 0, procedure = 0, size = 0, receive = 0, receive_size = 0;
    std::vector<u8> packet;
};

struct RpcServerEntry {
    u32 sid = 0, server = 0, function = 0, buffer = 0, cfunction = 0, cbuffer = 0, queue = 0;
};

struct Thread {
    u32 id = 0;
    Context c;
    s32 priority = 0;
    u64 queued = 0;                 // when it became ready, for FIFO among equals
    bool ready = false;
    Wait wait = Wait::Dormant;
    u32 object = 0;                 // the semaphore or event flag waited on
    u32 event_bits = 0, event_mode = 0, event_result = 0;
    s32 wakeups = 0;
    u64 until_us = 0;               // DelayThread
    u32 entry = 0, stack = 0, stack_size = 0;
    u32 attr = 0, option = 0;
    s32 initial_priority = 0;
    RpcRequest serving;             // the RPC request its server function is running
    u32 rpc_queue = 0;              // the queue it serves (sceSifRpcLoop)
    u32 exec_return = 0;            // where sceSifExecRequest returns to, once its function has
    bool exit_delete = false;       // ExitDeleteThread: deleted once it stops running
};

// An RPC a module made to the EE, until the EE's end of it: what it waits in.
struct RpcClient {
    u32 thread = 0;                 // the thread that waits for it; 0: none (no wait)
    u32 packet = 0;                 // the packet it was sent in (the client's own)
    u32 end_function = 0, end_param = 0;
    bool bind = false;
};

// A memory pool (thvpool, thfpool): a block of the IOP's heap, given out in
// pieces - any size for a variable-length pool, one size for a fixed one.
struct Pool {
    bool fixed = false;
    u32 attr = 0, option = 0;
    u32 base = 0, size = 0;
    u32 block_size = 0, blocks = 0;          // a fixed pool's
    std::map<u32, u32> free_blocks;           // address -> size
    std::map<u32, u32> used_blocks;
};

// A module the IOP has loaded, as modload reports it (ReferModuleStatus): from
// its .iopmod section, and where it lies if the host runs it.
struct ModuleRecord {
    std::string name;
    u16 version = 0, flags = 0;
    u32 entry = 0, gp = 0, text = 0, text_size = 0, data_size = 0, bss_size = 0;
};

// A file a module opened through ioman: on the disc, read-only; or a
// directory there (dopen), its entries given one by one (dread).
struct IoFile {
    Disc::File file;
    u32 position = 0;
    bool directory = false;
    std::vector<Disc::Entry> entries;
    std::size_t next = 0;
};

struct Semaphore {
    s32 count = 0, max = 0;
    u32 attr = 0;
    std::deque<u32> waiting;        // thread ids
};

struct EventFlag {
    u32 bits = 0, attr = 0;
};

// A message box (thmsgbx): messages are the guest's own structures, linked
// through their first word; kept here in arrival order.
struct MessageBox {
    u32 attr = 0;
    std::deque<u32> messages;
    std::deque<std::pair<u32, u32>> waiting;     // (thread, where its message goes)
};

// A hardware timer a module took (timrman): its handler runs as an interrupt
// every `compare` counts of its clock, and returns the next compare value (0:
// the timer stops).
struct HardTimer {
    u32 source = 1, prescale = 1, compare = 0, handler = 0, common = 0;
    u32 bits = 32;                  // its counter's width
    bool running = false;
    u64 due_us = 0, started_us = 0;
    u32 count = 0;                  // its counter when it stopped
};

/* A hardware timer's clock, before its prescaler; the time between its
   interrupts. */
double HardTimerHz(const HardTimer& t);
u64 HardTimerPeriodUs(const HardTimer& t);

struct Alarm {
    u64 due_us = 0;
    u32 handler = 0, arg = 0;
};

// What a library function did to the calling thread. Retry: it has blocked
// (Vm::Block) and makes the same call again, same arguments, when the thread
// wakes - a host call that waits on something outside the IOP (a socket)
// without holding the EE up.
enum class Outcome { Return, Block, Yield, Retry };

struct Vm;
using Library = std::function<Outcome(Vm& vm, Context& c)>;
using LibraryTable = std::map<std::pair<std::string, u32>, Library>;

/* The IOP network libraries (inet, netcnf, inetctl) on the host's sockets,
   iop_inet.cpp; FindLibrary looks there after its own table. */
const LibraryTable& NetworkLibraries();

struct Vm {
    u8* ram = nullptr;                          // the 2 MB, shared with the EE's view
    std::map<u32, std::pair<std::string, u32>> stubs;  // stub address -> (library, export)
    std::vector<Library> libraries;              // by id, patched into the stubs
    // Libraries the modules export (loadcore's RegisterLibraryEntries): name ->
    // its export table (magic, next, version, mode, the name in 8 bytes, then
    // the functions' addresses, ending in 0). A module importing a library the
    // host does not implement links to these.
    std::map<std::string, u32> exports;
    std::map<u32, Thread> threads;
    u32 current = 0;                             // the running thread, 0 in a handler
    bool in_interrupt = false;
    bool reschedule = false;
    u64 counter = 0;
    u32 next_id = 1;
    std::map<u32, Semaphore> semaphores;
    std::map<u32, EventFlag> events;
    std::map<u32, Alarm> alarms;
    std::map<u32, MessageBox> mailboxes;
    std::map<u32, HardTimer> hard_timers;
    std::vector<std::pair<u32, u32>> completions;  // (function, data): DMA-done callbacks due
    std::map<u32, std::pair<u32, u32>> command_handlers;   // command -> (function, data)
    std::map<u32, RpcServerEntry> rpc_servers;            // server address -> entry
    std::map<u32, u32> rpc_queues;                        // queue address -> thread
    std::map<u32, std::deque<RpcRequest>> rpc_pending;    // queue address -> requests
    std::map<u32, RpcClient> rpc_clients;                 // client data -> its RPC to the EE in flight
    std::set<u32> awaited_servers;                        // AwaitServer's: a module will register them
    u32 call_into = 0;                           // guest code a library function runs next (ExecRequest)
    std::map<u32, Pool> pools;                   // vpl and fpl ids -> their pools
    std::map<s32, ModuleRecord> modules;         // loadfile's module ids -> what each is
    std::map<s32, IoFile> files;                 // ioman descriptors -> their files
    std::string current_directory;               // ioman's chdir
    u32 command_buffer = 0;                      // where arriving packets are put (the host's)
    u32 handler_table = 0;                       // sceSifSetCmdBuffer's, for its answer
    u64 instructions = 0;
    bool trace = false;
    std::map<u32, std::pair<u64, u32>> calls;    // with trace: target -> (count, a caller)
    u32 executing = 0;                           // the instruction being run, for messages

    // Memory, by IOP address (any segment; the RAM mirrors every 2 MB).
    // Inline, each size its own copy: the interpreter reads every instruction
    // through Read, and a call with memcpy's own of a size it did not know
    // was a tenth of the EE thread's time in gameplay.
    u8* At(u32 address, u32 size) {
        const u32 physical = address & 0x1FFFFFFFu;
        if (physical < 0x00800000u) {                       // RAM and its mirrors
            const u32 offset = physical & (kRamSize - 1);
            if (offset + size <= kRamSize) return ram + offset;
        }
        return nullptr;
    }
    u32 Read(u32 address, u32 size) {
        const u8* p = At(address, size);
        if (!p) ReadFault(address);
        if (size == 4) { u32 value; std::memcpy(&value, p, 4); return value; }
        if (size == 2) { u16 value; std::memcpy(&value, p, 2); return value; }
        if (size == 1) return *p;
        u32 value = 0;
        std::memcpy(&value, p, size);
        return value;
    }
    void Write(u32 address, u32 value, u32 size) {
        u8* p = At(address, size);
        if (!p) WriteFault(address, value);
        if (size == 4) std::memcpy(p, &value, 4);
        else if (size == 2) { const u16 half = u16(value); std::memcpy(p, &half, 2); }
        else if (size == 1) *p = u8(value);
        else std::memcpy(p, &value, size);
    }
    [[noreturn]] void ReadFault(u32 address);
    [[noreturn]] void WriteFault(u32 address, u32 value);
    std::string String(u32 address, u32 limit = 1024);
    std::string Where();

    // The IOP's heap (sysmem): first fit over free RAM.
    u32 Allocate(u32 size);
    void Free(u32 address);
    u32 LargestFree() const;
    std::map<u32, u32> free_blocks{{0x00010000u, 0x00170000u}};   // address -> size
    std::map<u32, u32> used_blocks;

    // Calls an IOP function to completion in interrupt context (handlers,
    // module entry points); `args` go in $a0-$a3.
    u32 Call(u32 function, std::vector<u32> args);
    /* Runs `count` instructions of `c` (for tests). */
    void Step(Context& c, u64 count);

    Thread* Current() { return current ? &threads[current] : nullptr; }
    void MakeReady(Thread& t);
    // The calling thread waits; false (and the call fails) in interrupt context.
    bool Block(Context& c, Wait why, u32 object);
    u64 NowUs() const;
};

Vm& TheVm();

/* The i-th argument of a call: $a0-$a3, then the stack. */
u32 Arg(Vm& vm, const Context& c, int i);
inline void Return(Context& c, u32 value) { c.r[2] = value; }

/* The library function for (library, export), or null. */
Library FindLibrary(const std::string& library, u32 index);
/* Its documented name, for messages. */
std::string ExportName(const std::string& library, u32 index);

/* Starts the next waiting request of a server thread in sceSifRpcLoop. */
void ServeNext(Vm& vm, Thread& t);

/* Due sound-hardware interrupts (iop_libs.cpp), run by the scheduler. */
// Due sound interrupts, the SPU2 run on; `idle`: the IOP has nothing to run,
// the sound may go on to the present. True while the sound is behind it.
bool SoundInterrupts(Vm& vm, bool idle = false);

/* printf-style formatting from IOP memory: `next` yields each argument word. */
std::string Format(Vm& vm, u32 format, const std::function<u32()>& next);

}  // namespace iop
}  // namespace ps2host

#endif  /* PS2HOST_IOP_VM_INTERNAL_H */
