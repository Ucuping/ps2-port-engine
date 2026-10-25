# This fork and upstream

This is the `macos-arm64` branch of a fork of
[ps2-port-engine](https://github.com/danielgomesvieira2000/ps2-port-engine),
used by the PS2 Native Port Builder: macOS on Apple silicon first, then other
platforms, with the Windows build kept working. What differs from upstream,
and why.

## Policy of this fork

Function names come from analysis of the game's own code. A name has a public
source - ps2sdk, PCSX2 or open documentation - or it is descriptive, with the
analysis behind it written down; nothing is named from the SDK's own headers,
symbols or libraries.

- **The override of the graphics library's path wait is keyed `gs_sync_path`**
  (`host/src/sdk.cpp`); upstream's key is the SDK's own name, which no public
  source gives. A game gets the override by naming its routine
  `gs_sync_path`, with the evidence, in its `config/renames.csv`. A game
  repository made for upstream (Burnout 3's names it by the SDK name) gets no
  override from this fork until it does.
- **Seven names are gone from the signature library**
  (`signatures/library.json`): `sceDbcGetDepNumber`, `sceDbcSRData`,
  `sceDbcSendData`, `sceDbcSendData2`, `sceGsExecStoreImage`, `sceGsSyncPath`
  and `EnableIntr` - assigned by hand in Burnout 3 with no evidence recorded and
  no public source. 103 signatures remain.

## Fixes and additions meant for upstream

None of these changes what the Windows build does; the Windows CI
(`.github/workflows/windows.yml`: MSVC, AVX2 on and off, a port linked from a
stub dispatch table, every test) checks that it still builds and passes.

Six are on branches of their own for upstream, each from `upstream/master`
(6226b59) with the one fix and a test that fails without it, pushed to this
fork only: `pr/osd-config-syscalls` (services 0x4A and 0x4B),
`pr/iop-export-names` (ioman's, iomanx's and stdio's names),
`pr/lift-without-compiler`, `pr/elf-segment-overlap` (the ELF loader's
zeroed tails, with `LoadExecutable` for the test),
`pr/ee-refer-sema-status` (ReferSemaStatus, CreateSema's attr and option)
and `pr/likely-slot-liveness` (the structurer's liveness, below).
The two in the host cannot be built on macOS from upstream's tree; they
were checked on MSVC by the throwaway branches `ci-check/*`
(`.github/workflows/pr-check.yml` there): upstream's host tests with the
fix, all passing, and without it, where only the new checks failed (the
ELF loader's one, ReferSemaStatus's five).

- Kernel services 0x4A and 0x4B were swapped in `data/ee_syscalls.csv` (and so
  in the signature library), against ps2sdk, PCSX2 and the host's own
  `kernel.cpp`.
- `config/renames.csv`: a game's decided names, applied after every other
  source by `tools/signatures.py name`; `names.csv` written as real CSV.
- Building with clang outside Windows: no fused multiply-add for compilers
  other than MSVC; `comdlg32` and the RC language only on Windows; the C
  runtime calls that differ by platform in `host/src/crt.h`; the SSE2 store
  path in `gs_memory.cpp` only where there is SSE2; a `macos-arm64` CMake
  preset in the game template.
- The VUs' float mode through `runtime/include/ps2rt/fp_mode.h`: on x86 the
  same MXCSR values as before, on arm64 FPCR (round toward zero, flush to
  zero). Tests pin the modes - `runtime/tests/fp_mode_tests.cpp`, run by
  `ps2rt_tests` and `ps2host_tests`.
- The VUs' four lanes at once in NEON on arm64 (`PS2RT_VU_NEON`), wherever
  x86 has SSE: the FMAC lanes (`vu_math.h`'s `LanesNeon`, the same
  single-precision operations on the same cleaned operands, a multiply and
  an add never fused), macro mode's inline COP2 (`lifted.h`'s `MacroFmac`,
  FPCR set through `fp_mode.h`) and compiled microprograms (`vu_core.h`:
  the FMACs, OPMULA/OPMSUB, ITOF/FTOI/ABS, DIV/SQRT, the flags, the held
  writes). arm64 ran the scalar model for all of them, through a call and
  a decode each: in Tekken 5's fight two thirds of VU1's time went there,
  and half the EE's to macro mode's COP2. A VU1 bench of 3,000 of the
  fight's runs (`--vu-bench`): 9.26 -> 1.83 ns a pair, the state hash the
  same; an inline COP2 46 -> 39 ns, most of what is left the two FPCR
  writes (about 12 ns each on an M1 Pro). The fight's logic, by its round
  timer, went from half speed to 58-60 frames a second. `VuLanesAreTheModel`
  now runs in both float modes, and `MacroFmacIsTheModel` compares the
  inline COP2 with the model; both, and `VuCompiledIsTheInterpreter`, fail
  on a fused multiply-add, a wrong shuffle or wrong flag weights.
- The GPU bridge's texture blocks reach as far as the format's pages do
  (`gs::Memory::BlocksEnd`): a texture's blocks were counted by its bytes
  and a page more, which falls short wherever a level is shorter than its
  format's page and wider than two of them - it takes the top of every page
  across. Tekken 5's menu font, a 512 x 64 PSMT4 texture over memory its
  opening movie had used, spans four pages where the count made three; the
  GPU decoded the glyphs of the fourth from stale blocks and the menus'
  small text came out garbled under `--gpu` (right on the software GS).
  `GsBlocksEndReachesEveryPixel` checks the end against every pixel's
  address for each format; the old count stops short of the last block in
  453 of its 1,365 cases. The software GS's own estimates
  (`gs_draw.cpp`'s texture, frame and depth ends) count the same way and
  are left as they are for now.
- A DMA channel's MADR or TADR with bit 31 (SPR) set reads the scratchpad
  (`dma.cpp`, `Source`), as a tag's ADDR already did: it read kseg0's view
  of main RAM instead. Tekken 5 sends its boot-time Starblade from the
  scratchpad (PATH3 with MADR 0x80000050, chains at TADR 0x80001260): the
  GS got empty tags and the screen stayed black while its sound played.
  The EE's DMA documentation and PCSX2 both take bit 31 as the scratchpad.
  `DmaChainsAreWalked` covers both registers and fails on the old code.
- The run log outside Windows (`platform/run_log.cpp`, `main.cpp`): a run
  started with arguments copies its stdout and stderr, as they are
  written, into `<executable>.log` beside the executable (`<name>.<pid>.log`
  while another run holds that one; `--log FILE` names it) while they still
  go where they went - a player's window has no terminal, and a terminal's
  output is gone once it is cleared. A run without arguments wrote its log
  already. Whatever ends a run says so there: the host's report, the exit
  code, a crash (the report written to the log directly, since stderr's
  pipe dies with the process), an exception nothing caught, `exit()` from a
  library, SIGHUP, SIGINT, SIGTERM and SIGQUIT (`NoteEndingSignals`, which
  also ignores SIGPIPE: a closed pipe is a failed write, not a silent end).
  A run the window ended says what closed it - the app's Quit (Cmd+Q) or
  the window's close (its button, Cmd+W), a picture it could not show, the
  game's own EXIT GAME - and the window's life goes to the log as it
  happens (focus, minimize, hide, fullscreen, the drawable's and the
  swapchain's sizes); a failed Vulkan call is said once. The platform
  tests check the log, a held log, and a crash's report in it, once.
  A run's log also says, at its start, every PS2HOST_* and PS2RT_*
  variable in its environment (a tool's PS2RT_EE_ROUND_NEAREST had gone
  unseen), and the display mode each time it changes (`[gs] display:` -
  size, interlaced or progressive, field or frame, the buffer's format;
  not the buffer shown, which a game flips every picture).
- padman's results where libpad reads them (ps2sdk's `libpad.c`): +0x14
  for SET_MMODE, SET_ACTDIR and SET_ACTALIGN, +0x10 for SET_BTNINFO (each
  had what the request left there). And off by default, on for a game with
  `SetPadmanDualShock2`: the pad a DualShock 2 - the model (3), its two
  actuators and their combination in the pad's area, as ps2sdk's padman
  keeps the pad's replies there (the values the controller protocol's
  public descriptions give: the small motor on or off, the large one
  0-255); GET_BTNMASK answering the 18 pressure-sensitive buttons,
  SET_BTNINFO turning pressure mode (0x79, the report's twelve pressure
  bytes) on and off, and SET_ACTDIR driving the host's motors through the
  bytes SET_ACTALIGN gave them - padman's games had no vibration.
  `PadmanIsADualShock2` calls the server through SIF DMA as the EE does.
- The GPU bridge samples a texture from the target drawn at the texture's
  own address, in either of the formats it could be (PSMCT32, PSMCT24),
  before one the address lies inside (`Containing`). It tried the first
  format's containing image before the second format's exact one:
  Starblade, the game Tekken 5 offers while it loads, draws its scene into a
  PSMCT24 target and copies each field out of it as a PSMCT32 texture, where
  a PSMCT32 image had held the address since a clear at boot - its old rows
  came out as yellow and black bars, the halves apart at the middle. Under
  `--gpu`, Tekken 5's `boot_to_title` changes over Starblade's frames only.
  `GpuTexturesTakeTheTargetDrawnThere` (a Vulkan device's, skipped without
  one) fails without it.
- On Apple's systems the Vulkan window's drawable is in the screen's own
  pixels (`SDL_WINDOW_HIGH_PIXEL_DENSITY`): a Retina screen's fullscreen
  picture was drawn at half its resolution and scaled up by the compositor
  (1512 x 949 of 3024 x 1898 on a 14-inch MacBook Pro). The game's pointer
  keeps its size in points.
- The compiled checks build outside Windows: `cppcheck.py`, `liftcheck.py`
  and `equivalence.py` compile their libraries with clang++ (`$CXX`) at
  `-O3 -ffp-contract=off`, as a Release port is compiled; MSVC's path is
  unchanged. `derive.py` marks a derivation `verified` only if its emitted
  C++ was compiled and run against the original - without a compiler it is
  `reconstructed` - and `lift.py` without a compiler emits its functions
  marked NOT VERIFIED instead of stopping. A structured form the equivalence
  check could not build is left out of it, not the check; and a function
  with no verification of its own keeps its plain form unless the check ran.
- A derived function's result is modelled, compared and emitted as 32 bits,
  so `derive.py` refuses a derivation whose original ever returns more - a
  soft-float double or a long long in the whole of `$v0` - on its own test
  cases or on probes with doubles, 64-bit integers and large magnitudes
  (`testplan.wide_result`); the lifted copy, verified on the whole state,
  stands. Modelling 64-bit results instead is proposed in Tekken 5's
  FINDINGS.md (engine finding 7).
- The structured form declares a branch's kept test, then sets it
  (`bool t; t = C;`): a goto into the block may pass that in standard C++,
  where `const bool t = C` - which MSVC accepts and clang refuses - it may
  not.
- A platform layer, `host/src/platform` (library `ps2platform`, tests
  `ps2platform_tests`): fibers, waits, sleeps, thread priorities, guarded
  reads and the crash report behind one face - Windows' calls as before,
  Apple's (macOS, iOS) beside them - and `threads.cpp` on it. On Apple's
  arm64 the fibers are a switch of our own with stacks mapped under a guard
  of max(16 KB, a page). The float control register goes with each fiber
  on Windows as on arm64. On x64, Windows' own fibers - made without
  `FIBER_FLAG_FLOAT_SWITCH` - were seen in CI to keep MXCSR per fiber
  already (the platform tests print it); the layer saves and restores the
  register itself rather than lean on that, so arm64 does the same.
- `tools/toolchain.py`: where CMake, Ninja and the compiler come from, how a
  port's build is configured and run, and what its executable is called.
  Windows keeps what `port.py` did - Visual Studio's CMake and Ninja under
  `vcvars64.bat` in `cmd`, clang-cl or MSVC, `<name>.exe`; macOS builds
  through the port's `macos-arm64` preset into
  `build/port/cmake-macos-arm64`, a plain executable `<name>` (no app bundle
  yet). `port.py`, `regress.py` and `bisect_lifted.py` find the executable
  through it, and the CI builds its stub port with it. `pgo` and `--sse4`
  stay Windows' (x86, clang-cl).
  Correction (25 October 2026): `toolchain.py` imports
  `ps2decomp/cppcheck.py`, which sets `PS2RT_EE_ROUND_NEAREST` for
  verification's own runs, so `regress.py` and `bisect_lifted.py` started
  the port rounding to nearest from then on - their pictures another FPU's
  than the console's, which `port.py` and a run started by hand have.
  Upstream's two did not import it. The port's environment is now
  `toolchain.game_env` (what `port.py` had as `_game_env`), and every tool
  that starts the port uses it; `test_a_scripted_run_rounds_as_the_console_does`
  fails without it. Tekken 5's scripted runs recorded since then are
  recorded again.
- The host builds and runs outside Windows. The executable's path and the
  process's processors are the platform layer's; keys are named by
  Windows' virtual-key numbers everywhere (`host/src/keycodes.h`); where no
  Vulkan display opens, a plain SDL window takes GDI's place
  (`host/src/platform/sdl_display.cpp`, SDL's 2D renderer; tests
  `ps2host_sdl_tests`), reading gamepads as the Vulkan display does
  (`gpu/include/ps2gpu/sdl_pad.h`). The overlay - the F1 counter, the F9
  panel - is drawn with GDI, so it is Windows' alone. There is no audio
  device yet: a run is silent, and `--audio-wav` records. `main.cpp`'s
  Windows calls sit behind `_WIN32`; elsewhere there is no disc dialog yet
  (`--disc`, or the image beside the executable).
- Code met at run time is looked for in every segment the executable loaded,
  not only the first, and `port.py` translates it from the section that holds
  its source: Tekken 5 keeps kernel extensions in `.data` and its loader in
  a second code section (`.y4`). Each such translation is marked NOT
  VERIFIED with its section, and the build counts them apart, by section.
  `LoadExecutable` (`run.h`) loads an executable from bytes, for the host's
  tests.
- The ELF loader zeroes every segment's memory first and copies every
  segment's bytes after: a segment with no bytes in the file laid over
  another (Tekken 5's `.dmh` over its `.y4`) no longer erases it.
- Functions the analysis guessed - found by the prologue scan or recovered
  from the gaps, not entered, called or pointed to - are counted apart: the
  status table gains a `confidence` column (`high`/`low`, written by derive
  and lift), and `progress.py` leaves the `low` ones out of every figure and
  lists them tier by tier (METHODOLOGY.md, "Counted apart").
- A game can take its executable's FDEs as the bounds of the functions they
  describe (game.json `"eh_frame_boundaries": true`, `ps2decomp/ehframe.py`):
  each such function gets its FDE's extent, and a "function" starting inside
  one - a switch's case, an entry a data word made up - is dropped unless
  code calls it. Off by default; Tekken 5's game program turns it on.
- The check libraries' clang build reports every compile error
  (`-ferror-limit=0`): derive drops each function that does not compile and
  builds again, and clang's limit of twenty made that a build per handful of
  functions - seven builds of a 90 MB file for Tekken 5's game program.
- `plzcw` and `prot3w` in the interpreter and the lifter (the MMI's leading
  sign bits, and a rotation of three words), with a semantic test of each.
- Two choices of a game's analysis, off by default: call targets from the
  code only (`"calls_from_code_only"`: an overlay's image holds its data,
  and data that decodes as `jal` made up 103 entries in Tekken 5's game
  program), and functions that end at their returns (`"end_at_returns"`:
  after a `jr $ra` and its delay slot, when padding or the next function's
  prologue follows - GCC's scheduled prologues included - and nothing before
  it branches past; a `j` to a function's start is a tail call, not a
  branch). Against Tekken 5's FDEs the second brings seven functions' ends
  nearer and none further.
- Guessed functions another source confirms count as sure: a game's
  `"confirmed"` table (`tools/confirm.py`), each entry with its sources - a
  run's call into it (the host's `--called` record, which leaves jumps out),
  or Ghidra's analysis of the same image. `progress.py` reports how many,
  by source.
- derive's compiled check compiles the emitted file in parts side by side, as
  many at once as the cores and the memory available allow
  (`cppcheck.split_definitions`, `compile_jobs`); the parts link into the one
  library, and errors are reported at the emitted file's lines. Tekken 5's 88
  MB file: 349 s with five at once, where it was 1,058 s as one.
- `data/iop_exports.csv`: ioman's, iomanx's and stdio's names at their places
  (a line of ps2sdk's tables that opens with a comment had been left out).
- The IOP libraries Tekken 5's modules import: thbase's thread queries and
  ExitDeleteThread, thvpool and thfpool, setjmp/longjmp, modload's module
  queries over a record of the modules loaded, cdvdman's file search, ioman
  over the disc (read-only), timrman's counter, and sifcmd's RPC to the EE's
  servers (the EE's RPC end now reaches the module's client) and its
  request-by-request serving. `Disc::List` gives a directory's entries.
  modload's SearchModuleByName answers 1 for a name no module recorded has,
  as the DirtySock stub answered every name before, and logs each name it
  answers so.
- VU0's flags in macro mode: the clip flag (`vclipw`'s last four
  judgements) and the status flag's Z and S with their sticky copies, in
  the interpreter and the lifted code alike; verification compares them as
  the rest of the state. The MAC flag and the O, U, I and D bits are not
  kept, and `cfc2` of vi17 stays refused. The host's VU interpreter keeps
  its own flags, unchanged.
- A CPU-model choice, `cop0_status` in game.json (`ps2decomp/cpumodel.py`),
  off by default: Status reads IE, and EIE until `di` and again after `ei`,
  per thread; every other COP0 register still reads 0. The interpreter and
  the lifted code follow it alike, and the verdict cache's inputs include
  it. Tekken 5 turns it on: its thread start refuses while Status says
  interrupts are disabled. (Each interpreted call starts enabled since
  8e1fad4: before it, a run left between `di` and `ei` carried into the
  next call on the same machine.)
- `ps2host::iop::AwaitServer`: an EE bind to a server that a module in the
  IOP interpreter will register gets server 0 until the module registers
  it, so the EE binds again; the port names the servers.
- Vulkan on macOS through MoltenVK and the Vulkan loader: the executables
  built here have the loader's folder in their build rpath (only for
  development, `PS2GPU_VULKAN_BUILD_RPATH`; a release bundles the loader);
  the instance asks for portability drivers, and the device enables
  `VK_KHR_portability_subset` with its features, where they are offered.
- Found by the validation layers, on every device: the shaders' `discard`
  (a demote for Vulkan 1.3) needs `shaderDemoteToHelperInvocation`, now
  enabled; the renderer's destructor destroys the image swizzle's, the
  decode's, the patch's and the depth paths' objects too. And the
  swapchain takes `B8G8R8A8_UNORM` in sRGB's colour space, not the last
  space offered (Display P3 linear on MoltenVK).
- `GsRenderer::ExactPartialMasks`: whether a channel FBMSK masks in part is
  drawn exactly (logic ops). The renderer's tests mark
  `PartlyMaskedChannels` a known failure where it is not - run and
  reported, not skipped - and `PS2GPU_REQUIRE` makes a missing GPU fail.
- The EE kernel answers ReferSemaStatus and iReferSemaStatus (ps2sdk's
  `ee_sema_t`: count, max_count, init_count, wait_threads, attr, option),
  CreateSema keeping the attributes and option it was given. A game that
  takes a result's address from a semaphore's option got nothing.
- The IOP heap refuses a size that would wrap when rounded up (0xFFFFFFFF),
  as sysmem refuses what no free block holds.
- loadfile's module requests (procedures 9, 7, 8: search by name, stop,
  unload) are answered as modload does, for the modules the IOP
  interpreter runs: stopped if they said at their start they can be, and
  unloaded once nothing points into them any more. A module whose services
  are the host's stays resident.
- The IOP notes a module loaded while one of its name still is, and an RPC
  server registered while one of its id still is, with both: which of the
  two a game reaches is the host's choice.
- VIF1's interrupt bit, off by default (`SetVif1InterruptBit`; a port turns
  it on): a VIFcode with bit 31 runs to its end, its data and all, and VIF1
  then stands still - VIF1_STAT shows INT and VIS, INTC's VIF1 cause is
  raised - until FBRST's STC; a MARK right after the code runs first
  (ps2tek), and VIF1_ERR's MII masks the bit. The stop is found on the EE's
  thread as the words are handed over (`VifCodeScan::UntilStop`, which
  follows each command's data), so it falls in the same place with VIF1's
  worker or without, and the walk of the DMA chain stops there (`DmaStop`):
  the transfer's rest waits in the channel's registers with STR set, the
  rest of the quadword it stopped in held, and the transfer ends - D_STAT's
  CIS1, the DMAC interrupt - once VIF1 has taken all of it; with the bit on
  the chain's last command, after STC (as PCSX2's VIF1 DMA does not end
  while VIF1 is stalled). Stores to VIF1's FIFO wait while it stands; PATH3
  does not stop, so what a handler uploads reaches the GS before the draws
  after the bit. RST clears VIF1_STAT and the stop, and drops a stop owed and
  what the FIFO held; the transfer's rest goes on (the DMA channel is not
  reset). A VIF1 left standing 60 frames is said once (a line and a note)
  and in the run's report, never resumed by itself. Tekken 5 lets each
  texture upload go when its VIF1 handler sees INT and draws from it after
  STC: without the stop its title and its fight's HUD were drawn from
  textures not yet uploaded. INTC causes a device posts (`PostIntc`) are
  delivered as DMA completions are, before them.
