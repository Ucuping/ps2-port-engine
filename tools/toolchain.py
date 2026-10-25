"""The platform's side of building and running a port: where CMake, Ninja and
the compiler come from, how the build is configured and run, and what the
executable is called.

Windows: Visual Studio's CMake and Ninja, run through vcvars64.bat in cmd,
with LLVM's clang-cl when installed (PS2_COMPILER=msvc: MSVC's own) - as
port.py always built - and <name>.exe. macOS: the port's CMake preset
(port/CMakePresets.json's `macos-arm64`: Apple clang, Ninja, the default SDK
by name, deployment target 26.0), CMake and Ninja from the PATH, and <name>
- a plain executable. An .app bundle and its signing come with the builder
(Phase 2).

    python engine/tools/toolchain.py build SOURCE BINARY GENERATED [--avx2 ON|OFF]

builds a port folder as port.py does - CI builds its port of no game so,
through the same calls.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from ps2decomp import cppcheck  # noqa: E402

WINDOWS = os.name == "nt"
# An executable's suffix: Windows' .exe, nothing elsewhere.
EXE = ".exe" if WINDOWS else ""
# The CMake preset a port builds with outside Windows (port/CMakePresets.json).
PRESET = "macos-arm64"


def executable_name(name: str) -> str:
    return name + EXE


def game_env(**extra) -> dict:
    """The environment a port runs in: this process's, less what verification
    sets for itself - ps2decomp/cppcheck.py, imported above, makes the runtime
    round to nearest as the interpreter does (PS2RT_EE_ROUND_NEAREST); the
    game itself must round as the console does (a run started from a tool
    that imported it drew other pictures, and played so)."""
    env = dict(os.environ, **extra)
    env.pop("PS2RT_EE_ROUND_NEAREST", None)
    return env


def port_folder() -> str:
    """The main build's folder under build/port: Windows' `cmake`, the
    preset's own name elsewhere (its binaryDir)."""
    return "cmake" if WINDOWS else "cmake-" + PRESET


def port_executable(game, folder: str | None = None) -> str:
    """games/<name>/build/port/<folder>/<name>, with the platform's suffix."""
    name = os.path.basename(os.path.normpath(game.directory))
    return game.path(os.path.join(game.build, "port", folder or port_folder(), executable_name(name)))


def compiler_name(clang: str | None) -> str:
    if not WINDOWS:
        return "Apple clang"
    return "clang-cl" if clang else "MSVC"


# ---------------------------------------------------------------- Windows

def _visual_studio_tool(*parts) -> str:
    vcvars = cppcheck.find_vcvars()
    root = os.path.abspath(os.path.join(os.path.dirname(vcvars), "..", "..", ".."))
    return os.path.join(root, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", *parts)


def clang_cl() -> str | None:
    """LLVM's clang-cl, if installed and not turned away (PS2_COMPILER=msvc):
    on the path, in LLVM's own folder, or the one Visual Studio installs.
    Windows only: elsewhere the preset names the compiler."""
    if not WINDOWS or os.environ.get("PS2_COMPILER", "").lower() == "msvc":
        return None
    candidates = [shutil.which("clang-cl"),
                  os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "LLVM", "bin", "clang-cl.exe")]
    try:
        vc = os.path.abspath(os.path.join(os.path.dirname(cppcheck.find_vcvars()), "..", ".."))
        candidates.append(os.path.join(vc, "Tools", "Llvm", "x64", "bin", "clang-cl.exe"))
    except Exception:
        pass
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return os.path.abspath(candidate)
    return None


def forget_other_compiler(binary: str, compiler: str | None) -> None:
    """A build folder configured with the other compiler: CMake keeps the
    compiler it found first, so its cache goes (the objects are rebuilt).
    Windows only: the preset always names the same one."""
    if not WINDOWS:
        return
    cache = os.path.join(binary, "CMakeCache.txt")
    if not os.path.exists(cache):
        return
    text = open(cache, encoding="utf-8", errors="replace").read()
    configured = re.search(r"^CMAKE_CXX_COMPILER:[A-Z]+=(.*)$", text, re.M)
    configured = configured.group(1).strip() if configured else ""
    clang = "clang-cl" in configured.lower()
    if clang == (compiler is not None) and (compiler is None or
                                             os.path.normcase(os.path.abspath(configured)) == os.path.normcase(compiler)):
        return
    print("build: %s now (the folder had %s) - configuring again"
          % (compiler or "MSVC", configured or "another compiler"))
    os.remove(cache)
    shutil.rmtree(os.path.join(binary, "CMakeFiles"), ignore_errors=True)


def _physical_memory() -> int:
    """Bytes of memory the machine has, or 0 where it cannot be asked."""
    if WINDOWS:
        import ctypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong),
                        ("total_physical", ctypes.c_ulonglong), ("free_physical", ctypes.c_ulonglong),
                        ("total_page", ctypes.c_ulonglong), ("free_page", ctypes.c_ulonglong),
                        ("total_virtual", ctypes.c_ulonglong), ("free_virtual", ctypes.c_ulonglong),
                        ("free_extended", ctypes.c_ulonglong)]

        status = MemoryStatus()
        status.length = ctypes.sizeof(MemoryStatus)
        return status.total_physical if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)) else 0
    try:
        return os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (ValueError, OSError, AttributeError):
        return 0


