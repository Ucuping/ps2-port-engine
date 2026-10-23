"""Function boundaries from an executable's .eh_frame.

Each FDE (frame description entry) gives one function's start and length: the
compiler writes one for every function it compiles with unwind information. A
game that turns them on (game.json, "eh_frame_boundaries") takes them as the
truth about the functions they describe (`apply_bounds`): the analysis finds
where functions start, but where one ends it can take a switch's dispatch for
the end and its cases for functions, or a word in data that decodes as a `jal`
for an entry (Tekken 5's FINDINGS.md, "Step 8").

The forms GCC 2.9x-3.x emit for the PS2 are read: CIEs with the augmentation ""
or "eh" (an exception-table pointer after the string), whose FDEs hold absolute
4-byte addresses; and "z" CIEs, whose FDEs' addresses take the CIE's pointer
encoding ("R": absolute, or relative to the field, as 4-byte words).
"""

from __future__ import annotations

import struct

_ABSOLUTE, _UDATA4, _SDATA4, _PCREL = 0x00, 0x03, 0x0B, 0x10


def _uleb(data: bytes, at: int) -> tuple:
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, at


def _cie_encoding(data: bytes, at: int, end: int) -> int:
    """The pointer encoding of a CIE whose content starts at `at` (after its id)."""
    version = data[at]
    at += 1
    augmentation_end = data.index(b"\0", at)
    augmentation = data[at:augmentation_end].decode("latin1")
    at = augmentation_end + 1
    if "eh" in augmentation:
        at += 4                                     # GCC 2.9x's exception-table pointer
    _code_align, at = _uleb(data, at)
    _data_align, at = _uleb(data, at)               # signed, but only skipped here
    at = at + 1 if version == 1 else _uleb(data, at)[1]
    encoding = _ABSOLUTE
    if augmentation.startswith("z"):
        _length, at = _uleb(data, at)
        for letter in augmentation[1:]:
            if letter == "R":
                encoding = data[at]
                at += 1
            elif letter == "L":
                at += 1
            elif letter == "P":
                personality = data[at]
                at += 1 + (4 if personality & 0x0F in (_ABSOLUTE, _UDATA4, _SDATA4) else 0)
            if at > end:
                break
    return encoding


def fde_ranges(data: bytes, address: int) -> list:
    """[(start, length)] of every FDE in the .eh_frame bytes `data`, loaded at
    `address`. FDEs with a zero start - functions the linker discarded - are
    left out; the walk ends at a zero length or the end of the bytes."""
    found, encodings, at = [], {}, 0
    while at + 8 <= len(data):
        length = struct.unpack_from("<I", data, at)[0]
        if length == 0 or length == 0xFFFFFFFF or at + 4 + length > len(data):
            break
        content, end = at + 4, at + 4 + length
        pointer = struct.unpack_from("<I", data, content)[0]
        if pointer == 0:
            encodings[at] = _cie_encoding(data, content + 4, end)
        else:
            encoding = encodings.get(content - pointer, _ABSOLUTE)
            field = content + 4
            begin, size = struct.unpack_from("<iI" if encoding & 0x0F == _SDATA4 else "<II", data, field)
            if encoding & 0x70 == _PCREL:
                begin = (address + field + begin) & 0xFFFFFFFF
            begin &= 0xFFFFFFFF
            if begin:
                found.append((begin, size))
        at = end
    return found


def apply_bounds(functions: list, bounds, describe, in_code, padding, first_code) -> tuple:
    """`functions` (the analysis's, sorted by address) with each one an FDE
    describes given the FDE's extent - its out-of-line pieces inside it - and
    marked "bounds": "eh_frame". A "function" starting strictly inside an FDE's
    range is a label of it - a switch's case, an entry a data word made up - and
    is dropped, unless code (`in_code(address)`) outside the range calls it.
    Code past an FDE's end that the analysis had given to its function, where no
    other function starts and that is not padding (`padding(start, end)`),
    becomes a function of its own, "recovered: past an FDE", starting at its
    first word of code (`first_code(start, end)`): the alignment between two
    functions is neither's. `describe(start, end, confidence)` builds a function
    as the analysis would.

    Returns (functions, report): the report lists the functions whose extent
    changed (address, FDE length, the analysis's size), the dropped starts, the
    inner starts kept because code calls them, and the functions added."""
    import bisect
    by_start = {f["addr"]: f for f in functions}
    spans = sorted((start, start + length) for start, length in bounds if start in by_start and length)
    report = {"changed": [], "dropped": [], "kept_inner": [], "added": []}
    starts = sorted(by_start)
    for start, end in spans:
        for address in starts[bisect.bisect_right(starts, start):bisect.bisect_left(starts, end)]:
            if any(in_code(caller) and not start <= caller < end for caller in by_start[address]["callers"]):
                report["kept_inner"].append(address)
            else:
                report["dropped"].append(address)
                del by_start[address]
    for start, end in spans:
        f = by_start[start]
        if f["end"] != end:
            report["changed"].append((start, end - start, f["size"]))
            bounded = describe(start, end, f["confidence"])
            bounded["callers"] = f["callers"]
            if (f["end"] > end and not any(end <= a < f["end"] for a in by_start)
                    and not padding(end, f["end"])):
                tail = first_code(end, f["end"])
                by_start[tail] = describe(tail, f["end"], "recovered: past an FDE")
                report["added"].append(tail)
            f = bounded
        f["bounds"] = "eh_frame"
        by_start[start] = f
    return [by_start[a] for a in sorted(by_start)], report
