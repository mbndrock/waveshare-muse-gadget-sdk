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

from pathlib import Path
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


class LinkObdTest(unittest.TestCase):
    def test_command_is_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        self.assertEqual(noise.count('"obd.read"'), 1)
        self.assertIsNotNone(gated_block_with(noise, 'add_command(commands, "obd.read"'))

        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        self.assertEqual(dispatch.count('"obd.read"'), 1)
        block = gated_block_with(dispatch, '"obd.read"')
        self.assertIsNotNone(block)
        self.assertIn("obd_read_command()", block)
        self.assertIsNotNone(gated_block_with(app, '#include "muse_obd.h"'))
        self.assertIsNotNone(gated_block_with(app, "static cJSON *obd_read_command(void)"))

    def test_header_declares_the_snapshot(self):
        header = (ROOT / "components/muse/muse_obd.h").read_text()
        self.assertIn("} muse_obd_status_t;", header)
        self.assertIn("void muse_obd_status(muse_obd_status_t *out);", header)

    def test_console_entry_is_gated(self):
        source = (ROOT / "components/muse/muse_input.c").read_text()
        start = source.index("static bool console_command(")
        body = source[start:source.index("\n}\n", start)]
        block = gated_block_with(body, 'if (!strcmp(line, "obd"))')
        self.assertIsNotNone(block)
        self.assertIn("print_obd();", block)
        self.assertIsNotNone(gated_block_with(source, "static void print_obd(void)"))


if __name__ == "__main__":
    unittest.main()
