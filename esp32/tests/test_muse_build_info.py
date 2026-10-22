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

"""The build branch stamp: the branch on the host, and where it's shown."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseBuildInfoTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)

    def run_case(self, name, define):
        binary = Path(self.temp.name) / name
        cc = shlex.split(os.environ.get("CC", "cc"))
        flags = [define] if define else []
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
             "-I", str(MUSE),
             str(ROOT / "tests/muse_build_info_harness.c"),
             str(MUSE / "muse_build_info.c"),
             "-o", str(binary)],
            capture_output=True, text=True,
        )
        self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        return dict(line.split("=", 1) for line in ran.stdout.splitlines())

    def has(self, needle, src):
        self.assertTrue(needle in src, f"missing: {needle}")

    def matches(self, src, pattern):
        self.assertTrue(re.search(pattern, src), f"no match: {pattern}")

    def test_main(self):
        self.assertEqual(self.run_case("main", '-DMUSE_GIT_BRANCH="main"'),
                         {"branch": "main"})

    def test_obd(self):
        self.assertEqual(self.run_case("obd", '-DMUSE_GIT_BRANCH="obd-vtec-reaction"'),
                         {"branch": "obd-vtec-reaction"})

    def test_other_branch(self):
        self.assertEqual(self.run_case("other", '-DMUSE_GIT_BRANCH="feature/new-thing"'),
                         {"branch": "feature/new-thing"})

    def test_no_macro(self):
        self.assertEqual(self.run_case("none", None),
                         {"branch": "unknown"})

    def test_cmake_stamps_branch(self):
        src = (MUSE / "CMakeLists.txt").read_text()
        self.has('"muse_build_info.c"', src)
        self.has("/.git/HEAD", src)
        self.matches(src, r"set_property\(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\s+\"\$\{muse_git_head\}\"\)")
        self.has("^ref: refs/heads/(.+)$", src)
        self.has('set(muse_branch "detached")', src)
        self.has('set(muse_branch "unknown")', src)
        self.has('string(REGEX REPLACE "[^A-Za-z0-9._/-]" "-" muse_branch', src)
        self.has('string(SUBSTRING "${muse_branch}" 0 64 muse_branch)', src)
        self.assertIsNone(re.search(r"set\(muse_branch[^)]*CACHE", src))
        self.has('target_compile_definitions(${COMPONENT_LIB} PRIVATE "MUSE_GIT_BRANCH=\\"${muse_branch}\\"")', src)
        # After registration, inside the Muse-only block.
        self.assertLess(src.index("idf_component_register("), src.index("MUSE_GIT_BRANCH="))

    def test_no_face_badge(self):
        src = (MUSE / "muse_ui.c").read_text()
        self.assertNotIn("muse_build_info.h", src)
        self.assertNotIn("s_branch_lbl", src)
        self.assertNotIn("muse_build_badge", src)
        self.assertNotIn("muse_build_branch", src)
        self.has("lv_obj_t *hides[4];", src)
        self.has("add_hides(&s_answers[ANSWER_READ], 2);", src)
        self.has("add_hides(&s_answers[ANSWER_HEARD], 0);", src)

    def test_no_badge_api(self):
        for name in ("muse_build_info.h", "muse_build_info.c"):
            self.assertNotIn("muse_build_badge", (MUSE / name).read_text(), name)
        self.assertNotIn("muse_build_badge", (ROOT / "tests/muse_build_info_harness.c").read_text())

    def test_exact_branch_in_settings_and_menu(self):
        settings = (MUSE / "muse_settings_ui.c").read_text()
        self.has('#include "muse_build_info.h"', settings)
        self.matches(settings, r"\*s_about, \*s_build;")
        # The build line is the note directly under the version/IP line.
        self.matches(settings, r's_about = note\(list, ""\);\n    s_build = note\(list, ""\);')
        self.matches(settings, r'"Muse %s - %s", esp_app_get_description\(\)->version,\s*w\.state == MUSE_WIFI_CONNECTED')
        self.matches(settings, r'snprintf\(buf, sizeof\(buf\), "Build: %s", muse_build_branch\(\)\);\n    set_text\(s_build, buf\);')
        self.has("char buf[128];", settings)
        menu = (MUSE / "muse_menu.c").read_text()
        self.has('#include "muse_build_info.h"', menu)
        self.matches(menu, r"Power %s\\nBuild %s\\nVer   %s")
        self.matches(menu, r"batt,\s*muse_build_branch\(\),")
        self.matches(menu, r"static void refresh\(void\)\n\{\n    char buf\[192\];")


if __name__ == "__main__":
    unittest.main()
