#!/usr/bin/env python3
"""The generated sources of the CI stub port (tests/ci/stub_game): a dispatch
table that names one address and translates nothing, no overlays, no code found
at run time and no VU programs - written by engine/tools/port.py's and
vurecomp.py's own functions, so the port links as a game's does, with no game.

    python tests/ci/stub_generated.py <out_dir>
"""

import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(os.path.dirname(HERE))
sys.path[:0] = [os.path.join(ENGINE, "tools"), ENGINE]

import port  # noqa: E402
import vurecomp  # noqa: E402


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else list(argv)
    if len(argv) != 1:
        raise SystemExit(__doc__)
    out = os.path.abspath(argv[0])
    os.makedirs(out, exist_ok=True)
    # One entry with no translation: a table of none is a zero-length array,
    # which C++ refuses.
    port._write_if_changed(os.path.join(out, "dispatch.cpp"),
                           port.dispatch_source([], {}, untranslated=[0x00100000], sizes={0x00100000: 4}))
    port._write_if_changed(os.path.join(out, "discovered.cpp"), '#include "ps2rt/lifted.h"\n')
    port._write_if_changed(os.path.join(out, "dispatch_overlays.cpp"),
                           port.overlays_source(types.SimpleNamespace(overlays=[]), out))
    vurecomp.main(["--out-dir", out])
    print("stub generated sources: %s" % ", ".join(sorted(os.listdir(out))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
