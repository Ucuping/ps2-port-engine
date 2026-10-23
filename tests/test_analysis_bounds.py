"""Two choices a game can make about its analysis (ps2decomp/analysis.py): call
targets from the code only, and functions that end at their returns."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp.analysis import ImageAnalyzer  # noqa: E402

BASE = 0x00100000
PROLOGUE, EPILOGUE, JR_RA, NOP = 0x27BDFFF0, 0x27BD0010, 0x03E00008, 0
LI_V0_1 = 0x24020001


def j(target): return 0x08000000 | (target >> 2 & 0x03FFFFFF)
def jal(target): return 0x0C000000 | (target >> 2 & 0x03FFFFFF)
def beqz_a0(pc, target): return 0x10800000 | ((target - (pc + 4)) >> 2 & 0xFFFF)


def analyse(words, **choices):
    analyzer = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE, **choices)
    analyzer.run()
    return {a: f.end for a, f in analyzer.functions.items()}


# A function that tail-calls the next one, after its padding: the traversal
# follows the `j` there and the extent bridges the padding, so the two are one.
TAIL_CALL = [
    PROLOGUE,                               # 0x00 f1
    beqz_a0(BASE + 0x04, BASE + 0x14),      # 0x04 if a0 == 0: return
    NOP,
    j(BASE + 0x24),                         # 0x0C tail call to f2
    EPILOGUE,
    JR_RA,                                  # 0x14
    EPILOGUE,
    NOP, NOP,                               # 0x1C padding
    PROLOGUE,                               # 0x24 f2, which no jal names
    LI_V0_1,
    JR_RA,
    EPILOGUE,
]

# An early return: the code after the first `jr $ra` is reached by a branch.
EARLY_RETURN = [
    PROLOGUE,                               # 0x00
    beqz_a0(BASE + 0x04, BASE + 0x18),      # 0x04 if a0 == 0: the rest
    NOP,
    JR_RA,                                  # 0x0C the early return
    EPILOGUE,
    NOP,                                    # 0x14 padding
    LI_V0_1,                                # 0x18 the rest
    JR_RA,
    EPILOGUE,
]


class TestEndAtReturns(unittest.TestCase):
    def test_off_the_tail_call_joins_the_functions(self):
        self.assertEqual(analyse(TAIL_CALL), {BASE: BASE + 0x34})

    def test_on_a_function_ends_at_its_return(self):
        functions = analyse(TAIL_CALL, end_at_returns=True)
        self.assertEqual(functions[BASE], BASE + 0x1C)
        self.assertIn(BASE + 0x24, functions)           # found on its own, by its prologue

    def test_an_early_return_is_not_an_end(self):
        self.assertEqual(analyse(EARLY_RETURN, end_at_returns=True), {BASE: BASE + 0x24})

    def test_an_early_return_without_padding_is_not_an_end(self):
        words = list(EARLY_RETURN)
        del words[5]                                     # no padding: the rest at 0x14
        words[1] = beqz_a0(BASE + 0x04, BASE + 0x14)
        self.assertEqual(analyse(words, end_at_returns=True), {BASE: BASE + 0x20})


class TestCallsFromCodeOnly(unittest.TestCase):
    # Code at 0x00-0x10 calls 0x40; 0x20-0x30 is data holding a word that
    # decodes as `jal 0x60`; real functions at 0x40 and 0x60.
    WORDS = [jal(BASE + 0x40), NOP, JR_RA, NOP,                  # 0x00
             0x11111111, 0x22222222, 0x33333333, 0x44444444,    # 0x10 data
             jal(BASE + 0x60), 0x55555555, 0x66666666, 0x77777777,  # 0x20 data
             0x88888888, 0x99999999, 0xAAAAAAAA, 0xBBBBBBBB,    # 0x30 data
             LI_V0_1, JR_RA, NOP, NOP,                          # 0x40
             NOP, NOP, NOP, NOP,                                # 0x50
             LI_V0_1, JR_RA, NOP, NOP]                          # 0x60

    def targets(self, **choices):
        analyzer = ImageAnalyzer(struct.pack("<%dI" % len(self.WORDS), *self.WORDS), BASE, BASE, **choices)
        analyzer.harvest_call_targets()
        return sorted(analyzer.call_sites)

    def test_anywhere_a_data_word_names_a_call(self):
        self.assertEqual(self.targets(), [BASE + 0x40, BASE + 0x60])

    def test_from_the_code_only_it_does_not(self):
        code = [(BASE, BASE + 0x10), (BASE + 0x40, BASE + 0x70)]
        self.assertEqual(self.targets(call_ranges=code), [BASE + 0x40])


if __name__ == "__main__":
    unittest.main()
