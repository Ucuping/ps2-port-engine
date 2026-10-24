"""Tests for function discovery's "does this look like code" judgement.

The check has to reject data that merely decodes - runs of zeroes are valid
`nop`s - while accepting real functions, including the very short ones: a getter
is `jr $ra; lw $v0, 8($a0)`, and an empty function is just `jr $ra; nop`. Getting
the short ones wrong lost 424 call targets, one called from 866 sites.
"""

import os
import struct
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

from ps2decomp.analysis import ImageAnalyzer

BASE = 0x00100000
JR_RA = 0x03E00008
NOP = 0
LW_V0_8_A0 = (0x23 << 26) | (4 << 21) | (2 << 16) | 8
J_ELSEWHERE = (2 << 26) | ((BASE + 0x100) >> 2)
JAL = (3 << 26) | ((BASE + 0x100) >> 2)
B_FORWARD = 0x10000004         # beq $zero, $zero, +4


def analyzer(words):
    words = list(words) + [NOP] * 64
    return ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)


class TestLooksLikeCode(unittest.TestCase):
    def test_getter_with_its_body_in_the_delay_slot(self):
        self.assertTrue(analyzer([JR_RA, LW_V0_8_A0])._looks_like_code(BASE))

    def test_empty_function_is_accepted_only_as_a_call_target(self):
        a = analyzer([JR_RA, NOP])
        self.assertTrue(a._looks_like_code(BASE, called=True))
        self.assertFalse(a._looks_like_code(BASE))

    def test_thunk_is_accepted_as_a_call_target(self):
        self.assertTrue(analyzer([J_ELSEWHERE, NOP])._looks_like_code(BASE, called=True))

    def test_zeroes_are_not_code(self):
        self.assertFalse(analyzer([NOP] * 8)._looks_like_code(BASE, called=True))

    def test_empty_delay_slot_does_not_count_as_padding(self):
        """`jal; nop; b; ...` is real code; counting its nop delay slot as padding
        tipped 37 pointer-table entries below the threshold."""
        self.assertTrue(analyzer([JAL, NOP, B_FORWARD, NOP])._looks_like_code(BASE))


class TestSwitches(unittest.TestCase):
    """A switch's cases belong to its function, not to functions of their own."""

    # switch (a0) { case 0: ...; case 1: ...; }, its table at BASE + 0x100.
    CODE = [
        0x2C810002,     # 00 sltiu $at, $a0, 2
        0x1020000A,     # 04 beq $at, $zero, 0x30
        NOP,            # 08
        0x3C030010,     # 0C lui $v1, 0x0010
        0x00042080,     # 10 sll $a0, $a0, 2
        0x24630100,     # 14 addiu $v1, $v1, 0x0100
        0x00832021,     # 18 addu $a0, $a0, $v1
        0x8C820000,     # 1C lw $v0, 0($a0)
        0x00400008,     # 20 jr $v0
        NOP,            # 24
        0x24420001,     # 28 case 0: addiu $v0, $v0, 1
        JR_RA, NOP,     # 2C
        0x24420002,     # 34 case 1: addiu $v0, $v0, 2
        JR_RA, NOP,     # 38
    ]

    def image(self):
        words = self.CODE + [NOP] * (0x40 - len(self.CODE))
        words += [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        return ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)

    def test_the_table_is_read_from_the_code_before_the_jump(self):
        self.assertEqual(self.image().jump_table(BASE + 0x20, 2), [BASE + 0x28, BASE + 0x34])

    def test_the_low_half_may_be_in_the_load(self):
        # lui $v1, 0x0010; addu $v1, $v1, $a0; lw $v1, 0x0100($v1); jr $v1
        code = list(self.CODE)
        code[3:9] = [0x00042080, 0x3C030010, 0x00641821, NOP, 0x8C630100, 0x00600008]
        words = code + [NOP] * (0x40 - len(code)) + [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        a = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)
        self.assertEqual(a.jump_table(BASE + 0x20, 3), [BASE + 0x28, BASE + 0x34])

    def test_the_table_may_be_summed_into_another_register(self):
        # CodeWarrior: lui $v0, 0x0010; addiu $v1, $v0, 0x0100; sll $v0, $a0, 2;
        # addu $v0, $v0, $v1; lw $v0, 0($v0); jr $v0
        code = list(self.CODE)
        code[3:9] = [0x3C020010, 0x24430100, 0x00041080, 0x00431021, 0x8C420000, 0x00400008]
        words = code + [NOP] * (0x40 - len(code)) + [BASE + 0x28, BASE + 0x34] + [NOP] * 32
        a = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE)
        self.assertEqual(a.jump_table(BASE + 0x20, 2), [BASE + 0x28, BASE + 0x34])

    def test_cases_are_labels_of_the_switch(self):
        a = self.image()
        a.run()
        self.assertEqual(sorted(a.functions), [BASE])
        self.assertEqual(a.functions[BASE].end, BASE + 0x40)



