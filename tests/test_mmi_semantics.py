"""PLZCW and PROT3W in the interpreter (ps2decomp/emu.py), one instruction at a
time, against values worked out by hand from their definitions: PLZCW counts,
for each word of rs's low doubleword, the bits after the sign bit that equal
it; PROT3W rotates rt's low three words a word down and keeps word 3. PCSX2
behaves so (its MMI.cpp, read as a reference only)."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import emu  # noqa: E402
from ps2decomp.r5900 import DISASM  # noqa: E402

MASK64 = (1 << 64) - 1


def mmi(name, rd, rs, rt):
    """The MMI word the decoder names `name` (as test_lift's mmi_word finds it)."""
    for funct in (0x04, 0x08, 0x28, 0x09, 0x29):
        for sub in range(32):
            word = (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | funct
            if DISASM.decode(word, 0).mnemonic == name:
                return word
    raise KeyError(name)


def run(word, registers):
    """One instruction on a fresh machine: `registers` {index: 128-bit value}."""
    machine = emu.Machine(0x00100000, struct.pack("<2I", word, 0))
    for index, value in registers.items():
        machine.regs[index] = value & MASK64
        machine.regs_hi[index] = value >> 64
    machine.pc = 0x00100000
    machine.step()
    return lambda index: (machine.regs_hi[index] << 64) | machine.get(index)


class TestPlzcw(unittest.TestCase):
    CASES = [(0x00000000, 31), (0xFFFFFFFF, 31), (0x00000001, 30), (0xFFFFFFFE, 30),
             (0x80000000, 0), (0x40000000, 0), (0x7FFFFFFF, 0), (0xC0000000, 1),
             (0x3FFFFFFF, 1), (0x0000FFFF, 15), (0xFFFF0000, 15), (0x00800000, 7)]

    def test_each_word_counted(self):
        for low, expect_low in self.CASES:
            for high, expect_high in self.CASES[::5]:
                got = run(mmi("plzcw", 8, 9, 0), {9: high << 32 | low})(8)
                self.assertEqual(got & MASK64, expect_high << 32 | expect_low, (hex(low), hex(high)))

    def test_every_count(self):
        # n bits after the sign equal it: 1 << (30 - n) when positive, its
        # complement when negative; and 0, all 31.
        for n in range(31):
            for word in (1 << (30 - n), ~(1 << (30 - n)) & 0xFFFFFFFF):
                got = run(mmi("plzcw", 8, 9, 0), {9: word << 32 | word})(8)
                self.assertEqual(got & MASK64, n << 32 | n, hex(word))
        self.assertEqual(run(mmi("plzcw", 8, 9, 0), {9: 0})(8) & MASK64, 31 << 32 | 31)

    def test_the_high_doubleword_stays(self):
        get = run(mmi("plzcw", 8, 9, 0), {8: 0x1122334455667788 << 64 | 0xFFFF, 9: 0x00000001_00000000})
        self.assertEqual(get(8) >> 64, 0x1122334455667788)
        self.assertEqual(get(8) & MASK64, 30 << 32 | 31)        # the high word is 1, the low 0

    def test_in_place_and_into_zero(self):
        self.assertEqual(run(mmi("plzcw", 9, 9, 0), {9: 0x80000000_00000001})(9) & MASK64, 0 << 32 | 30)
        self.assertEqual(run(mmi("plzcw", 0, 9, 0), {9: 1})(0), 0)


class TestProt3w(unittest.TestCase):
    WORDS = [0x00000000, 0x11111111, 0x22222222, 0x33333333]

    @staticmethod
    def pack(words):
        return sum(w << 32 * k for k, w in enumerate(words))

    def test_three_words_rotate_and_the_fourth_stays(self):
        source = self.pack([0xA0A0A0A0, 0xB1B1B1B1, 0xC2C2C2C2, 0xD3D3D3D3])
        got = run(mmi("prot3w", 8, 0, 9), {9: source})(8)
        self.assertEqual(got, self.pack([0xB1B1B1B1, 0xC2C2C2C2, 0xA0A0A0A0, 0xD3D3D3D3]))

    def test_in_place_and_into_zero(self):
        source = self.pack([1, 2, 3, 4])
        self.assertEqual(run(mmi("prot3w", 9, 0, 9), {9: source})(9), self.pack([2, 3, 1, 4]))
        self.assertEqual(run(mmi("prot3w", 0, 0, 9), {9: source})(0), 0)


if __name__ == "__main__":
    unittest.main()
