"""Test cases for checking a function against the original machine code.

How a function is tested - the test memory and its seeding, the arguments and
memory profile chosen, running the original and a candidate on each case, and
comparing what they did to memory - shared by derivation (engine/tools/derive.py)
and lifting (liftverify.py). It decides verdicts, so remembered lift verdicts are
keyed by it (verdicts.py); derivation itself is not part of that key.
"""

from __future__ import annotations

import bisect
import hashlib
import random
import re
import struct
import time

from . import layout, structs, testmem

# Instructions one verification case may execute, callees included. Interpreting
# is slow, and a function that calls into a large original routine would otherwise
# spend minutes per case; one that needs more than this is reported untestable.
VERIFY_BUDGET = 100_000
# The callee's own frame. Writes here are not observable effects, so they are
# excluded when comparing what the original and the derivation did to memory.
# The stack, scratch memory and the rest of the layout come from the game; see
# ps2decomp/layout.py.
ARG_SETS = 8

# Why the last test that could not be run failed: (stage, exception). Set wherever
# a test run is abandoned, and read by _attempt to explain an untestable verdict.
_LAST_FAILURE = None
_HEX = re.compile(r"0x[0-9A-Fa-f]+|\b\d+\b")


def _failed(stage, exc) -> None:
    global _LAST_FAILURE
    _LAST_FAILURE = (stage, exc)


def failure_reason() -> str:
    """The last failure as a category: numbers and addresses blanked so that
    failures of one kind count together."""
    if _LAST_FAILURE is None:
        return "unknown"
    stage, exc = _LAST_FAILURE
    message = _HEX.sub("N", str(exc).splitlines()[0] if str(exc) else "")
    return "%s: %s: %s" % (stage, type(exc).__name__, message[:90])


_GRAPHS = {}


def recovered_graph(database, image, base):
    """The struct-type graph for this binary, recovered once per run.

    It is needed to seed global pointers before testing and again to type the
    emitted source; recovering it twice was a fifth of a whole run.
    """
    key = (id(database), base)
    if key not in _GRAPHS:
        _GRAPHS[key] = structs.recover(database, image, base)
    return _GRAPHS[key]


def prepare_test_memory(machine, database, image, base, functions, verbose=False,
                        seeds=None) -> list:
    """Fill scratch and seed null global pointers, identically for every tool.

    Global pointers that are null until the game runs would make every function
    using them untestable, so they are pointed at scratch: first the ones the
    recovered types mark as pointers, then the ones found by running the
    functions (see testmem.discover_null_pointers).

    Returns the addresses seeded. Passing them back as `seeds` reproduces the same
    memory without the discovery - how a worker process matches its parent.
    """
    rng = random.Random(0xB3)
    machine.memory.write(layout.CURRENT.scratch,
                         bytes(rng.randrange(256) for _ in range(layout.CURRENT.scratch_size)))
    testmem.fill_caller_frame(machine.memory)
    if seeds is not None:
        testmem.seed_global_pointers(machine.memory, seeds)
        machine.memory.page_source = testmem.page_contents
        machine.map_faults = True
        return seeds
    static = global_pointers(database, image, base)
    seeded = testmem.seed_global_pointers(machine.memory, static)
    spans = sorted((f["addr"], f["end"]) for f in database["functions"])
    starts = [start for start, _end in spans]
    scratch_low = layout.CURRENT.scratch
    scratch_high = scratch_low + layout.CURRENT.scratch_size

    def is_global(address):
        if not base <= address < machine.memory.base + machine.memory.size:
            return False
        if scratch_low <= address < scratch_high:
            return False
        index = bisect.bisect_right(starts, address) - 1
        return index < 0 or address >= spans[index][1]

    started = time.time()
    found = testmem.discover_null_pointers(machine, functions, is_global)
    if verbose:
        print("  seeded %d null global pointers for testing, and %d more found by "
              "running (%.0fs)" % (seeded, len(found), time.time() - started))
    # From here on a test that faults on unmapped memory maps the page and runs
    # again. Only after the discovery above, which finds null pointers by their
    # faults.
    machine.memory.page_source = testmem.page_contents
    machine.map_faults = True
    return sorted(set(static) | set(found))


