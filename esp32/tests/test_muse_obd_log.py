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

"""OBD-LOG-TASK: the trip log. Its files (muse_obd_log_store.c) against a
temporary directory: formats, CRC-32, base64, a trip's bytes through chunked
reads, delete, a simulated reboot, recovery and a full store. Then the
contracts: the obdlog partition, the three pull commands under the OBD gate,
obd.read's log fields, the kill switch, and the cards' layout pass."""
from pathlib import Path
import base64
import os
import re
import shlex
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_link_obd import function_body, gated_block_with, strip_gated  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"
LOG_FIELDS = ("log_card", "log_flash", "log_store", "log_active", "log_trip", "log_pending", "log_dropped")


class MuseObdLogStoreTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_obd_log"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wformat-truncation=2", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(MUSE),
             str(ROOT / "tests/muse_obd_log_harness.c"),
             str(MUSE / "muse_obd_log_store.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True,
        )
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)
        cls.warnings = compiled.stderr

    def test_compiles_clean(self):
        self.assertEqual(self.warnings, "")

    def test_store(self):
        env = {**os.environ,
               "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
               "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"}
        ran = subprocess.run([str(self.binary)], capture_output=True, text=True, env=env, timeout=120)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertEqual(ran.stdout.strip(), "PASS muse_obd_log", ran.stdout + ran.stderr)
        self.assertEqual(ran.stderr, "", ran.stderr)

    def test_crc_and_base64_match_what_muse_checks_with(self):
        """The harness's vectors are zlib's and Python's base64's."""
        self.assertEqual(zlib.crc32(b"123456789"), 0xCBF43926)
        self.assertEqual(zlib.crc32(b"The quick brown fox jumps over the lazy dog"), 0x414FA339)
        self.assertEqual(base64.b64encode(bytes(range(256)))[-8:], b"/P3+/w==")
        harness = (ROOT / "tests/muse_obd_log_harness.c").read_text()
        self.assertIn("0xCBF43926u", harness)
        self.assertIn('"/P3+/w=="', harness)


