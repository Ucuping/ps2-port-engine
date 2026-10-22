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

None of these changes what a Windows build compiles or does.

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

Found and not yet changed, with the evidence in the Tekken 5 repository's
`games/tekken5/docs/FINDINGS.md` ("Engine findings worth upstreaming"): a call
that never returns makes the analysis swallow the next function; tables
outside the code section are invisible to the analysis; only the first
executable section is analysed.
