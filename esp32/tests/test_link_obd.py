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
        block = gated_block_with(body, 'if (!strcmp(line, "warmup"))')
        self.assertIsNotNone(block)
        self.assertIn("muse_obd_fire_warmup();", block)

    def test_car_reactions_are_visual_only(self):
        """The fires start the reaction and say nothing: a spoken line would
        swap the reaction face for the speaking pose."""
        source = (ROOT / "components/muse/muse_obd.c").read_text()
        self.assertNotIn("muse_hatch_say", source)
        for name in ("vtec", "coolant", "warmup"):
            start = source.index(f"void muse_obd_fire_{name}(void)")
            body = source[start:source.index("\n}\n", start)]
            self.assertIn(f"muse_state_start_{name}();", body)
        header = (ROOT / "components/muse/muse_obd.h").read_text()
        self.assertIn("void muse_obd_fire_warmup(void);", header)

    def test_warmup_trigger_is_fed_and_reset(self):
        source = (ROOT / "components/muse/muse_obd.c").read_text()
        start = source.index("static void got_coolant(void)")
        body = source[start:source.index("\n}\n", start)]
        self.assertIn("muse_warmup_update(&s_warmup, coolant_c)", body)
        start = source.index("static void drop(void)")
        body = source[start:source.index("\n}\n", start)]
        self.assertIn("muse_warmup_init(&s_warmup);", body)
        self.assertIn('#define COOLANT_EVERY 4 ', source)   # poll cadence unchanged

    def test_status_row_indicator_is_gated(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIsNotNone(gated_block_with(ui, '#include "muse_obd.h"'))
        self.assertIsNotNone(gated_block_with(ui, "static lv_obj_t *s_obd_icon;"))
        build = gated_block_with(ui, 'lv_label_set_text(s_obd_icon, "OBD");')
        self.assertIsNotNone(build)
        self.assertIn("&lv_font_montserrat_14", build)
        tick = gated_block_with(ui, "muse_obd_status(&o);")
        self.assertIsNotNone(tick)
        self.assertIn("LV_OBJ_FLAG_HIDDEN", tick)
        self.assertIn("obd != s_shown_obd", tick)
        # Nothing about the indicator outside the gate.
        stripped = ui
        start = stripped.find(GATE)
        while start != -1:
            end = stripped.index("#endif", start)
            stripped = stripped[:start] + stripped[end:]
            start = stripped.find(GATE)
        self.assertNotIn("s_obd_icon", stripped)
        self.assertNotIn("muse_obd_", stripped)


if __name__ == "__main__":
    unittest.main()
