"""The port's generated dispatch table."""
import os
import sys
import tempfile
import unittest

sys.path[:0] = [os.path.join(os.path.dirname(__file__), "..", "tools"),
                os.path.join(os.path.dirname(__file__), "..")]

import port  # noqa: E402


class TestDispatch(unittest.TestCase):
    def test_translated_functions_are_found(self):
        folder = tempfile.mkdtemp()
        with open(os.path.join(folder, "lifted_000.cpp"), "w") as handle:
            handle.write("void F_00100010(lifted::Context& c) {\n}\n"
                         "void F_00100008(lifted::Context& c) {\n}\n"
                         "/* void F_00100020(lifted::Context& c) { in a comment */\n")
        self.assertEqual(port.translated_functions(folder), [0x100008, 0x100010])

    def test_table_is_sorted_and_names_what_has_no_translation(self):
        text = port.dispatch_source([0x100010, 0x100008], {0x100008: "_start"}, [0x100004],
                                    {0x100008: 528})
        rows = [line.strip() for line in text.splitlines() if line.strip().startswith("{0x")]
        self.assertEqual(rows, ['{0x00100004u, nullptr, nullptr, 0u},',
                                '{0x00100008u, F_00100008, "_start", 528u},',
                                '{0x00100010u, F_00100010, nullptr, 0u},'])
        self.assertIn("kGuestFunctionCount = 3;", text)
        self.assertIn("void F_00100008(lifted::Context& c);", text)


class TestToolchain(unittest.TestCase):
    """The platform's side of a port's build (tools/toolchain.py): the same
    names and folders port.py, regress.py and bisect_lifted.py use."""

    def test_the_executable_and_its_folder_are_the_platforms(self):
        import types
        import toolchain

        game = types.SimpleNamespace(directory=os.path.join("games", "tekken5"), build="build",
                                     path=lambda relative: os.path.join("games", "tekken5", relative))
        if os.name == "nt":
            self.assertEqual((toolchain.EXE, toolchain.port_folder()), (".exe", "cmake"))
            expected = os.path.join("games", "tekken5", "build", "port", "cmake", "tekken5.exe")
        else:
            self.assertEqual((toolchain.EXE, toolchain.port_folder()), ("", "cmake-macos-arm64"))
            expected = os.path.join("games", "tekken5", "build", "port", "cmake-macos-arm64", "tekken5")
        self.assertEqual(toolchain.port_executable(game), expected)
        self.assertEqual(toolchain.executable_name("ps2host_tests"), "ps2host_tests" + toolchain.EXE)

    def test_the_preset_is_the_templates(self):
        import json
        import toolchain

        template = os.path.join(os.path.dirname(__file__), "..", "templates", "game", "port",
                                "CMakePresets.json")
        with open(template, encoding="utf-8") as handle:
            presets = json.load(handle)
        names = [p["name"] for p in presets["configurePresets"]]
        self.assertIn(toolchain.PRESET, names)
        self.assertTrue(presets["configurePresets"][names.index(toolchain.PRESET)]["binaryDir"]
                        .endswith("/" + "cmake-" + toolchain.PRESET))

    def test_jobs_and_compiler_are_named(self):
        import toolchain

        self.assertGreaterEqual(toolchain.build_jobs(), 1)
        if os.name != "nt":
            self.assertIsNone(toolchain.clang_cl())
            self.assertEqual(toolchain.compiler_name(None), "Apple clang")
        else:
            self.assertEqual(toolchain.compiler_name(None), "MSVC")


if __name__ == "__main__":
    unittest.main()
