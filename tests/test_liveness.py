"""Folding single-use temporaries: what may move, and what must not."""

import os
import sys
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path[:0] = [ROOT, os.path.join(ROOT, "tools")]

from ps2decomp import liveness

SYNCED = {"v0", "v1", "a0", "a1", "t0", "t1"}
RETURNED = {"v0", "v1"}


def fold(code, condition=None, live=()):
    return liveness.fold(code, condition, set(live), SYNCED, RETURNED)


class TestFold(unittest.TestCase):
    def test_a_temporary_folds_into_the_condition_that_uses_it(self):
        code, condition = fold(["v0 = ReadS32((u32)a0 + 0x20u);", "v0 = (u64)((s64)a1 < (s64)v0);"],
                               "v0 != 0ull")
        self.assertEqual(code, [])
        self.assertIn("ReadS32((u32)a0 + 0x20u)", condition)

    def test_a_live_result_is_kept(self):
        code, _ = fold(["v0 = ReadS32((u32)a0);", "v0 = v0 + 1;"], live={"v0"})
        self.assertEqual(code, ["v0 = (ReadS32((u32)a0)) + 1;"])

    def test_a_load_is_not_moved_past_a_store(self):
        code, _ = fold(["t0 = ReadS32((u32)a0);", "Write32((u32)a0, 5u);", "v0 = t0;"])
        self.assertEqual(code[0], "t0 = ReadS32((u32)a0);")

    def test_nothing_moves_into_or_past_a_call(self):
        code, _ = fold(["t0 = a0 & 1;", "out(); Call(c, 0x100u); in();", "v0 = v0 + t0;"])
        self.assertEqual(code[0], "t0 = a0 & 1;")

    def test_an_operand_redefined_in_between_blocks_the_fold(self):
        code, _ = fold(["t0 = a0 + 1;", "a0 = 7;", "v0 = t0;"], live={"a0"})
        self.assertEqual(code[0], "t0 = a0 + 1;")

    def test_two_uses_keep_the_definition(self):
        code, _ = fold(["t0 = a0 + 1;", "v0 = t0 + t0;"])
        self.assertEqual(code[0], "t0 = a0 + 1;")

    def test_a_compound_statement_never_takes_a_fold(self):
        code, _ = fold(["t0 = lo + 1;", "{ u64 lo = Read64(q); t1 = t0; }"])
        self.assertEqual(code[0], "t0 = lo + 1;")


class TestReferenceOutputs(unittest.TestCase):
    def test_hi_is_not_read_after_the_next_divide(self):
        # CRI's adxt_stat_decinfo (Resident Evil Outbreak 0x001341B0): div; addiu;
        # mfhi; div. mfhi takes the first remainder - the loop's 488-byte skip.
        code, _ = fold(["Divide(hi, lo, v0, t0, true);", "v0 = Sx32(v0 + 0x7FFu);", "v1 = hi;",
                        "Divide(hi, lo, v0, t0, true);", "v1 = Sx32(t0 - v1);", "Write32((u32)a0, (u32)v1);"])
        mfhi = code.index("v1 = hi;") if "v1 = hi;" in code else None
        self.assertIsNotNone(mfhi, code)
        self.assertLess(mfhi, code.index("Divide(hi, lo, v0, t0, true);", 1) if code.count(
            "Divide(hi, lo, v0, t0, true);") > 1 else len(code), code)
        second = [k for k, line in enumerate(code) if line.startswith("Divide(") and k > 0][-1]
        self.assertLess(mfhi, second, code)

    def test_a_multiply_defines_hi_and_lo(self):
        _, defined = liveness.uses_defs("Multiply(hi, lo, a0, a1, true, false); v0 = lo;", SYNCED, RETURNED)
        self.assertEqual(defined, {"hi", "lo", "v0"})

    def test_a_lane_operation_defines_its_destination(self):
        _, defined = liveness.uses_defs("Lanes(kPadduw, v0, c.rh[2], a0, c.rh[4], a1, c.rh[5]);",
                                        SYNCED, RETURNED)
        self.assertEqual(defined, {"v0"})
        # t0 = v0 + 1 may not move past the Lanes that rewrites v0.
        code, _ = fold(["t0 = v0 + 1;", "Lanes(kPadduw, v0, c.rh[2], a0, c.rh[4], a1, c.rh[5]);",
                        "v1 = t0;"])
        self.assertEqual(code[0], "t0 = v0 + 1;")

    def test_nothing_folds_into_a_reference(self):
        code, _ = fold(["hi = a0;", "Multiply(hi, lo, a1, t0, true, true); v0 = lo;"])
        self.assertEqual(code[0], "hi = a0;")


