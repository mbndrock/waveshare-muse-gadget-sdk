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

"""OBD-VTEC-DID-TASK: the VTEC solenoid from Honda's DID 0x2612. Against the
real OBD task (muse_obd.c over obd_enable_fakes/, a stateful scripted
adapter): the exchange is state-safe, takes one slot at the start of each
rotation, survives garbled and hung replies without dropping the link or
losing RPM, is probed once per connection and only on CAN 29-bit. The held
pose (muse_state.c) on a stepped clock. Then the contracts: the RPM fire path
is gone, obd.read and >obd carry the DID fields, the bench's >vtecsol, the
Engine card's VTEC readout, and the trip log's two columns. The parser itself
runs against the real S2000 frames in test_muse_obd_elm.py."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_obd import function_body, gated_block_with, strip_gated  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
SANITIZE = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
ENV = {**os.environ,
       "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
       "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}


def build_and_run(test, name, args, expect):
    cc = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / name
        built = subprocess.run([*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", *SANITIZE,
                                *args, "-o", str(binary)], capture_output=True, text=True)
        test.assertEqual(built.returncode, 0, built.stdout + built.stderr)
        test.assertEqual(built.stderr, "")
        ran = subprocess.run([str(binary)], capture_output=True, text=True, env=ENV, timeout=180)
        test.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        test.assertEqual(ran.stdout.strip(), expect, ran.stdout + ran.stderr)
        test.assertEqual(ran.stderr, "", ran.stderr)


class MuseObdDidTaskTest(unittest.TestCase):
    def test_did_against_the_task(self):
        build_and_run(self, "muse_obd_did", [
            "-DMUSE_OBD_HARNESS_DID", "-I", str(ROOT / "tests/obd_enable_fakes"), "-I", str(MUSE),
            "-I", str(ROOT / "tests"), str(ROOT / "tests/muse_obd_enable_harness.c"),
            str(MUSE / "muse_obd_elm.c"), "-lpthread"], "PASS muse_obd_did")

    def test_held_pose(self):
        build_and_run(self, "muse_state_vtec", [
            "-D_DEFAULT_SOURCE", "-I", str(ROOT / "tests/state_fakes"), "-I", str(MUSE),
            str(ROOT / "tests/muse_state_vtec_harness.c"), "-lm"], "PASS muse_state_vtec")


class MuseObdDidContractTest(unittest.TestCase):
    def test_the_rpm_fire_path_is_gone(self):
        """Solenoid only: no code may fire VTEC from RPM, at any threshold."""
        for path in [*MUSE.glob("*.[ch]"), *MUSE.glob("*.cpp"), *(ROOT / "main").glob("*.[ch]"),
                     ROOT / "avatar/muse_pixel.c"]:
            text = path.read_text()
            for name in ("MUSE_VTEC_ARM_RPM", "MUSE_VTEC_FIRE_RPM", "MUSE_VTEC_COOLDOWN_MS", "muse_vtec_update",
                         "muse_vtec_trigger_t", "muse_vtec_disarm", "muse_obd_fire_vtec", "muse_state_start_vtec"):
                self.assertNotIn(name, text, f"{name} in {path.name}")
        obd = (MUSE / "muse_obd.c").read_text()
        poll = function_body(obd, "static bool poll_rpm(void)")
        self.assertNotIn("muse_state_", poll)
        self.assertIsNone(re.search(r"\b(6000|5500)\b", obd))
        on = function_body(obd, "bool muse_obd_vtec_on(const muse_obd_status_t *st)")
        self.assertNotIn("rpm", on)
        self.assertIn("st->vtec_sol.valid, st->vtec_sol.value != 0, st->vtec_sol.age_ms", on)
        elm = (MUSE / "muse_obd_elm.c").read_text()
        self.assertIn("return valid && sol && age_ms <= MUSE_VTEC_FRESH_MS;",
                      function_body(elm, "bool muse_vtec_engaged("))
        header = (MUSE / "muse_obd_elm.h").read_text()
        self.assertIn("#define MUSE_VTEC_FRESH_MS 3000u", header)
        self.assertIn("#define MUSE_OBD_CARD_STALE_MS 3000u", (MUSE / "muse_obd_cards.h").read_text())
        # RPM polling itself is untouched: the scheduler keeps it coming
        # and its single-PID path still lands in set_rpm.
        self.assertIn("muse_sched_pick(&s_sched, now_ms(), &req);", poll)
        self.assertIn("set_rpm(locked, rpm);", poll)

    def test_the_level_is_fed_every_chrome_tick(self):
        ui = (MUSE / "muse_ui.c").read_text()
        chrome = function_body(ui, "static void update_chrome(float now)")
        block = gated_block_with(chrome, "muse_state_set_vtec(muse_obd_vtec_on(&o));")
        self.assertIsNotNone(block)
        self.assertLess(block.index("muse_obd_status(&o);"), block.index("muse_state_set_vtec("))
        self.assertNotIn("muse_state_set_vtec", strip_gated(ui))
        state = (MUSE / "muse_state.c").read_text()
        self.assertIsNotNone(gated_block_with(state, "void muse_state_set_vtec(bool on)"))
        # The tickle machinery's shape: wait at the hold while it goes on.
        self.assertIn("t > MUSE_VTEC_HOLD ? MUSE_VTEC_HOLD : t", function_body(state, "static float vtec_locked("))
        pixel = (MUSE / "muse_pixel.h").read_text()
        self.assertIn("#define MUSE_VTEC_HOLD 0.5f", pixel)
        self.assertIn("#define MUSE_VTEC_SETTLE 0.82f", pixel)
        # Jollybot animates VTEC on the clock, like tickle, so the hold lives.
        jolly = (ROOT / "avatar/muse_pixel.c").read_text()
        branch = jolly[jolly.index("r->kind = RE_VTEC;"):jolly.index("} else if (p->warmup > 0) {")]
        self.assertIn("r->ts = p->t;", branch)

    def test_exchange_is_read_only_and_puts_the_state_back(self):
        obd = (MUSE / "muse_obd.c").read_text()
        read = function_body(obd, "static muse_elm_result_t did_read(")
        begin = function_body(obd, "static bool did_begin(void)")
        self.assertIn('elm_cmd("ATSHDA10F1", CMD_MS)', begin)
        self.assertIn('elm_cmd("ATCRA18DAF110", CMD_MS)', begin)
        ask = function_body(obd, "static muse_elm_result_t did_ask(")
        self.assertIn("elm_cmd(MUSE_DID_INFO[d].req, CMD_MS)", ask)   # OBD-DID-CARDS-TASK: one table
        self.assertIn("*link = did_restore();", read)   # whatever happened before it
        # Every DID the table asks is a UDS 0x22 read (ReadDataByIdentifier):
        # 2612, 2610 and 2663, nothing else.
        header = (MUSE / "muse_obd_elm.h").read_text()
        reqs = dict(re.findall(r'#define (MUSE_\w+_REQ) "([0-9A-F]+)"', header))
        self.assertEqual(reqs, {"MUSE_VTEC_DID_REQ": "222612", "MUSE_DID_2610_REQ": "222610",
                                "MUSE_DID_2663_REQ": "222663"})
        table = (MUSE / "muse_obd_elm.c").read_text()
        table = table[table.index("const muse_did_info_t MUSE_DID_INFO[MUSE_DIDS] = {"):]
        table = table[:table.index("};")]
        self.assertEqual(re.findall(r"(MUSE_\w+_REQ)", table), ["MUSE_VTEC_DID_REQ", "MUSE_DID_2610_REQ",
                                                               "MUSE_DID_2663_REQ"])
        restore = function_body(obd, "static bool did_restore(void)")
        self.assertIn('elm_cmd("ATAR", CMD_MS)', restore)
        self.assertIn('elm_cmd("ATSHDB33F1", CMD_MS)', restore)
        self.assertIn('#define MUSE_VTEC_DID_REQ "222612"', (MUSE / "muse_obd_elm.h").read_text())
        # Every command the task sends: ELM setup, Mode 01 PIDs, and the one
        # 0x22 read. Nothing that writes to, resets or unlocks the car.
        sent = set(re.findall(r'elm_cmd\("([^"]+)"', obd))
        self.assertEqual(sent, {"ATDPN", "ATAR", "ATSHDB33F1", "ATSHDA10F1", "ATCRA18DAF110", "ATAT2"})
        # ATST's byte varies, so the command is built into a buffer by
        # apply_atst(), never sent as a literal; the batch's Mode 01 string
        # is built the same way in the poll loop.
        self.assertNotIn('elm_cmd("ATST', obd)
        self.assertIn("static void apply_atst(int byte)", obd)
        init = re.search(r'static const char \*const CMDS\[\] = \{([^}]*)\}', obd).group(1)
        self.assertEqual(re.findall(r'"([^"]+)"', init), ["ATZ", "ATE0", "ATL0", "ATH0", "ATS0", "ATSP0"])
        for line in obd.splitlines():
            for bad in ('"2E', '"31', '"27', '"14', '"04"', '"1001', '"1003', '"11'):
                self.assertNotIn("elm_cmd(" + bad, line.replace(" ", ""))

    def test_probe_once_per_connection_on_can_29_only(self):
        obd = (MUSE / "muse_obd.c").read_text()
        probe = function_body(obd, "static bool probe_did(void)")
        # OBD-DID-CARDS-TASK: each DID in turn, each once per connection.
        self.assertIn("if (s_protocol != 7) {", probe)
        self.assertIn("if (s_did[d] != DID_UNPROBED) {", probe)
        self.assertIn("s_did[d] = res == DID_OK[d] ? DID_YES : DID_NO;", probe)
        task = function_body(obd, "static void obd_task(void *arg)")
        self.assertLess(task.index("scan_support()"), task.index("probe_did()"))
        self.assertLess(task.index("probe_did()"), task.index("poll_rpm()"))
        drop = function_body(obd, "static void drop(void)")
        self.assertIn("s_did[d] = DID_UNPROBED;", drop)
        self.assertNotIn("set_did_supported", drop)   # availability stays, like the protocol
        self.assertNotIn("s_status.did_2612 =", obd.replace("s_status.did_2612 = yes", ""))
        self.assertNotIn("s_status.did_2610 =", obd.replace("s_status.did_2610 = yes", ""))
        self.assertNotIn("s_status.did_2663 =", obd.replace("s_status.did_2663 = yes", ""))
        down = function_body(obd, "static void set_link_down(void)")
        for field in ("vtec_sol", "vtec_psw", "did_2612_b23"):
            self.assertIn(f"s_status.{field}.valid = false;", down)
        # The poll loop's scheduler starts from the probe's answer (and
        # re-reads it if the protocol only turned up late).
        self.assertIn("muse_sched_init(&s_sched, &s_support, did_mask(), now_ms());",
                      function_body(obd, "static bool poll_rpm(void)"))

    def test_obd_read_and_console_carry_the_did_fields(self):
        header = (MUSE / "muse_obd.h").read_text()
        for field in ("muse_obd_reading_t vtec_sol;", "muse_obd_reading_t vtec_psw;", "bool did_2612;",
                      "muse_obd_reading_t did_2612_b23;"):
            self.assertIn(field, header)
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static cJSON *obd_read_command(void)")
        self.assertIn('add_obd_reading(payload, "vtec_sol", "vtec_sol_age_ms"', body)
        self.assertIn('add_obd_reading(payload, "vtec_psw", "vtec_psw_age_ms"', body)
        self.assertIn('cJSON_AddBoolToObject(payload, "did_2612", st.did_2612);', body)
        self.assertIn('cJSON_AddNullToObject(payload, "did_2612_b23");', body)
        console = gated_block_with((MUSE / "muse_input.c").read_text(), "static void print_obd(void)")
        for key in ("vtec_sol", "vtec_psw", "did_2612", "did_2612_b23"):
            self.assertIn(f'\\"{key}\\"', console)

    def test_bench_vtecsol_replaces_vtec(self):
        source = (MUSE / "muse_input.c").read_text()
        body = function_body(source, "static bool console_command(")
        block = gated_block_with(body, "muse_obd_bench_vtec_sol(")
        self.assertIsNotNone(block)
        self.assertIn('!strcmp(line, "vtecsol 0") || !strcmp(line, "vtecsol 1")', block)
        self.assertIn("@obd.off", block)
        self.assertNotIn('"vtec"', source)
        self.assertNotIn("muse_obd_bench_vtec_sol", strip_gated(body))
        obd = (MUSE / "muse_obd.c").read_text()
        # A real reading replaces the bench's; the bench's is always fresh.
        self.assertIn("set_vtec(&v->vtec, false);", function_body(obd, "static void got_did("))
        self.assertIn("uint32_t vtec_age = bench ? 0 : now - vtec_at;", obd)

    def test_engine_card_shows_vtec(self):
        cards = (MUSE / "muse_obd_cards.c").read_text()
        self.assertIn('ROW(1, "RPM", "rpm", SRC_RPM, .hero = true, .bar_max = 8000, .vtec = true),', cards)
        row = function_body(cards, "static void build_row(")
        self.assertLess(row.index("row->bar = build_bar("), row.index('label(status, &lv_font_montserrat_16, COLOR_DIM, "VTEC");'))
        tick = function_body(cards, "static void tick_vtec(")
        self.assertIn("st->vtec_sol.age_ms <= MUSE_OBD_CARD_STALE_MS", tick)
        self.assertIn('!fresh ? "--" : st->vtec_sol.value ? "ON" : "OFF"', tick)
        self.assertIn("fresh && st->vtec_sol.value ? COLOR_ACCENT : COLOR_DIM", tick)
        self.assertIn("set_text(row->vtec, word);", tick)   # only on change

    def test_trip_log_columns(self):
        store = (MUSE / "muse_obd_log_store.c").read_text()
        # The two VTEC columns, in place; OBD-MORE-PIDS-TASK appends after them.
        self.assertIn('[MUSE_LOG_VTEC_SOL] = "vtec_sol",\n    [MUSE_LOG_VTEC_PSW] = "vtec_psw",\n'
                      '    [MUSE_LOG_AFR] = "afr",', store)
        log = (MUSE / "muse_obd_log.c").read_text()
        sample = function_body(log, "void muse_obd_log_sample_of(")
        self.assertIn("out->has[MUSE_LOG_VTEC_SOL] = st->vtec_sol.valid;", sample)
        self.assertIn("out->has[MUSE_LOG_VTEC_PSW] = st->vtec_psw.valid;", sample)
        self.assertIn('"\\n# did_2612=yes"', store)

    def test_new_text_says_muse(self):
        for path in (MUSE / "muse_obd.c", MUSE / "muse_obd.h", MUSE / "muse_obd_elm.c", MUSE / "muse_obd_elm.h",
                     MUSE / "muse_obd_cards.c", MUSE / "muse_state.c"):
            self.assertNotIn("hatch", path.read_text().lower(), path.name)


if __name__ == "__main__":
    unittest.main()
