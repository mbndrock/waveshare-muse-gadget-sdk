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

"""The commit and push check (tools/muse/check_secrets.py), in a scratch git
repo with made-up secrets: it stops real values, their pieces, token-shaped
strings and secret files, says where without saying what, and lets the
pairing test's fake token and ordinary changes through."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / "tools" / "muse" / "check_secrets.py"
TOKEN = "mgst_" + "Zq7" * 14 + "x"        # made up, 48 bytes like a real one
KEY = "sk_" + "4f9e" * 12                 # made up


class CheckSecretsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / "repo"
        secrets = Path(self.tmp.name) / "secrets"
        secrets.mkdir()
        (secrets / "muse_sdk_token").write_text(TOKEN + "\n")
        (secrets / "elevenlabs_api_key").write_text(KEY + "\n")
        self.env = {k: v for k, v in os.environ.items() if k not in ("MUSE_SDK_TOKEN", "ELEVENLABS_API_KEY")}
        self.env.update(MUSE_SECRETS_DIR=str(secrets), GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM="1")
        self.git("init", "-q", str(self.repo), cwd=self.tmp.name)
        self.git("config", "user.email", "test@example.com")
        self.git("config", "user.name", "Test")
        self.write("README.md", "hello\n")
        self.git("add", "-A")
        self.git("commit", "-qm", "base")

    def git(self, *args, cwd=None):
        subprocess.run(["git", *args], cwd=cwd or self.repo, env=self.env, check=True, capture_output=True)

    def write(self, path, text):
        p = self.repo / path
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def staged(self, path, text):
        self.write(path, text)
        self.git("add", "-f", path)
        return self.run_check("--staged")

    def run_check(self, *args):
        return subprocess.run([sys.executable, str(CHECK), *args], cwd=self.repo, env=self.env,
                              capture_output=True, text=True)

    def assertStopped(self, r, where):
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn(where, r.stderr)
        for secret in (TOKEN, KEY, TOKEN[10:22], KEY[5:17]):
            self.assertNotIn(secret, r.stderr + r.stdout, "never says what it found")

    def test_ordinary_change_passes(self):
        self.assertEqual(self.staged("src/a.c", "int x = 1;\n").returncode, 0)

    def test_real_token_in_a_tracked_file_is_stopped_with_its_line(self):
        r = self.staged("secrets/muse_sdk_token.example", "# template\n" + TOKEN + "\n")
        self.assertStopped(r, "secrets/muse_sdk_token.example:2: contains your muse_sdk_token")

    def test_a_piece_of_the_key_is_stopped(self):
        self.assertStopped(self.staged("notes.md", "key starts " + KEY[3:20] + "\n"), "contains your elevenlabs_api_key")

    def test_token_shaped_strings_are_stopped(self):
        self.assertStopped(self.staged("a.py", 'T = "mgst_' + "b" * 30 + '"\n'), "looks like an SDK token")
        self.assertStopped(self.staged("b.py", 'K = "sk_' + "a1" * 16 + '"\n'), "looks like an API key")

    def test_the_pairing_tests_fake_token_passes_there_only(self):
        fake = "mgst_" + "A" * 43
        self.assertEqual(self.staged("esp32/tests/link_pairing_handshake_harness.c", f'"{fake}"\n').returncode, 0)
        self.assertStopped(self.staged("elsewhere.c", f'"{fake}"\n'), "looks like an SDK token")

    def test_secret_files_and_build_folders_are_stopped(self):
        self.assertStopped(self.staged("secrets/elevenlabs_voices", "Sarah=abc\n"), "secrets/elevenlabs_voices: holds secrets")
        self.git("reset", "-q")
        self.assertStopped(self.staged("esp32/build-muse-x/sdkconfig", "X=1\n"), "build-muse-x/sdkconfig: holds secrets")
        self.git("reset", "-q")
        self.assertEqual(self.staged("secrets/README.md", "how to\n").returncode, 0)

    def test_pushed_commits_are_checked_even_if_committed_unchecked(self):
        base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=self.repo, env=self.env, capture_output=True,
                              text=True).stdout.strip()
        self.write("a.txt", "fine\n")
        self.git("add", "a.txt")
        self.git("commit", "-qm", "fine")
        self.assertEqual(self.run_check("--commits", f"{base}..HEAD").returncode, 0)
        self.write("b.txt", TOKEN + "\n")
        self.git("add", "b.txt")
        self.git("commit", "-qm", "oops")
        self.assertStopped(self.run_check("--commits", f"{base}..HEAD"), "b.txt:1: contains your muse_sdk_token")


if __name__ == "__main__":
    unittest.main()