class TestConditionalAssignment(unittest.TestCase):
    def test_a_conditional_assignment_does_not_kill_its_target(self):
        # movn after a shift: the shifted value survives when the condition fails.
        code = ["s1 = (s1 << 1);", "v0 = s1 | a3;", "if (v1 != 0) s1 = v0;"]
        folded, _ = liveness.fold(code, "v0 == 0ull", {"s1"}, {"s1", "v0", "v1", "a3"}, set())
        self.assertIn("s1 = (s1 << 1);", folded)


class _Block:
    def __init__(self, code, kind="cond", taken=None, next=None, condition=None):
        self.code, self.kind, self.taken, self.next = code, kind, taken, next
        self.condition, self.slot, self.likely = condition, "", False


class TestLeavingBranch(unittest.TestCase):
    def test_a_branch_out_of_the_function_keeps_everything_live(self):
        # 0x002A5D40: `s3 = 5` then `beq v1, s3` to code past the function's end,
        # which returns s3. The branch is the only use the block sees, so without
        # the exit the 5 folded into the condition and the target returned 0.
        synced = {"v1", "a0", "s3"}
        blocks = {0x100: _Block(["s3 = 0x5ull;", "v1 = ReadS32((u32)a0 + 0x8u);"],
                                taken=0x900, next=0x110, condition="v1 == s3"),
                  0x110: _Block(["s3 = 0x2ull;"], kind="return")}
        live = liveness.live_out(blocks, {0x100: [0x110], 0x110: []}, synced, set(),
                                 lambda b: b.condition, lambda b: b.slot)
        self.assertIn("s3", live[0x100])
        code, _ = liveness.fold(blocks[0x100].code, "v1 == s3", live[0x100], synced, set())
        self.assertIn("s3 = 0x5ull;", code)



class TestLikelySlot(unittest.TestCase):
    def test_a_likely_slot_kills_nothing_on_the_untaken_path(self):
        # Tekken 5's 0x0032C740, a map's erase: `lw v1, 0(a2)`, `bne v1, a3`; then
        # `bnel v0, a3` with `addiu v1, a2, 4` in its slot, and `sw v1, 0(t0)` on
        # the path it does not take - where the slot never ran and v1 is still
        # the loaded child. Counting the slot's definition on both paths folded
        # the load into the bne's condition and dropped it: the store wrote
        # a2 + 4, the map's tree got a cycle, and the game spun in its lookup.
        synced = {"v0", "v1", "a2", "a3", "t0"}
        blocks = {0x100: _Block(["v1 = ReadS32((u32)a2);"], taken=0x120, next=0x110,
                                condition="v1 != a3"),
                  0x110: _Block(["Write32((u32)t0, (u32)v0);"], kind="return"),
                  0x120: _Block([], taken=0x140, next=0x130, condition="v0 != a3"),
                  0x130: _Block(["Write32((u32)t0, (u32)v1);"], kind="return"),
                  0x140: _Block(["Write32((u32)t0, (u32)ReadS32((u32)v1));"], kind="return")}
        blocks[0x100].slot = "v0 = ReadS32((u32)a2 + 0x4u);"
        blocks[0x120].slot, blocks[0x120].likely = "v1 = Sx32(a2 + 0x4u);", True
        succs = {0x100: [0x110, 0x120], 0x110: [], 0x120: [0x130, 0x140], 0x130: [], 0x140: []}
        live = liveness.live_out(blocks, succs, synced, set(), lambda b: b.condition, lambda b: b.slot)
        self.assertIn("v1", live[0x100])
        code, condition = liveness.fold(blocks[0x100].code, "v1 != a3", live[0x100], synced, set(),
                                        [blocks[0x100].slot])
        self.assertEqual((code, condition), (["v1 = ReadS32((u32)a2);"], "v1 != a3"))

    def test_a_slot_that_always_runs_still_kills(self):
        # The same shape with a plain branch: its slot runs on both paths, so the
        # loaded v1 is dead past it and folds as before.
        synced = {"v0", "v1", "a2", "a3", "t0"}
        blocks = {0x100: _Block(["v1 = ReadS32((u32)a2);"], taken=0x120, next=0x110,
                                condition="v1 != a3"),
                  0x110: _Block(["Write32((u32)t0, (u32)v0);"], kind="return"),
                  0x120: _Block([], taken=0x140, next=0x130, condition="v0 != a3"),
                  0x130: _Block(["Write32((u32)t0, (u32)v1);"], kind="return"),
                  0x140: _Block(["Write32((u32)t0, (u32)v1);"], kind="return")}
        blocks[0x120].slot = "v1 = Sx32(a2 + 0x4u);"
        succs = {0x100: [0x110, 0x120], 0x110: [], 0x120: [0x130, 0x140], 0x130: [], 0x140: []}
        live = liveness.live_out(blocks, succs, synced, set(), lambda b: b.condition, lambda b: b.slot)
        self.assertNotIn("v1", live[0x100])

if __name__ == "__main__":
    unittest.main()
