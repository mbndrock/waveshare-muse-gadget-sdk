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

"""OBD-DID-CARDS-TASK: Honda DIDs 0x2663 (misfire counts, and the misfire
warning) and 0x2610 (A/C clutch, generator duty). The parsers against OBDb's
fixture frames, the trigger matrix and the per-DID schedule run in the ELM
harness (test_muse_obd_elm.py); the probe, the choreography, the reaction,
the bench and the kill switch against the real task in the DID harness
(test_muse_obd_did.py). This checks the wiring and the surfaces around them:
obd.read and >obd, the cards, the trip log's columns, and the invariants."""
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_obd import function_body, gated_block_with  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseObdDidCardsTest(unittest.TestCase):
    def test_fixtures_cite_obdb(self):
        frames = (ROOT / "tests/did_cards_frames.h").read_text()
        self.assertIn("DA10.10.222610.yaml", frames)
        self.assertIn("DA10.10.222663.yaml", frames)
        # Every fixture is a 57-byte reply: first frame 10 39, then 62 26 xx.
        for name, did in (("S2000_2610_AC_OFF", "2610"), ("S2000_2610_AC_ON", "2610"),
                          ("S2000_2663_NONE", "2663"), ("S2000_2663_CYL1", "2663"), ("S2000_2663_CYL2", "2663"),
                          ("S2000_2663_CYL3", "2663"), ("S2000_2663_CYL4", "2663")):
            body = frames[frames.index(f"static const char *const {name}[]"):]
            first = re.search(r'"([0-9A-F]+)"', body).group(1)
            self.assertEqual(first[:8], "18DAF110", name)
            self.assertEqual(first[8:18], "103962" + did, name)

    def test_decodes(self):
        header = (MUSE / "muse_obd_elm.h").read_text()
        for define in ('#define MUSE_DID_2610_REQ "222610"', "#define MUSE_DID_2610_MIN 33",
                       "#define MUSE_DID_2610_GEN 23", "#define MUSE_DID_2610_AC 32", "#define MUSE_DID_2610_AC_ON 0x02",
                       '#define MUSE_DID_2663_REQ "222663"', "#define MUSE_DID_2663_MIN 42",
                       "#define MUSE_DID_2663_MIS 34", "#define MUSE_MISFIRE_CYLS 4",
                       "#define MUSE_DID_2610_MIN_MS 3000u", "#define MUSE_DID_2663_MIN_MS 3000u",
                       "#define MUSE_SCHED_DID_MIN_MS 1000u", "#define MUSE_MISFIRE_COOLDOWN_MS 120000u"):
            self.assertIn(define, header)
        elm = (MUSE / "muse_obd_elm.c").read_text()
        # The 0x2612 parser keeps its exact length; the new ones need at least theirs.
        self.assertIn("if (n != MUSE_VTEC_DID_LEN) {", function_body(elm, "muse_elm_result_t muse_elm_parse_vtec_did("))
        self.assertIn("if (n - 3 < MUSE_DID_2610_MIN) {", function_body(elm, "muse_elm_result_t muse_elm_parse_did_2610("))
        self.assertIn("if (n - 3 < MUSE_DID_2663_MIN) {", function_body(elm, "muse_elm_result_t muse_elm_parse_did_2663("))

    def test_schedule_items(self):
        elm = (MUSE / "muse_obd_elm.c").read_text()
        for row in ("{ SCHED_DID_PID(MUSE_DID_2612), MUSE_SCHED_DID_MIN_MS, 1 },",
                    "{ SCHED_DID_PID(MUSE_DID_2610), MUSE_DID_2610_MIN_MS, 1 },",
                    "{ SCHED_DID_PID(MUSE_DID_2663), MUSE_DID_2663_MIN_MS, 1 },"):
            self.assertIn(row, elm)
        # A DID is eligible only once its probe answered, and on its floor.
        eligible = function_body(elm, "static bool sched_eligible(")
        self.assertIn("s->did_on[d] && (!s->have[i] || now_ms - s->last_ms[i] >= SCHED[i].target_ms)", eligible)

    def test_the_trigger_reads_the_did(self):
        """The misfire trigger lives with the others (muse_obd_elm.c) and is
        fed by the 0x2663 reading with the latest RPM, as the coolant one
        is; it fires through muse_obd_fire_misfire, the coolant warning's
        plumbing, and only while OBD is on."""
        obd = (MUSE / "muse_obd.c").read_text()
        got = function_body(obd, "static void got_did(")
        self.assertIn("if (s_enabled && s_conn != BLE_HS_CONN_HANDLE_NONE && set_2663(&v->misfire, s_rpm)) {", got)
        self.assertIn("muse_obd_fire_misfire();", got)
        set2663 = function_body(obd, "static bool set_2663(")
        self.assertIn("bool fire = muse_misfire_update(&s_misfire, v->count, rpm, at);", set2663)
        self.assertIn("s_status.misfire_rose = s_misfire.rose;", set2663)
        fire = function_body(obd, "void muse_obd_fire_misfire(void)")
        self.assertIn("if (s_enabled) {", fire)
        self.assertIn("muse_state_start_coolant();", fire)
        # The bench goes through the same trigger, and puts the car's baseline back.
        bench = function_body(obd, "int muse_obd_bench_misfire(void)")
        self.assertIn("if (!s_enabled) {", bench)
        self.assertIn("muse_misfire_update(&s_misfire, up, MUSE_COOLANT_RUNNING_RPM, now);", bench)
        self.assertIn("memcpy(s_misfire.base, real.base, sizeof(real.base));", bench)
        self.assertIn("muse_obd_fire_misfire();", bench)
        # A dropped link: the next connection starts from a baseline.
        self.assertIn("muse_misfire_reset(&s_misfire);", function_body(obd, "static void drop(void)"))
        # No RPM inference, no trigger logic in the UI.
        ui = (MUSE / "muse_ui.c").read_text()
        self.assertNotIn("misfire", ui)

    def test_bench_console(self):
        source = (MUSE / "muse_input.c").read_text()
        body = function_body(source, "static bool console_command(")
        block = gated_block_with(body, "muse_obd_bench_misfire()")
        self.assertIsNotNone(block)
        self.assertIn('|| !strcmp(line, "misfire"))) {', block)   # "@obd.off" while off
        self.assertIn('"@misfire fired" : "@misfire cooldown"', block)

    def test_obd_read_and_console(self):
        header = (MUSE / "muse_obd.h").read_text()
        for field in ("bool did_2610;", "bool did_2663;", "muse_obd_reading_t misfire[MUSE_OBD_MISFIRE_CYLS];",
                      "uint8_t misfire_rose;", "muse_obd_reading_t ac_clutch;", "muse_obd_reading_t generator_pct;"):
            self.assertIn(field, header)
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static cJSON *obd_read_command(void)")
        self.assertIn('cJSON_AddBoolToObject(payload, "did_2610", st.did_2610);', body)
        self.assertIn('cJSON_AddBoolToObject(payload, "did_2663", st.did_2663);', body)
        for key in ("misfire_1", "misfire_2", "misfire_3", "misfire_4", "misfire_1_age_ms", "misfire_4_age_ms"):
            self.assertIn(f'"{key}"', body)
        self.assertIn('add_obd_reading(payload, "ac_clutch", "ac_clutch_age_ms"', body)
        self.assertIn('add_obd_reading(payload, "generator_pct", "generator_age_ms"', body)
        console = gated_block_with((MUSE / "muse_input.c").read_text(), "static void print_obd(void)")
        for key in ('\\"did_2610\\"', '\\"did_2663\\"', '\\"misfire_%d\\"', '\\"ac_clutch\\"', '\\"generator_pct\\"'):
            self.assertIn(key, console)
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index('add_command(commands, "obd.read"')
        text = noise[start:noise.index("nullptr, nullptr);", start)]
        for key in ("misfire_1", "ac_clutch", "generator_pct", "did_2610", "did_2663"):
            self.assertIn(key, text)
        # The link drop clears the readings, the answers stay.
        obd = (MUSE / "muse_obd.c").read_text()
        down = function_body(obd, "static void set_link_down(void)")
        for line in ("s_status.misfire[c].valid = false;", "s_status.misfire_rose = 0;",
                     "s_status.ac_clutch.valid = false;", "s_status.generator_pct.valid = false;"):
            self.assertIn(line, down)
        self.assertNotIn("set_did_supported", function_body(obd, "static void drop(void)"))

    def test_cards(self):
        cards = (MUSE / "muse_obd_cards.c").read_text()
        self.assertIn('ROW(0, "A/C", "", SRC_AC, .onoff = true),', cards)
        self.assertIn('ROW(1, "MISFIRE", "", SRC_MISFIRE, .misfire = true),', cards)
        self.assertIn('ROW(1, "TIMING", "deg", MUSE_OBD_TIMING, .half = true),', cards)
        self.assertIn('ROW(1, "AFR", ":1", MUSE_OBD_AFR, .half = true),', cards)
        # Every value from before stays on its card.
        names = re.findall(r'ROW\((\d), "([^"]+)"', cards)
        for card, name in (("0", "COOLANT"), ("0", "INTAKE AIR"), ("0", "AMBIENT"), ("1", "RPM"), ("1", "TIMING"),
                           ("1", "LOAD"), ("1", "MAP"), ("1", "AFR"), ("2", "SPEED"), ("2", "THROTTLE"),
                           ("2", "FUEL"), ("2", "STFT"), ("2", "LTFT")):
            self.assertIn((card, name), names)
        # The misfire row: "--" until a read, amber on the latest increase,
        # the 3 s dimming as elsewhere; the counts by cylinder, 1 to 4.
        tick = function_body(cards, "static void tick_misfire(")
        self.assertIn('snprintf(text, sizeof(text), "--");', tick)
        self.assertIn("v->age_ms > MUSE_OBD_CARD_STALE_MS ? COLOR_STALE", tick)
        self.assertIn("st->misfire_rose & (1u << k) ? COLOR_AMBER : COLOR_TEXT", tick)
        self.assertIn('{ "1", "2", "3", "4" }', cards)
        self.assertIn("#define COOLANT_AMBER_C MUSE_COOLANT_REARM_C", cards)   # the same amber
        # The A/C cell: ON accent, OFF dim, "--" until the first read.
        main_tick = function_body(cards, "void muse_obd_cards_tick(void)")
        self.assertIn('!v.valid ? "--" : v.value ? "ON" : "OFF"', main_tick)
        self.assertIn("!v.valid ? COLOR_DIM : stale ? COLOR_STALE : v.value ? COLOR_ACCENT : COLOR_DIM", main_tick)
        # A DID row is never hidden by the PID support scan.
        self.assertIn("*pid = 0;   /* a DID, not a Mode 01 PID: the support scan never hides it */", cards)
        # snprintf into the misfire text: at most five digits and the NUL.
        self.assertIn('char text[8];   /* 0..65535: five digits and the NUL */', tick)
        self.assertIn('snprintf(text, sizeof(text), "%u", (unsigned)(v->value & 0xFFFF));', tick)

    def test_trip_log_columns(self):
        store = (MUSE / "muse_obd_log_store.c").read_text()
        cols = re.findall(r'\[MUSE_LOG_\w+\] = "(\w+)",', store)
        self.assertEqual(cols[-7:], ["baro_kpa", "misfire_1", "misfire_2", "misfire_3", "misfire_4", "ac_clutch",
                                     "generator_pct"])
        log = (MUSE / "muse_obd_log.c").read_text()
        sample = function_body(log, "void muse_obd_log_sample_of(")
        self.assertIn("out->has[MUSE_LOG_MISFIRE_1 + c] = st->misfire[c].valid;", sample)
        self.assertIn("out->has[MUSE_LOG_AC_CLUTCH] = st->ac_clutch.valid;", sample)
        self.assertIn("out->has[MUSE_LOG_GENERATOR] = st->generator_pct.valid;", sample)

    def test_invariants(self):
        obd = (MUSE / "muse_obd.c").read_text()
        self.assertEqual(obd.count("muse_hatch_say"), 0)
        elm = (MUSE / "muse_obd_elm.h").read_text()
        for define in ("#define MUSE_COOLANT_FIRE_C 105", "#define MUSE_COOLANT_REARM_C 100",
                       "#define MUSE_COOLANT_SUSTAIN_MS 5000u", "#define MUSE_COOLANT_COOLDOWN_MS 120000u",
                       "#define MUSE_COOLANT_RUNNING_RPM 400", "#define MUSE_WARMUP_FIRE_C 82",
                       "#define MUSE_WARMUP_COLD_C 70", "#define MUSE_VTEC_FRESH_MS 3000u"):
            self.assertIn(define, elm)
        # Nothing but reads go to the car: every request string is a 22 or Mode 01.
        sent = set(re.findall(r'elm_cmd\("([^"]+)"', obd))
        self.assertEqual(sent, {"ATDPN", "ATAR", "ATSHDB33F1", "ATSHDA10F1", "ATCRA18DAF110", "ATAT2"})
        for path in (MUSE / "muse_obd.c", MUSE / "muse_obd.h", MUSE / "muse_obd_elm.c", MUSE / "muse_obd_elm.h",
                     MUSE / "muse_obd_cards.c", MUSE / "muse_obd_cards.h", MUSE / "muse_obd_log_store.c",
                     ROOT / "tests/did_cards_frames.h"):
            self.assertNotIn("hatch", path.read_text().lower(), path.name)


if __name__ == "__main__":
    unittest.main()