- Sound outside Windows through SDL 3 (Core Audio on macOS): SDL's audio
  thread asks for what the device needs and the same ring answers, at the
  same controlled rate, as WASAPI's event does; Windows keeps WASAPI and
  waveOut. Without SDL built in (no PS2HOST_VULKAN) a run is silent as
  before. Tested on SDL's dummy audio driver (`ps2host_sdl_tests`).
- The EE kernel runs an INTC or DMAC handler, or an alarm's callback, with
  the `$gp` its code had when it was registered (AddIntcHandler,
  AddDmacHandler, SetAlarm), not the interrupted code's - as Play!'s EE
  kernel does (a behavioural reference) and ps2sdk's alarm patch does for
  alarms. Tekken 5's MPEG library runs its own callbacks with `$gp` 0, and
  its vertical blank handler, interrupting one, read through `$gp`.
- Run entries, off by default (`run_entries` in game.json, a table of
  address, kind and evidence): code a run reached that no function covered
  - by a switch's case, a branch from another such piece, a pointer, a
  `j`, a `jal` the analysis does not see, or an address computed - which
  the analysis then takes as a sure start (confidence `run`), so a pass
  derives, lifts and verifies it with the rest. Inside a function, a start
  cuts it short; a piece of that function's own (a case of its switch, a
  label) does not, and waits for its switch's table to be read. A case or a
  branch's label is a piece of a function, not one:
  `confidence_label` calls it by its kind, progress.py counts the pieces
  apart from the functions and from the guesses, and confirm.py leaves them
  out. Pieces are seeded last, once every function is found, and only where
  none covers them.
