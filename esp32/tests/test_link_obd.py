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
EXTRA_KEYS = (
    ("MUSE_OBD_LOAD", 0x04, "load_pct", "load_age_ms"),
    ("MUSE_OBD_MAP", 0x0B, "map_kpa", "map_age_ms"),
    ("MUSE_OBD_SPEED", 0x0D, "speed_kmh", "speed_age_ms"),
    ("MUSE_OBD_TIMING", 0x0E, "timing_deg", "timing_age_ms"),
    ("MUSE_OBD_IAT", 0x0F, "iat_c", "iat_age_ms"),
    ("MUSE_OBD_THROTTLE", 0x11, "throttle_pct", "throttle_age_ms"),
    ("MUSE_OBD_STFT", 0x06, "stft_pct", "stft_age_ms"),
    ("MUSE_OBD_LTFT", 0x07, "ltft_pct", "ltft_age_ms"),
    ("MUSE_OBD_AMBIENT", 0x46, "ambient_c", "ambient_age_ms"),
)


def strip_gated(source):
    """source with every CONFIG_MUSE_OBD_VTEC block (to its first #endif) cut out."""
    start = source.find(GATE)
    while start != -1:
        end = source.index("#endif", start)
        source = source[:start] + source[end:]
        start = source.find(GATE)
    return source


