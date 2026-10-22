# VU1 compiled code: the next steps

What the compiled VU programs (tools/vurecomp.py, host/include/ps2host/vu_core.h)
still spend, and the plan to take it down, every step exact.

## Why

A high frame rate draws every picture with VU1: at 144 a second Burnout 3's
crash-breaker explosion takes VU1 8-16 ms for a picture's worth of work
against the 6.94 ms a picture has, and VIF1's worker (VU1 is ~75-80% of it)
is the bound there, with the GS thread close behind (games/burnout3/docs/
PERFORMANCE_PLAN.md, "High frame rates with the modern graphics and a texture
pack").

## Where a pair's time goes

Sampled (`--vu-fast --sample --vu-bench`, the explosion's 20,000 runs,
`vu_crash.bin` = `--vu-snapshot 20060 20000` of the crash run): ~1.73 ns a
pair. Of the compiled code's samples, by VuCore step:

| | share |
| --- | --- |
| operand clamps (ClampLanes) | ~9% |
| stall checks (Wait and its max) | ~11% |
| MAC flags worked out (MacFlagsOf) | ~8% |
| flags landed and queued (LandFlags, QueueFlags, FinishUpper's queue) | ~8% |
| a pair's end: landing times, pending writes (Finish, FinishUpper) | ~8% |
| the block's own lines (count, cycle, exits) | ~7% |
| the arithmetic itself (Fmac, LanesSse, Result) | ~10% |

In a hot block of a program that reads MAC (program 88's 0x108, a 10-pair
loop body), MacFlagsOf 18%, LandFlags 17%, stall checks 17%, clamps 10%,
arithmetic 13%: the flags handed from block to block through the queue, and
the stall checks at a block's start, cost more than the work.

## The steps

Each is checked by VuCompiledIsTheInterpreter (every register, flag,
pipeline and cycle; the corpus also tried at 600 programs, seed 7), the VU
bench's two hashes (exact and fast), and the GPU regressions. Speed: two
`--no-pgo` builds side by side on `--vu-bench vu_crash.bin` (an edit to
vu_core.h changes every block, which would lose its PGO profile and skew a
comparison with a profiled build).

1. **Landing times only where they can be read** (done). A write's time
   (vf_ready_, four u64 stores) is read only by a stall check within the
   three pairs after it, or after the block leaves within two pairs of it.
   The rest are left out (`untimed_writes`; `PS2_VU_ALL_TIMES=1` keeps them
   all). A held lower write other than a load (MOVE, MR32, MFIR, COP2) lands
   only where the value changes, so it does not count as overwriting an
   earlier time. Vu::SameState compares times as far as they can still
   stall (max with the cycle). Bench: fast 60.8 -> 57.6 ms (-5%), exact
   82.6 -> 80.8 ms (-2%), both hashes the same.
2. **Flags across a known edge.** A block that goes on into a known block
   (PS2HOST_VU_NEXT) queues its in-flight flags and the next lands them in a
   loop. A version of the next block for entry from that edge can take them
   as they are - their cycles known relative to the exit - and land them
   straight-line where it reads MAC.
3. **Stall checks across a known edge** (done, before 2: the larger share).
   An exit that goes on into a known block, three pairs or more into its
   own, goes into an *entry version* of it (vurecomp's Chain, up to
   MAX_VERSIONS a block; `PS2_VU_NO_VERSIONS=1` for none): one compiled
   knowing which fields were written in the last three pairs before it and
   how many pairs before the exit (`recent`). A field written d pairs before
   lands by the exit pair's issue + 4 - d; the version's pair i issues at
   that issue + 1 + i at the earliest - no stall where d + i >= 3; anything
   else landed before it. In the explosion's hottest block all fifteen
   checks were in its first three pairs. Burnout 3's generated files 58 ->
   131 (the build ~2 -> ~4 minutes). Bench: fast 58.7 -> 54.2 ms (-8%),
   exact 81.1 -> 74.2 ms (-8.5%), hashes the same; with step 1, the
   explosion's VU1 time -11%.
4. **Clean operands across an edge.** Fields an FMAC left clean before the
   edge need no clamp after it.
5. **Superblocks for loops.** A loop body chained into itself (most of the
   hot code) compiled as one function with the loop kept, so the steps above
   hold from one iteration to the next.

Steps 2-4 need an entry version per known edge: code grows; only edges a
recorded run took (PS2HOST_VU_BY_BLOCK counts) are worth one.