class TestScheduledEntry(unittest.TestCase):
    """An instruction scheduled above the frame setup belongs to the function.

        f:  jr $ra; nop
        g:  lui $v0, 0x21; addiu $sp, $sp, -0x10; lw $v1, 0($v0); ...; jr $ra
    """

    LUI_V0 = (0x0F << 26) | (2 << 16) | 0x21
    ADDIU_SP = (0x09 << 26) | (29 << 21) | (29 << 16) | 0xFFF0
    ADDIU_SP_BACK = (0x09 << 26) | (29 << 21) | (29 << 16) | 0x10
    LW_V1_V0 = (0x23 << 26) | (2 << 21) | (3 << 16)

    def run_on(self, words):
        a = analyzer(words)
        a.run()
        return sorted(a.functions)

    def test_the_function_starts_at_the_scheduled_instruction(self):
        words = [JR_RA, NOP, self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertIn(BASE + 8, self.run_on(words))
        self.assertNotIn(BASE + 12, self.run_on(words))

    def test_padding_may_come_between(self):
        words = [JR_RA, NOP, NOP, self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertIn(BASE + 12, self.run_on(words))
        self.assertNotIn(BASE + 16, self.run_on(words))

    def test_five_scheduled_instructions(self):
        # 0x001ADF00: sll, lui, addu, sll, sll, then the frame setup.
        SLL_V0 = (2 << 11) | (4 << 16) | (1 << 6)
        words = [JR_RA, NOP] + [SLL_V0] * 4 + [self.LUI_V0, self.ADDIU_SP, self.LW_V1_V0,
                                               JR_RA, self.ADDIU_SP_BACK]
        self.assertIn(BASE + 8, self.run_on(words))
        self.assertNotIn(BASE + 28, self.run_on(words))

    def test_a_branch_before_the_setup_is_not_taken_in(self):
        words = [JR_RA, NOP, B_FORWARD, self.ADDIU_SP, self.LW_V1_V0, JR_RA,
                 self.ADDIU_SP_BACK]
        self.assertNotIn(BASE + 8, self.run_on(words))


class TestTablesAnywhere(unittest.TestCase):
    """A switch's table read wherever the program has bytes (game.json
    "jump_tables_anywhere"): in another section, with its base kept in a
    callee-saved register across a call, and - with no bound in sight - read
    entry by entry under safeguards, flagged as such."""

    TABLE = 0x00200000                  # in a section of its own, past the code
    SLTIU_V0_V1_2 = 0x2C620002
    BEQ_V0_DEFAULT = 0x1040000A         # beq $v0, $zero, 0x38 (from 0x10)

    def switch(self, bound=True):
        """lui $s3, 0x20; jal; [sltiu $v0, $v1, 2]; beq $v0, $zero, default;
        sll $v0, $v1, 2; addiu $v1, $s3, 0; addu $v0, $v0, $v1; lw $a0, 0($v0);
        jr $a0 - Tekken 5's game program at 0x004881C4 - cases at 0x30, 0x38."""
        return [0x3C130020, 0x0C000000 | ((BASE + 0x100) >> 2), NOP,
                self.SLTIU_V0_V1_2 if bound else NOP, self.BEQ_V0_DEFAULT, NOP,
                0x00031080, 0x26630000, 0x00431021, 0x8C440000, 0x00800008, NOP,
                JR_RA, NOP, JR_RA, NOP] + [NOP] * 48

    def analyzer(self, words, table, anywhere=True, run_entries=None, cls=ImageAnalyzer):
        blob = struct.pack("<%dI" % len(table), *table)
        return cls(struct.pack("<%dI" % len(words), *words), BASE, BASE, run_entries=run_entries,
                   data_sections=[(blob, self.TABLE)], tables_anywhere=anywhere)

    def test_a_table_in_another_section(self):
        # The near form (TestSwitches), its table moved out of the code.
        words = list(TestSwitches.CODE) + [NOP] * (0x40 - len(TestSwitches.CODE))
        words[3], words[5] = 0x3C030020, 0x24630000         # lui $v1, 0x20; addiu $v1, $v1, 0
        cases = [BASE + 0x28, BASE + 0x34]
        self.assertIsNone(self.analyzer(words, cases, anywhere=False).jump_table(BASE + 0x20, 2))
        a = self.analyzer(words, cases)
        self.assertEqual(a.jump_table(BASE + 0x20, 2, BASE), cases)
        self.assertEqual(a.tables[BASE + 0x20], (self.TABLE, 2, True))

    def test_a_base_kept_across_a_call(self):
        a = self.analyzer(self.switch(), [BASE + 0x30, BASE + 0x38])
        self.assertEqual(a.jump_table(BASE + 0x28, 4, BASE), [BASE + 0x30, BASE + 0x38])
        self.assertEqual(a.tables[BASE + 0x28], (self.TABLE, 2, True))

    def test_without_a_bound_read_while_the_entries_name_the_switch(self):
        cases = [BASE + 0x30, BASE + 0x38]
        a = self.analyzer(self.switch(bound=False), cases + [0x00300000, BASE + 0x30])
        self.assertEqual(a.jump_table(BASE + 0x28, 4, BASE), cases)
        self.assertEqual(a.tables[BASE + 0x28], (self.TABLE, 2, False))    # flagged: no bound

    def test_the_safeguards_end_an_unbounded_table(self):
        switch = self.switch(bound=False)
        one = [BASE + 0x30]
        # An entry not word-aligned.
        self.assertEqual(self.analyzer(switch, one + [BASE + 0x3A]).jump_table(BASE + 0x28, 4, BASE), one)
        # The section's end.
        self.assertEqual(self.analyzer(switch, one).jump_table(BASE + 0x28, 4, BASE), one)
        # Where the code builds another address: lui $t0, 0x20; addiu $t0, $t0, 4.
        other = switch[:0x30] + [0x3C080020, 0x25080004] + switch[0x32:]
        self.assertEqual(self.analyzer(other, one + [BASE + 0x38]).jump_table(BASE + 0x28, 4, BASE), one)
        # The next start the analysis is sure of ends the switch's region.
        self.assertEqual(self.analyzer(switch, one + [BASE + 0x38], run_entries={BASE + 0x38: "pointer"})
                         .jump_table(BASE + 0x28, 4, BASE), one)

        class Small(ImageAnalyzer):
            MAX_UNBOUNDED_CASES = 1
        self.assertEqual(self.analyzer(switch, one + [BASE + 0x38], cls=Small).jump_table(BASE + 0x28, 4, BASE),
                         one)


class TestTableReport(unittest.TestCase):
    """analyze.py reports the tables read and the jumps through a register no
    table explains (a player's build's risky sites) - with jump_tables_anywhere."""

    def test_the_tables_and_the_unexplained_jumps(self):
        import tempfile
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from test_port import tiny_elf, SHF_ALLOC, SHF_EXECINSTR, SHT_PROGBITS
        import analyze
        code = [0x2C810002, 0x1020000A, NOP, 0x3C030011, 0x00042080, 0x24630000, 0x00832021, 0x8C820000,
                0x00400008, NOP,
                0x0C040010, NOP,            # case 0: jal 0x00100040
                JR_RA, NOP,                 # default
                JR_RA, NOP,                 # case 1
                0x24420001, 0x03200008, NOP]    # 0x00100040: addiu $v0, $v0, 1; jr $t9, a tail call
        image = tiny_elf([
            (".text", BASE, struct.pack("<%dI" % len(code), *code), SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS),
            (".rodata", 0x00110000, struct.pack("<2I", BASE + 0x28, BASE + 0x38), SHF_ALLOC, SHT_PROGBITS)])
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "TEST.ELF")
            with open(path, "wb") as handle:
                handle.write(image)
            plain = analyze.build_database(path, verbose=False)
            wide = analyze.build_database(path, verbose=False, tables_anywhere=True)
        self.assertNotIn("jump_tables", plain)
        self.assertEqual(wide["jump_tables"], [{"jr": BASE + 0x20, "table": 0x00110000, "cases": 2,
                                                "bounded": True}])
        self.assertEqual(wide["unexplained_jumps"], [BASE + 0x44])


class TestRunEntries(unittest.TestCase):
    """Code a run reached that no function covered (game.json "run_entries"):
    a sure start, with how the run reached it - unless a function covers it."""

    ADDIU_V0 = (0x09 << 26) | (2 << 21) | (2 << 16) | 1     # addiu $v0, $v0, 1

    def functions(self, words, run_entries=None):
        words = list(words) + [NOP] * 64
        a = ImageAnalyzer(struct.pack("<%dI" % len(words), *words), BASE, BASE,
                          run_entries=run_entries)
        a.run()
        return a.functions

    def test_code_nothing_calls_starts_a_function(self):
        # The entry returns at once; the block after it is reached only by a run.
        words = [JR_RA, NOP, NOP, NOP, self.ADDIU_V0, JR_RA, NOP]
        self.assertEqual(sorted(self.functions(words)), [BASE])
        found = self.functions(words, {BASE + 16: "pointer"})
        self.assertEqual(sorted(found), [BASE, BASE + 16])
        self.assertEqual((found[BASE + 16].confidence, found[BASE + 16].run_kind), ("run", "pointer"))
        self.assertEqual(found[BASE + 16].end, BASE + 28)       # its delay slot with it

    def test_a_start_inside_a_function_cuts_it_short(self):
        # A `j` lands in what the analysis took for the middle of one function.
        words = [self.ADDIU_V0, self.ADDIU_V0, self.ADDIU_V0, JR_RA, NOP]
        self.assertEqual(self.functions(words)[BASE].end, BASE + 20)
        found = self.functions(words, {BASE + 8: "j"})
        self.assertEqual(sorted(found), [BASE, BASE + 8])
        self.assertEqual((found[BASE].end, found[BASE + 8].end), (BASE + 8, BASE + 20))

    def test_a_covered_one_cuts_no_function_short(self):
        # A case of the switch the analysis read: its function covers it.
        words = TestSwitches.CODE + [NOP] * (0x40 - len(TestSwitches.CODE))
        words += [BASE + 0x28, BASE + 0x34]
        found = self.functions(words, {BASE + 0x28: "case"})
        self.assertEqual(sorted(found), [BASE])
        self.assertEqual(found[BASE].end, BASE + 0x40)


if __name__ == "__main__":
    unittest.main()
