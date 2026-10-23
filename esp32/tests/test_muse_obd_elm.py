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

"""The OBD reactions' ELM327 parser and triggers (muse_obd_elm.c): RPM, DID
0x2612's VTEC flags against real 2007 S2000 replies and the held VTEC level,
coolant (PID 05), the coolant warning and the warm-up thumbs-up; and the car
data cards' PIDs, the supported-PID scan, the secondary poll cycle (with the
DID slot) and ATDPN."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseObdElmTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_obd_elm"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_obd_elm_harness.c"),
             str(ROOT / "components/muse/muse_obd_elm.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.warnings = compiled.stderr

    def test_compiles_clean(self):
        self.assertEqual(self.warnings, "")

    def test_parser_and_trigger(self):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_obd_elm", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "", ran.stderr)


if __name__ == "__main__":
    unittest.main()
