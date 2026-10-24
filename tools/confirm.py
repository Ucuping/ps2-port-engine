#!/usr/bin/env python3
"""Confirm the functions the analysis guessed, from sources other than itself.

A function the prologue scan found, or one recovered from the gaps, may be a
piece of another (game.confidence_label), and progress.py counts it apart. Two
sources can say that a function starts where one was guessed:

  run     a run entered it by a call: a jal, a jalr, the kernel starting a
          thread or a handler (the host's record, --called; port.py keeps it in
          build/port/called.txt). A jump confirms nothing: it lands on code
          inside functions - a switch's case, the rest of a function cut short -
          as often as on their starts.
  ghidra  Ghidra's analysis of the same image has a function starting there: a
          function table with an "entry" column of hex addresses, exported from
          Ghidra (a script such as ExportFunctionList.java writes one).

    python engine/tools/confirm.py --game games/<name> [--run <called.txt>] [--ghidra <functions.csv>]

Writes the table game.json's "confirmed" names (a game without one has none to
write): each guessed function a source confirms, with its sources joined by
"+". A source given replaces what it confirmed before; a source not given keeps
its confirmations. A row whose function the analysis no longer guesses goes. The
run's record defaults to the port's, for a game or an overlay alike: an
overlay's calls are recorded under its directory's name.

The status table's confidence column follows (derive.py and lift.py write it
too). Passing its tests confirms nothing: a piece of a function passes them as
well as a function does.
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]

from ps2decomp import game as game_config  # noqa: E402

SOURCES = ("run", "ghidra")


def read_called(path: str, label: str = "") -> set:
    """The addresses a call entered, by the runs' record: one per line, with the
    name of the overlay whose function it was (`label`; none for the
    executable's)."""
    starts = set()
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if parts and (parts[1] if len(parts) > 1 else "") == label:
                starts.add(int(parts[0], 16))
    return starts


def read_ghidra(path: str) -> set:
    """The entries of a function table exported from Ghidra."""
    with open(path, newline="", encoding="utf-8") as handle:
        return {int(row["entry"], 16) for row in csv.DictReader(handle)}


def confirmations(guessed, previous: dict, given: dict) -> dict:
    """{address: sources} for the guessed functions (their addresses) a source
    confirms. `given`: {source: the addresses it says functions start at} for
    the sources consulted now; `previous`'s confirmations by the others stand."""
    out = {}
    for address in guessed:
        sources = set(previous.get(address, "").split("+")) - {""}
        for source, starts in given.items():
            sources.discard(source)
            if address in starts:
                sources.add(source)
        if sources:
            out[address] = "+".join(sorted(sources))
    return out


def write(path: str, confirmed: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=game_config.CONFIRMED_FIELDS)
        writer.writeheader()
        for address in sorted(confirmed):
            writer.writerow({"address": "0x%08X" % address, "source": confirmed[address]})


def refresh_status(path: str, database: dict, confirmed: dict) -> int:
    """The status table's confidence column after the confirmations changed (it
    is derive.py's and lift.py's, which write it as they are told): how many
    rows changed."""
    if not os.path.exists(path):
        return 0
    with open(path, newline="", encoding="utf-8") as handle:
        rows = list(csv.DictReader(handle))
    sure = {f["addr"]: game_config.confidence_label(f, confirmed) for f in database["functions"]}
    changed = 0
    for row in rows:
        label = sure.get(int(row["address"], 16))
        if label and row.get("confidence") != label:
            row["confidence"] = label
            changed += 1
    if changed:
        with open(path, "w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=game_config.STATUS_FIELDS)
            writer.writeheader()
            writer.writerows(rows)
    return changed


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    game_config.add_argument(parser)
    parser.add_argument("--run", default=None,
                        help="the runs' record of calls (default: the port's, if there is one)")
    parser.add_argument("--ghidra", default=None, help="a function table exported from Ghidra")
    args = parser.parse_args(argv)

    game = game_config.from_args(args)
    if not game.confirmed_path:
        raise SystemExit('%s names no table of confirmations: add "confirmed": "config/confirmed.csv" '
                         "to its game.json" % game.directory)
    owner = game.base or game
    label = os.path.basename(os.path.normpath(game.directory)) if game.base else ""
    run = args.run or owner.path(os.path.join(owner.build, "port", "called.txt"))
    given = {}
    if args.run or os.path.exists(run):
        given["run"] = read_called(run, label)
    if args.ghidra:
        given["ghidra"] = read_ghidra(args.ghidra)
    if not given:
        raise SystemExit("no source to confirm from: no run has recorded its calls at %s, "
                         "and no --ghidra" % run)

    database = game.load_database()
    # (A piece of a function a run reached - a case, a label - is no guess, nor a function.)
    guessed = [f["addr"] for f in database["functions"] if game_config.confidence_label(f) == "low"]
    confirmed = confirmations(guessed, game.confirmed, given)
    write(game.confirmed_path, confirmed)
    refreshed = refresh_status(game.path(game.status), database, confirmed)

    counts = {source: sum(1 for s in confirmed.values() if source in s.split("+")) for source in SOURCES}
    gone = len(set(game.confirmed) - set(confirmed))
    print("%d guessed functions; %d confirmed (%s)%s"
          % (len(guessed), len(confirmed),
             ", ".join("%s %d" % (source, counts[source]) for source in SOURCES),
             "; %d earlier confirmations gone" % gone if gone else ""))
    for source in sorted(given):
        print("  %s: %d starts, from %s" % (source, len(given[source]),
                                            run if source == "run" else args.ghidra))
    print("wrote %s%s" % (game.confirmed_path, "; %d rows of %s now say how sure"
                          % (refreshed, game.path(game.status)) if refreshed else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
