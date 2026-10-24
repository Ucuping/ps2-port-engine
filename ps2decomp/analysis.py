"""Code discovery for a stripped PS2 executable.

The retail Burnout 3 binary is one flat PROGBITS blob: no symbol table, and no
section headers marking where code stops and read-only data begins.  Everything
about the layout therefore has to be recovered by analysis.

The approach here is deliberately conservative, in the order that produces the
fewest false positives:

1. **Call-target harvesting.**  Every `jal` in the image names a function entry
   directly.  A `jal` that lands inside the image is near-proof of a function,
   because the compiler only emits it for real call sites.
2. **Recursive traversal.**  Starting from the ELF entry point and every
   harvested call target, walk the control-flow graph, following branches and
   jumps, and stop at returns.  This marks bytes that are *provably* reachable
   code rather than bytes that merely decode as code.
3. **Prologue scanning.**  In the gaps, look for the conventional frame setup
   (`addiu $sp, $sp, -N`) to catch functions only reached through function
   pointers - vtables, jump tables, and callback tables, which a 2004 C++ game
   uses heavily.
4. **Data classification.**  Whatever is left is scored: runs that decode poorly,
   or that look like pointers, floats, or strings, are recorded as data.

Each stage records *why* an address was classified, so the results stay
auditable instead of being an opaque guess.
"""

from __future__ import annotations

import bisect
import collections
import dataclasses
import struct

from .r5900 import DISASM, Flags, GPR
from .signatures import written_register

# The registers a call keeps (s0-s7, $gp, $sp, $fp): a value in one of them
# outlives the calls between where it is set and where it is used.
CALLEE_SAVED = frozenset(range(16, 24)) | {28, 29, 30}
# The loads and stores whose offset a `lui` on their base register completes
# into an address (lb ... sd, lq/sq, the COP1 and COP2 ones).
_BASED_ACCESSES = frozenset(range(0x20, 0x30)) | {0x1A, 0x1B, 0x1E, 0x1F, 0x31, 0x36, 0x37, 0x39, 0x3E, 0x3F}

# Confidence levels attached to each discovered function.
CONF_ENTRY = "entry"          # the ELF entry point
CONF_CALLED = "called"        # at least one direct jal targets it
CONF_PROLOGUE = "prologue"    # found by frame-setup pattern
CONF_POINTER = "pointer"      # referenced by a plausible pointer word
CONF_RUN = "run"              # a run reached it, and no function covered it (run entries)

# The run entries that are pieces of a function, never functions themselves:
# a switch's case, a label a branch leads to (game.RUN_ENTRY_KINDS).
PIECE_KINDS = ("case", "branch")


@dataclasses.dataclass
class Function:
    """A discovered function: a half-open address range plus provenance."""

    start: int
    end: int = 0
    confidence: str = CONF_PROLOGUE
    callers: set = dataclasses.field(default_factory=set)
    calls: set = dataclasses.field(default_factory=set)
    instruction_count: int = 0
    has_return: bool = False
    uses_vu: bool = False
    name: str = ""
    # For CONF_RUN: how the run reached it (game.RUN_ENTRY_KINDS).
    run_kind: str = ""

    @property
    def size(self) -> int:
        return max(0, self.end - self.start)

    @property
    def label(self) -> str:
        return self.name or ("func_%08X" % self.start)


@dataclasses.dataclass
class Region:
    """A classified span of the image."""

    start: int
    end: int
    kind: str  # "code" | "data" | "unknown"

    @property
    def size(self) -> int:
        return self.end - self.start


