/*
 * Host implementations of Sony SDK library routines, applied by name.
 *
 * Library routines that drive the hardware - wait for the graphics paths, poll
 * a DMA channel - are better replaced whole than emulated register by register:
 * the routine's contract is small, and on the host the hardware it waits for is
 * already done. A game's analysis names such routines (config/names.csv, from
 * diagnostic strings, the cross-game signature library and the game's own
 * renames.csv); every routine named here is overridden wherever the game's
 * dispatch table gives that name, so a new game gets these for free once its
 * routines are named. A name here has a public source, or is descriptive
 * (docs/FORK.md).
 */

#include <cstring>

#include "ps2host/host.h"
#include "ps2host/run.h"

namespace ps2host {

namespace {

void Return(lifted::Context& c, u64 value) { c.r[2] = u64(s64(s32(u32(value)))); }

// gs_sync_path(mode, timeout): the graphics library's wait until the paths to
// the GS are idle - VIF1, GIF and VU1 - returning 0, or a negative value on
// timeout. The host's transfers complete when they are started, so the paths
// are always idle. Named by what it does: no public source names the routine
// (upstream's key is the SDK's own name; docs/FORK.md).
void GsSyncPath(lifted::Context& c) { Return(c, 0); }

struct SdkRoutine {
    const char* name;
    GuestFunction function;
};

const SdkRoutine kRoutines[] = {
    {"gs_sync_path", GsSyncPath},
};

}  // namespace

void ApplySdkOverrides(const FunctionEntry* table, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!table[i].name) continue;
        for (const SdkRoutine& routine : kRoutines) {
            if (std::strcmp(table[i].name, routine.name) == 0) {
                Override(table[i].address, routine.function, routine.name);
                Note(std::string("sdk ") + routine.name,
                     std::string("SDK routine ") + routine.name + " runs on the host");
            }
        }
    }
}

}  // namespace ps2host
