/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_obd_cards.h"

#include <stdio.h>
#include <string.h>

#include "muse_board.h"
#include "muse_obd.h"

/* The settings pages' look (muse_settings_ui.c): rows are cards on black. */
#define LIST_W 330
#define ROW_H 54
#define HERO_H 76
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_STALE 0x4a4466
#define COLOR_CARD 0x1a1530
#define COLOR_ACCENT 0xa77dff

/* Where a row's reading comes from: an extra (muse_obd_extra_t), or these. */
#define SRC_RPM MUSE_OBD_EXTRAS
#define SRC_COOLANT (MUSE_OBD_EXTRAS + 1)

typedef struct {
    uint8_t card;
    const char *name;
    const char *unit;
    uint8_t src;
    bool hero;      /* Engine's RPM: taller, in the accent colour */
    bool mph;       /* the speed row: km/h, and mph beside it */
    bool sign;      /* fuel trims: +3 / -2 */
} row_def_t;

#define ROW(c, n, u, s, ...) { .card = (c), .name = (n), .unit = (u), .src = (s), __VA_ARGS__ }
static const row_def_t ROWS[] = {
    ROW(0, "Coolant", "C", SRC_COOLANT),
    ROW(0, "Intake air", "C", MUSE_OBD_IAT),
    ROW(0, "Ambient", "C", MUSE_OBD_AMBIENT),
    ROW(1, "RPM", "rpm", SRC_RPM, .hero = true),
    ROW(1, "Timing", "deg", MUSE_OBD_TIMING),
    ROW(1, "Load", "%", MUSE_OBD_LOAD),
    ROW(1, "MAP", "kPa", MUSE_OBD_MAP),
    ROW(2, "Speed", "km/h", MUSE_OBD_SPEED, .mph = true),
    ROW(2, "Throttle", "%", MUSE_OBD_THROTTLE),
    ROW(2, "STFT", "%", MUSE_OBD_STFT, .sign = true),
    ROW(2, "LTFT", "%", MUSE_OBD_LTFT, .sign = true),
};
#define ROW_COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

static const char *const TITLES[MUSE_OBD_CARDS] = { "Temps", "Engine", "Drive" };

typedef struct {
    lv_obj_t *value, *mph;
    uint32_t color;     /* the value's colour as shown; 0 before the first tick */
} row_t;

static row_t s_rows[ROW_COUNT];
static lv_obj_t *s_list[MUSE_OBD_CARDS];    /* the rows, hidden while not polling */
static lv_obj_t *s_state[MUSE_OBD_CARDS];   /* the link's state instead */
static bool s_built;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

/* A box for layout only: no look, no touch, no scrolling of its own, so a
 * swipe anywhere on a card moves the tileview. */
static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void build_row(lv_obj_t *list, const row_def_t *def, row_t *row)
{
    lv_obj_t *c = box(list);
    lv_obj_set_size(c, lv_pct(100), def->hero ? HERO_H : ROW_H);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_pad_hor(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 8, 0);

    lv_obj_t *name = label(c, &lv_font_montserrat_16, COLOR_DIM, def->name);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    row->value = label(c, &lv_font_montserrat_28, def->hero ? COLOR_ACCENT : COLOR_TEXT, "...");
    lv_obj_t *unit = label(c, &lv_font_montserrat_16, COLOR_DIM, def->unit);
    /* A fixed width, so the values of a card line up on their right edge. */
    lv_obj_set_width(unit, 40);
    if (def->mph) {
        row->mph = label(c, &lv_font_montserrat_16, COLOR_DIM, "... mph");
    }
}

void muse_obd_cards_build(lv_obj_t *const tiles[MUSE_OBD_CARDS])
{
    int w = muse_board->width, h = muse_board->height;
    int list_w = LV_MIN(LIST_W, w - 24);
    for (int i = 0; i < MUSE_OBD_CARDS; i++) {
        lv_obj_t *t = tiles[i];
        lv_obj_set_style_bg_color(t, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = label(t, &lv_font_montserrat_20, COLOR_ACCENT, TITLES[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, h / 16 + (muse_board->round ? 24 : 0));

        s_state[i] = label(t, &lv_font_montserrat_20, COLOR_DIM, "");
        lv_obj_set_style_text_align(s_state[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_state[i]);

        lv_obj_t *list = box(t);
        lv_obj_set_size(list, list_w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(list, 10, 0);
        lv_obj_align(list, LV_ALIGN_CENTER, 0, h / 32);
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        s_list[i] = list;
    }
    for (size_t r = 0; r < ROW_COUNT; r++) {
        build_row(s_list[ROWS[r].card], &ROWS[r], &s_rows[r]);
    }
    s_built = true;
}

void muse_obd_cards_drop(void)
{
    s_built = false;
    memset(s_rows, 0, sizeof(s_rows));
    memset(s_list, 0, sizeof(s_list));
    memset(s_state, 0, sizeof(s_state));
}

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

/* A row's reading out of the snapshot, and its PID. */
static muse_obd_reading_t reading(const muse_obd_status_t *st, uint8_t src, int *pid)
{
    if (src == SRC_RPM) {
        *pid = 0x0C;
        return (muse_obd_reading_t){ st->rpm_valid, st->rpm, st->rpm_age_ms };
    }
    if (src == SRC_COOLANT) {
        *pid = 0x05;
        return (muse_obd_reading_t){ st->coolant_valid, st->coolant_c, st->coolant_age_ms };
    }
    *pid = MUSE_OBD_EXTRA[src].pid;
    return st->extra[src];
}

void muse_obd_cards_tick(void)
{
    if (!s_built) {
        return;
    }
    muse_obd_status_t st;
    muse_obd_status(&st);

    /* Not polling: the link's state, not numbers that stopped. */
    for (int i = 0; i < MUSE_OBD_CARDS; i++) {
        if (lv_obj_has_flag(s_list[i], LV_OBJ_FLAG_HIDDEN) == st.polling) {
            lv_obj_set_flag(s_list[i], LV_OBJ_FLAG_HIDDEN, !st.polling);
            lv_obj_set_flag(s_state[i], LV_OBJ_FLAG_HIDDEN, st.polling);
        }
        if (!st.polling) {
            char line[40];
            snprintf(line, sizeof(line), "OBD\n%s", st.state);
            set_text(s_state[i], line);
        }
    }
    if (!st.polling) {
        return;
    }

    for (size_t r = 0; r < ROW_COUNT; r++) {
        const row_def_t *def = &ROWS[r];
        row_t *row = &s_rows[r];
        int pid;
        muse_obd_reading_t v = reading(&st, def->src, &pid);
        char text[16], mph[16];
        uint32_t color;
        if (v.valid) {
            snprintf(text, sizeof(text), def->sign ? "%+d" : "%d", v.value);
            snprintf(mph, sizeof(mph), "%d mph", (v.value * 621 + 500) / 1000);
            bool stale = v.age_ms > MUSE_OBD_CARD_STALE_MS;
            color = stale ? COLOR_STALE : def->hero ? COLOR_ACCENT : COLOR_TEXT;
        } else {
            /* "--": the scan proved the ECU hasn't got it; "...": not yet. */
            const char *none = muse_obd_pid_unsupported(&st, pid) ? "--" : "...";
            snprintf(text, sizeof(text), "%s", none);
            snprintf(mph, sizeof(mph), "%s mph", none);
            color = COLOR_DIM;
        }
        set_text(row->value, text);
        if (row->mph) {
            set_text(row->mph, mph);
        }
        if (color != row->color) {
            lv_obj_set_style_text_color(row->value, lv_color_hex(color), 0);
            row->color = color;
        }
    }
}
