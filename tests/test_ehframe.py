"""Function boundaries from .eh_frame (ps2decomp/ehframe.py)."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import ehframe  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402


def record(content: bytes) -> bytes:
    return struct.pack("<I", len(content)) + content


def cie(augmentation: bytes, extra: bytes = b"") -> bytes:
    """id 0, version 1, the augmentation, code and data alignment, return register."""
    return record(struct.pack("<IB", 0, 1) + augmentation + b"\0" + extra + b"\x01\x7c\x1f")


def fde(at: int, cie_at: int, begin: int, length: int, signed: bool = False) -> bytes:
    """An FDE at offset `at`, its CIE at `cie_at`: the CIE pointer counts back from its own field."""
    return record(struct.pack("<I", at + 4 - cie_at) + struct.pack("<iI" if signed else "<II", begin, length))


class TestFdeRanges(unittest.TestCase):
    def test_gcc_29x_forms(self):
        data = cie(b"")                                         # at 0
        first = len(data)
        data += fde(first, 0, 0x001F0180, 0x208)
        data += fde(len(data), 0, 0, 0x40)                      # discarded by the linker
        eh_at = len(data)
        data += cie(b"eh", struct.pack("<I", 0x12345678))       # GCC 2.9x's exception pointer
        data += fde(len(data), eh_at, 0x00200000, 0x10)
        data += struct.pack("<I", 0)                            # the end
        self.assertEqual(ehframe.fde_ranges(data, 0x00134D88),
                         [(0x001F0180, 0x208), (0x00200000, 0x10)])

    def test_pc_relative_addresses(self):
        # "zR": augmentation data of one byte, the encoding pc-relative signed 4 bytes.
        data = record(struct.pack("<IB", 0, 1) + b"zR\0" + b"\x01\x7c\x1f" + b"\x01\x1b")
        at = len(data)
        address = 0x00400000
        field = address + at + 8                                # where the FDE's address field lies
        data += fde(at, 0, 0x00100040 - field, 0x24, signed=True)
        self.assertEqual(ehframe.fde_ranges(data, address), [(0x00100040, 0x24)])


def describe(start, end, confidence):
    return {"addr": start, "end": end, "size": end - start, "confidence": confidence,
            "callers": [], "calls": [], "instructions": (end - start) // 4, "has_return": True}


def function(start, end, confidence="called", callers=()):
    f = describe(start, end, confidence)
    f["callers"] = list(callers)
    return f


class TestApplyBounds(unittest.TestCase):
    def setUp(self):
        self.functions = [
            function(0x100, 0x140),                             # the FDE says 0x80 long
            function(0x150, 0x158, "pointer"),                  # a case, named by a table
            function(0x160, 0x170, "called", [0xA00]),          # a "call" from data
            function(0x174, 0x180, "called", [0x500]),          # a call from code
            function(0x200, 0x260),                             # the FDE says 0x40
            function(0x300, 0x330),                             # the FDE says 0x2C; then padding
            function(0x400, 0x440),                             # no FDE
        ]
        in_code = lambda address: address < 0x800              # noqa: E731
        padding = lambda start, end: start == 0x32C             # noqa: E731
        bounds = [(0x100, 0x80), (0x200, 0x40), (0x300, 0x2C), (0x999000, 0x10)]
        self.out, self.report = ehframe.apply_bounds(self.functions, bounds, describe, in_code, padding)
        self.by = {f["addr"]: f for f in self.out}

    def test_extents_are_the_fdes(self):
        self.assertEqual((self.by[0x100]["end"], self.by[0x200]["end"], self.by[0x300]["end"]),
                         (0x180, 0x240, 0x32C))
        self.assertTrue(all(self.by[a]["bounds"] == "eh_frame" for a in (0x100, 0x200, 0x300)))
        self.assertNotIn("bounds", self.by[0x400])
        self.assertEqual(self.by[0x400]["end"], 0x440)
        self.assertEqual(sorted(self.report["changed"]), [(0x100, 0x80, 0x40), (0x200, 0x40, 0x60),
                                                          (0x300, 0x2C, 0x30)])

    def test_starts_inside_go_unless_code_calls_them(self):
        self.assertEqual(sorted(self.report["dropped"]), [0x150, 0x160])
        self.assertEqual(self.report["kept_inner"], [0x174])
        self.assertNotIn(0x150, self.by)
        self.assertIn(0x174, self.by)

    def test_code_past_an_fde_stands_alone_padding_does_not(self):
        self.assertEqual(self.report["added"], [0x240])
        self.assertEqual((self.by[0x240]["end"], self.by[0x240]["confidence"]), (0x260, "recovered: past an FDE"))
        self.assertNotIn(0x32C, self.by)

    def test_bounded_functions_are_sure(self):
        self.assertEqual(game_config.confidence_label({"confidence": "prologue", "bounds": "eh_frame"}), "high")
        self.assertEqual(game_config.confidence_label(self.by[0x240]), "low")


if __name__ == "__main__":
    unittest.main()
