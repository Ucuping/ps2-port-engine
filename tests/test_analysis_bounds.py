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
LUI_V0, LW_V1_V0, SW_V0_SP = 0x3C020073, 0x8C43F8F4, 0xAFA20000    # lui $v0, 0x73; lw $v1, -0x70c($v0); sw $v0, 0($sp)


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


class TestScheduledPrologues(unittest.TestCase):
    """GCC sets up registers before the stack adjustment (Tekken 5's 0x0048EE70,
    0x0048FA50): that is the next function's prologue too."""

    @staticmethod
    def tail_call_into(*opening):
        # 0x0048FA20's shape: an early exit that tail-calls the next function,
        # a return, and the next function right after - no padding.
        f2 = BASE + 0x20
        return [PROLOGUE, beqz_a0(BASE + 0x04, BASE + 0x14), NOP,
                j(f2), EPILOGUE,                        # 0x0C the tail call
                LI_V0_1, JR_RA, EPILOGUE,               # 0x14 the return
                ] + list(opening) + [LI_V0_1, JR_RA, EPILOGUE]   # 0x20 the next

    def test_set_up_then_the_frame(self):
        functions = analyse(self.tail_call_into(LUI_V0, PROLOGUE), end_at_returns=True)
        self.assertEqual(functions[BASE], BASE + 0x20)

    def test_what_touches_the_stack_first_opens_no_frame(self):
        words = self.tail_call_into(SW_V0_SP, PROLOGUE)
        self.assertEqual(analyse(words, end_at_returns=True), {BASE: BASE + 0x34})

    def test_too_far_from_the_start(self):
        words = self.tail_call_into(LUI_V0, LW_V1_V0, LUI_V0, LW_V1_V0, PROLOGUE)
        self.assertEqual(analyse(words, end_at_returns=True), {BASE: BASE + 0x40})

    @staticmethod
    def through_a_tail_call(named):
        # 0x0048EDE0's shape: an early exit tail-calls f3, which tail-calls f2,
        # the function right after f1's return - so f1's traversal reaches f2.
        f2, f3 = BASE + 0x28, BASE + 0x3C
        return [PROLOGUE, jal(f3) if named else NOP, NOP,
                beqz_a0(BASE + 0x0C, BASE + 0x1C), NOP,
                j(f3), EPILOGUE,                        # 0x14 the tail call
                LI_V0_1, JR_RA, EPILOGUE,               # 0x1C the return
                LUI_V0, PROLOGUE, LI_V0_1, JR_RA, EPILOGUE,    # 0x28 f2
                LI_V0_1, j(f2), NOP]                    # 0x3C f3, a leaf

    def test_a_tail_call_to_a_called_function_is_no_branch_into_this(self):
        self.assertEqual(analyse(self.through_a_tail_call(named=True), end_at_returns=True)[BASE], BASE + 0x28)

    def test_a_jump_to_what_is_no_function_is(self):
        self.assertEqual(analyse(self.through_a_tail_call(named=False), end_at_returns=True)[BASE], BASE + 0x48)

    def test_which_openings_open_a_frame(self):
        def opens(*words):
            image = list(words) + [LI_V0_1, JR_RA, EPILOGUE]
            return ImageAnalyzer(struct.pack("<%dI" % len(image), *image), BASE, BASE)._opens_frame(BASE)
        self.assertTrue(opens(PROLOGUE))
        self.assertTrue(opens(LUI_V0, LW_V1_V0, LUI_V0, PROLOGUE))          # three before
        self.assertFalse(opens(LUI_V0, LW_V1_V0, LUI_V0, LW_V1_V0, PROLOGUE))
        self.assertFalse(opens(SW_V0_SP, PROLOGUE))
        self.assertFalse(opens(NOP, PROLOGUE))
        self.assertFalse(opens(j(BASE + 0x40), PROLOGUE))
        self.assertFalse(opens(LUI_V0, LI_V0_1))


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