def global_pointers(database, image, base) -> set:
    """Addresses of global variables that some code dereferences as a pointer."""
    graph = recovered_graph(database, image, base)
    if structs.GLOBALS not in graph.ids:
        return set()
    fields = graph.fields[graph.find(graph.ids[structs.GLOBALS])]
    return {address & 0xFFFFFFFF for address, field in fields.items()
            if field.target is not None and 4 in field.widths()}


def _effects_digest(memory) -> bytes:
    """A function's net memory effects, reduced to a digest for comparison.

    Only equality is ever asked of the effects in a test, and the original's are
    kept for every verified function so the compiled check can reuse them. As
    dictionaries, a function filling a large buffer cost about a hundred bytes per
    byte written, eight cases over, for every such function - enough to exhaust
    memory once seeded globals let those functions run to completion.
    """
    effects = _journal_effects(memory)
    digest = hashlib.sha1()
    for position in sorted(effects):
        digest.update((position & 0xFFFFFFFF).to_bytes(4, "little"))
        digest.update(bytes((effects[position],)))
    return digest.digest()


def _journal_effects(memory) -> dict:
    """Collapse a write journal into a net address -> byte map.

    Keyed by address, not by the journal's buffer positions: a page mapped on
    demand sits at a different position in every process that mapped pages in a
    different order, and effects recorded in one process are compared in another
    (the parallel derivation's compiled check).

    Comparing journals directly would be wrong: the original and the derivation may
    write the same bytes in a different order, or write one location twice. What has
    to match is the resulting memory, so the journal is reduced to final values.

    Writes into the callee's own stack frame are excluded. They are not observable
    effects - the frame is dead the moment the function returns - and the original
    makes plenty of them spilling registers while a derived expression makes none,
    so counting them would fail every function that has a prologue.
    """
    frame_low = layout.CURRENT.stack_low - memory.base
    frame_high = layout.CURRENT.stack_top + 0x10 - memory.base
    effects = {}
    for offset, previous in getattr(memory, "_journal", None) or ():
        for index in range(len(previous)):
            position = offset + index
            if frame_low <= position <= frame_high:
                continue
            effects[position] = memory.bytes[position]
    return {memory.address_at(position): value for position, value in effects.items()}


def _choose_profile(machine, func, args):
    """Return the first memory profile under which the original executes, or None.

    The profile only decides whether the test can *reach* the function; the original
    and the derivation always read the same memory, so it never changes a verdict.
    """
    for profile in testmem.PROFILES:
        # Pages a failing profile mapped are given back: they would otherwise use
        # up the mapping budget (emu.MAX_PAGES), and the functions tested later
        # would fault for want of a page - a loss that depends on the order.
        mark = machine.memory.page_mark()
        machine.memory.end_journal()
        machine.memory.write(layout.CURRENT.scratch, testmem.build(profile, func["addr"]))
        machine.memory.begin_journal()
        try:
            machine.call(func["addr"], args, max_instructions=VERIFY_BUDGET)
            ran = True
        except Exception as exc:
            _failed("original, every memory profile", exc)
            ran = False
        finally:
            machine.memory.rollback()
            machine.memory.end_journal()
        if ran:
            return profile
        machine.memory.unmap_to(mark)
    return None


def test_plan(func, machine):
    """The argument sets and memory profile a function is tested with, or None.

    Separate from the test itself so that anything else that must face exactly the
    same cases - the compiled C++ check - can ask for them.
    """
    # Seeded from the address so argument sets are stable across passes: a
    # function retried in a later pass must be tested on the same inputs, or a
    # pass-dependent result would be impossible to reproduce.
    local_rng = random.Random(func["addr"])
    arg_sets = [testmem.arguments(local_rng) for _ in range(ARG_SETS - 1)]
    arg_sets.append(tuple(layout.CURRENT.scratch + 16 * i for i in range(8))
                    + tuple(testmem.float_bits(local_rng) for _ in range(8)))
    profile = _choose_profile(machine, func, arg_sets[0])
    if profile is None:
        return _count_plan(func, machine, arg_sets)
    return arg_sets, profile


# Integer arguments tried as a count when every argument a pointer runs out of
# instructions, and the largest count drawn.
COUNT_ARGUMENTS = 4
COUNT_LIMIT = 64


