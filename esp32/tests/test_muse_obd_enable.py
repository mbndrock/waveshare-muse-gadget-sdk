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

"""Settings' OBD switch against the real OBD task (components/muse/muse_obd.c
over tests/obd_enable_fakes/, a scripted adapter and a virtual clock): off at
boot never scans; off while polling, scanning or connecting drops the link
(a connection that beats the cancel included), reports "off" with no readings
and makes no BLE call after; the reactions don't play; on again scans without
a restart."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MuseObdEnableTest(unittest.TestCase):
    def test_switch_against_the_task(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "muse_obd_enable"
            built = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "tests/obd_enable_fakes"), "-I", str(ROOT / "components/muse"),
                 str(ROOT / "tests/muse_obd_enable_harness.c"),
                 str(ROOT / "components/muse/muse_obd_elm.c"), "-lpthread",
                 "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            env = {**os.environ,
                   "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=120)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "PASS muse_obd_enable", ran.stdout + ran.stderr)
            self.assertEqual(ran.stderr, "", ran.stderr)


if __name__ == "__main__":
    unittest.main()