class MuseObdLogContractTest(unittest.TestCase):
    def test_partition_appended_without_moving_anything(self):
        rows = []
        for line in (ROOT / "partitions_muse.csv").read_text().splitlines():
            if line.strip() and not line.startswith("#"):
                rows.append([f.strip() for f in line.split(",")][:5])
        self.assertEqual(rows, [
            ["nvs", "data", "nvs", "0x11000", "0xC000"],
            ["otadata", "data", "ota", "0x1D000", "0x2000"],
            ["phy_init", "data", "phy", "0x1F000", "0x1000"],
            ["ota_0", "app", "ota_0", "0x20000", "4M"],
            ["ota_1", "app", "ota_1", "0x420000", "4M"],
            ["prod_data", "data", "0x40", "0x820000", "0x1000"],
            ["prod_bak", "data", "0x41", "0x821000", "0x1000"],
            ["obdlog", "data", "fat", "0x830000", "0x7D0000"],
        ])
        self.assertEqual(0x830000 + 0x7D0000, 16 * 1024 * 1024)   # to the end of a 16 MB flash
        self.assertGreaterEqual(0x830000, 0x821000 + 0x1000)
        for other in ROOT.glob("partitions*.csv"):
            if other.name != "partitions_muse.csv":
                self.assertNotIn("obdlog", other.read_text(), other.name)

    def test_flash_store_mounts_wear_levelled_and_formats_only_a_bad_volume(self):
        log = (MUSE / "muse_obd_log.c").read_text()
        mount = function_body(log, "static bool mount_flash(void)")
        self.assertIn("esp_vfs_fat_spiflash_mount_rw_wl(FLASH_BASE, FLASH_LABEL, &config, &wl)", mount)
        self.assertIn(".format_if_mount_failed = true,", mount)
        self.assertIn("ESP_PARTITION_SUBTYPE_DATA_FAT, FLASH_LABEL", mount)
        self.assertIn('#define FLASH_LABEL "obdlog"', log)
        self.assertNotIn("esp_vfs_fat_spiflash_format", log)
        self.assertNotIn("erase", log.lower().replace("erased", ""))
        # The card: a stub, no driver invented.
        self.assertIn("return false;", function_body(log, "static bool mount_card(void)"))
        self.assertNotIn("sdspi", log)
        self.assertNotIn("sdmmc", log)

    def test_only_muse_deletes(self):
        log = (MUSE / "muse_obd_log.c").read_text()
        self.assertEqual(log.count("muse_log_store_delete("), 1)
        self.assertIn("muse_log_store_delete(", function_body(log, "int muse_obd_log_delete(uint32_t id)"))
        self.assertNotIn("remove(", log)
        self.assertNotIn("unlink(", log)
        store = (MUSE / "muse_obd_log_store.c").read_text()
        # The store removes a trip file in delete only (and a header that
        # never fit, before the trip existed).
        self.assertEqual(store.count("remove_in(dir, name)"), 2)
        self.assertIn("remove_in(dir, name);", function_body(store, "bool muse_log_store_delete("))

    def test_ids_persist_in_nvs_and_are_never_reused(self):
        log = (MUSE / "muse_obd_log.c").read_text()
        self.assertIn('#define NVS_NS "muse"', log)
        self.assertIn('#define NVS_NEXT_ID "obd_trip"', log)
        task = function_body(log, "static void log_task(void *arg)")
        self.assertIn("s_next_id = muse_log_next_id(load_next_id(), max_seen);", task)
        start = function_body(log, "static void start_trip(const item_t *it)")
        self.assertLess(start.index("muse_log_trip_open("), start.index("save_next_id(s_next_id);"))

    def test_logging_never_holds_up_polling(self):
        log = (MUSE / "muse_obd_log.c").read_text()
        for name in ("void muse_obd_log_sample(", "void muse_obd_log_trip_start(", "void muse_obd_log_trip_end(void)"):
            body = function_body(log, name)
            self.assertNotIn("portMAX_DELAY", body)
            self.assertNotIn("xSemaphoreTake", body)
            for send in re.findall(r"xQueueSend\([^()]*\)", body):
                self.assertTrue(send.endswith(", 0)"), send)
        self.assertIn("atomic_fetch_add(&s_lost, 1);", function_body(log, "void muse_obd_log_sample("))
        # No bus requests: the logger never talks to the adapter.
        self.assertNotIn("elm_cmd", log)
        obd = (MUSE / "muse_obd.c").read_text()
        poll = function_body(obd, "static bool poll_rpm(void)")
        self.assertIn("log_sample();", poll)
        self.assertEqual(poll.count("elm_cmd("), 1)
        # A trip per polling link; drop() ends it, whatever dropped the link.
        task = function_body(obd, "static void obd_task(void *arg)")
        self.assertLess(task.index("muse_obd_log_trip_start(&st, LOG_DID[s_did], now_ms());"), task.index("poll_rpm()"))
        self.assertIn("muse_obd_log_trip_end();", function_body(obd, "static void drop(void)"))
        self.assertIn("muse_obd_log_start();", function_body(obd, "void muse_obd_start(void)"))
        # The writer has its own task, with an internal-RAM stack: it writes the flash.
        self.assertIn('xTaskCreate(log_task, "muse_obd_log"', log)

    def test_commands_are_advertised_and_dispatched_with_the_feature(self):
        noise = (ROOT / "main/noise_control.cpp").read_text()
        app = (ROOT / "main/app.c").read_text()
        start = app.index("static cJSON *on_ws_command(")
        dispatch = app[start:app.index("unsupported command", start)]
        for command, handler in (("obd.trips", "obd_trips_command()"),
                                 ("obd.trip_read", "obd_trip_read_command(params)"),
                                 ("obd.trip_delete", "obd_trip_delete_command(params)")):
            self.assertEqual(noise.count(f'add_command(commands, "{command}"'), 1, command)
            self.assertIsNotNone(gated_block_with(noise, f'add_command(commands, "{command}"'), command)
            self.assertEqual(dispatch.count(f'"{command}"'), 1, command)
            block = gated_block_with(dispatch, f'"{command}"')
            self.assertIsNotNone(block, command)
            self.assertIn(handler, block)
            self.assertNotIn(command, strip_gated(noise))
            self.assertNotIn(command, strip_gated(dispatch))
        self.assertIsNotNone(gated_block_with(app, '#include "muse_obd_log.h"'))
        for fn in ("static cJSON *obd_trips_command(void)", "static cJSON *obd_trip_read_command(cJSON *params)",
                   "static cJSON *obd_trip_delete_command(cJSON *params)"):
            self.assertIsNotNone(gated_block_with(app, fn), fn)

    def test_trip_read_is_chunked_base64_and_validated(self):
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static cJSON *obd_trip_read_command(cJSON *params)")
        self.assertIn("muse_obd_log_read(id, offset, buf, MUSE_OBD_LOG_CHUNK, &total)", body)
        self.assertIn("muse_log_base64(", body)
        for key in ('"id"', '"offset"', '"bytes"', '"total"', '"eof"', '"data"'):
            self.assertIn(key, body)
        self.assertIn('command_error("not_found"', body)
        self.assertIn("offset is past the end of the trip", body)
        self.assertIn('"offset is required"', body)
        header = (MUSE / "muse_obd_log.h").read_text()
        self.assertIn("#define MUSE_OBD_LOG_CHUNK 6144", header)
        params = function_body(app, "static bool obd_uint_param(")
        self.assertIn("cJSON_IsNumber(v)", params)
        self.assertIn("(double)u != v->valuedouble", params)
        delete = function_body(app, "static cJSON *obd_trip_delete_command(cJSON *params)")
        self.assertIn('cJSON_AddBoolToObject(payload, "deleted", true);', delete)
        trips = function_body(app, "static cJSON *obd_trips_command(void)")
        for key in ('"id"', '"store"', '"bytes"', '"samples"', '"dropped"', '"started"', '"ended"', '"crc32"',
                    '"trips"', '"card"', '"flash"', '"logging"'):
            self.assertIn(key, trips)

    def test_commands_work_with_obd_off(self):
        """Part 4: the data already exists, so the switch doesn't gate it."""
        app = (ROOT / "main/app.c").read_text()
        for fn in ("static cJSON *obd_trips_command(void)", "static cJSON *obd_trip_read_command(cJSON *params)",
                   "static cJSON *obd_trip_delete_command(cJSON *params)"):
            self.assertNotIn("enabled", function_body(app, fn))
        log = (MUSE / "muse_obd_log.c").read_text()
        self.assertNotIn("muse_obd_enabled", log)

    def test_obd_read_and_console_carry_the_log_fields(self):
        header = (MUSE / "muse_obd.h").read_text()
        for field in ("bool log_card;", "bool log_flash;", "const char *log_store;", "bool log_active;",
                      "uint32_t log_trip;", "uint32_t log_pending;", "uint32_t log_dropped;"):
            self.assertIn(field, header)
        app = (ROOT / "main/app.c").read_text()
        body = function_body(app, "static cJSON *obd_read_command(void)")
        console = gated_block_with((MUSE / "muse_input.c").read_text(), "static void print_obd(void)")
        for key in LOG_FIELDS:
            self.assertIn(f'"{key}"', body, key)
            self.assertIn(f'\\"{key}\\"', console, key)
        self.assertIn('cJSON_AddNullToObject(payload, "log_store");', body)
        self.assertIn('cJSON_AddNullToObject(payload, "log_trip");', body)
        obd = (MUSE / "muse_obd.c").read_text()
        self.assertIn("muse_obd_log_status(&trips);", function_body(obd, "void muse_obd_status(muse_obd_status_t *out)"))

    def test_logging_compiles_out_without_the_option(self):
        cmake = (MUSE / "CMakeLists.txt").read_text()
        start = cmake.index("if(CONFIG_MUSE_OBD_VTEC)")
        block = cmake[start:cmake.index("endif()", start)]
        self.assertIn('"muse_obd_log.c"', block)
        self.assertIn('"muse_obd_log_store.c"', block)
        self.assertEqual(cmake.count("muse_obd_log.c"), 1)
        self.assertEqual(cmake.count("muse_obd_log_store.c"), 1)
        for path in (ROOT / "main/app.c", MUSE / "muse_input.c", ROOT / "main/noise_control.cpp"):
            stripped = strip_gated(path.read_text())
            self.assertNotIn("muse_obd_log", stripped, path.name)
            self.assertNotIn("obd.trip", stripped, path.name)

    def test_doorbell_is_the_pending_flag(self):
        """Part 3: no device-initiated push path exists for a custom line
        (pushes come from the VM; the typed-turn path starts an agent turn),
        so the doorbell is log_pending alone."""
        log = (MUSE / "muse_obd_log.c").read_text()
        for name in ("muse_hatch_say", "muse_hatch_text_turn", "noise_ctrl_send"):
            self.assertNotIn(name, log)
        self.assertIn("log_pending is the doorbell", (ROOT / "main/app.c").read_text())

    def test_firmware_commit_is_stamped(self):
        cmake = (MUSE / "CMakeLists.txt").read_text()
        self.assertIn('set_source_files_properties("muse_build_info.c" PROPERTIES COMPILE_DEFINITIONS '
                      '"MUSE_GIT_COMMIT=\\"${muse_commit}\\"")', cmake)
        self.assertIn('string(SUBSTRING "${muse_hash}" 0 7 muse_commit)', cmake)
        self.assertIn("const char *muse_build_commit(void);", (MUSE / "muse_build_info.h").read_text())
        log = (MUSE / "muse_obd_log.c").read_text()
        self.assertIn("muse_build_commit()", log)

    def test_new_text_says_muse(self):
        for path in (MUSE / "muse_obd_log.c", MUSE / "muse_obd_log.h", MUSE / "muse_obd_log_store.c",
                     MUSE / "muse_obd_log_store.h", MUSE / "muse_obd_cards.c"):
            self.assertNotIn("hatch", path.read_text().lower(), path.name)
        app = (ROOT / "main/app.c").read_text()
        for fn in ("static cJSON *obd_trips_command(void)", "static cJSON *obd_trip_read_command(cJSON *params)",
                   "static cJSON *obd_trip_delete_command(cJSON *params)", "static cJSON *obd_trip_id_param("):
            self.assertNotIn("hatch", function_body(app, fn).lower())
        noise = (ROOT / "main/noise_control.cpp").read_text()
        block = gated_block_with(noise, 'add_command(commands, "obd.trips"')
        self.assertNotIn("hatch", block.lower())


