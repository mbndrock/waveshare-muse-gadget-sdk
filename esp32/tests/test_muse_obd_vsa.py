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

"""OBD-STEERING-ANGLE-TASK Phase 1: the VSA modulator at DA28. The decodes,
the raw parse, the scan list and the 500 ms tier run in the ELM harness
(test_muse_obd_elm.py); the probe, the grouped DA28 polling, the scan and
the kill switch against the real task in the DID harness
(test_muse_obd_did.py). This checks the wiring and the surfaces around
them: obd.read and >obd, the two Link commands, the Drive card's BRAKE
cell, the trip log's brake_p column, and the invariants."""
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_obd import function_body, gated_block_with  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseObdVsaTest(unittest.TestCase):
    def test_decodes(self):
        header = (MUSE / "muse_obd_elm.h").read_text()
        for define in ('#define MUSE_DID_4004_REQ "224004"', "#define MUSE_DID_4004_MIN 16",
                       "#define MUSE_DID_4004_BRK 15", "#define MUSE_DID_4004_BRK_ON 0x80",
                       '#define MUSE_DID_4005_REQ "224005"', "#define MUSE_DID_4005_MIN 9",
                       "#define MUSE_DID_4005_PRESS 7", "#define MUSE_DID_4004_MIN_MS 500u",
                       "#define MUSE_DID_4005_MIN_MS 500u", "#define MUSE_VSA_SCAN_DIDS_N 26"):
            self.assertIn(define, header)
        elm = (MUSE / "muse_obd_elm.c").read_text()
        self.assertIn("if (n - 3 < MUSE_DID_4004_MIN) {", function_body(elm, "muse_elm_result_t muse_elm_parse_vsa_4004("))
        self.assertIn("if (n - 3 < MUSE_DID_4005_MIN) {", function_body(elm, "muse_elm_result_t muse_elm_parse_vsa_4005("))
        # The provisional pressure scale: -25..120 bar linear over the count.
        self.assertIn("return (raw * 14500 + 32767) / 65535 - 2500;",
                      function_body(elm, "int muse_vsa_brake_centibar("))

    def test_generalised_sessions(self):
        obd = (MUSE / "muse_obd.c").read_text()
        # A session is its DID's (tx header, rx filter); due DIDs group only
        # with their own ECU's, so a DA28 session never carries a DA10 DID.
        poll = function_body(obd, "static bool poll_rpm(void)")
        self.assertIn('strcmp(MUSE_DID_INFO[d].atsh, MUSE_DID_INFO[req.which].atsh) == 0', poll)
        self.assertIn("bool began = did_begin((muse_did_t)req.which);", poll)

    def test_presence_probe(self):
        obd = (MUSE / "muse_obd.c").read_text()
        probe = function_body(obd, "static bool probe_vsa(void)\n{")   # not the forward declaration
        # One DA28 session reading 4004, once per connection (s_did's states
        # reset in drop(), like the DA10 DIDs'); 4005 joins on its verdict.
        self.assertIn("if (s_did[MUSE_DID_4004] != DID_UNPROBED) {", probe)
        self.assertIn("muse_elm_result_t res = did_read(MUSE_DID_4004, &v, &link);", probe)
        self.assertIn("s_did[MUSE_DID_4005] = s_did[MUSE_DID_4004];", probe)
        self.assertIn("set_did_supported(MUSE_DID_4004, yes);", probe)
        # After the DA10 probes, inside probe_did, so the first-RPM retry
        # covers it too; and probe_did only runs on CAN 29-bit.
        da10 = function_body(obd, "static bool probe_did(void)")
        self.assertIn("if (s_protocol != 7) {", da10)
        self.assertIn("for (int i = 0; i < MUSE_DID_4004; i++) {", da10)
        self.assertIn("return probe_vsa();", da10)
        # The verdict stays across a drop (like did_2612); the readings go.
        down = function_body(obd, "static void set_link_down(void)")
        self.assertIn("s_status.vsa_brk_sw.valid = false;", down)
        self.assertIn("s_status.vsa_brk_p.valid = false;", down)
        self.assertNotIn("s_status.vsa =", down)
        self.assertIn("case MUSE_DID_4004: s_status.vsa = yes ? 1 : 0; break;",
                      function_body(obd, "static void set_did_supported("))

    def test_scan_commands(self):
        app = (ROOT / "main/app.c").read_text()
        for fn in ("static cJSON *obd_vsa_scan_command(void)", "static cJSON *obd_vsa_raw_command(cJSON *params)"):
            self.assertIsNotNone(gated_block_with(app, fn), fn)
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        self.assertEqual(dispatch.count('"obd.vsa_scan"'), 1)
        self.assertEqual(dispatch.count('"obd.vsa_raw"'), 1)
        self.assertIn("obd_vsa_scan_command()", gated_block_with(dispatch, '"obd.vsa_scan"'))
        self.assertIn("obd_vsa_raw_command(params)", gated_block_with(dispatch, '"obd.vsa_raw"'))
        noise = (ROOT / "main/noise_control.cpp").read_text()
        self.assertEqual(noise.count('"obd.vsa_scan"'), 1)
        self.assertEqual(noise.count('"obd.vsa_raw"'), 1)
        self.assertIsNotNone(gated_block_with(noise, 'add_command(commands, "obd.vsa_scan"'))
        self.assertIsNotNone(gated_block_with(noise, 'add_command(commands, "obd.vsa_raw"'))
        # The scan runs on the OBD task, one DID per poll-loop pass, in its
        # own DA28 session; off refuses the start outright.
        obd = (MUSE / "muse_obd.c").read_text()
        self.assertIn("if (vsa_scan_running() && !vsa_scan_step()) {", function_body(obd, "static bool poll_rpm(void)"))
        step = function_body(obd, "static bool vsa_scan_step(void)")
        self.assertIn("did_begin(MUSE_DID_4004)", step)
        self.assertIn("muse_elm_parse_did_raw(s_rx.resp, did,", step)
        self.assertIn("bool link = did_restore();", step)
        start_fn = function_body(obd, "bool muse_obd_vsa_scan_start(void)")
        self.assertIn("if (!s_enabled) {", start_fn)
        self.assertIn("vsa_scan_reset();", function_body(obd, "static void drop(void)"))

    def test_obd_read_and_console(self):
        header = (MUSE / "muse_obd.h").read_text()
        for field in ("int vsa;", "muse_obd_reading_t vsa_brk_sw;", "muse_obd_reading_t vsa_brk_p;",
                      "const char *vsa_scan;", "int vsa_scan_answered;",
                      "bool muse_obd_vsa_scan_start(void);", "int muse_obd_vsa_raw(uint16_t did, muse_vsa_raw_t *out);"):
            self.assertIn(field, header)
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static cJSON *obd_read_command(void)")
        self.assertIn('cJSON_AddNullToObject(payload, "vsa");', body)   # unprobed: null, not false
        self.assertIn('add_obd_reading(payload, "vsa_brk_sw", "vsa_brk_sw_age_ms"', body)
        self.assertIn('add_obd_reading(payload, "vsa_brk_p", "vsa_brk_p_age_ms"', body)
        self.assertIn("st.vsa_brk_p.age_ms, 2);", body)   # bar, two decimals
        self.assertIn('cJSON_AddItemToObject(payload, "vsa_scan", scan);', body)
        console = gated_block_with((MUSE / "muse_input.c").read_text(), "static void print_obd(void)")
        for key in ('\\"vsa\\"', '\\"vsa_brk_sw\\"', '\\"vsa_brk_p\\"', '\\"vsa_scan\\"', '\\"vsa_scan_answered\\"'):
            self.assertIn(key, console)
        self.assertIn("obd_fixed(psw, sizeof(psw), st.vsa_brk_p.valid, st.vsa_brk_p.value, 2)", console)
        noise = (ROOT / "main/noise_control.cpp").read_text()
        start = noise.index('add_command(commands, "obd.read"')
        text = noise[start:noise.index("nullptr, nullptr);", start)]
        for key in ("vsa_brk_sw", "vsa_brk_p", "vsa_scan", "DA28"):
            self.assertIn(key, text)

    def test_drive_card_brake_cell(self):
        cards = (MUSE / "muse_obd_cards.c").read_text()
        self.assertIn('ROW(2, "BRAKE", "bar", SRC_BRAKE, .half = true, .bar_max = 12000, .decimals = 1),', cards)
        # Paired with THROTTLE (the two pedals), BRAKE first as on the car.
        rows = re.findall(r'ROW\(2, "([^"]+)"', cards)
        self.assertEqual(rows, ["SPEED", "BRAKE", "THROTTLE", "FUEL", "STFT", "LTFT"])
        # Every value from before stays on its card.
        names = re.findall(r'ROW\((\d), "([^"]+)"', cards)
        for card, name in (("0", "COOLANT"), ("0", "INTAKE AIR"), ("0", "AMBIENT"), ("0", "A/C"),
                           ("1", "RPM"), ("1", "TIMING"), ("1", "AFR"), ("1", "LOAD"), ("1", "MAP"),
                           ("1", "MISFIRE"), ("2", "SPEED"), ("2", "THROTTLE"), ("2", "FUEL"),
                           ("2", "STFT"), ("2", "LTFT")):
            self.assertIn((card, name), names)
        tick = function_body(cards, "void muse_obd_cards_tick(void)")
        # Only once DA28 is confirmed, and "--" until the first read.
        self.assertIn("(def->src == SRC_BRAKE && st.vsa != 1)", tick)
        self.assertIn('unsupported || def->src == SRC_BRAKE ? "--" : "..."', tick)
        # A DID row: the PID support scan never hides it.
        self.assertIn("src == SRC_AC || src == SRC_MISFIRE || src == SRC_BRAKE",
                      function_body(cards, "static muse_obd_reading_t reading("))

    def test_trip_log_column(self):
        store = (MUSE / "muse_obd_log_store.c").read_text()
        cols = re.findall(r'\[MUSE_LOG_\w+\] = "(\w+)",', store)
        self.assertEqual(cols[-1], "brake_p")
        self.assertEqual(cols[-2], "generator_pct")
        self.assertIn("[MUSE_LOG_BRAKE_P] = 2,", store)   # bar, two decimals
        log = (MUSE / "muse_obd_log.c").read_text()
        sample = function_body(log, "void muse_obd_log_sample_of(")
        self.assertIn("out->has[MUSE_LOG_BRAKE_P] = st->vsa_brk_p.valid;", sample)
        self.assertIn("out->value[MUSE_LOG_BRAKE_P] = st->vsa_brk_p.value;", sample)

    def test_invariants(self):
        obd = (MUSE / "muse_obd.c").read_text()
        self.assertEqual(obd.count("muse_hatch_say"), 0)
        elm_h = (MUSE / "muse_obd_elm.h").read_text()
        for define in ("#define MUSE_COOLANT_FIRE_C 105", "#define MUSE_COOLANT_REARM_C 100",
                       "#define MUSE_COOLANT_SUSTAIN_MS 5000u", "#define MUSE_COOLANT_COOLDOWN_MS 120000u"):
            self.assertIn(define, elm_h)
        # Read-only toward the car: service 0x22 only. No session control
        # (0x10), routine control (0x31), security access (0x27) or writes
        # (0x2E / 0x14 / Mode 04) anywhere in the OBD task's requests; the
        # scan's request is built as "22" plus the scanned DID.
        for line in obd.splitlines():
            for bad in ('"2E', '"31', '"27', '"14', '"04"', '"1001', '"1003', '"1002', '"11'):
                self.assertNotIn("elm_cmd(" + bad, line.replace(" ", ""))
        step = function_body(obd, "static bool vsa_scan_step(void)")
        self.assertIn("char req[7] = { '2', '2',", step)
        for path in (MUSE / "muse_obd.c", MUSE / "muse_obd.h", MUSE / "muse_obd_elm.c", MUSE / "muse_obd_elm.h",
                     MUSE / "muse_obd_cards.c", MUSE / "muse_obd_cards.h"):
            self.assertNotIn("hatch", path.read_text().lower(), path.name)
        block = gated_block_with((ROOT / "main/noise_control.cpp").read_text(), 'add_command(commands, "obd.vsa_scan"')
        self.assertNotIn("hatch", block.lower())


if __name__ == "__main__":
    unittest.main()