def _count_plan(func, machine, arg_sets):
    """A plan with one integer argument a small count, or None.

    Every integer argument is a pointer into scratch, about 0x01800000 - as a
    length, a copy of 25 MB. A function passing its argument on to memcpy or
    memset ran past the budget on every case and every profile, and was
    untestable (Resident Evil Outbreak: 318 functions, most of them spinning
    in those two). Only then are the first few integer arguments tried as a
    count, one at a time, each case's drawn from its own seed; the first under
    which the original runs is the plan. A function that runs as it was never
    meets these, so no verdict reached before changes.
    """
    if _LAST_FAILURE is None or type(_LAST_FAILURE[1]).__name__ != "ExecutionLimit":
        return None
    for index in range(COUNT_ARGUMENTS):
        rng = random.Random("%d:count:%d" % (func["addr"], index))
        sets = [args[:index] + (rng.randrange(COUNT_LIMIT + 1),) + args[index + 1:]
                for args in arg_sets]
        profile = _choose_profile(machine, func, sets[0])
        if profile is not None:
            return sets, profile
    return None


def run_cases(func, machine, arg_sets, profile, evaluate, record=None, recorded=None,
              float_result=False):
    """Run the original and a candidate on every case; return (outcome, tested).

    `evaluate(args)` returns the candidate's (result, writes) for one case, with
    memory as the original saw it on entry; result is None for a void function and
    writes are (address, value, size) triples. The outcome is "ok", "mismatch" or
    "failed" (the case could not be run at all).

    `record`, a list, receives the original's (result, effects) for each case;
    `recorded` supplies them instead of running the original again. The compiled
    check uses the results recorded while the derivation was verified: they are
    the same runs on the same memory, and repeating them doubled its cost.
    """
    tested = 0
    for case, args in enumerate(arg_sets):
        # Fresh memory of the chosen shape for every case, so the test varies
        # memory as well as arguments. Written outside the journal on purpose:
        # the journal does not nest, and this region is rewritten before every
        # case anyway, so nothing leaks between them.
        machine.memory.end_journal()
        machine.memory.write(layout.CURRENT.scratch,
                             testmem.build(profile, func["addr"] + case))
        # A function with stores is only verified if its *memory effects* match
        # too, so let the original run under a journal, and then check that
        # applying the candidate's stores to the same starting state reproduces
        # exactly what the original produced.
        # Journalling rather than snapshotting a region: these functions write
        # to globals well outside the scratch area, and restoring only scratch
        # left those changes in place - which both corrupted later cases and
        # made correct derivations look wrong.
        if recorded is not None:
            if recorded[case] is None:
                continue                        # the original could not run it
            expected, effects = recorded[case]
        else:
            machine.memory.begin_journal()
            try:
                expected = machine.call(func["addr"], args, max_instructions=VERIFY_BUDGET)
                if float_result:
                    # A function returning a float returns it in $f0.
                    expected = machine.fpr[0] & 0xFFFFFFFF
            except Exception as exc:
                # An input the original cannot finish with - a loop that runs
                # past the budget on this memory, a trap, a fault beyond the
                # pages it may map - is no evidence either way: the case is left
                # out, not the function. Its callers decide how many must run
                # (derive: five of the eight), and a disagreement on any case
                # that did run still fails it.
                _failed("original", exc)
                machine.memory.rollback()
                machine.memory.end_journal()
                if record is not None:
                    record.append(None)
                continue
            effects = _effects_digest(machine.memory)
            machine.memory.rollback()
            if record is not None:
                record.append((expected, effects))

        # The candidate runs inside the replay journal: one that calls an underived
        # function writes memory as it goes - the callee must see the stores made
        # before it - rather than handing back a list of writes at the end.
        machine.memory.begin_journal()
        try:
            actual, writes = evaluate(args)
        except Exception as exc:
            _failed("derivation", exc)
            machine.memory.rollback()
            machine.memory.end_journal()
            return "failed", tested
        try:
            for address, value, size in writes:
                machine.memory.write_int(address, value, size)
        except Exception as exc:
            _failed("derivation's stores", exc)
            machine.memory.rollback()
            machine.memory.end_journal()
            return "failed", tested
        replayed = _effects_digest(machine.memory)
        machine.memory.rollback()
        machine.memory.end_journal()

        tested += 1
        # A void function leaves whatever the caller had in $v0, so there is no
        # return value to compare - only its memory effects carry meaning.
        # Comparing $v0 anyway registered 175 correct derivations as failures.
        if actual is not None and float_result:
            if expected != actual & 0xFFFFFFFF:
                return "mismatch", tested
        elif actual is not None and expected != actual:
            return "mismatch", tested
        if replayed != effects:
            return "mismatch", tested
    return "ok", tested


