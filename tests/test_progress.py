"""Progress: the functions the analysis guessed are counted apart."""
import csv
import os
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.join(os.path.dirname(__file__), "..", "tools"),
                os.path.join(os.path.dirname(__file__), "..")]

import lift as lift_tool  # noqa: E402
import progress  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402


def database(functions):
    return {"functions": functions, "meta": {"image_size": 1000, "source": "test"},
            "stats": {"code_bytes": 600}}


def function(addr, size, confidence):
    return {"addr": addr, "size": size, "confidence": confidence}


class TestConfidence(unittest.TestCase):
    def test_what_the_analysis_is_sure_of(self):
        for confidence in ("entry", "called", "pointer"):
            self.assertEqual(game_config.confidence_label({"confidence": confidence}), "high")
        for confidence in ("prologue", "recovered: adjacent", "recovered: entry inside 0x00100000", None):
            self.assertEqual(game_config.confidence_label({"confidence": confidence}), "low")


class TestSummary(unittest.TestCase):
    def setUp(self):
        self.database = database([
            function(0x100, 100, "called"), function(0x200, 50, "pointer"),
            function(0x300, 40, "prologue"), function(0x400, 30, "recovered: adjacent")])
        self.status = {0x100: {"status": "verified"}, 0x200: {"status": "lifted"},
                       0x300: {"status": "verified"}, 0x400: {"status": "reconstructed"}}

    def test_the_guessed_are_counted_apart(self):
        summary = progress.summarise(self.database, self.status)
        self.assertEqual((summary["total_functions"], summary["total_code"]), (2, 150))
        self.assertEqual(summary["by_tier"]["verified"], {"count": 1, "bytes": 100})
        self.assertEqual(summary["by_tier"]["lifted"], {"count": 1, "bytes": 50})
        self.assertEqual((summary["guessed_functions"], summary["guessed_code"]), (2, 70))
        self.assertEqual(summary["guessed_by_tier"]["verified"], {"count": 1, "bytes": 40})
        self.assertEqual(summary["guessed_by_tier"]["reconstructed"], {"count": 1, "bytes": 30})

    def test_the_headline_leaves_them_out(self):
        summary = progress.summarise(self.database, self.status)
        text = progress.render_text(summary, "test")
        self.assertIn("VERIFIED: 66.6667%", text)          # 100 of the 150 sure bytes
        self.assertIn("COUNTED APART: 2 functions (70 bytes)", text)
        markdown = progress.render_markdown(summary)
        self.assertIn("**66.6667%**", markdown)
        self.assertIn("## Counted apart", markdown)


class TestStatusTable(unittest.TestCase):
    def test_every_row_says_how_sure_the_analysis_is(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "functions.csv")
            with open(path, "w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=["address", "name", "status", "source", "notes"])
                writer.writeheader()
                writer.writerow({"address": "0x00000300", "name": "generated::func_00000300",
                                 "status": "verified", "source": "src/generated/accessors.cpp",
                                 "notes": "derived"})
            lift_tool.update_status(path, {0x100}, {}, unverified={0x200: "too few cases"},
                                    confidence={0x100: "high", 0x200: "high", 0x300: "low"})
            with open(path, newline="", encoding="utf-8") as handle:
                rows = {row["address"]: row for row in csv.DictReader(handle)}
        self.assertEqual(list(rows["0x00000100"].keys()), game_config.STATUS_FIELDS)
        self.assertEqual(rows["0x00000100"]["confidence"], "high")
        self.assertEqual(rows["0x00000200"]["status"], "reconstructed")
        self.assertEqual(rows["0x00000300"]["confidence"], "low")      # a derived row keeps its place


if __name__ == "__main__":
    unittest.main()