def build_jobs() -> int:
    """Compilers at once: by the memory there is, not the processors - a
    file of compiled VU code takes the compiler 2 GB and more, and a build
    from nothing with Ninja's default (processors + 2) ran out of it on 16 GB."""
    jobs = os.cpu_count() or 4
    memory = _physical_memory()
    if memory:
        jobs = min(jobs, int(memory / (2.5 * 2 ** 30)))
    return max(1, jobs)


def _windows_build(source: str, binary: str, generated: str, avx2: bool, pgo: str, lto: str,
                   jobs: int, clang: str | None) -> subprocess.CompletedProcess:
    cmake = _visual_studio_tool("CMake", "bin", "cmake.exe")
    ninja = _visual_studio_tool("Ninja", "ninja.exe")
    compilers = (' -DCMAKE_C_COMPILER="%s" -DCMAKE_CXX_COMPILER="%s"' % ((clang.replace("\\", "/"),) * 2)
                 if clang else "")
    script = os.path.join(binary, "build.bat")
    with open(script, "w", encoding="utf-8") as handle:
        handle.write('@echo off\ncall "%s" >nul\n' % cppcheck.find_vcvars())
        handle.write('"%s" -S "%s" -B "%s" -G Ninja -DCMAKE_BUILD_TYPE=Release '
                     '-DCMAKE_MAKE_PROGRAM="%s" -DPS2_GENERATED_DIR="%s" -DPS2HOST_AVX2=%s "-DPS2HOST_PGO=%s"'
                     ' -DPS2HOST_LTO=%s -DPS2HOST_LTO_JOBS=%d%s >nul || exit /b 1\n'
                     % (cmake, source, binary, ninja, generated.replace("\\", "/"), "ON" if avx2 else "OFF",
                        pgo, lto, jobs, compilers))
        handle.write('"%s" --build "%s" -j %d || exit /b 1\n' % (cmake, binary, jobs))
    return subprocess.run(["cmd", "/c", script], capture_output=True, text=True)


# ---------------------------------------------------------------- macOS

def _preset_build(source: str, binary: str, generated: str, jobs: int) -> subprocess.CompletedProcess:
    cmake = shutil.which("cmake")
    if not cmake or not shutil.which("ninja"):
        raise SystemExit("port build: CMake and Ninja on the PATH (Homebrew: brew install cmake ninja)")
    if not os.path.exists(os.path.join(source, "CMakePresets.json")):
        raise SystemExit("port build: no CMakePresets.json in %s - copy the game template's "
                         "(engine/templates/game/port/CMakePresets.json)" % source)
    configure = subprocess.run([cmake, "--preset", PRESET, "-B", binary, "-DPS2_GENERATED_DIR=%s" % generated],
                               cwd=source, capture_output=True, text=True)
    if configure.returncode:
        return configure
    built = subprocess.run([cmake, "--build", binary, "-j", str(jobs)], capture_output=True, text=True)
    built.stdout = configure.stdout + built.stdout
    return built


# ---------------------------------------------------------------- both

def build_port(source: str, binary: str, generated: str, *, avx2: bool = True, pgo: str = "",
               lto: str = "OFF", jobs: int | None = None, clang: str | None = None) -> None:
    """Configure and build a port folder (games/<name>/port, or CI's port of
    no game). Windows: avx2, pgo ("generate", a .profdata, or "") and lto as
    port.py passes them. Elsewhere those are Windows' options: the preset's
    build only. Stops with the compiler's errors on failure."""
    source, binary, generated = os.path.abspath(source), os.path.abspath(binary), os.path.abspath(generated)
    os.makedirs(binary, exist_ok=True)
    jobs = jobs or build_jobs()
    if WINDOWS:
        result = _windows_build(source, binary, generated, avx2, pgo, lto, jobs, clang)
    else:
        result = _preset_build(source, binary, generated, jobs)
    if result.returncode:
        errors = [line for line in (result.stdout + result.stderr).splitlines()
                  if "error" in line.lower()]
        raise SystemExit("port build failed:\n" + "\n".join(errors[:40] or
                                                           (result.stdout + result.stderr)[-4000:].splitlines()))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Build a port folder as port.py does.")
    parser.add_argument("command", choices=("build",))
    parser.add_argument("source")
    parser.add_argument("binary")
    parser.add_argument("generated")
    parser.add_argument("--avx2", choices=("ON", "OFF"), default="ON", help="Windows: PS2HOST_AVX2")
    parser.add_argument("--jobs", type=int, default=0)
    args = parser.parse_args(argv)
    build_port(args.source, args.binary, args.generated, avx2=args.avx2 == "ON", clang=clang_cl(),
               jobs=args.jobs or None)
    print("built %s (%s)" % (args.binary, compiler_name(clang_cl())))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