def reset_failure() -> None:
    global _LAST_FAILURE
    _LAST_FAILURE = None


def _double(value: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", value))[0]


def _single(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


# Arguments a routine with a 64-bit result takes: doubles - soft-float, in the
# integer argument registers - and 64-bit integers, with floats in the float
# registers; and magnitudes past 2^32, which a conversion to a 64-bit integer
# needs before its result is wider than 32 bits. The plan's integer arguments
# are 32-bit values, addresses in scratch, which read as doubles are tiny
# denormals: a double add or multiply returns a result whose upper half is
# zero on every one of them (Tekken 5: 0x00123240, 0x00123300), and so looks
# 32 bits wide; its floats are small, so a float's conversion to a long long
# returns a small one (0x001219B0).
WIDE_PROBES = tuple(
    tuple(integers) + tuple(_single(f) for f in floats)
    for integers, floats in (
        ((_double(1.0), _double(2.0), _double(3.0), _double(-0.5),
          _double(65536.0), _double(1.5), _double(1e10), _double(-3.25)),
         (1.5, -2.0, 3.0, 0.25, 100.0, -1.0, 7.5, 0.5)),
        ((_double(3.0), _double(1.0), _double(65536.0), _double(1.5),
          _double(-2.5), _double(0.1), _double(4.0), _double(8.0)),
         (2.0, 0.5, -3.0, 1.0, 10.0, -0.25, 6.0, 1.0)),
        ((0x0000000100000003, 0x0000000200000001, 0x0000000300000007, 5,
          0x0000000123456789, 7, 0xFFFFFFFF00000001, 3),
         (1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)),
        ((_double(1e10), _double(-3e12), _double(2.5e15), _double(4294967296.0),
          _double(6e9), _double(-7e9), _double(1e18), _double(8589934592.0)),
         (1e10, -3e12, 2.5e15, 4294967296.0, 6e9, -7e9, 1e18, 8589934592.0)),
    ))


_MASK64 = (1 << 64) - 1


def _narrow(value: int) -> bool:
    """Whether a register holds a 32-bit value, sign-extended as the EE keeps
    every 32-bit value in its 64-bit registers."""
    low = value & 0xFFFFFFFF
    extended = low - (1 << 32) if low & 0x80000000 else low
    return value & _MASK64 == extended & _MASK64


def wide_result(func, machine, arg_sets, profile):
    """The first $v0 wider than 32 bits the original returns, or None.

    A derivation's result is modelled, compared and emitted as 32 bits
    (paths.PathDerivation.evaluate, Machine.call, the s32 the C++ returns), so
    a function returning a double or a long long in the whole 64-bit $v0 would
    be verified on the low half of its result, and its C++ would return only
    that half. The original is run on the plan's cases, on the memory they
    had, and on WIDE_PROBES; a case it cannot finish is left out, as in
    run_cases. A function that returns an argument unchanged is wide on the
    probes too: what it hands back is as wide as what it is given.
    """
    memory = machine.memory
    memory.end_journal()
    # The probes' doubles, read as addresses, reach pages nothing else maps:
    # given back afterwards, as testplan's search does.
    mark = memory.page_mark()
    cases = [(args, func["addr"] + case) for case, args in enumerate(arg_sets)]
    cases += [(args, func["addr"] + len(arg_sets) + n) for n, args in enumerate(WIDE_PROBES)]
    found = None
    for args, seed in cases:
        memory.write(layout.CURRENT.scratch, testmem.build(profile, seed))
        memory.begin_journal()
        try:
            machine.call(func["addr"], args, max_instructions=VERIFY_BUDGET)
            value = machine.get(2)
        except Exception:
            value = None
        memory.rollback()
        memory.end_journal()
        if value is not None and not _narrow(value):
            found = value & _MASK64
            break
    memory.unmap_to(mark)
    return found