def function_body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


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
        for name in ("coolant", "warmup"):
            start = source.index(f"void muse_obd_fire_{name}(void)")
            body = source[start:source.index("\n}\n", start)]
            self.assertIn(f"muse_state_start_{name}();", body)
        header = (ROOT / "components/muse/muse_obd.h").read_text()
        self.assertIn("void muse_obd_fire_warmup(void);", header)
        # VTEC (OBD-VTEC-DID-TASK) is held, not fired: no fire, no speech either.
        self.assertNotIn("muse_obd_fire_vtec", header)
        self.assertNotIn("muse_obd_fire_vtec", source)

    def test_warmup_trigger_is_fed_and_reset(self):
        source = (ROOT / "components/muse/muse_obd.c").read_text()
        # Both poll paths (the single PID 05 and a batch carrying it) land
        # in coolant_reading(), which feeds the two triggers.
        start = source.index("static void coolant_reading(int coolant_c)")
        body = source[start:source.index("\n}\n", start)]
        self.assertIn("muse_warmup_update(&s_warmup, coolant_c)", body)
        start = source.index("static void drop(void)")
        body = source[start:source.index("\n}\n", start)]
        self.assertIn("muse_warmup_init(&s_warmup);", body)

    def test_poll_cycle_is_the_schedulers(self):
        """OBD-PIPELINE-TASK: the poll loop is response-driven. The scheduler
        picks each request (RPM the standing filler, the DID on its ~1 s
        floor, the rest in freshness tiers, up to six PIDs per Mode 01
        request); nothing in the poll path waits, a refused batch latches
        batching off, and coolant still feeds the triggers through
        coolant_reading(). The fixed RPM/secondary rotation and its cursor
        are gone, and nothing fires on RPM."""
        source = (ROOT / "components/muse/muse_obd.c").read_text()
        self.assertNotIn("COOLANT_EVERY", source)
        self.assertNotIn("muse_elm_next_slot", source)
        self.assertNotIn("s_cursor", source)
        body = function_body(source, "static bool poll_rpm(void)")
        self.assertIn("muse_sched_init(&s_sched, &s_support, s_did == DID_YES, now_ms());", body)
        self.assertIn("muse_sched_pick(&s_sched, now_ms(), &req);", body)
        self.assertIn("muse_elm_parse_batch(s_rx.resp, req.pids, req.n, values, got);", body)
        self.assertIn("muse_sched_latch_batch_off(&s_sched);", body)
        self.assertIn("got = got_coolant();", body)
        self.assertIn("got = got_extra(pid);", body)
        self.assertNotIn("muse_vtec_update", body)
        self.assertNotIn("vTaskDelay", body)
        body = function_body(source, "static void coolant_reading(int coolant_c)")
        self.assertIn("muse_coolant_update(&s_coolant, coolant_c, s_rpm, now_ms())", body)
        elm = (ROOT / "components/muse/muse_obd_elm.c").read_text()
        self.assertIn("{ 0x0C, 0, 0 },", elm)   # the schedule's RPM row: target 0

    def test_support_scan_and_protocol_run_before_polling(self):
        source = (ROOT / "components/muse/muse_obd.c").read_text()
        task = function_body(source, "static void obd_task(void *arg)")
        self.assertIn("if (elm_init() && scan_support()) {\n                        read_protocol();", task)
        self.assertLess(task.index("scan_support()"), task.index("poll_rpm()"))
        scan = function_body(source, "static bool scan_support(void)")
        for cmd in ('"0100"', '"0120"', '"0140"'):
            self.assertIn(cmd, scan)
        self.assertIn("i == 0 ? SEARCH_MS : CMD_MS", scan)
        self.assertIn('elm_cmd("ATDPN", CMD_MS)', function_body(source, "static void read_protocol(void)"))
        # A new connection scans again; the dropped link clears every reading.
        drop = function_body(source, "static void drop(void)")
        self.assertIn("s_support = (muse_elm_support_t){ 0 };", drop)
        self.assertIn("s_have_protocol = false;", drop)
        down = function_body(source, "static void set_link_down(void)")
        self.assertIn("s_status.extra[i].valid = false;", down)
        self.assertIn("s_status.rpm_valid = false;", down)
        self.assertIn("s_status.coolant_valid = false;", down)

    def test_obd_read_exposes_the_card_fields(self):
        header = (ROOT / "components/muse/muse_obd.h").read_text()
        enum = header[header.index("typedef enum {"):header.index("} muse_obd_extra_t;")]
        names = [line.split(",")[0].strip() for line in enum.splitlines()[1:] if line.strip().startswith("MUSE_OBD_")]
        self.assertEqual(names, [k[0] for k in EXTRA_KEYS] + ["MUSE_OBD_EXTRAS"])
        for field in ("muse_obd_reading_t extra[MUSE_OBD_EXTRAS];", "int protocol;",
                      "char protocol_name[20];", "bool support_valid[MUSE_OBD_SUPPORT_RANGES];",
                      "uint32_t support[MUSE_OBD_SUPPORT_RANGES];"):
            self.assertIn(field, header)
        obd = (ROOT / "components/muse/muse_obd.c").read_text()
        for enum_name, pid, key, age_key in EXTRA_KEYS:
            self.assertIn(f'[{enum_name}] = {{ 0x{pid:02X}, "{key}", "{age_key}" }},', obd)

        app = (ROOT / "main/app.c").read_text()
        block = gated_block_with(app, "static cJSON *obd_read_command(void)")
        body = block[block.index("static cJSON *obd_read_command(void)"):]
        self.assertIn("for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {", body)
        self.assertIn("add_obd_reading(payload, MUSE_OBD_EXTRA[i].key, MUSE_OBD_EXTRA[i].age_key", body)
        self.assertIn('cJSON_AddStringToObject(payload, "protocol", st.protocol_name);', body)
        self.assertIn('cJSON_AddNullToObject(payload, "protocol");', body)
        self.assertIn('"supported_00", "supported_20", "supported_40",', body)
        self.assertIn('"%08lX"', body)

        console = (ROOT / "components/muse/muse_input.c").read_text()
        block = gated_block_with(console, "static void print_obd(void)")
        self.assertIsNotNone(block)
        for needle in ("MUSE_OBD_EXTRA[i].key", "MUSE_OBD_EXTRA[i].age_key", '\\"protocol\\"',
                       '\\"supported_%02X\\":\\"%08lX\\"'):
            self.assertIn(needle, block)

    def test_card_tiles_only_with_the_feature(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIsNotNone(gated_block_with(ui, '#include "muse_obd_cards.h"'))
        statics = gated_block_with(ui, "static lv_obj_t *s_cards[MUSE_OBD_CARDS];")
        self.assertIsNotNone(statics)
        self.assertIn("#else\n#define CARD_TILES 0", statics)
        self.assertIn("#define TILES (2 + CARD_TILES)", ui)
        self.assertIsNotNone(gated_block_with(ui, "muse_obd_cards_tick();"))
        # One dot per tile there is, laid out so two tiles keep their old -8 / +8.
        self.assertIn("static lv_obj_t *s_dots[TILES];", ui)
        self.assertIn("16 * i - 8 * (n - 1)", function_body(ui, "static void place_dots(void)"))
        # Without the option: no cards, and the old two tiles.
        stripped = strip_gated(ui)
        for name in ("s_cards", "muse_obd_cards", "MUSE_OBD_CARDS", "muse_ui_bench_tile", "sync_cards",
                     "s_obd_linked"):
            self.assertNotIn(name, stripped)
        self.assertIn("return TILES;", function_body(ui, "static int tile_count(void)"))
        cmake = (ROOT / "components/muse/CMakeLists.txt").read_text()
        start = cmake.index("if(CONFIG_MUSE_OBD_VTEC)")
        self.assertIn('"muse_obd_cards.c"', cmake[start:cmake.index("endif()", start)])
        self.assertEqual(cmake.count("muse_obd_cards.c"), 1)

    def test_card_tiles_follow_the_link(self):
        """OBD-PRESENCE-TASK: the UI starts with the face and settings only,
        and the cards come and go with the link snapshot (sync_cards)."""
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        build = function_body(ui, "static void build_screen(void)")
        self.assertIn("s_settings = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);", build)
        self.assertNotIn("s_cards", build)
        self.assertNotIn("muse_obd_cards", function_body(ui, "esp_err_t muse_ui_start(void)"))
        self.assertEqual(ui.count("lv_tileview_add_tile(s_tv, 1 + i, 0, LV_DIR_HOR);"), 1)
        self.assertEqual(ui.count("muse_obd_cards_build(s_cards);"), 1)

        sync = gated_block_with(ui, "static void sync_cards(bool linked)")
        self.assertIsNotNone(sync)
        for needle in ("lv_tileview_add_tile(s_tv, 1 + i, 0, LV_DIR_HOR);", "muse_obd_cards_build(s_cards);",
                       "muse_obd_cards_drop();", "lv_obj_delete(s_cards[i]);", "s_cards[i] = NULL;",
                       "s_cards_shown = linked;",
                       "lv_obj_set_pos(s_settings, lv_pct(100 * (tile_count() - 1)), 0);",
                       # The tile on screen stays, by object; a card that goes lands on the face.
                       "lv_obj_t *active = lv_tileview_get_tile_active(s_tv);",
                       "on_card = on_card || active == s_cards[i];",
                       "lv_obj_t *show = active && !on_card ? active : s_face;",
                       "lv_tileview_set_tile(s_tv, show, LV_ANIM_OFF);",
                       "place_dots();",
                       # Not under a finger or a slide.
                       "lv_indev_get_state(s_indev) == LV_INDEV_STATE_PRESSED", "lv_obj_is_scrolling(s_tv)"):
            self.assertIn(needle, sync)

        # Driven by the snapshot, on the LVGL task's chrome tick, before the dots.
        chrome = function_body(ui, "static void update_chrome(float now)")
        tick = gated_block_with(chrome, "sync_cards(s_obd_linked);")
        self.assertIsNotNone(tick)
        self.assertIn("muse_obd_status(&o);", tick)
        self.assertIn("s_obd_linked = muse_obd_linked(&o);", tick)
        self.assertLess(chrome.index("sync_cards(s_obd_linked);"), chrome.index("if (s_tv) {"))
        self.assertIn("i >= tiles || (settings && subpage)", chrome)
        self.assertIn("(tiles - 2) * lv_obj_get_width(s_tv)", chrome)

        # "Linked" is the status-row indicator's condition, and never while off.
        obd = (ROOT / "components/muse/muse_obd.c").read_text()
        self.assertIn("return st->enabled && (st->subscribed || st->polling);",
                      function_body(obd, "bool muse_obd_linked(const muse_obd_status_t *st)"))
        self.assertIn("int obd = o.polling ? 2 : o.subscribed ? 1 : 0;", chrome)

        cards = (ROOT / "components/muse/muse_obd_cards.c").read_text()
        drop = function_body(cards, "void muse_obd_cards_drop(void)")
        self.assertIn("s_built = false;", drop)
        header = (ROOT / "components/muse/muse_obd_cards.h").read_text()
        self.assertIn("void muse_obd_cards_drop(void);", header)

    def test_cards_read_the_snapshot_and_stay_passive(self):
        cards = (ROOT / "components/muse/muse_obd_cards.c").read_text()
        self.assertIn("muse_obd_status(&st);", cards)
        self.assertIn("muse_obd_pid_unsupported(&st, pid)", cards)
        self.assertIn('"--" : "..."', cards)
        self.assertIn("v.age_ms > MUSE_OBD_CARD_STALE_MS", cards)
        self.assertIn('snprintf(line, sizeof(line), "OBD\\n%s", st.state);', cards)
        self.assertIn("LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE", cards)
        self.assertNotIn("lv_obj_add_event_cb", cards)   # no gestures or buttons of their own
        self.assertNotIn("lv_button_create", cards)
        for title in ('"Temps"', '"Engine"', '"Drive"'):
            self.assertIn(title, cards)
        header = (ROOT / "components/muse/muse_obd_cards.h").read_text()
        self.assertIn("#define MUSE_OBD_CARD_STALE_MS 3000u", header)

    def test_card_bench_command_is_gated(self):
        source = (ROOT / "components/muse/muse_input.c").read_text()
        body = function_body(source, "static bool console_command(")
        block = gated_block_with(body, 'if (!strncmp(line, "card ", 5))')
        self.assertIsNotNone(block)
        self.assertIn("muse_ui_bench_tile((int)n)", block)
        self.assertIn("@card.error no cards: the OBD link is down", block)
        self.assertNotIn("muse_ui_bench_tile", strip_gated(source))
        header = (ROOT / "components/muse/muse_ui.h").read_text()
        self.assertIsNotNone(gated_block_with(header, "bool muse_ui_bench_tile(int index);"))
        self.assertIsNotNone(gated_block_with(header, "bool muse_ui_cards_shown(void);"))
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        block = gated_block_with(ui, "bool muse_ui_bench_tile(int index)")
        self.assertIsNotNone(block)
        self.assertIn("s_bench_page = index;", block)
        # A card while the cards are out is refused; one gone since is a no-op.
        self.assertIn("(card && !s_cards_shown)", block)
        frame = function_body(ui, "static void frame_tick(lv_timer_t *timer)")
        self.assertIn("if (s_tv && tile) lv_tileview_set_tile(s_tv, tile, LV_ANIM_OFF);", frame)
        self.assertIn("return index == TILES - 1 ? s_settings : s_face;", ui)

    def test_settings_switch_only_with_the_feature(self):
        """OBD-PRESENCE-TASK: Settings' OBD switch, saved in NVS as obd_on, on
        by default, present only with the option."""
        header = (ROOT / "components/muse/muse_settings.h").read_text()
        for needle in ("bool muse_settings_obd_on(void);", "void muse_settings_set_obd_on(bool on);",
                       "MUSE_SETTING_OBD,"):
            self.assertIsNotNone(gated_block_with(header, needle), needle)
        self.assertNotIn("obd", strip_gated(header).lower())

        settings = (ROOT / "components/muse/muse_settings.c").read_text()
        self.assertIsNotNone(gated_block_with(settings, ".obd_on = true,"))
        load = gated_block_with(settings, 'nvs_get_u8(s_nvs, "obd_on", &b)')
        self.assertIsNotNone(load)
        self.assertIn("s.obd_on = b;", load)
        setter = gated_block_with(settings, "void muse_settings_set_obd_on(bool on)")
        self.assertIsNotNone(setter)
        self.assertIn('save_u8("obd_on", on);', setter)
        self.assertIn("notify(MUSE_SETTING_OBD);", setter)
        self.assertNotIn("obd", strip_gated(settings).lower())

        page = (ROOT / "components/muse/muse_settings_ui.c").read_text()
        row = gated_block_with(page, 's_home_obd = switch_row_icon(')
        self.assertIsNotNone(row)
        self.assertIn('"OBD", muse_settings_obd_on(), on_obd_sw);', row)
        cb = gated_block_with(page, "static void on_obd_sw(lv_event_t *e)")
        self.assertIsNotNone(cb)
        self.assertIn("muse_settings_set_obd_on(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));", cb)
        self.assertIsNotNone(gated_block_with(page, "lv_obj_set_state(s_home_obd, LV_STATE_CHECKED, obd);"))
        self.assertNotIn("obd", strip_gated(page).lower())

    def test_settings_switch_drives_the_obd_task(self):
        app = (ROOT / "components/muse/muse_app.c").read_text()
        case = gated_block_with(app, "case MUSE_SETTING_OBD:")
        self.assertIsNotNone(case)
        self.assertIn("muse_obd_set_enabled(muse_settings_obd_on());", case)
        start = gated_block_with(app, "muse_obd_start();")
        self.assertLess(start.index("muse_obd_set_enabled(muse_settings_obd_on());"), start.index("muse_obd_start();"))

        obd = (ROOT / "components/muse/muse_obd.c").read_text()
        header = (ROOT / "components/muse/muse_obd.h").read_text()
        self.assertIn("void muse_obd_set_enabled(bool on);", header)
        self.assertIn("bool enabled;", header)
        setter = function_body(obd, "void muse_obd_set_enabled(bool on)")
        self.assertIn("s_status.enabled = on;", setter)
        self.assertIn("xTaskNotifyGive(s_task);", setter)
        # Every wait gives up once off, except the ones taking a link down.
        wait = function_body(obd, "static int wait_for(")
        self.assertIn("if (!s_enabled && !(mask & UNWIND)) {\n            return 0;", wait)
        self.assertIn("#define UNWIND BIT_EV(EV_DISCONNECT)", obd)
        self.assertIn("wait_for(BIT_EV(EV_DISCONNECT), false, 2000, NULL);", function_body(obd, "static void drop(void)"))
        connect = function_body(obd, "static bool connect(const ble_addr_t *addr)")
        self.assertIn("wait_for(BIT_EV(EV_CONNECT) | UNWIND, false, 1000, &ev)", connect)
        # Off: no scan, no connection, until it is turned back on.
        task = function_body(obd, "static void obd_task(void *arg)")
        off = task.index("if (!s_enabled) {\n            set_state(\"off\");")
        self.assertLess(off, task.index("scan(&addr)"))
        self.assertIn("ulTaskNotifyTake(pdTRUE, portMAX_DELAY);", task)
        self.assertNotIn("vTaskDelay", task)   # naps that end once turned off
        for name in ("coolant", "warmup"):
            body = function_body(obd, f"void muse_obd_fire_{name}(void)")
            self.assertIn(f"if (s_enabled) {{\n        muse_state_start_{name}();", body)
        # VTEC's level and its bench input are off with the switch too.
        self.assertIn("return st->enabled && muse_vtec_engaged(",
                      function_body(obd, "bool muse_obd_vtec_on(const muse_obd_status_t *st)"))
        self.assertIn("if (s_enabled) {", function_body(obd, "void muse_obd_bench_vtec_sol(bool on)"))

    def test_obd_read_reports_enabled_and_off(self):
        app = (ROOT / "main/app.c").read_text()
        block = gated_block_with(app, "static cJSON *obd_read_command(void)")
        body = block[block.index("static cJSON *obd_read_command(void)"):]
        self.assertIn('cJSON_AddStringToObject(payload, "state", st.state);', body)
        self.assertIn('cJSON_AddBoolToObject(payload, "enabled", st.enabled);', body)
        console = (ROOT / "components/muse/muse_input.c").read_text()
        block = gated_block_with(console, "static void print_obd(void)")
        self.assertIn('\\"enabled\\":%s', block)
        self.assertIn('st.enabled ? "true" : "false"', block)
        obd = (ROOT / "components/muse/muse_obd.c").read_text()
        self.assertIn('set_state("off");', function_body(obd, "static void obd_task(void *arg)"))
        self.assertIn('s_status.state = s_enabled ? "waiting" : "off";', obd)
        # The bench fires say why they do nothing while off.
        body = function_body(console, "static bool console_command(")
        block = gated_block_with(body, "!muse_obd_enabled()")
        self.assertIsNotNone(block)
        self.assertIn("@obd.off", block)

    def test_status_row_indicator_is_gated(self):
        ui = (ROOT / "components/muse/muse_ui.c").read_text()
        self.assertIsNotNone(gated_block_with(ui, '#include "muse_obd.h"'))
        self.assertIsNotNone(gated_block_with(ui, "static lv_obj_t *s_obd_icon;"))
        build = gated_block_with(ui, 'lv_label_set_text(s_obd_icon, "OBD");')
        self.assertIsNotNone(build)
        self.assertIn("&lv_font_montserrat_14", build)
        tick = gated_block_with(ui, "obd != s_shown_obd")
        self.assertIsNotNone(tick)
        self.assertIn("LV_OBJ_FLAG_HIDDEN", tick)
        # Read from the snapshot taken at the top of the tick.
        chrome = function_body(ui, "static void update_chrome(float now)")
        self.assertLess(chrome.index("muse_obd_status(&o);"), chrome.index("obd != s_shown_obd"))
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
