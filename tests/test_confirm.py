"""Guessed functions confirmed by other sources (tools/confirm.py), counted as sure."""
import json
import os
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.join(os.path.dirname(__file__), "..", "tools"),
                os.path.join(os.path.dirname(__file__), "..")]

import confirm  # noqa: E402
import progress  # noqa: E402
from ps2decomp import game as game_config  # noqa: E402


class TestConfirmations(unittest.TestCase):
    def test_each_source_given_replaces_its_own(self):
        guessed = [0x100, 0x200, 0x300, 0x400]
        previous = {0x100: "run", 0x200: "ghidra", 0x300: "ghidra+run", 0x900: "run"}
        out = confirm.confirmations(guessed, previous, {"run": {0x200, 0x400}})
        # 0x100's run is gone, 0x200 and 0x400 gain it, Ghidra's stand; 0x900
        # is no longer a guessed function.
        self.assertEqual(out, {0x200: "ghidra+run", 0x300: "ghidra", 0x400: "run"})

    def test_a_source_kept_that_this_does_not_know(self):
        out = confirm.confirmations([0x100], {0x100: "manual"}, {"ghidra": {0x100}})
        self.assertEqual(out, {0x100: "ghidra+manual"})

    def test_the_runs_record_by_overlay(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "called.txt")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("0x00100008\n0x001F0180 game\n0x00200000 other\n0x00100008\n")
            self.assertEqual(confirm.read_called(path), {0x00100008})
            self.assertEqual(confirm.read_called(path, "game"), {0x001F0180})


class TestCountedAsSure(unittest.TestCase):
    def test_a_confirmed_guess_is_sure(self):
        guessed = {"addr": 0x300, "confidence": "prologue"}
        self.assertEqual(game_config.confidence_label(guessed), "low")
        self.assertEqual(game_config.confidence_label(guessed, {0x300: "run"}), "high")
        self.assertEqual(game_config.confidence_label(guessed, {0x400: "run"}), "low")

    def test_progress_counts_them_with_their_sources(self):
        database = {"functions": [
            {"addr": 0x100, "size": 100, "confidence": "called"},
            {"addr": 0x200, "size": 40, "confidence": "prologue"},
            {"addr": 0x300, "size": 30, "confidence": "prologue"},
            {"addr": 0x400, "size": 20, "confidence": "recovered: adjacent"}],
            "meta": {"image_size": 1000, "source": "test"}, "stats": {"code_bytes": 600}}
        status = {0x200: {"status": "verified"}, 0x400: {"status": "verified"}}
        summary = progress.summarise(database, status, {0x200: "ghidra+run", 0x300: "ghidra"})
        self.assertEqual((summary["total_functions"], summary["total_code"]), (3, 170))
        self.assertEqual(summary["by_tier"]["verified"], {"count": 1, "bytes": 40})
        self.assertEqual((summary["guessed_functions"], summary["guessed_code"]), (1, 20))
        self.assertEqual(summary["confirmed_functions"], 2)
        self.assertEqual(summary["confirmed_by"], {"ghidra": 2, "run": 1})
        self.assertIn("2 of them guessed by the analysis, and confirmed (ghidra 2, run 1)",
                      progress.render_text(summary, "test"))
        self.assertIn("| 2 (ghidra 2, run 1) |", progress.render_markdown(summary))


class TestTheGamesTable(unittest.TestCase):
    def write_game(self, folder, rows, confirmed="config/confirmed.csv"):
        values = {"executable": "GAME.ELF"}
        if confirmed:
            values["confirmed"] = confirmed
        with open(os.path.join(folder, "game.json"), "w", encoding="utf-8") as handle:
            json.dump(values, handle)
        os.makedirs(os.path.join(folder, "config"))
        with open(os.path.join(folder, "config", "confirmed.csv"), "w", encoding="utf-8") as handle:
            handle.write("address,source\n" + "".join(rows))

    def test_read_when_game_json_names_it(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write_game(folder, ["0x00200000,run\n", "0x00300000,ghidra+run\n"])
            game = game_config.load(folder)
        self.assertEqual(game.confirmed, {0x00200000: "run", 0x00300000: "ghidra+run"})

    def test_off_by_default(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write_game(folder, ["0x00200000,run\n"], confirmed=None)
            game = game_config.load(folder)
        self.assertEqual((game.confirmed, game.confirmed_path), ({}, None))

    def test_a_confirmation_without_its_source_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write_game(folder, ["0x00200000,\n"])
            with self.assertRaises(SystemExit):
                game_config.load(folder)

    def test_the_tool_writes_it(self):
        with tempfile.TemporaryDirectory() as folder:
            self.write_game(folder, ["0x00000300,ghidra\n"])
            os.makedirs(os.path.join(folder, "build", "port"))
            with open(os.path.join(folder, "build", "functions.json"), "w", encoding="utf-8") as handle:
                json.dump({"functions": [{"addr": 0x100, "confidence": "called"},
                                         {"addr": 0x200, "confidence": "prologue"},
                                         {"addr": 0x300, "confidence": "prologue"}]}, handle)
            with open(os.path.join(folder, "build", "port", "called.txt"), "w", encoding="utf-8") as handle:
                handle.write("0x00000100\n0x00000200\n0x00000200 an_overlay\n")
            with open(os.path.join(folder, "config", "functions.csv"), "w", encoding="utf-8") as handle:
                handle.write("address,name,status,source,notes,confidence\n"
                             "0x00000200,F_200,lifted,src/lifted/,,low\n")
            with open(os.devnull, "w") as quiet:
                stdout, sys.stdout = sys.stdout, quiet
                try:
                    confirm.main(["--game", folder])
                finally:
                    sys.stdout = stdout
            self.assertEqual(game_config.read_confirmed(os.path.join(folder, "config", "confirmed.csv")),
                             {0x200: "run", 0x300: "ghidra"})
            self.assertEqual(progress.load_status(os.path.join(folder, "config", "functions.csv"))[0x200]["status"],
                             "lifted")
            with open(os.path.join(folder, "config", "functions.csv"), encoding="utf-8") as handle:
                self.assertTrue(handle.read().endswith("0x00000200,F_200,lifted,src/lifted/,,high\n"))


if __name__ == "__main__":
    unittest.main()
