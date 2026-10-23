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

"""OBD-MORE-PIDS-TASK: wideband AFR (34), pedals D and E (49, 4A), module
voltage (42), barometric pressure (33) and fuel level (2F). The parsers, the
scheduler and the end to end run are in the ELM and enable harnesses; this
checks the contracts around them: the tiers, obd.read and the console line,
the card cells and the trip log's appended columns, and the invariants."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_obd import function_body  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MorePidsTest(unittest.TestCase):
    def test_tiers(self):
        """Freshness floors as the task gives them: fast 1 s for AFR and the
        pedals, mid 2 s for fuel (off the cards' 3 s dimming edge), slow 3 s
        for volts and baro."""
        elm = (MUSE / "muse_obd_elm.c").read_text()
        for pid, target, rank in ((0x34, 1000, 2), (0x49, 1000, 2), (0x4A, 1000, 2),
                                  (0x2F, 2000, 3), (0x42, 3000, 4), (0x33, 3000, 4)):
            self.assertIn(f"{{ 0x{pid:02X}, {target}, {rank} }},", elm)
        self.assertIn("#define MUSE_SCHED_ITEMS 18", (MUSE / "muse_obd_elm.h").read_text())
        # The wide PIDs' byte counts, which a batch's walk depends on.
        nbytes = function_body(elm, "static int pid_nbytes(int pid)")
        self.assertIn("case 0x34: return 4;", nbytes)
        self.assertIn("case 0x42: return 2;", nbytes)

    def test_obd_read_and_console_carry_decimals(self):
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static void add_obd_reading(")
        self.assertIn("static const double SCALE[] = { 1.0, 10.0, 100.0, 1000.0 };", body)
        self.assertIn("cJSON_AddNumberToObject(payload, key, value / SCALE[decimals]);", body)
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index('add_command(commands, "obd.read"')
        text = noise[start:noise.index("nullptr, nullptr);", start)]
        for key in ("afr", "lambda", "pedal_d_pct", "pedal_e_pct", "module_v", "baro_kpa", "fuel_pct"):
            self.assertIn(key, text)

    def test_console_fixed_point(self):
        """The >obd line's decimals, compiled from muse_input.c itself."""
        source = (MUSE / "muse_input.c").read_text()
        number = function_body(source, "static const char *obd_number(")
        fixed = function_body(source, "static const char *obd_fixed(")
        program = ("#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n"
                   + number + "\n}\n" + fixed + "\n}\n" + r'''
int main(void)
{
    char b[16];
    static const struct { bool valid; int value, decimals; const char *want; } C[] = {
        { true, 147, 1, "14.7" }, { true, 1000, 3, "1.000" }, { true, 13845, 3, "13.845" },
        { true, 7, 3, "0.007" }, { true, 0, 1, "0.0" }, { true, -5, 1, "-0.5" }, { true, 62, 0, "62" },
        { false, 147, 1, "null" }, { true, 2147483647, 3, "2147483.647" }, { true, -2147483647 - 1, 3, "-2147483.648" },
    };
    for (size_t i = 0; i < sizeof(C) / sizeof(C[0]); i++) {
        const char *got = obd_fixed(b, sizeof(b), C[i].valid, C[i].value, C[i].decimals);
        if (strcmp(got, C[i].want)) {
            printf("%d/%d: %s, want %s\n", C[i].value, C[i].decimals, got, C[i].want);
            return 1;
        }
    }
    puts("PASS");
    return 0;
}
''')
        cc = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / "fixed.c", Path(tmp) / "fixed"
            src.write_text(program)
            built = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wformat-truncation=2",
                                    "-fsanitize=address,undefined", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.stdout.strip(), "PASS", ran.stdout + ran.stderr)

    def test_card_cells(self):
        """AFR joins Engine and fuel joins Drive; every existing value stays,
        and pairing keeps each card's height units as before, so normal cells
        keep Montserrat 40 (OBD-LOG-TASK Part 5)."""
        cards = (MUSE / "muse_obd_cards.c").read_text()
        rows = re.findall(r'ROW\((\d), "([^"]+)", "[^"]*", (\w+)(.*)\),', cards)
        names = [(int(card), name) for card, name, _, _ in rows]
        self.assertIn((1, "AFR"), names)
        self.assertIn((2, "FUEL"), names)
        for card, name in ((0, "COOLANT"), (0, "INTAKE AIR"), (0, "AMBIENT"), (1, "RPM"), (1, "TIMING"),
                           (1, "LOAD"), (1, "MAP"), (2, "SPEED"), (2, "THROTTLE"), (2, "STFT"), (2, "LTFT")):
            self.assertIn((card, name), names)
        self.assertNotIn("PEDAL", cards)   # pedals, volts and baro: obd.read and the log only
        self.assertNotIn("BARO", cards)
        units = [0, 0, 0]
        run = {}
        for card, name, src, rest in rows:
            card = int(card)
            if ".half = true" in rest:
                run[card] = run.get(card, 0) + 1
                units[card] += 1 if run[card] % 2 == 1 else 0
                continue
            run[card] = 0
            units[card] += 2 if ".hero = true" in rest else 1
        self.assertEqual(units, [3, 5, 4])   # as before this task: Temps 3, Engine 5, Drive 4
        # The AFR reads in tenths: "14.7".
        tick = function_body(cards, "void muse_obd_cards_tick(void)")
        self.assertIn('snprintf(text, sizeof(text), "%d.%d", shown / 10, shown % 10);', tick)

    def test_trip_log_columns_appended(self):
        store = (MUSE / "muse_obd_log_store.c").read_text()
        cols = re.findall(r'\[MUSE_LOG_\w+\] = "(\w+)",', store)
        self.assertEqual(cols[-8:], ["vtec_sol", "vtec_psw", "afr", "fuel_pct", "pedal_d_pct", "pedal_e_pct",
                                     "module_v", "baro_kpa"])
        self.assertEqual(cols[:11], ["rpm", "coolant_c", "iat_c", "timing_deg", "map_kpa", "load_pct",
                                     "throttle_pct", "speed_kmh", "stft_pct", "ltft_pct", "ambient_c"])
        log = (MUSE / "muse_obd_log.c").read_text()
        sample = function_body(log, "void muse_obd_log_sample_of(")
        for ch, extra in (("AFR", "AFR"), ("FUEL", "FUEL"), ("PEDAL_D", "PEDAL_D"), ("PEDAL_E", "PEDAL_E"),
                          ("MODULE_V", "MODULE_V"), ("BARO", "BARO")):
            self.assertIn(f"{{ MUSE_LOG_{ch}, MUSE_OBD_{extra} }}", sample)
        # A row is now past 255 bytes at its widest: the queue's length is 16-bit.
        self.assertIn("uint16_t len;", log)
        self.assertIn("it.len = (uint16_t)n;", log)

    def test_invariants(self):
        obd = (MUSE / "muse_obd.c").read_text()
        self.assertEqual(obd.count("muse_hatch_say"), 0)
        elm = (MUSE / "muse_obd_elm.h").read_text()
        for define in ("#define MUSE_COOLANT_FIRE_C 105", "#define MUSE_COOLANT_REARM_C 100",
                       "#define MUSE_COOLANT_SUSTAIN_MS 5000u", "#define MUSE_COOLANT_COOLDOWN_MS 120000u",
                       "#define MUSE_WARMUP_FIRE_C 82", "#define MUSE_WARMUP_COLD_C 70",
                       "#define MUSE_VTEC_FRESH_MS 3000u"):
            self.assertIn(define, elm)
        for path in (MUSE / "muse_obd.c", MUSE / "muse_obd.h", MUSE / "muse_obd_elm.c", MUSE / "muse_obd_elm.h",
                     MUSE / "muse_obd_cards.c", MUSE / "muse_obd_log_store.c", MUSE / "muse_obd_log_store.h"):
            self.assertNotIn("hatch", path.read_text().lower(), path.name)


if __name__ == "__main__":
    unittest.main()
