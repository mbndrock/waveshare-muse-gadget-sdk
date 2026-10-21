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

"""How a reply still arriving is cut into pieces to speak (muse_chat_text.c
muse_hatch_speech_cut): at sentence ends once there's enough to say, never
inside a number or abbreviation, and nothing lost or said twice."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUN = 160   # MUSE_HATCH_SPEECH_RUN


class SpeechPiecesTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_serial_chat_harness"
        proc = subprocess.run(
            [*cc, "-include", str(ROOT / "tests" / "host_compat.h"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "components" / "muse"), str(ROOT / "tests" / "muse_serial_chat_harness.c"),
             str(ROOT / "components" / "muse" / "muse_chat_text.c"), str(ROOT / "components" / "muse" / "muse_text.c"),
             "-o", str(cls.binary)],
            cwd=ROOT, text=True, capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def pieces(self, text: str, first_min: int = 30, step: int = 1) -> list[str]:
        out = subprocess.run([str(self.binary), "speech", str(first_min), str(step)], input=text.encode(),
                             capture_output=True, check=True).stdout.decode()
        parts = out.split("\x1f")
        self.assertEqual(parts[-1], "")
        parts = parts[:-1]
        self.assertEqual("".join(parts), text, "every byte said once, in order")
        return parts

    def test_first_piece_goes_once_a_long_enough_sentence_is_in(self):
        text = "Sure. The capital of France is Paris, on the Seine. It has about two million people."
        parts = self.pieces(text)
        self.assertEqual(parts[0], "Sure. The capital of France is Paris, on the Seine.")
        self.assertEqual(len(parts), 2)

    def test_numbers_and_abbreviations_are_not_sentence_ends(self):
        text = "It costs 3.50 euros, e.g. at the market today! Then more words follow here"
        self.assertEqual(self.pieces(text)[0], "It costs 3.50 euros, e.g. at the market today!")

    def test_text_that_arrives_in_one_go_is_one_piece(self):
        text = "One. Two. Three. Four sentences in a row, all here at once."
        self.assertEqual(self.pieces(text, step=len(text)), [text])

    def test_later_pieces_take_whatever_sentences_are_done(self):
        text = "This first sentence is long enough. Then. Another one. And the end"
        parts = self.pieces(text)
        self.assertEqual(parts, ["This first sentence is long enough.", " Then.", " Another one.", " And the end"])

    def test_a_long_run_without_a_sentence_end_breaks_at_a_space(self):
        text = ("word " * 60).strip()
        parts = self.pieces(text)
        self.assertGreater(len(parts), 1)
        self.assertTrue(parts[0].endswith(" ") and len(parts[0]) <= RUN)

    def test_newlines_and_cjk_full_stops_end_sentences(self):
        self.assertEqual(self.pieces("A list of things to say first\n- one\n- two")[0], "A list of things to say first\n")
        self.assertEqual(self.pieces("今日はとてもいい天気ですね。散歩しましょう", first_min=10)[0], "今日はとてもいい天気ですね。")

    def test_short_reply_waits_for_the_end(self):
        self.assertEqual(self.pieces("Yes. Sure."), ["Yes. Sure."])


if __name__ == "__main__":
    unittest.main()
