"""The port's generated dispatch table."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import types
import unittest

sys.path[:0] = [os.path.join(os.path.dirname(__file__), "..", "tools"),
                os.path.join(os.path.dirname(__file__), "..")]

import port  # noqa: E402
from ps2decomp.elf import ElfFile  # noqa: E402


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


SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SHT_PROGBITS, SHT_STRTAB, SHT_NOBITS = 1, 3, 8


def tiny_elf(sections) -> bytes:
    """An ELF32 executable of `sections` - (name, address, bytes, flags, type) -
    and the table of their names; no segments (code found at run time is
    translated from the sections)."""
    names, name_at = bytearray(b"\0"), []
    for name in [s[0] for s in sections] + [".shstrtab"]:
        name_at.append(len(names))
        names += name.encode() + b"\0"
    out = bytearray(0x40)
    entries = [bytes(40)]
    for (_name, address, data, flags, kind), at in zip(sections, name_at):
        offset = len(out)
        if kind != SHT_NOBITS:
            out += data
        entries.append(struct.pack("<10I", at, kind, flags, address, offset, len(data), 0, 0, 4, 0))
    entries.append(struct.pack("<10I", name_at[-1], SHT_STRTAB, 0, 0, len(out), len(names), 0, 0, 1, 0))
    out += names
    out += bytes(-len(out) % 4)
    table = len(out)
    out += b"".join(entries)
    struct.pack_into("<4s5B7x", out, 0, b"\x7fELF", 1, 1, 1, 0, 0)
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 8, 1, sections[0][1], 0, table, 0, 52, 32, 0, 40,
                     len(entries), len(entries) - 1)
    return bytes(out)


def returns_a0_plus(n: int) -> bytes:
    """jr $ra; addiu $v0, $a0, n"""
    return struct.pack("<2I", 0x03E00008, 0x24820000 | n)


class TestDiscovered(unittest.TestCase):
    """Code met at run time is translated from whichever section holds it, not
    only the first code section: a kernel extension kept in .data and copied
    out (Tekken 5's), a call into a second code section (its .y4)."""

    def test_the_section_holding_the_source_is_translated_from(self):
        elf = tiny_elf([
            (".text", 0x00100000, returns_a0_plus(1) + bytes(0x18), SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS),
            (".data", 0x00110000, bytes(0x10) + returns_a0_plus(5) + bytes(0x10), SHF_ALLOC | SHF_WRITE,
             SHT_PROGBITS),
            (".y4", 0x00D05700, bytes(0x10) + returns_a0_plus(7) + bytes(0x10), SHF_ALLOC | SHF_EXECINSTR,
             SHT_PROGBITS),
            (".bss", 0x00120000, bytes(0x100), SHF_ALLOC | SHF_WRITE, SHT_NOBITS),
        ])
        game = types.SimpleNamespace(load_elf=lambda: ElfFile(elf))
        pairs = {(0x80076000, 0x00110010),      # copied out of .data to kernel RAM
                 (0x00D05710, 0x00D05710),      # reached where it lies, in .y4
                 (0x00120010, 0x00120010),      # .bss: nothing in the file
                 (0x00130000, 0x00130000)}      # in no section
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            found = port.discovered_functions(game, "", pairs)
        self.assertEqual(sorted(found), [0x00D05710, 0x80076000])
        source, text = found[0x80076000]
        self.assertEqual(source, 0x00110010)
        self.assertIn("void F_80076000(", text)
        self.assertIn("+ 0x5u", text)
        source, text = found[0x00D05710]
        self.assertEqual(source, 0x00D05710)
        self.assertIn("void F_00D05710(", text)
        self.assertIn("+ 0x7u", text)
        self.assertIn("0x00120010 (from 0x00120010): no section", said.getvalue())
        self.assertIn("0x00130000 (from 0x00130000): no section", said.getvalue())

    def test_a_section_with_no_bytes_laid_over_another_is_not_it(self):
        # Tekken 5's .dmh (no bytes in the file) spans its .y4.
        elf = ElfFile(tiny_elf([
            (".text", 0x00100000, returns_a0_plus(1), SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS),
            (".dmh", 0x00488000, bytes(0x01000000), SHF_ALLOC | SHF_WRITE, SHT_NOBITS),
            (".y4", 0x00D05700, bytes(0x10) + returns_a0_plus(7), SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS),
        ]))
        self.assertEqual(port.section_with_bytes(elf, 0x00D05710).name, ".y4")
        self.assertIsNone(port.section_with_bytes(elf, 0x00500000))
        found = port.discovered_functions(types.SimpleNamespace(load_elf=lambda: elf), "",
                                          {(0x00D05710, 0x00D05710)})
        self.assertIn("+ 0x7u", found[0x00D05710][1])

    def test_the_code_section_as_before(self):
        elf = tiny_elf([(".text", 0x00100000, bytes(0x20) + returns_a0_plus(3) + bytes(0x10),
                         SHF_ALLOC | SHF_EXECINSTR, SHT_PROGBITS)])
        game = types.SimpleNamespace(load_elf=lambda: ElfFile(elf))
        found = port.discovered_functions(game, "", {(0x00200000, 0x00100020)})
        self.assertEqual(found[0x00200000][0], 0x00100020)
        self.assertIn("+ 0x3u", found[0x00200000][1])


if __name__ == "__main__":
    unittest.main()
