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
FIRST = 30  # SPEECH_FIRST_MIN, muse_chat_session.cpp
LATER = 240 # SPEECH_LATER_MIN, muse_chat_session.cpp

# A reply of many short sentences, about 600 bytes.
STORY = ("The weather today is mild and calm. " + " ".join(
    f"Point {i} costs 3.50 euros, e.g at noon." for i in range(14)) + " That is all for now")


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

    def pieces(self, text: str, first_min: int = 30, step: int = 1, later: int | None = None) -> list[str]:
        args = [str(self.binary), "speech", str(first_min), str(step)] + ([] if later is None else [str(later)])
        out = subprocess.run(args, input=text.encode(), capture_output=True, check=True).stdout.decode()
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

    # ---- The production minimums: 30 bytes first, 240 after ----

    def test_later_pieces_group_several_sentences(self):
        for step in (1, 7, 40):
            old = self.pieces(STORY, FIRST, step)
            new = self.pieces(STORY, FIRST, step, LATER)
            self.assertEqual(new[0], old[0], "the first piece is unchanged")
            self.assertLess(len(new), len(old))
            self.assertLessEqual(len(new), 4)
            longest_old = max(len(p) for p in old[1:])
            self.assertTrue(all(len(p) > 3 * longest_old for p in new[1:-1]), (step, new))

    def test_a_later_piece_waits_for_240_bytes_before_a_sentence_end(self):
        for step in (1, 5, 33):
            parts = self.pieces(STORY, FIRST, step, LATER)
            self.assertGreater(len(parts), 2)
            for p in parts[1:-1]:
                self.assertGreaterEqual(len(p), LATER, (step, p))
                self.assertIn(p[-1], ".!?\n")
        # Short of 240 when it's done: the rest goes anyway, as one piece.
        text = "This first sentence is long enough. Then. Another one. And the end."
        self.assertEqual(self.pieces(text, FIRST, 1, LATER),
                         ["This first sentence is long enough.", " Then. Another one. And the end."])

    def test_a_long_run_without_a_sentence_end_waits_for_the_later_minimum(self):
        text = ("word " * 200).strip()
        parts = self.pieces(text, FIRST, 1, LATER)
        self.assertTrue(parts[0].endswith(" ") and len(parts[0]) <= RUN, "the first still breaks at 160")
        self.assertGreater(len(parts), 2)
        for p in parts[1:-1]:
            self.assertTrue(p.endswith(" "), p)
            self.assertGreater(len(p), RUN)
            self.assertLessEqual(len(p), LATER)
            self.assertGreater(len(p), LATER - len("word "))

    def test_production_minimums_keep_sentence_rules(self):
        text = STORY + "\n今日はとてもいい天気ですね。" * 30 + "\nThe end"
        for step in (1, 3, 64):
            parts = self.pieces(text, FIRST, step, LATER)   # checks every byte said once, in order
            raw, cut = text.encode(), 0
            for p in parts[:-1]:
                cut += len(p.encode())
                # Only at a real sentence end: never inside "3.50" or "e.g".
                self.assertTrue(raw[cut - 1:cut] == b"\n" or raw[cut - 3:cut] == "。".encode()
                                or (raw[cut - 1:cut] in (b".", b"!", b"?") and raw[cut:cut + 1] in (b" ", b"\n")),
                                (step, p[-12:]))
        self.assertEqual(self.pieces("Yes. Sure.", FIRST, 1, LATER), ["Yes. Sure."])
        self.assertEqual(self.pieces(STORY, FIRST, len(STORY), LATER), [STORY])


if __name__ == "__main__":
    unittest.main()
