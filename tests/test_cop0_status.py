"""COP0 Status as a thread sees it (ps2decomp/cpumodel.py's cop0_status): off by
default, every COP0 register reads 0; on, Status reads IE, and EIE until `di`
and again after `ei` - in the interpreter and the lifted code alike."""
import json
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import cpumodel, cppcheck, lift, liftcheck  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402
from ps2decomp.emu import Machine  # noqa: E402

BASE = 0x00100000
V0, T0, T1, T2, RA = 2, 8, 9, 10, 31
DI, EI, SYNC, JR_RA, NOP = 0x42000039, 0x42000038, 0x0000000F, 0x03E00008, 0


def mfc0(rt, rd):
    return (0x10 << 26) | (rt << 16) | (rd << 11)


def lui(rt, imm):
    return (0x0F << 26) | (rt << 16) | imm


def and_(rd, rs, rt):
    return (rs << 21) | (rt << 16) | (rd << 11) | 0x24


def bne(rs, rt, pc, target):
    return (0x05 << 26) | (rs << 21) | (rt << 16) | (((target - pc - 4) >> 2) & 0xFFFF)


# Status read around di and ei, and Count and Cause, which still read 0.
READS = [mfc0(T0, 12), DI, mfc0(T1, 12), EI, mfc0(T2, 12),
         mfc0(V0, 9), mfc0(3, 13), DI, JR_RA, NOP]
# The executable's `di` routine's loop (0x001189A8): di, then Status again until
# EIE is clear - which never comes while Status reads as a constant.
DI_LOOP = [lui(T1, 1),                                     # 0x00 EIE's mask
           DI, SYNC, mfc0(V0, 12), and_(V0, V0, T1),        # 0x04
           bne(V0, 0, BASE + 0x14, BASE + 0x04), NOP,       # 0x14
           JR_RA, NOP]


def machine(words):
    return Machine(BASE, struct.pack("<%dI" % len(words), *words), 0x02000000 - BASE)


class Model(unittest.TestCase):
    def setUp(self):
        self.saved = cpumodel.CURRENT.cop0_status

    def tearDown(self):
        cpumodel.configure(cop0_status=self.saved)


class TestInterpreter(Model):
    def run_reads(self):
        m = machine(READS)
        m.call(BASE, [])
        return [m.get(r) for r in (T0, T1, T2, V0, 3)], m.di

    def test_off_every_register_reads_0(self):
        cpumodel.configure(cop0_status=False)
        self.assertEqual(self.run_reads(), ([0, 0, 0, 0, 0], 0))

    def test_on_status_follows_di_and_ei(self):
        cpumodel.configure(cop0_status=True)
        self.assertEqual(self.run_reads(), ([0x10001, 0x00001, 0x10001, 0, 0], 1))

    def test_on_the_di_routine_ends(self):
        cpumodel.configure(cop0_status=True)
        m = machine(DI_LOOP)
        m.call(BASE, [], max_instructions=1000)
        self.assertEqual((m.get(V0), m.di), (0, 1))


class TestLifted(Model):
    @classmethod
    def setUpClass(cls):
        try:
            cppcheck.find_compiler()
        except cppcheck.CompilerUnavailable as exc:
            raise unittest.SkipTest(str(exc))

    def check(self, words):
        image = struct.pack("<%dI" % len(words), *words)
        work = tempfile.mkdtemp()
        source = os.path.join(work, "lifted.cpp")
        with open(source, "w", encoding="utf-8") as handle:
            handle.write('#include "ps2rt/lifted.h"\n\n' + lift.lift_function(image, BASE, BASE, BASE + len(image)))
        m = machine(words)
        lifted = liftcheck.LiftedFunctions(liftcheck.build([source], work), m)
        m.call(BASE, [])
        expected = liftcheck.machine_state(m)
        context = liftcheck.entry_context(())
        self.assertIsNone(lifted.run(BASE, context))
        self.assertEqual(liftcheck.context_state(context), expected)
        return context

    def test_off_as_before(self):
        cpumodel.configure(cop0_status=False)
        context = self.check(READS)
        self.assertEqual((context.r[T0], context.r[T1], context.di), (0, 0, 0))

    def test_on_the_same_as_the_interpreter(self):
        cpumodel.configure(cop0_status=True)
        context = self.check(READS)
        self.assertEqual((context.r[T0], context.r[T1], context.r[T2], context.di), (0x10001, 1, 0x10001, 1))
        self.check(DI_LOOP)


class TestTheGamesChoice(Model):
    def write(self, folder, values):
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "game.json"), "w", encoding="utf-8") as handle:
            json.dump(values, handle)

    def test_off_by_default_on_from_game_json_an_overlay_takes_its_games(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write(folder, {"executable": "GAME.ELF"})
            self.assertFalse(game_config.load(folder).cop0_status)
            self.assertFalse(cpumodel.CURRENT.cop0_status)
            self.write(folder, {"executable": "GAME.ELF", "cop0_status": True})
            self.write(os.path.join(folder, "overlays", "part"),
                       {"overlay": {"of": "../..", "image": "part.bin", "address": "0x00200000"}})
            self.assertTrue(game_config.load(folder).cop0_status)
            self.assertTrue(game_config.load(os.path.join(folder, "overlays", "part")).cop0_status)
            self.assertTrue(cpumodel.CURRENT.cop0_status)


if __name__ == "__main__":
    unittest.main()
