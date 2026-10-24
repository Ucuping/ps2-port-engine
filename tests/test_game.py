"""Tests for the per-game configuration: what is detected, what is configured.

The engine is generic; a game supplies where its files are and the facts that
differ between games. The global pointer is the one fact that always differs, and
it is detected from the startup code rather than written down.
"""

import json
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

from ps2decomp import game, layout

BASE = 0x00100000


class FakeSection:
    def __init__(self, addr):
        self.addr = addr


class FakeElf:
    """Just enough of an ELF for detect_gp: an entry point and one code section."""

    def __init__(self, words, entry=BASE):
        self.data = struct.pack("<%dI" % len(words), *words)
        self.e_entry = entry
        self.exec_sections = [FakeSection(BASE)]

    def section_data(self, _section):
        return self.data


def lui(rt, imm): return (0x0F << 26) | (rt << 16) | (imm & 0xFFFF)
def addiu(rt, rs, imm): return (0x09 << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
def move(rd, rs): return (rs << 21) | (rd << 11) | 0x25          # or rd, rs, $zero
def j(target): return (2 << 26) | ((target >> 2) & 0x03FFFFFF)


class TestGlobalPointerDetection(unittest.TestCase):
    def test_built_in_another_register_then_moved(self):
        """Burnout 3's startup: lui $a0, 0x4F; addiu $a0, $a0, -0x7990; or $gp, $a0."""
        elf = FakeElf([lui(4, 0x4F), addiu(4, 4, -0x7990), move(28, 4), 0x0000000C])
        self.assertEqual(game.detect_gp(elf), 0x004E8670)

    def test_found_past_a_loop_that_jumps_backwards(self):
        """The BSS is cleared by loops before $gp is set; the scan must not follow
        them round forever."""
        elf = FakeElf([0, j(BASE), 0, lui(28, 0x50), addiu(28, 28, 0x10), 0x0000000C])
        self.assertEqual(game.detect_gp(elf), 0x00500010)

    def test_none_when_the_entry_never_sets_it(self):
        self.assertIsNone(game.detect_gp(FakeElf([0, 0, 0x03E00008, 0])))


class TestConfiguration(unittest.TestCase):
    def setUp(self):
        self.saved = vars(layout.CURRENT).copy()

    def tearDown(self):
        layout.configure(**self.saved)

    def test_layout_overrides_come_from_game_json(self):
        directory = tempfile.mkdtemp()
        with open(os.path.join(directory, "game.json"), "w") as handle:
            json.dump({"executable": "NONE.00", "layout": {"scratch": "0x01400000",
                                                          "gp": 0x1234}}, handle)
        loaded = game.load(directory)
        self.assertEqual(loaded.executable, "NONE.00")
        self.assertEqual(layout.CURRENT.scratch, 0x01400000)
        self.assertEqual(layout.CURRENT.gp, 0x1234)
        self.assertEqual(loaded.database_path,
                         os.path.normpath(os.path.join(directory, "build/functions.json")))

    def test_unknown_layout_field_is_refused(self):
        with self.assertRaises(KeyError):
            layout.configure(not_a_field=1)

    def test_run_entries_come_with_how_they_were_reached(self):
        directory = tempfile.mkdtemp()
        os.makedirs(os.path.join(directory, "config"))
        with open(os.path.join(directory, "game.json"), "w") as handle:
            json.dump({"executable": "NONE.00", "run_entries": "config/run_entries.csv"}, handle)
        table = os.path.join(directory, "config", "run_entries.csv")
        with open(table, "w") as handle:
            handle.write("address,kind,evidence\n0x0012AE58,case,.rodata 0x0014F87C\n"
                         "0x004B2080,j,j at 0x004B1AB8\n")
        loaded = game.load(directory)
        self.assertEqual(loaded.run_entries, {0x0012AE58: "case", 0x004B2080: "j"})
        self.assertEqual(loaded.run_entries_path, os.path.normpath(table))
        with open(table, "a") as handle:
            handle.write("0x00100000,maybe,\n")
        with self.assertRaises(SystemExit):
            game.read_run_entries(table)

    def test_no_run_entries_by_default(self):
        directory = tempfile.mkdtemp()
        with open(os.path.join(directory, "game.json"), "w") as handle:
            json.dump({"executable": "NONE.00"}, handle)
        loaded = game.load(directory)
        self.assertEqual((loaded.run_entries, loaded.run_entries_path), ({}, None))


class TestCallersKnown(unittest.TestCase):
    """A loads with B resident; C loads with B too. B's callers include C,
    which B's own list leaves out; A's are all in A's list."""

    def setUp(self):
        self.root = tempfile.mkdtemp()

        def overlay(name, image, resident):
            directory = os.path.join(self.root, "overlays", name)
            os.makedirs(directory)
            with open(os.path.join(directory, "game.json"), "w") as handle:
                json.dump({"overlay": {"of": "../..", "image": "../../%s.bin" % image,
                                       "resident": [{"image": "../../%s.bin" % r}
                                                    for r in resident]}}, handle)
            return directory

        self.base = game.Game(self.root, "base", "x.elf", "", "", "", "", "", "", {}, [],
                              overlays=["overlays/a", "overlays/b", "overlays/c"])
        self.parts = {name: game.Game(overlay(name, image, resident), name, "", "", "", "",
                                      "", "", "", {}, [], base=self.base)
                      for name, image, resident in (("a", "1", ["2"]), ("b", "2", ["1"]),
                                                    ("c", "3", ["2"]))}

    def test_an_overlay_whose_every_neighbour_it_lists(self):
        self.assertTrue(game.callers_known(self.parts["a"]))
        self.assertTrue(game.callers_known(self.parts["c"]))

    def test_an_overlay_another_loads_beside_it_unlisted(self):
        self.assertFalse(game.callers_known(self.parts["b"]))

    def test_a_game_with_overlays(self):
        self.assertFalse(game.callers_known(self.base))


if __name__ == "__main__":
    unittest.main()


class TestReadOnlySections(unittest.TestCase):
    """The verification's interpreter loads the program's read-only sections
    (.rodata) with its code where a game asks (game.json
    "verify_read_only_sections"): a switch's table there is read under test as
    on the console. Off, by default, they read zero as before."""

    def setUp(self):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from test_port import tiny_elf, SHF_ALLOC, SHF_WRITE, SHF_EXECINSTR, SHT_PROGBITS, SHT_NOBITS
        self.rodata = struct.pack("<2I", 0x00100008, 0x0010000C)
        self.image = tiny_elf([
            (".text", 0x00100000, struct.pack("<4I", 0x03E00008, 0, 0x03E00008, 0), SHF_ALLOC | SHF_EXECINSTR,
             SHT_PROGBITS),
            (".data", 0x00100100, struct.pack("<I", 0x11111111), SHF_ALLOC | SHF_WRITE, SHT_PROGBITS),
            (".rodata", 0x00100200, self.rodata, SHF_ALLOC, SHT_PROGBITS),
            (".bss", 0x00100300, bytes(16), SHF_ALLOC | SHF_WRITE, SHT_NOBITS),
        ])
        self.directory = tempfile.mkdtemp()
        os.makedirs(os.path.join(self.directory, "extracted"))
        with open(os.path.join(self.directory, "extracted", "TEST.ELF"), "wb") as handle:
            handle.write(self.image)

    def tearDown(self):
        from ps2decomp import emu
        emu.configure(())

    def load(self, **settings):
        with open(os.path.join(self.directory, "game.json"), "w") as handle:
            json.dump(dict({"executable": "TEST.ELF"}, **settings), handle)
        return game.load(self.directory)

    def test_off_by_default(self):
        from ps2decomp import emu
        loaded = self.load()
        self.assertFalse(loaded.verify_read_only_sections or loaded.jump_tables_anywhere)
        machine = emu.Machine.from_elf(loaded.load_elf())
        self.assertEqual(machine.memory.read(0x00100200, 8), bytes(8))

    def test_the_read_only_sections_alone_are_loaded(self):
        from ps2decomp import emu
        loaded = self.load(verify_read_only_sections=True, jump_tables_anywhere=True)
        self.assertTrue(loaded.jump_tables_anywhere)
        self.assertEqual(game.read_only_sections(loaded), [(0x00100200, self.rodata)])
        machine = emu.Machine.from_elf(loaded.load_elf())
        self.assertEqual(machine.memory.read(0x00100200, 8), self.rodata)
        self.assertEqual(machine.memory.read(0x00100100, 4), bytes(4))         # .data: not read-only
        self.assertNotEqual(emu.read_only_key(), ())
