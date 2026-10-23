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

"""OBD-LOG-DOORBELL-TASK: the announce decision (muse_obd_doorbell.c) against
its harness, plus the wiring contracts: the Settings row, the session watcher
in the trip log, no speech and no car-side traffic from the doorbell."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


def function_body(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError(f"unterminated {signature}")


class MuseObdDoorbellTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_obd_doorbell"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wformat-truncation=2", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(MUSE),
             str(ROOT / "tests/muse_obd_doorbell_harness.c"),
             str(MUSE / "muse_obd_doorbell.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.warnings = compiled.stderr

    def test_compiles_clean(self):
        self.assertEqual(self.warnings, "")

    def test_decision(self):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_obd_doorbell", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "", ran.stderr)


class MuseObdDoorbellContractTest(unittest.TestCase):
    def test_exact_prefix_lives_in_the_pure_unit(self):
        doorbell = (MUSE / "muse_obd_doorbell.c").read_text()
        self.assertIn('"Trip logs ready: trips "', doorbell)
        self.assertIn('" bytes total)."', doorbell)
        # The decision unit is plain C: no IDF, chat or car headers.
        for banned in ("esp_", "freertos", "muse_chat", "muse_obd.h", "nimble", "muse_hatch_say"):
            self.assertNotIn(banned, doorbell, banned)

    def test_settings_row(self):
        ui = (MUSE / "muse_settings_ui.c").read_text()
        self.assertIn('"Send logs to Muse"', ui)
        self.assertIn("muse_obd_doorbell_ring()", ui)
        self.assertIn("muse_obd_doorbell_pending(", ui)
        ring = function_body(ui, "static void on_doorbell(lv_event_t *e)")
        self.assertIn("MUSE_DOORBELL_SENT", ring)
        self.assertIn("MUSE_DOORBELL_NOT_CONNECTED", ring)
        self.assertIn("Not connected", ring)
        self.assertIn("Sent", ring)

    def test_watcher_and_send_live_with_the_trip_log(self):
        # The IDF side is its own file: the older OBD-LOG contracts keep
        # muse_obd_log.c free of chat calls, and this task does not weaken
        # them.
        task = (MUSE / "muse_obd_doorbell_task.c").read_text()
        log = (MUSE / "muse_obd_log.c").read_text()
        self.assertNotIn("muse_hatch_text_turn", log)
        self.assertIn("muse_hatch_text_turn(", task)
        self.assertIn("muse_hatch_ready()", task)
        self.assertNotIn("muse_hatch_say", task)
        # The send goes through the decision unit over the store listing.
        send = function_body(task, "static bool doorbell_send(void)")
        self.assertIn("muse_obd_log_list(", send)
        self.assertIn("muse_doorbell_announce(", send)
        self.assertIn("muse_hatch_text_turn(", send)
        # Started beside the writer, so it runs with Car mode off too, and
        # nothing in the doorbell touches the car side.
        self.assertIn("muse_obd_doorbell_start();", function_body(log, "void muse_obd_log_start(void)"))
        for banned in ("nimble", "elm_", "ATZ", "muse_obd_set_enabled"):
            self.assertNotIn(banned, task, banned)

    def test_no_speech_from_obd_and_gate(self):
        self.assertEqual((MUSE / "muse_obd.c").read_text().count("muse_hatch_say"), 0)
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn('"muse_obd_doorbell.c"', cmake)
        self.assertIn('"muse_obd_doorbell_task.c"', cmake)
        self.assertIn("muse_obd_log.h", (MUSE / "muse_settings_ui.c").read_text())


if __name__ == "__main__":
    unittest.main()
