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

"""The avatar's racing helmet (pose.helmet in avatar/muse_pixel.c), through
the real renderer: in every mode and reaction the face, arms and overlays stay
as they were, the helmet adds only its own colours over the head, the visor is
up, the stripe is there and thinking drops the headphones. The flag must be
plumbed from the OBD link to the renderer only with CONFIG_MUSE_OBD_VTEC."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
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


class MusePixelHelmetTest(unittest.TestCase):
    def test_helmet(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "muse_pixel_helmet"
            built = subprocess.run(
                [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "components/muse"),
                 str(ROOT / "tests/muse_pixel_helmet_harness.c"), "-lm",
                 "-o", str(binary)],
                capture_output=True, text=True,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            env = {**os.environ,
                   "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
                   "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
            ran = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=300)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), "PASS muse_pixel_helmet", ran.stdout + ran.stderr)
            self.assertEqual(ran.stderr, "", ran.stderr)


class MuseHelmetPlumbingTest(unittest.TestCase):
    """pose.helmet comes from the OBD link (muse_obd_linked), gated."""

    def test_pose_field(self):
        header = (ROOT / "components/muse/muse_pixel.h").read_text()
        self.assertRegex(header, r"\n    bool helmet; ")

    def test_ui_sets_it_from_the_link_only_with_the_feature(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertEqual(ui.count(".helmet = s_obd_linked,"), 1)
        self.assertIsNotNone(gated_block_with(ui, ".helmet = s_obd_linked,"))
        self.assertIsNotNone(gated_block_with(ui, "s_obd_linked = muse_obd_linked(&o);"))
        self.assertIsNotNone(gated_block_with(ui, "static volatile bool s_obd_linked;"))
        # Nowhere else: a non-OBD build leaves it false.
        self.assertEqual(ui.count(".helmet"), 1)

    def test_renderer_draws_it_instead_of_the_headphones(self):
        source = (ROOT / "avatar/muse_pixel.c").read_text()
        start = source.index("void muse_pixel_render(const muse_pose_t *p)")
        render = source[start:source.index("\n}\n", start)]
        self.assertIn("    if (p->helmet) {\n        draw_helmet(&j);", render)
        self.assertIn("} else if (mode == MUSE_MODE_THINKING) {\n        draw_headphones(&j, eye_y);", render)
        # Under the foreground: sparkles, banners, steam and the thumbs-up go over it.
        self.assertLess(render.index("draw_helmet(&j);"), render.index("/* ---- foreground ---- */"))
        # The fork's additions are listed in the character's notice.
        self.assertIn("racing helmet (draw_helmet, pose.helmet)", source[:1200])


if __name__ == "__main__":
    unittest.main()
