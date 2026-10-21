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

"""Settings > Voice's list: secrets/elevenlabs_voices as tools/muse/secrets.py
joins it, and as muse_voices.c reads the joined form back on the device."""

from __future__ import annotations

import importlib.util
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# Loaded by path: "secrets" would otherwise be the standard library's.
_spec = importlib.util.spec_from_file_location("muse_secrets", ROOT / "tools" / "muse" / "secrets.py")
muse_secrets = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(muse_secrets)

HARNESS = r"""
#include <stdio.h>
#include "muse_voices.h"
int main(int argc, char **argv)
{
    muse_voice_t v[MUSE_VOICES_MAX];
    int n = muse_voices_parse(argc > 1 ? argv[1] : NULL, v, MUSE_VOICES_MAX);
    for (int i = 0; i < n; i++) {
        printf("%s|%s\n", v[i].name, v[i].id);
    }
    return 0;
}
"""


class Joining(unittest.TestCase):
    def test_lines_join_with_comments_blanks_and_spaces_dropped(self):
        text = "# mine\n\n  Sarah = EXAVITQu4vr4xnSDxMaL \nGeorge=JBFqnCBsd6RMkjVDRZzb\n"
        self.assertEqual(muse_secrets.voices(text), "Sarah=EXAVITQu4vr4xnSDxMaL;George=JBFqnCBsd6RMkjVDRZzb")

    def test_empty_file_is_an_empty_list(self):
        self.assertEqual(muse_secrets.voices("# nothing yet\n"), "")

    def test_bad_lines_stop_the_build_without_echoing_them(self):
        for bad in ("JustAnId", "=abc", "Na;me=abc", 'Q"=abc', "Sarah=not-an-id", "N" * 24 + "=abc"):
            with self.subTest(bad=bad), self.assertRaises(SystemExit) as e:
                muse_secrets.voices(bad + "\n")
            self.assertIn("line 1", str(e.exception.code))
            self.assertNotIn(bad, str(e.exception.code))

    def test_more_than_fit_on_the_device_is_refused(self):
        text = "".join(f"V{i}=abc{i}\n" for i in range(muse_secrets.VOICES_MAX + 1))
        with self.assertRaises(SystemExit):
            muse_secrets.voices(text)


@unittest.skipUnless(shutil.which(os.environ.get("CC", "cc")), "needs a host C compiler")
class Parsing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        (out / "harness.c").write_text(HARNESS)
        cls.exe = out / "voices"
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "components/muse"), str(out / "harness.c"),
                        str(ROOT / "components/muse/muse_voices.c"), "-o", str(cls.exe)], check=True)

    def parse(self, spec):
        args = [str(self.exe)] + ([] if spec is None else [spec])
        out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
        return [tuple(line.split("|")) for line in out.splitlines()]

    def test_reads_back_what_secrets_py_joins(self):
        spec = muse_secrets.voices("Sarah=EXAVITQu4vr4xnSDxMaL\nGeorge=JBFqnCBsd6RMkjVDRZzb\n")
        self.assertEqual(self.parse(spec), [("Sarah", "EXAVITQu4vr4xnSDxMaL"), ("George", "JBFqnCBsd6RMkjVDRZzb")])

    def test_empty_and_missing_give_no_voices(self):
        self.assertEqual(self.parse(""), [])
        self.assertEqual(self.parse(None), [])

    def test_skips_entries_it_cannot_use(self):
        spec = "NoId=;=abc;bare;Bad=a-b;Ok=abc;Long=" + "a" * 40 + ";;Two = xyz "
        self.assertEqual(self.parse(spec), [("Ok", "abc"), ("Two", "xyz")])

    def test_stops_at_the_maximum(self):
        spec = ";".join(f"V{i}=id{i}" for i in range(12))
        self.assertEqual(len(self.parse(spec)), 8)

    def test_long_name_is_cut_on_a_character_boundary(self):
        name = "a" * 22 + "é"   # 24 bytes: the é can't be half kept
        self.assertEqual(self.parse(f"{name}=abc"), [("a" * 22, "abc")])


if __name__ == "__main__":
    unittest.main()