- Jump tables read wherever they are, off by default
  (`jump_tables_anywhere`): a switch's table is read in any of the
  program's sections; where the code next to the `jr` does not show it,
  its base and bound are followed back to what sets them - past calls for
  a register a call keeps - and without a bound the table is read entry by
  entry while each is word-aligned and names code of the switch's own
  region (its function's start to the next start the analysis is sure of),
  ending at the first that does not, at its section's end, where the code
  builds another address, or at 256 entries. analyze.py reports every table
  - those sized by their words alone flagged - and every jump through a
  register no table explains. A gaps-recovered "function" at a case goes.
- The verification's interpreter loads the program's read-only sections
  (`.rodata`) with its code, off by default (`verify_read_only_sections`),
  on bytes nothing else holds (an overlay's image takes its executable's in
  the gap); they are part of lift's verdict fingerprint.
- The structurer's liveness runs a likely branch's delay slot on its taken
  path only (`liveness.live_out`): what the slot reads is live, what it
  writes no longer looks dead on the other path. Counted on both, a
  register's last definition before the branch could fold away while the
  untaken path still read it - wrong code in a verified or
  equivalence-checked form, since random tests seldom take that path.
  Tekken 5's map erase stored a stale pointer and the game hung; 139 of its
  functions get another folded form.

Found and not yet changed, with the evidence in the Tekken 5 repository's
`games/tekken5/docs/FINDINGS.md` ("Engine findings worth upstreaming"): a call
that never returns makes the analysis swallow the next function; only the
first executable section is analysed; four faults of the C++ rendering, which the
compiled check catches and drops; `callers_known` holds for an executable
whose own modules, loaded at run time, call it, until they are listed as
overlays; a store made in kernel mode and ended by `eret` has no
translation (Tekken 5 overrides its one; if another game has the same form,
the lifter should learn it).

## Merges from upstream

Upstream is merged into `macos-arm64`, never rebased onto, so the branch's
pushed history stays as it is.

- **6226b59**, 24 October 2026 (merge 60cf444): upstream's 37 commits since
  397c943. One conflict, in `host.cpp`'s `Enter`: upstream's fast path
  taken (the function in the call cache, the watch list as a flag, call
  counts only where a run asks), with this fork's tell of calls from jumps
  kept. None in the VIF, the GIF's decoding, the IOP's threads and
  semaphores or compiled VU code: upstream's changes there merged as they
  were. After it (1a8d28e): upstream's `WaitForGuestEvent` waits through
  `platform::WaitOnU32`, as the idle loops' wait does, not Windows'
  `WaitOnAddress`; the host test of calls against jumps counts what the
  fast path counts. Checked: every test on macOS (`ps2gpu_tests`'
  `PartlyMaskedChannels` the known failure it was), the Windows CI, and
  Tekken 5 under virtual time with and without a memory card - to the same
  frames, every frame's CRC and the run's report as before the merge but
  for the host's own timings.