class MuseObdCardLayoutTest(unittest.TestCase):
    """OBD-LOG-TASK Part 5: full-height open cells, big values, heroes with
    thin bars, rows the scan proved unsupported hidden, glance colours."""

    def setUp(self):
        self.cards = (MUSE / "muse_obd_cards.c").read_text()

    def test_cells_fill_the_tile_in_a_growing_column(self):
        build = function_body(self.cards, "void muse_obd_cards_build(")
        self.assertIn("lv_obj_set_size(list, list_w, list_h);", build)
        self.assertIn("int list_h = h - top - bottom;", build)
        self.assertIn("lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);", build)
        self.assertNotIn("LV_SIZE_CONTENT", build)
        row = function_body(self.cards, "static void build_row(")
        self.assertIn("lv_obj_set_flex_grow(c, def->hero ? 2 : 1);", row)

    def test_open_cells_with_hairlines_and_labels_above(self):
        self.assertNotIn("COLOR_CARD", self.cards)   # no filled pills
        row = function_body(self.cards, "static void build_row(")
        self.assertNotIn("bg_opa", row)
        self.assertLess(row.index("lv_font_montserrat_16, COLOR_DIM, def->name"), row.index("row->value = label("))
        self.assertIn("#define COLOR_RULE 0x2a2440", self.cards)
        rules = function_body(self.cards, "static void rule_lines(lv_obj_t *list)")
        self.assertIn("bool rule = slot != last && !lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN);", rules)
        self.assertIn("LV_BORDER_SIDE_BOTTOM", self.cards)
        for name in ('"COOLANT"', '"INTAKE AIR"', '"AMBIENT"', '"RPM"', '"TIMING"', '"LOAD"', '"MAP"',
                     '"SPEED"', '"THROTTLE"', '"STFT"', '"LTFT"', '"AFR"', '"FUEL"'):
            self.assertIn(name, self.cards)

    def test_big_fonts_enabled_for_the_obd_build_only(self):
        self.assertIn("#if LV_FONT_MONTSERRAT_48\n    { &lv_font_montserrat_48, 48 },", self.cards)
        self.assertIn("#if LV_FONT_MONTSERRAT_40\n    { &lv_font_montserrat_40, 40 },", self.cards)
        self.assertIn("fit_font(def->hero ? 48 : 40,", self.cards)
        overlay = (ROOT / "devices/sdkconfig.muse-waveshare-s3-18").read_text()
        self.assertIn("CONFIG_LV_FONT_MONTSERRAT_40=y", overlay)
        self.assertIn("CONFIG_LV_FONT_MONTSERRAT_48=y", overlay)
        self.assertNotIn("MONTSERRAT_48", (ROOT / "devices/sdkconfig.muse").read_text())

    def test_heroes_bars_and_the_shared_trim_row(self):
        for row in ('ROW(1, "RPM", "rpm", SRC_RPM, .hero = true, .bar_max = 8000, .vtec = true),',
                    'ROW(2, "SPEED", "mph", MUSE_OBD_SPEED, .hero = true, .mph = true),',
                    'ROW(2, "THROTTLE", "%", MUSE_OBD_THROTTLE, .half = true, .bar_max = 100),',
                    'ROW(2, "STFT", "%", MUSE_OBD_STFT, .sign = true, .half = true, .tone = TONE_TRIM),',
                    'ROW(2, "LTFT", "%", MUSE_OBD_LTFT, .sign = true, .half = true, .tone = TONE_TRIM),'):
            self.assertIn(row, self.cards)
        self.assertIn("#define BAR_H 10", self.cards)
        tick = function_body(self.cards, "void muse_obd_cards_tick(void)")
        self.assertIn("int shown = def->mph ? (v.value * 621 + 500) / 1000 : v.value;", tick)
        self.assertIn('snprintf(sub, sizeof(sub), "%d km/h", v.value);', tick)
        self.assertIn("lv_bar_set_value(row->bar, bar, LV_ANIM_OFF);", tick)

    def test_unsupported_rows_hide_and_reflow(self):
        tick = function_body(self.cards, "void muse_obd_cards_tick(void)")
        self.assertIn("bool unsupported = muse_obd_pid_unsupported(&st, pid);", tick)
        self.assertIn("set_hidden(row->cell, unsupported)", tick)
        self.assertIn("rule_lines(s_list[i]);", tick)

    def test_glance_colours(self):
        self.assertIn("#define COOLANT_AMBER_C MUSE_COOLANT_REARM_C", self.cards)
        self.assertIn("#define COOLANT_RED_C MUSE_COOLANT_FIRE_C", self.cards)
        self.assertIn("#define TRIM_AMBER_PCT 10", self.cards)
        elm = (MUSE / "muse_obd_elm.h").read_text()
        self.assertIn("#define MUSE_COOLANT_FIRE_C 105", elm)
        self.assertIn("#define MUSE_COOLANT_REARM_C 100", elm)
        tick = function_body(self.cards, "void muse_obd_cards_tick(void)")
        # Stale wins over every colour; nothing colours a value that isn't valid.
        self.assertIn("color = stale ? COLOR_STALE : fresh_color(def, v.value);", tick)
        self.assertIn("color = COLOR_DIM;", tick)


if __name__ == "__main__":
    unittest.main()
