"""VU0's flags in macro mode (ps2decomp/vu0.py), an instruction at a time: the
clip flag vclipw keeps, and the status flag's Z and S, with their sticky
copies, that the FMAC operations set. The lifted code follows the same model
(runtime/src/vu0.cpp; tests/test_lift.py checks the two against each other)."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import vu0  # noqa: E402
from ps2decomp.r5900 import DISASM  # noqa: E402

COP2 = 0x12 << 26


def f(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def s1(funct, fd, fs, ft, dest=0xF):
    return COP2 | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


def s2(index, fs, ft, dest=0xF):
    return COP2 | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((index >> 2) << 6) | 0x3C | (index & 3)


VSUB, VADD, VMUL, VMAX, VMINI = 0x2C, 0x28, 0x2A, 0x2B, 0x2F
VMULA, VOPMULA, VCLIPW, VDIV = 0x2A, 0x2E, 0x1F, 0x38


class Vu0:
    def __init__(self):
        self.vf = vu0.initial_vf()
        self.vc = vu0.initial_vc()
        self.vacc = [0, 0, 0, 0]

    def run(self, word):
        vu0.execute(self, word, DISASM.decode(word, 0).mnemonic)

    def status(self):
        return self.vc[vu0.REG_STATUS]

    def clip(self):
        return self.vc[vu0.REG_CLIP]


class TestStatus(unittest.TestCase):
    def setUp(self):
        self.m = Vu0()
        self.m.vf[1] = [f(1.0), f(-2.0), f(0.0), f(3.0)]
        self.m.vf[2] = [f(1.0), f(2.0), f(0.0), f(3.0)]

    def test_zero_and_sign_set_their_sticky_copies(self):
        self.m.run(s1(VSUB, 3, 1, 2))                        # (0, -4, 0, 0)
        self.assertEqual(self.m.status(), 0xC3)               # Z, S; ZS, SS
        self.m.run(s1(VADD, 4, 1, 2, dest=0b1000))           # x: 2
        self.assertEqual(self.m.status(), 0xC0)               # Z and S clear; the sticky copies stay

    def test_only_the_fields_dest_names(self):
        self.m.run(s1(VSUB, 3, 1, 2, dest=0b0100))           # y: -4
        self.assertEqual(self.m.status(), 0x82)               # S, SS
        self.m.run(s1(VSUB, 3, 1, 2, dest=0b1001))           # x, w: 0, 0
        self.assertEqual(self.m.status(), 0xC1)               # Z; ZS, and SS from before

    def test_an_operation_into_vf0_flags_its_result(self):
        self.m.run(s1(VSUB, 0, 1, 1))
        self.assertEqual(self.m.vf[0], [0, 0, 0, vu0.ONE])
        self.assertEqual(self.m.status(), 0x41)               # four +0s: Z and ZS, no sign

    def test_the_accumulator_forms_and_the_outer_product(self):
        self.m.run(s2(VMULA, 1, 0))                          # ACC = vf1 * (0, 0, 0, 1) = (0, -0, 0, 3)
        self.assertEqual(self.m.status(), 0xC3)               # -0 is zero, and signed
        self.m.vc[vu0.REG_STATUS] = 0
        self.m.run(s2(VOPMULA, 1, 2, dest=0b1110))           # xyz: y1*z2, z1*x2, x1*y2 = -0, 0, 2
        self.assertEqual(self.m.status(), 0xC3)

    def test_max_mini_and_divide_leave_it(self):
        self.m.run(s1(VSUB, 3, 1, 2))
        for word in (s1(VMAX, 4, 1, 2), s1(VMINI, 5, 1, 2), s2(VDIV, 1, 2) | (1 << 23)):
            self.m.run(word)
            self.assertEqual(self.m.status(), 0xC3)

    def test_ctc2_writes_only_the_sticky_bits(self):
        self.m.run(s1(VSUB, 3, 1, 2))
        vu0.move_to(self.m, COP2 | (6 << 21) | (16 << 11), "ctc2", 0, 0)
        self.assertEqual(self.m.status(), 0x03)
        vu0.move_to(self.m, COP2 | (6 << 21) | (16 << 11), "ctc2", 0xFFFFFFFF, 0)
        self.assertEqual(self.m.status(), 0xFC3)

    def test_cfc2_reads_the_status_and_clip_flags_not_mac(self):
        self.m.run(s1(VSUB, 3, 1, 2))
        self.assertEqual(vu0.move_from(self.m, COP2 | (2 << 21) | (16 << 11), "cfc2"), (0xC3, None))
        self.assertEqual(vu0.move_from(self.m, COP2 | (2 << 21) | (18 << 11), "cfc2"), (0, None))
        with self.assertRaises(vu0.Refused):
            vu0.move_from(self.m, COP2 | (2 << 21) | (17 << 11), "cfc2")


class TestClip(unittest.TestCase):
    def test_judgements_against_the_magnitude_of_w(self):
        m = Vu0()
        m.vf[1] = [f(5.0), f(-5.0), f(0.5), 0]
        m.vf[2] = [0, 0, 0, f(-2.0)]                          # |w| = 2
        m.run(s2(VCLIPW, 1, 2, dest=0b1110))
        self.assertEqual(m.clip(), 0b001001)                  # +x, -y
        m.vf[1] = [f(2.0), f(-2.0), f(-3.0), 0]              # on the bounds: not past them
        m.run(s2(VCLIPW, 1, 2, dest=0b1110))
        self.assertEqual(m.clip(), (0b001001 << 6) | 0b100000)

    def test_the_last_four_kept(self):
        m = Vu0()
        m.vf[1] = [f(9.0), 0, 0, 0]
        m.vf[2] = [0, 0, 0, f(1.0)]
        for _ in range(5):
            m.run(s2(VCLIPW, 1, 2, dest=0b1110))
        self.assertEqual(m.clip(), 0b000001000001000001000001)

    def test_ctc2_sets_its_24_bits(self):
        m = Vu0()
        vu0.move_to(m, COP2 | (6 << 21) | (18 << 11), "ctc2", 0xFFFFFFFF, 0)
        self.assertEqual(m.clip(), 0xFFFFFF)


if __name__ == "__main__":
    unittest.main()
