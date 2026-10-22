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

"""The avatar's warm-up thumbs-up (pose.warmup in avatar/muse_pixel.c), through
the real renderer: a whole thumbs-up on one raised arm beside the head, a
pleased face, priority below the coolant warning and VTEC, idle only, nothing
at the grid's edges. The renderer must also build warning-free, and the pose
must be plumbed from muse_state to the renderer like VTEC and coolant."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MusePixelWarmupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.cc = shlex.split(os.environ.get("CC", "cc"))
        cls.binary = Path(cls.temp.name) / "muse_pixel_warmup"
        compiled = subprocess.run(
            [*cls.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "components/muse"),
             str(ROOT / "tests/muse_pixel_warmup_harness.c"), "-lm",
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

    def test_warmup_reaction(self):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=env)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_pixel_warmup", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "", ran.stderr)


GATE = "#if CONFIG_MUSE_OBD_VTEC"


def gated_block_with(source, needle):
    """The CONFIG_MUSE_OBD_VTEC block that contains needle, or None."""
    start = source.find(GATE)
    while start != -1:
        block = source[start:source.index("#endif", start)]
        if needle in block:
            return block
        start = source.find(GATE, start + 1)
    return None


class MuseWarmupPlumbingTest(unittest.TestCase):
    """pose.warmup is plumbed like pose.vtec / pose.coolant."""

    def test_pose_field_and_duration(self):
        header = (ROOT / "components/muse/muse_pixel.h").read_text()
        self.assertRegex(header, r"\n    float warmup;")
        self.assertRegex(header, r"#define MUSE_WARMUP_S 3\.0f\b")

    def test_state_pair_is_declared_and_gated(self):
        header = (ROOT / "components/muse/muse_state.h").read_text()
        self.assertIn("void muse_state_start_warmup(void);", header)
        self.assertIn("float muse_state_warmup(void);", header)
        state = (ROOT / "components/muse/muse_state.c").read_text()
        block = gated_block_with(state, "void muse_state_start_warmup(void)")
        self.assertIsNotNone(block)
        self.assertIn("idle_reaction(&s_warmup_us, MUSE_WARMUP_S)", block)
        # Sleep clears it with the other car reactions.
        self.assertIsNotNone(gated_block_with(state, "s_warmup_us = 0;"))

    def test_ui_fills_the_pose(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertEqual(ui.count(".warmup = muse_state_warmup(),"), 1)
        self.assertIsNotNone(gated_block_with(ui, ".warmup = muse_state_warmup(),"))

    def test_renderer_ranks_it_below_coolant_and_vtec(self):
        source = (ROOT / "avatar/muse_pixel.c").read_text()
        start = source.index("static void react_setup(")
        setup = source[start:source.index("\n}\n", start)]
        order = [setup.index(f"p->{f} > 0") for f in ("coolant", "vtec", "warmup")]
        self.assertEqual(order, sorted(order))
        self.assertTrue(re.search(r"p->warmup > 0\) \{[^}]*RE_WARMUP", setup))


if __name__ == "__main__":
    unittest.main()