class ImageAnalyzer:
    """Recovers functions and code/data boundaries from a flat loaded image."""

    def __init__(self, data: bytes, base_addr: int, entry_point: int = 0, call_ranges=None,
                 end_at_returns: bool = False, run_entries=None, data_sections=None,
                 tables_anywhere: bool = False) -> None:
        self.data = data
        self.base = base_addr
        self.end = base_addr + len(data)
        self.entry_point = entry_point
        # Where a jal counts (game.json "calls_from_code_only"): an overlay's image
        # holds data that decodes as jal too, and its "calls" split real functions
        # (Tekken 5's game program: 101 of them). None: anywhere in the image.
        self.call_ranges = list(call_ranges) if call_ranges else None
        # Whether a function ends after a `jr $ra` that padding or the next
        # function's prologue follows (game.json "end_at_returns", _cut_after_return).
        self.end_at_returns = end_at_returns
        # Code a run reached that no function covered, {address: kind}
        # (game.json "run_entries"): sure starts. None: none.
        self.run_entries = dict(run_entries) if run_entries else {}
        # A switch's table read wherever the program has bytes, and when the
        # near reading misses it, its base and bound followed further back, or
        # its length taken from its words (game.json "jump_tables_anywhere",
        # _jump_table_far): the program's other loaded sections, [(bytes,
        # address)]. Off: the code section's tables alone, as before.
        self.tables_anywhere = tables_anywhere
        self.data_sections = [(bytes(blob), address) for blob, address in (data_sections or ())]
        self.tables: dict = {}            # jr -> (table, cases, bounded) of each table read
        self.unexplained: set = set()     # jr's through a register no table explains
        self._references = None           # what the code builds as data addresses
        self._sure_starts = None
        self.words = struct.unpack("<%dI" % (len(data) // 4), data[: (len(data) // 4) * 4])
        self.functions: dict = {}
        self.code_bytes = bytearray(len(data))  # 1 where traversal proved code
        self.call_sites: dict = collections.defaultdict(set)
        self.case_targets: set = set()    # labels reached through switch jump tables
        self.stats = collections.Counter()

    # -- address helpers ----------------------------------------------

    def contains(self, addr: int) -> bool:
        return self.base <= addr < self.end

    def word_at(self, addr: int) -> int:
        if not self.contains(addr) or (addr - self.base) % 4:
            return 0
        return self.words[(addr - self.base) // 4]

    def _index(self, addr: int) -> int:
        return addr - self.base

    # -- stage 1: harvest direct call targets --------------------------

    def harvest_call_targets(self) -> int:
        """Scan every aligned word for `jal` and record its target.

        This deliberately scans the whole image rather than only known code: a
        `jal` encoding appearing inside genuine data is rare, and the traversal
        stage discards targets that do not hold sane code.
        """
        found = 0
        for i, word in enumerate(self.words):
            if (word >> 26) != 0x03:  # jal
                continue
            addr = self.base + i * 4
            if self.call_ranges is not None and not any(low <= addr < high for low, high in self.call_ranges):
                continue
            target = ((addr + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            if not self.contains(target):
                continue
            self.call_sites[target].add(addr)
            found += 1
        return found

    # -- stage 2: recursive control-flow traversal ---------------------

    def _looks_like_code(self, addr: int, probe: int = 8, called: bool = False) -> bool:
        """Cheap sanity check that `addr` begins a decodable instruction run.

        Decoding successfully is not enough on its own: a zero word decodes as a
        perfectly valid `nop`, so a run of zeroes in read-only data passes any
        test that only asks "does this decode?".  Real code therefore also has to
        carry some actual work - at least two non-`nop` instructions, and not be
        mostly padding.
        """
        valid = 0
        meaningful = 0
        for k in range(probe):
            a = addr + k * 4
            if not self.contains(a):
                break
            ins = DISASM.decode(self.word_at(a), a)
            if not ins.is_valid:
                return False
            valid += 1
            if not ins.is_nop:
                meaningful += 1
            if ins.ends_block:
                # The delay slot executes too, and is often the whole body: a getter
                # is `jr $ra; lw $v0, 8($a0)`. Not counting it rejected 424 call
                # targets - one of them called from 866 sites - as "not code".
                # An empty delay slot counts for nothing either way: it is neither
                # work nor padding, and counting it as padding tipped 37 real
                # entries over the padding ratio.
                slot = a + 4
                if ins.has_delay_slot and self.contains(slot):
                    slot_ins = DISASM.decode(self.word_at(slot), slot)
                    if not slot_ins.is_valid:
                        return False
                    if not slot_ins.is_nop:
                        valid += 1
                        meaningful += 1
                break
        if called and valid == 1 and meaningful == 1:
            # A `jal` target that is just `jr $ra; nop` or `j elsewhere; nop` is an
            # empty function or a thunk. Data that happens to decode that way is
            # not what a call instruction points at.
            first = DISASM.decode(self.word_at(addr), addr)
            if first.is_return or first.mnemonic in ("j", "b"):
                return True
        if valid < 2 or meaningful < 2:
            return False
        # Padding-dominated windows are data, not a function body.
        return meaningful * 2 > valid

    def traverse(self, start: int, func: Function) -> set:
        """Walk one function's control-flow graph, marking reachable bytes.

        Returns the set of addresses proven to belong to this function, which the
        caller turns into an extent.  The traversal itself must not decide the
        extent: a tail jump or a shared epilogue can land far away, and treating
        the furthest address reached as the end merges everything in between into
        one enormous bogus function.
        """
        queue = collections.deque([start])
        seen = set()
        max_addr = start
        while queue:
            addr = queue.popleft()
            while True:
                if not self.contains(addr) or addr in seen:
                    break
                seen.add(addr)
                idx = self._index(addr)
                ins = DISASM.decode(self.words[idx // 4], addr)
                if not ins.is_valid:
                    break
                self.code_bytes[idx : idx + 4] = b"\x01\x01\x01\x01"
                func.instruction_count += 1
                if addr > max_addr:
                    max_addr = addr
                if ins.mnemonic.startswith("v") or ins.mnemonic in ("qmfc2", "qmtc2"):
                    func.uses_vu = True

                # The delay slot always executes, so claim it before branching.
                if ins.has_delay_slot:
                    slot = addr + 4
                    if self.contains(slot):
                        self.code_bytes[self._index(slot) : self._index(slot) + 4] = b"\x01\x01\x01\x01"
                        seen.add(slot)
                        if slot > max_addr:
                            max_addr = slot

                if ins.is_call:
                    if ins.target >= 0 and self.contains(ins.target):
                        func.calls.add(ins.target)
                elif ins.target >= 0 and self.contains(ins.target):
                    # Intra-function branch or tail jump: queue the target.
                    queue.append(ins.target)

                if ins.mnemonic == "jr" and ins.rs != 31:
                    # A switch: its cases are this function's own code.
                    targets = self.jump_table(addr, ins.rs, func.start)
                    if targets:
                        queue.extend(targets)
                        self.case_targets.update(targets)
                        self.stats["jump_tables"] += 1
                    else:
                        self.unexplained.add(addr)

                if ins.is_return:
                    func.has_return = True
                    break
                if ins.ends_block:
                    break

                addr += 8 if ins.has_delay_slot else 4
        return seen

    # How far back from a `jr` its table and bound are looked for.
    JUMP_TABLE_WINDOW = 32
    MAX_CASES = 1024

    def jump_table(self, jr_addr: int, register: int, func_start: int = None):
        """The targets of the switch that ends in `jr register`, or None: as
        _jump_table_near reads it, then, with jump_tables_anywhere, as
        _jump_table_far does. Each table read is kept in `tables`."""
        near = self._jump_table_near(jr_addr, register)
        if near is not None:
            targets, table = near
            self.tables[jr_addr] = (table, len(targets), True)
            return targets
        if not self.tables_anywhere:
            return None
        far = self._jump_table_far(jr_addr, register, self.base if func_start is None else func_start)
        if far is None:
            return None
        targets, table, bounded = far
        self.tables[jr_addr] = (table, len(targets), bounded)
        if not bounded:
            self.stats["jump_tables_unbounded"] += 1
        return targets

    def _jump_table_near(self, jr_addr: int, register: int):
        """(targets, table) of the switch that ends in `jr register`, or None.

        GCC's switch on the R5900 bounds the index, then loads the case's
        address from a table and jumps to it:

            sltiu $at, $i, N;  beq $at, $zero, default; ...
            lui $t, %hi(T);  sll $i, $i, 2;  addiu $t, $t, %lo(T)
            addu $i, $i, $t;  lw $r, 0($i);  jr $r

        Walking back from the `jr` finds the load, the sum, the table's two
        halves and the bound; the table's N words are the cases. Without the
        bound the table's length is unknown, and nothing is claimed.
        """
        load = index = low = table = bound = None
        register_of_sum = register
        addr = jr_addr - 4
        for _ in range(self.JUMP_TABLE_WINDOW):
            if not self.contains(addr):
                break
            ins = DISASM.decode(self.word_at(addr), addr)
            if not ins.is_valid or ins.is_call:
                break
            m = ins.mnemonic
            if load is None:
                if m == "lw" and ins.rt == register_of_sum:
                    load = ins
                    register_of_sum = ins.rs
            elif table is None:
                if index is None and m in ("addu", "daddu") and ins.rd == register_of_sum:
                    index = (ins.rs, ins.rt)
                elif index and low is None and m == "addiu" and ins.rt in index:
                    # The lui is the addiu's source: GCC adds in place
                    # (`addiu $t, $t, lo`), CodeWarrior into another register
                    # (`lui $v0, hi; addiu $v1, $v0, lo; sll $v0, $i, 2`).
                    low = (ins.rs, ins.imm)
                elif m == "lui" and ((low and ins.rt == low[0]) or (index and not low and ins.rt in index)):
                    # The table's low half is in the addiu, or else in the load's
                    # own offset: `lui $t, hi; addu $t, $t, $i; lw $r, lo($t)`.
                    table = ((ins.imm << 16) + (low[1] if low else 0) + load.imm) & 0xFFFFFFFF
            elif m == "sltiu":
                bound = ins.imm
                break
            addr -= 4
        if table is None or not bound or bound > self.MAX_CASES:
            return None
        targets = []
        for i in range(bound):
            target = self._table_word(table + 4 * i)
            if target is None or not self.contains(target) or target % 4:
                return None             # not a table of code addresses after all
            targets.append(target)
        return targets, table

    # -- tables anywhere (game.json "jump_tables_anywhere") ----------------

    # How far back a switch's pieces are followed, and how many entries a
    # table no bound sizes may have.
    FAR_WINDOW = 256
    MAX_UNBOUNDED_CASES = 256

    def _table_word(self, addr: int):
        """The word at `addr` in the image - or, with jump_tables_anywhere, in
        any of the program's sections - or None where it has none. Off, as
        word_at: 0 outside the image."""
        if not self.tables_anywhere:
            return self.word_at(addr)
        if addr % 4:
            return None
        if self.contains(addr):
            return self.word_at(addr)
        for blob, base in self.data_sections:
            if base <= addr and addr + 4 <= base + len(blob):
                return struct.unpack_from("<I", blob, addr - base)[0]
        return None

    def _section_of(self, addr: int):
        """(start, end) of the image or section holding `addr`, or None."""
        if self.contains(addr):
            return self.base, self.end
        for blob, base in self.data_sections:
            if base <= addr < base + len(blob):
                return base, base + len(blob)
        return None

    def _setter(self, register: int, before: int, start: int):
        """The instruction that last set `register` before `before`, walking
        back to `start` in address order; None past a call for a register a
        call does not keep, or if nothing sets it within FAR_WINDOW."""
        addr = before - 4
        for _ in range(self.FAR_WINDOW):
            if addr < start or not self.contains(addr):
                return None
            word = self.word_at(addr)
            ins = DISASM.decode(word, addr)
            if not ins.is_valid:
                return None
            if register not in CALLEE_SAVED:
                previous = DISASM.decode(self.word_at(addr - 4), addr - 4) if addr - 4 >= start else None
                if ins.is_call or (previous is not None and previous.is_call):
                    return None         # the call (or its delay slot) is where it was last set
            if not ins.is_call and written_register(word) == register:
                return ins
            addr -= 4
        return None

    def _constant(self, register: int, before: int, start: int, depth: int = 0):
        """The address `register` holds before `before`, built by `lui` and
        `addiu`/`daddiu`/`ori` or moved from another register; None if it is
        not such a constant."""
        if register == 0:
            return 0
        setter = self._setter(register, before, start)
        if setter is None or depth > 4:
            return None
        m = setter.mnemonic
        if m == "lui":
            return (setter.imm << 16) & 0xFFFFFFFF
        if m in ("addiu", "daddiu", "ori"):
            up = self._constant(setter.rs, setter.addr, start, depth + 1)
            if up is None:
                return None
            return (up | (setter.imm & 0xFFFF)) if m == "ori" else (up + setter.imm) & 0xFFFFFFFF
        if m in ("addu", "daddu", "or") and 0 in (setter.rs, setter.rt):
            return self._constant(setter.rt or setter.rs, setter.addr, start, depth + 1)
        return None

    def _bound(self, index: int, before: int, start: int):
        """N of the `sltiu $x, index, N` that bounds a switch on `index`,
        looked for back from `before` until `index` is set; None if none."""
        addr = before - 4
        for _ in range(self.FAR_WINDOW):
            if addr < start or not self.contains(addr):
                return None
            word = self.word_at(addr)
            ins = DISASM.decode(word, addr)
            if not ins.is_valid:
                return None
            if ins.mnemonic == "sltiu" and ins.rs == index:
                return ins.imm
            if (ins.is_call and index not in CALLEE_SAVED) or written_register(word) == index:
                return None
            addr -= 4
        return None

    def _data_references(self) -> set:
        """The addresses the code builds as data - a `lui` completed by an
        `addiu`, `daddiu`, `ori`, or a load's or store's offset on the same
        register a few instructions on - each the start of something: a
        table, a string, a symbol."""
        if self._references is None:
            found, upper = set(), {}
            for i, word in enumerate(self.words):
                op, rs, rt = word >> 26, (word >> 21) & 31, (word >> 16) & 31
                if op == 0x0F:
                    upper[rt] = ((word & 0xFFFF) << 16, i)
                    continue
                if rs in upper and i - upper[rs][1] <= 8:
                    imm = word & 0xFFFF
                    signed = imm - 0x10000 if imm & 0x8000 else imm
                    if op in (0x09, 0x19) or op in _BASED_ACCESSES:
                        found.add((upper[rs][0] + signed) & 0xFFFFFFFF)
                    elif op == 0x0D:
                        found.add(upper[rs][0] | imm)
            self._references = found
        return self._references

    def _next_sure_start(self, address: int) -> int:
        """The first function start after `address` the analysis is sure of:
        a call's target, the entry, a run entry that is no piece."""
        if self._sure_starts is None:
            starts = set(self.call_sites)
            starts.update(a for a, kind in self.run_entries.items() if kind not in PIECE_KINDS)
            if self.contains(self.entry_point):
                starts.add(self.entry_point)
            self._sure_starts = sorted(starts)
        i = bisect.bisect_right(self._sure_starts, address)
        return self._sure_starts[i] if i < len(self._sure_starts) else self.end

    def _jump_table_far(self, jr_addr: int, register: int, func_start: int):
        """(targets, table, bounded) of a switch the near reading misses, or
        None. Each piece is followed back to the instruction that sets it -
        past calls for a register a call keeps: the load into the `jr`'s
        register, the sum of the table's address and the scaled index, the
        address itself (Tekken 5's game program keeps one in $s3 across
        calls), and the bound: the `sltiu` on the index before it is scaled.
        With a bound, every target must lie in the switch's own region (from
        its function's start to the next start the analysis is sure of);
        without one, the table is read under _unbounded's safeguards."""
        load = self._setter(register, jr_addr, func_start)
        if load is None or load.mnemonic != "lw":
            return None
        total = self._setter(load.rs, load.addr, func_start)
        if total is None or total.mnemonic not in ("addu", "daddu"):
            return None
        base = index = None
        for first, second in ((total.rs, total.rt), (total.rt, total.rs)):
            value = self._constant(first, total.addr, func_start)
            if value is not None and value:
                base, index = value, second
                break
        if base is None:
            return None
        table = (base + load.imm) & 0xFFFFFFFF
        if self._section_of(table) is None:
            return None
        low, high = func_start, self._next_sure_start(jr_addr)
        scale = self._setter(index, total.addr, func_start)
        bound = None
        if scale is not None and scale.mnemonic == "sll" and scale.imm == 2:
            bound = self._bound(scale.rt, scale.addr, func_start)
        if bound is not None and bound > 0:
            if bound > self.MAX_CASES:
                return None
            targets = [self._table_word(table + 4 * i) for i in range(bound)]
            if any(t is None or t % 4 or not low <= t < high for t in targets):
                return None
            return targets, table, True
        targets = self._unbounded(table, low, high)
        return (targets, table, False) if targets else None

    def _unbounded(self, table: int, low: int, high: int):
        """A table no bound sizes, read entry by entry while each is
        word-aligned and names code of the switch's own region [low, high);
        it ends at the first that does not, at its section's end, where the
        code builds another address (another table, a symbol), or at
        MAX_UNBOUNDED_CASES entries."""
        start, end = self._section_of(table)
        stops = self._data_references() | {t for t, _n, _b in self.tables.values()}
        targets = []
        for i in range(self.MAX_UNBOUNDED_CASES):
            at = table + 4 * i
            if at + 4 > end or (i and at in stops):
                break
            target = self._table_word(at)
            if target is None or target % 4 or not low <= target < high:
                break
            targets.append(target)
        return targets

    # A function's pieces outside its range add up to no more than this; more is
    # a closure that leaked into other code. Burnout 3's largest, 0x00186850,
    # has 54 KB of them (a state machine's cases).
    MAX_CHUNK_BYTES = 0x10000

    def chunks(self, start: int, end: int, entries) -> list:
        """The function's out-of-line pieces: [(start, end)] outside [start, end).

        The compiler places a function's cold blocks after the hot code, among
        other functions' cold blocks, and reaches them by branch. Following the
        function's branches - conditional, `b` and switch tables, but not `j`,
        which is how it tail-calls, and stopping at the entries of other
        functions - gives every block that is its own. Those outside its range
        are its chunks, each ending in a transfer.
        """
        seen = self.closure(start, entries)
        if seen is None:
            return []                           # not code after all: claim nothing
        runs = []
        for a in sorted(x for x in seen if not (start <= x < end)):
            if runs and a == runs[-1][1]:
                runs[-1][1] = a + 4
            else:
                runs.append([a, a + 4])
        if sum(e - s for s, e in runs) > self.MAX_CHUNK_BYTES:
            return []
        return [(s, e) for s, e in runs]

    def closure(self, start: int, entries):
        """Every instruction a function reaches by its own branches (as
        `chunks` follows them), or None when that runs into what is not code."""
        seen = set()
        queue = [start]
        while queue:
            addr = queue.pop()
            while self.contains(addr) and addr not in seen:
                if addr != start and addr in entries:
                    break
                ins = DISASM.decode(self.word_at(addr), addr)
                if not ins.is_valid:
                    return None
                seen.add(addr)
                if ins.has_delay_slot:
                    seen.add(addr + 4)
                    if not ins.is_call and ins.mnemonic not in ("jr", "jalr", "j") and ins.target >= 0:
                        queue.append(ins.target)
                    if ins.mnemonic == "jr" and ins.rs != 31:
                        queue.extend(self.jump_table(addr, ins.rs) or [])
                if ins.is_return or ins.ends_block:
                    break
                addr += 8 if ins.has_delay_slot else 4
        return seen

    # -- stage 2b: pointer-table targets -------------------------------

    def _is_prologue(self, addr: int) -> bool:
        """True when `addr` holds `addiu $sp, $sp, -N`, or the large frame's
        `lui $at, 0xFFFF; ori $at, $at, lo; addu $sp, $sp, $at` (CodeWarrior's,
        past 32 KB: Mister Mosquito's task entries)."""
        word = self.word_at(addr)
        if (word & self._PROLOGUE_MASK) == self._PROLOGUE_MATCH and bool(word & 0x8000):
            return True
        return self._is_large_prologue(addr)

    def _is_large_prologue(self, addr: int) -> bool:
        if not (self.contains(addr) and self.contains(addr + 8)):
            return False
        return (self.word_at(addr) == 0x3C01FFFF and (self.word_at(addr + 4) >> 16) == 0x3421
                and self.word_at(addr + 8) in (0x03A1E821, 0x03A1E82D))

    def _follows_terminator(self, addr: int) -> bool:
        """True when the instruction two slots back ends a function.

        A function entry is normally preceded by the previous function's final
        delay slot, so the terminating `jr`/`j` sits two words back.
        """
        # Alignment padding (a few `nop`s) may come between: Burnout 3's
        # 0x002A6B80, a leaf reached only through a pointer, follows two, and was
        # missed - its switch's cases taken for functions instead.
        for padding in range(0, self.MAX_PADDING_WORDS + 1):
            terminator = addr - 8 - 4 * padding
            if not self.contains(terminator):
                return False
            if padding and self.word_at(addr - 4 * padding) != 0:
                return False                    # not padding: something else is there
            if DISASM.decode(self.word_at(terminator), terminator).ends_block:
                return True
        return False

    def harvest_pointer_targets(self) -> list:
        """Find function entries referenced only by pointer tables.

        Virtual calls and callback tables never produce a `jal`, so those targets
        are invisible to call-target harvesting.  Every aligned word that points
        at a plausible entry inside the image is considered, then filtered hard -
        a word has to land on a stack prologue or just past another function's
        terminator to be accepted, which is what keeps ordinary data from
        registering as code.
        """
        candidates = set()
        for word in self.words:
            if not self.contains(word) or word % 4:
                continue
            candidates.add(word)

        accepted = []
        for addr in sorted(candidates):
            if addr in self.functions:
                continue
            if not (self._is_prologue(addr) or self._follows_terminator(addr)):
                continue
            if not self._looks_like_code(addr):
                continue
            accepted.append(addr)
        return accepted

    # Alignment padding between functions is a short run of `nop`; bridging that
    # much keeps a function whole without letting a real gap merge two of them.
    MAX_PADDING_WORDS = 4

    # GCC schedules register set-up ahead of a function's stack adjustment: of
    # the 2,759 functions Tekken 5's FDEs describe in its game program, 2,513
    # open with `addiu $sp`, and 2,743 within three instructions.
    SCHEDULED_PROLOGUE_WORDS = 3

    def _opens_frame(self, addr: int) -> bool:
        """True when a function's prologue starts at `addr`: its stack adjustment
        there (_is_prologue), or after at most SCHEDULED_PROLOGUE_WORDS
        instructions that set up registers - no transfer of control, no nop,
        nothing that reads or writes $sp (Tekken 5's 0x0048EE70: `lui $v0, 0x73;
        addiu $sp, $sp, -0x20`). The prologue scan takes the first kind only."""
        for k in range(self.SCHEDULED_PROLOGUE_WORDS + 1):
            at = addr + 4 * k
            if not self.contains(at):
                return False
            if self._is_prologue(at):
                return True
            word = self.word_at(at)
            ins = DISASM.decode(word, at)
            registers = (ins.rs, ins.rt, ins.rd) if word >> 26 in (0x00, 0x1C) else (ins.rs, ins.rt)
            if not word or ins.has_delay_slot or ins.ends_block or 29 in registers:
                return False
        return False

    def _cut_after_return(self, start: int, end: int) -> int:
        """Where a function `start`-`end` stops when it ends at returns: just after
        the first `jr $ra` and its delay slot that padding or the next function's
        prologue (_opens_frame) follows, and that nothing before it in the
        function branches past. An early return's later code is reached by a
        branch - a conditional one, a `b`, a switch's case - and stays the
        function's. A `j` to a function's start - one a `jal` calls, or code that
        opens a frame - is a tail call into that one, not a branch into this: of
        the 393 `j`s in Tekken 5's game program's FDE-bounded functions, none lands
        inside its own function, and 364 land on such a start. The traversal
        follows tail calls, and through them reaches the next function (Tekken 5's
        0x0048EDE0 tail-calls three functions that each tail-call 0x0048EE70,
        right after it). Without such a point, `end`."""
        for addr in range(start, end - 8, 4):
            ins = DISASM.decode(self.word_at(addr), addr)
            if ins.mnemonic != "jr" or ins.rs != 31:
                continue
            cut = addr + 8
            following = cut
            while following < end and self.word_at(following) == 0:
                following += 4
            if following >= end or (following == cut and not self._opens_frame(cut)):
                continue
            into = False
            for at in range(start, cut, 4):
                transfer = DISASM.decode(self.word_at(at), at)
                if transfer.is_call:
                    continue
                if transfer.mnemonic == "jr":
                    targets = (self.jump_table(at, transfer.rs) or []) if transfer.rs != 31 else []
                else:
                    targets = [transfer.target] if transfer.target >= 0 else []
                for target in targets:
                    if not cut <= target < end:
                        continue
                    if transfer.mnemonic == "j" and (target in self.call_sites or self._opens_frame(target)):
                        continue
                    into = True
            if not into:
                return cut
        return end

    def extent_from(self, start: int, visited: set) -> int:
        """Turn a traversal's visited set into a contiguous end address.

        The body of a function is the unbroken run of its own instructions. A gap
        means the run has ended - the next bytes belong to something else - except
        when the gap is only `nop` padding inserted for alignment.
        """
        addr = start
        end = start
        while self.contains(addr):
            if addr in visited:
                addr += 4
                end = addr
                continue
            # Look past a short run of nops for more of this function.
            probe = addr
            padding = 0
            while (
                self.contains(probe)
                and padding < self.MAX_PADDING_WORDS
                and self.word_at(probe) == 0
                and probe not in visited
            ):
                probe += 4
                padding += 1
            if padding and self.contains(probe) and probe in visited:
                addr = probe
                continue
            break
        return max(end, start + 4)

    # -- stage 3: prologue scan ----------------------------------------

    _PROLOGUE_MASK = 0xFFFF0000
    # addiu $sp, $sp, -N  ->  op=0x09 rs=29 rt=29, negative immediate
    _PROLOGUE_MATCH = (0x09 << 26) | (29 << 21) | (29 << 16)

    def scan_prologues(self) -> list:
        """Find `addiu $sp, $sp, -N` in spans not already proven to be code."""
        candidates = []
        # Skip only what lies inside a known function. Bytes merely *reached* by
        # another function's traversal - the target of a tail jump or a shared
        # epilogue - can still begin a function of their own; skipping every
        # visited byte lost 37 functions, one of them already verified, when
        # better call-target discovery made more traversals reach them.
        extents = sorted((f.start, f.end) for f in self.functions.values())
        starts = [start for start, _end in extents]

        def inside_function(addr):
            index = bisect.bisect_right(starts, addr) - 1
            return index >= 0 and extents[index][0] <= addr < extents[index][1]

        for i, word in enumerate(self.words):
            if word == 0x3C01FFFF:              # lui $at, 0xFFFF: perhaps a large frame
                if not self._is_large_prologue(self.base + i * 4):
                    continue
            elif (word & self._PROLOGUE_MASK) != self._PROLOGUE_MATCH:
                continue
            elif not (word & 0x8000):  # immediate must be negative
                continue
            addr = self.base + i * 4
            if inside_function(addr):
                continue
            if not self._looks_like_code(addr):
                continue
            candidates.append(self._scheduled_entry(addr, inside_function))
        return candidates

    # How far back from a frame's setup the entry is looked for. The walk stops
    # at the first branch, jump or padding anyway; four left out 0x001ADF00 of
    # Resident Evil Outbreak, five instructions above its `addiu $sp`, called
    # only from an overlay - whose port stopped there entering a room.
    MAX_SCHEDULED = 16

    def _scheduled_entry(self, addr: int, inside_function) -> int:
        """The entry of a function whose frame is set up at `addr`.

        CodeWarrior schedules an instruction or two above the `addiu $sp`: a
        `lui` of a global's address (`lui $v0, 0x21; addiu $sp, $sp, -0x30;
        lw $v1, -0x6208($v0)`, Resident Evil Outbreak's 0x0011B920), an `srl`.
        Taking the frame setup for the entry left them out, and the function
        read a register it never set. Plain instructions between the previous
        function's last delay slot and the setup are reachable only as this
        function's start, so it begins there; anything else - a branch, a
        jump, padding, another function's code - and it begins at `addr`.
        """
        entry = addr
        for _step in range(self.MAX_SCHEDULED + 1):
            # After a terminator's delay slot, or the padding after it (0x00106880:
            # `lui $v0, 0x21` after a `nop`, the `addiu $sp` at 0x00106884).
            if self._follows_terminator(entry):
                return entry
            before = entry - 4
            if not self.contains(before) or inside_function(before):
                return addr
            word = self.word_at(before)
            ins = DISASM.decode(word, before)
            if word == 0 or ins.is_branch or ins.is_jump or ins.is_return or ins.has_delay_slot:
                return addr
            entry = before
        return addr

    # -- driver ---------------------------------------------------------

    def run(self) -> None:
        self.stats["jal_sites"] = self.harvest_call_targets()
        self.stats["call_targets"] = len(self.call_sites)

        # Seed with the entry point and every direct call target.
        seeds = []
        if self.contains(self.entry_point):
            seeds.append((self.entry_point, CONF_ENTRY))
        for target in sorted(self.call_sites):
            seeds.append((target, CONF_CALLED))

        for addr, confidence in seeds:
            if addr in self.functions:
                continue
            if confidence != CONF_ENTRY and not self._looks_like_code(addr, called=True):
                self.stats["rejected_call_targets"] += 1
                continue
            func = Function(start=addr, confidence=confidence)
            func.callers = set(self.call_sites.get(addr, ()))
            self.functions[addr] = func
            visited = self.traverse(addr, func)
            func.end = self.extent_from(addr, visited)
            if self.end_at_returns:
                func.end = self._cut_after_return(addr, func.end)
            func.instruction_count = sum(
                1 for a in visited if func.start <= a < func.end
            )

        self._seed_run_entries(pieces=False)

        # Functions reached only through pointer tables emit no jal at all, so
        # harvest plausible pointer targets before falling back to a blind scan.
        for addr in self.harvest_pointer_targets():
            if addr in self.functions:
                continue
            func = Function(start=addr, confidence=CONF_POINTER)
            visited = self.traverse(addr, func)
            # A pointer-table entry is a real function - a virtual method or a
            # callback - so it has to return. Without a `jr $ra` the candidate is
            # almost always data that merely decodes: tables of small integers
            # read as instructions on $zero, or pointer arrays pointing at
            # themselves. Requiring a return discards those without needing a
            # hand-maintained list of data ranges.
            # One that opens a stack frame is code all the same: a task's entry
            # never returns (it kills itself, or loops forever).
            if not func.has_return and not self._is_prologue(addr):
                self.stats["pointer_rejected_no_return"] += 1
                continue
            self.functions[addr] = func
            func.end = self.extent_from(addr, visited)
            if self.end_at_returns:
                func.end = self._cut_after_return(addr, func.end)
            func.instruction_count = sum(
                1 for a in visited if func.start <= a < func.end
            )
            self.stats["pointer_found"] += 1

        # A final pass catches anything still missing.
        for addr in self.scan_prologues():
            if addr in self.functions:
                continue
            func = Function(start=addr, confidence=CONF_PROLOGUE)
            self.functions[addr] = func
            visited = self.traverse(addr, func)
            func.end = self.extent_from(addr, visited)
            if self.end_at_returns:
                func.end = self._cut_after_return(addr, func.end)
            func.instruction_count = sum(
                1 for a in visited if func.start <= a < func.end
            )
            self.stats["prologue_found"] += 1

        # A switch's cases are labels of the function that jumps to them. Their
        # table's words point at code just past a `jr` or `b`, which is what the
        # pointer harvest looks for; 2,258 of Burnout 3's 2,986 pointer-found
        # "functions" were cases. Called ones stay: a `jal` is evidence enough.
        for addr in sorted(self.case_targets):
            func = self.functions.get(addr)
            if func is not None and func.confidence in (CONF_POINTER, CONF_PROLOGUE) and not func.callers:
                del self.functions[addr]
                self.stats["switch_cases_not_functions"] += 1

        self._seed_run_entries(pieces=True)
        self._tighten_bounds()
        self._compute_stats()

    def _seed_run_entries(self, pieces: bool) -> None:
        """Start a function at each run entry of one sort (game.json
        "run_entries"): the starts right after the calls' targets, the pieces
        last, once every function is found. A start inside a function is one
        the analysis took for part of it - a `j`'s target, a pointer's - and
        cuts it short (_tighten_bounds). A piece a function covers - inside
        its extent, or code its traversal reached: a case of its switch, a
        label - is that function's own, and a start there would cut it
        wrongly; one none covers keeps its own translation, verified."""
        for addr in sorted(self.run_entries):
            kind = self.run_entries[addr]
            if (kind in PIECE_KINDS) != pieces:
                continue
            if not self.contains(addr):
                self.stats["run_entries_outside"] += 1
                continue
            covered = addr in self.functions or (pieces and (
                self.code_bytes[self._index(addr)]
                or any(f.start < addr < f.end for f in self.functions.values())))
            if covered:
                self.stats["run_entries_covered"] += 1
                continue
            func = Function(start=addr, confidence=CONF_RUN, run_kind=kind)
            self.functions[addr] = func
            visited = self.traverse(addr, func)
            func.end = self.extent_from(addr, visited)
            if self.end_at_returns:
                func.end = self._cut_after_return(addr, func.end)
            func.instruction_count = sum(
                1 for a in visited if func.start <= a < func.end
            )
            self.stats["run_entries"] += 1

    def _tighten_bounds(self) -> None:
        """Clamp each function to end no later than the next function start."""
        starts = sorted(self.functions)
        for i, addr in enumerate(starts):
            func = self.functions[addr]
            limit = starts[i + 1] if i + 1 < len(starts) else self.end
            # Clamp only. The extent already reflects the function's own
            # contiguous code, so growing it here would reintroduce the merging
            # this pass exists to prevent.
            if func.end > limit:
                func.end = limit
            if func.end <= func.start:
                func.end = min(func.start + 4, limit)

    def _compute_stats(self) -> None:
        self.stats["functions"] = len(self.functions)
        self.stats["code_bytes"] = sum(self.code_bytes)
        self.stats["image_bytes"] = len(self.data)
        self.stats["with_return"] = sum(1 for f in self.functions.values() if f.has_return)
        self.stats["uses_vu"] = sum(1 for f in self.functions.values() if f.uses_vu)
        for f in self.functions.values():
            self.stats["conf_" + f.confidence] += 1

    # -- reporting -------------------------------------------------------

    def regions(self) -> list:
        """Collapse the code bitmap into contiguous code/unknown regions."""
        out = []
        start = 0
        current = self.code_bytes[0] if self.code_bytes else 0
        for i in range(1, len(self.code_bytes)):
            if self.code_bytes[i] != current:
                out.append(Region(self.base + start, self.base + i, "code" if current else "unknown"))
                start = i
                current = self.code_bytes[i]
        out.append(
            Region(self.base + start, self.base + len(self.code_bytes), "code" if current else "unknown")
        )
        return out

    def coverage(self) -> float:
        total = len(self.data)
        return (sum(self.code_bytes) / total * 100.0) if total else 0.0
