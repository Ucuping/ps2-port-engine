"""The check libraries' build (ps2decomp/cppcheck.py): every compile error at once."""
import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from ps2decomp import cppcheck  # noqa: E402


class TestClangBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.name == "nt":
            raise unittest.SkipTest("clang's path only: MSVC builds through vcvars")
        try:
            cppcheck.find_compiler()
        except cppcheck.CompilerUnavailable as exc:
            raise unittest.SkipTest(str(exc))

    def test_every_error_is_reported_at_once(self):
        # Thirty functions with an error each. A compile stopped at clang's
        # default limit of twenty names only some, and derive, dropping the
        # functions that do not compile, compiled the whole file again for the
        # rest - seven times over Tekken 5's game program, 90 MB each time.
        with tempfile.TemporaryDirectory() as folder:
            source = os.path.join(folder, "accessors.cpp")
            with open(source, "w", encoding="utf-8") as handle:
                handle.write("".join("int f%d() { return undefined_%d; }\n" % (k, k) for k in range(30)))
            with self.assertRaises(cppcheck.CompileError) as caught:
                cppcheck.clang_build([source], [], os.path.join(folder, "check" + cppcheck.SHARED_SUFFIX))
        self.assertEqual(len(set(re.findall(r"accessors\.cpp\((\d+)\)", str(caught.exception)))), 30)


def emitted(folder, bodies):
    """An emitted file as derive.emit writes one: its prelude, the definitions,
    the namespace's end. Returns its path and its lines."""
    addresses = [0x00100000 + 16 * k for k in range(len(bodies))]
    lines = ['#include "ps2rt/generated.h"', "", "namespace generated {", ""]
    lines += ["s32 func_%08X(u64 arg0);" % a for a in addresses] + [""]
    for address, body in zip(addresses, bodies):
        lines += ["/* 0x%08X */" % address, "s32 func_%08X(u64 arg0) {" % address]
        lines += ["    " + line for line in body] + ["}", ""]
    lines.append("}  // namespace generated")
    path = os.path.join(folder, "accessors.cpp")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines) + "\n")
    return path, lines


class TestParts(unittest.TestCase):
    def test_each_part_is_the_prelude_and_a_share_in_order(self):
        bodies = [["u64 x%d = arg0 + %d;" % (n, n) for n in range(count)] + ["return (s32)arg0;"]
                  for count in (2, 3, 40, 1, 2, 2)]
        with tempfile.TemporaryDirectory() as folder:
            path, lines = emitted(folder, bodies)
            parts = cppcheck.split_definitions(path, folder, part_bytes=200)
            texts = []
            for part in parts:
                with open(part.path, encoding="utf-8") as handle:
                    texts.append(handle.read().splitlines())
        prelude = lines[:lines.index("/* 0x00100000 */") + 1]
        definitions = [[line for line in text if line.startswith("s32 func_") and line.endswith("{")]
                       for text in texts]
        self.assertEqual(sum(definitions, []), [line for line in lines if line.startswith("s32 func_")
                                                and line.endswith("{")])
        self.assertIn(["s32 func_00100020(u64 arg0) {"], definitions)       # past the size: alone
        for part, text in zip(parts, texts):
            self.assertEqual(text[:len(prelude) - 1], prelude[:-1])
            self.assertEqual(text[-1], "}  // namespace generated")
            for number, line in enumerate(text[:-1], 1):
                self.assertEqual(lines[part.emitted_line(number) - 1], line)

    def test_a_file_without_definitions_is_its_own_part(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "accessors.cpp")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write('#include "ps2rt/generated.h"\n')
            parts = cppcheck.split_definitions(path, folder)
        self.assertEqual([part.path for part in parts], [os.path.abspath(path)])


class TestCompileJobs(unittest.TestCase):
    def need(self, size):
        return cppcheck.COMPILER_BASE_BYTES + cppcheck.COMPILER_BYTES_PER_BYTE * size

    def test_one_per_core_when_memory_allows(self):
        self.assertEqual(cppcheck.compile_jobs([1500000] * 20, available=16 << 30, cpus=8), 8)

    def test_the_largest_that_run_together_fit(self):
        sizes = [4600000] + [1500000] * 20
        available = (self.need(4600000) + 3 * self.need(1500000)) * 5 // 4 + 1
        self.assertEqual(cppcheck.compile_jobs(sizes, available=available, cpus=8), 4)

    def test_at_least_one(self):
        self.assertEqual(cppcheck.compile_jobs([90000000], available=1 << 20, cpus=8), 1)

    def test_no_more_than_there_are_parts(self):
        self.assertEqual(cppcheck.compile_jobs([1000, 1000], available=16 << 30, cpus=8), 2)

    def test_memory_is_found(self):
        self.assertGreater(cppcheck.available_memory(), 0)


class TestBuildInParts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        TestClangBuild.setUpClass()

    def setUp(self):
        self.saved = cppcheck.PART_BYTES
        cppcheck.PART_BYTES = 60                       # a part per function or two

    def tearDown(self):
        cppcheck.PART_BYTES = self.saved

    def build(self, bodies):
        folder = self.folder.name
        path, lines = emitted(folder, bodies)
        dll, functions = cppcheck.build(path, [], os.path.join(cppcheck.RUNTIME, "include"),
                                        os.path.join(folder, "work"))
        return dll, functions, lines

    def test_the_parts_link_into_one_library(self):
        import ctypes
        self.folder = tempfile.TemporaryDirectory()
        with self.folder:
            dll, functions, _lines = self.build([["return (s32)(arg0 + %d);" % k] for k in range(5)])
            library = ctypes.CDLL(dll)
            library.b3_call.restype = ctypes.c_int
            result = ctypes.c_int32(0)
            arguments = (ctypes.c_uint64 * 17)(41)
            self.assertEqual(library.b3_call(0x00100030, arguments, ctypes.byref(result)), 1)
            self.assertEqual(result.value, 44)
            self.assertEqual(len(functions), 5)

    def test_an_error_is_reported_at_the_emitted_files_line(self):
        self.folder = tempfile.TemporaryDirectory()
        with self.folder:
            bodies = [["return (s32)arg0;"]] * 6
            bodies[4] = ["u64 a = arg0;", "return (s32)undefined_name;"]
            with self.assertRaises(cppcheck.CompileError) as caught:
                self.build(bodies)
            _path, lines = emitted(self.folder.name, bodies)
        reported = [int(n) for n in re.findall(r"accessors\.cpp\((\d+)\)", str(caught.exception))]
        self.assertEqual(reported, [lines.index("    return (s32)undefined_name;") + 1])
        self.assertNotIn(".part", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
