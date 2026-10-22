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

Found and not yet changed, with the evidence in the Tekken 5 repository's
`games/tekken5/docs/FINDINGS.md` ("Engine findings worth upstreaming"): a call
that never returns makes the analysis swallow the next function; tables
outside the code section are invisible to the analysis; only the first
executable section is analysed; four faults of the C++ rendering, which the
compiled check catches and drops; `callers_known` holds for an executable
whose own modules, loaded at run time, call it, until they are listed as
overlays; code copied at run time is looked for only in the executable's
first loaded segment, and translated only from its first executable section.
