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


if __name__ == "__main__":
    unittest.main()
