# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The avatar's coolant warning (pose.coolant in avatar/muse_pixel.c), through
the real renderer: HOT! banner whole and clear of the head and the steam, a
worried face, sweat and steam, priority over VTEC, nothing at the grid's
edges. The renderer must also build warning-free."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MusePixelCoolantTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.binary = Path(cls.temp.name) / "muse_pixel_coolant"
        compiled = subprocess.run(
            [*cls.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_pixel_coolant_harness.c"), "-lm",
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def test_renderer_builds_without_warnings(self):
        built = subprocess.run(
            [*self.cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-c",
             "-I", str(ROOT / "components/muse"), str(ROOT / "avatar/muse_pixel.c"),
             "-o", str(Path(self.temp.name) / "muse_pixel.o")],
            capture_output=True, text=True,
        )
        self.assertEqual(built.returncode, 0, built.stderr)
        self.assertEqual(built.stderr, "")

    def test_coolant_reaction(self):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_pixel_coolant", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "", ran.stderr)


if __name__ == "__main__":
    unittest.main()
