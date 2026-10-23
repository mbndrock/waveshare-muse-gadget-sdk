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
#include "muse_obd_elm.h"

/*
 * A gauge cluster rather than a settings list: each card's cells share the
 * tile's height under the title and above the page dots (a full-height flex
 * column, each cell growing by its weight), open cells on black split by
 * hairlines, a small dim label above each big value.
 */
#define SIDE_PAD 12          /* each side of the cells, inside the tile */
#define TOP_GAP 8            /* between the title and the first cell */
#define BOTTOM_INSET 30      /* the page dots (8 px, 14 above the bottom) and a gap */
#define CELL_PAD 2           /* above and below a cell's content */
#define BAR_H 10
#define BAR_GAP 4
#define COLOR_TEXT 0xf2efff
#define COLOR_DIM 0x8b84a8
#define COLOR_STALE 0x4a4466
#define COLOR_ACCENT 0xa77dff
#define COLOR_RULE 0x2a2440    /* the hairline between cells */
#define COLOR_TRACK 0x2a2440   /* a bar's empty part */
#define COLOR_AMBER 0xffb020
#define COLOR_RED 0xff3b30

/* Glance colours, fresh values only: coolant amber from the warning's re-arm
 * temperature, red from its firing one; a fuel trim amber from +-10 %. */
#define COOLANT_AMBER_C MUSE_COOLANT_REARM_C
#define COOLANT_RED_C MUSE_COOLANT_FIRE_C
#define TRIM_AMBER_PCT 10

/* Where a row's reading comes from: an extra (muse_obd_extra_t), or these. */
#define SRC_RPM MUSE_OBD_EXTRAS
#define SRC_COOLANT (MUSE_OBD_EXTRAS + 1)

typedef enum { TONE_NONE, TONE_COOLANT, TONE_TRIM } tone_t;

typedef struct {
    uint8_t card;
    const char *name;   /* the label, above the value */
    const char *unit;
    uint8_t src;
    bool hero;          /* twice the height, the biggest value, in the accent colour */
    bool mph;           /* speed: the value in mph, km/h on a small line under it */
    bool sign;          /* fuel trims: +3 / -2 */
    bool half;          /* half a row, beside the next half (STFT | LTFT) */
    int bar_max;        /* > 0: a thin bar under the value, 0..bar_max */
    uint8_t tone;
    bool vtec;          /* RPM: "VTEC ON / OFF / --" under the bar, from DID 0x2612 */
} row_def_t;

#define ROW(c, n, u, s, ...) { .card = (c), .name = (n), .unit = (u), .src = (s), __VA_ARGS__ }
static const row_def_t ROWS[] = {
    ROW(0, "COOLANT", "C", SRC_COOLANT, .tone = TONE_COOLANT),
    ROW(0, "INTAKE AIR", "C", MUSE_OBD_IAT),
    ROW(0, "AMBIENT", "C", MUSE_OBD_AMBIENT),
    ROW(1, "RPM", "rpm", SRC_RPM, .hero = true, .bar_max = 8000, .vtec = true),
    ROW(1, "TIMING", "deg", MUSE_OBD_TIMING),
    ROW(1, "LOAD", "%", MUSE_OBD_LOAD),
    ROW(1, "MAP", "kPa", MUSE_OBD_MAP),
    ROW(2, "SPEED", "mph", MUSE_OBD_SPEED, .hero = true, .mph = true),
    ROW(2, "THROTTLE", "%", MUSE_OBD_THROTTLE, .bar_max = 100),
    ROW(2, "STFT", "%", MUSE_OBD_STFT, .sign = true, .half = true, .tone = TONE_TRIM),
    ROW(2, "LTFT", "%", MUSE_OBD_LTFT, .sign = true, .half = true, .tone = TONE_TRIM),
};
#define ROW_COUNT (sizeof(ROWS) / sizeof(ROWS[0]))

static const char *const TITLES[MUSE_OBD_CARDS] = { "Temps", "Engine", "Drive" };

typedef struct {
    lv_obj_t *cell;     /* hidden when the scan proves its PID unsupported */
    lv_obj_t *slot;     /* what sits in the column: the cell, or the pair it shares */
    lv_obj_t *value, *sub, *bar;
    lv_obj_t *vtec;     /* the VTEC word, ON / OFF / -- */
    uint32_t color;     /* the value's colour as shown; 0 before the first tick */
    uint32_t vtec_color;
    uint32_t bar_color;
    int bar_value;      /* as shown; -1 before the first tick */
} row_t;

static row_t s_rows[ROW_COUNT];
static lv_obj_t *s_list[MUSE_OBD_CARDS];    /* the cells, hidden while not polling */
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

/* ---- Fonts: the biggest that fits the cell's height ---- */

static const struct {
    const lv_font_t *font;
    int size;
} VALUE_FONTS[] = {
#if LV_FONT_MONTSERRAT_48
    { &lv_font_montserrat_48, 48 },
#endif
#if LV_FONT_MONTSERRAT_40
    { &lv_font_montserrat_40, 40 },
#endif
    { &lv_font_montserrat_28, 28 },
};
#define VALUE_FONT_COUNT (sizeof(VALUE_FONTS) / sizeof(VALUE_FONTS[0]))

/* The biggest value font up to `size` (48 for heroes, 40 for the rest)
 * whose line, with `extra` px of label, bar and padding, fits in `h`; the
 * smallest if none does. */
static const lv_font_t *fit_font(int size, int h, int extra)
{
    for (size_t i = 0; i < VALUE_FONT_COUNT; i++) {
        if (VALUE_FONTS[i].size <= size && lv_font_get_line_height(VALUE_FONTS[i].font) + extra <= h) {
            return VALUE_FONTS[i].font;
        }
    }
    return VALUE_FONTS[VALUE_FONT_COUNT - 1].font;
}

/* Room a cell needs beside its value: label, bar, the km/h line, padding. */
static int cell_extra(const row_def_t *def)
{
    int small = lv_font_get_line_height(&lv_font_montserrat_16);
    int extra = 2 * CELL_PAD + small;
    if (def->bar_max) {
        extra += BAR_GAP + BAR_H;
    }
    if (def->mph) {
        extra += small;
    }
    if (def->vtec) {
        extra += BAR_GAP + lv_font_get_line_height(&lv_font_montserrat_20);
    }
    return extra;
}

/* ---- Building ---- */

/* A thin bar, BAR_GAP under what's above it. */
static lv_obj_t *build_bar(lv_obj_t *parent, int max)
{
    lv_obj_t *holder = box(parent);
    lv_obj_set_size(holder, lv_pct(100), BAR_GAP + BAR_H);
    lv_obj_set_style_pad_top(holder, BAR_GAP, 0);
    lv_obj_t *bar = lv_bar_create(holder);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, lv_pct(100), BAR_H);
    lv_bar_set_range(bar, 0, max);
    lv_obj_set_style_radius(bar, BAR_H / 2, 0);
    lv_obj_set_style_radius(bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

static void build_row(lv_obj_t *list, const row_def_t *def, row_t *row, lv_obj_t *pair, const lv_font_t *font)
{
    lv_obj_t *c = box(pair ? pair : list);
    if (pair) {
        lv_obj_set_height(c, lv_pct(100));
        lv_obj_set_flex_grow(c, 1);
    } else {
        lv_obj_set_width(c, lv_pct(100));
        lv_obj_set_flex_grow(c, def->hero ? 2 : 1);
    }
    lv_obj_set_style_pad_ver(c, CELL_PAD, 0);
    lv_obj_set_style_pad_hor(c, 4, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    row->cell = c;
    row->slot = pair ? pair : c;
    row->bar_value = -1;

    lv_obj_t *name = label(c, &lv_font_montserrat_16, COLOR_DIM, def->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);

    /* The value with its unit beside it, on the value's baseline. */
    lv_obj_t *line = box(c);
    lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(line, 6, 0);
    row->value = label(line, font, def->hero ? COLOR_ACCENT : COLOR_TEXT, "...");
    lv_obj_t *unit = label(line, &lv_font_montserrat_16, COLOR_DIM, def->unit);
    int drop = font->base_line - lv_font_montserrat_16.base_line;
    lv_obj_set_style_pad_bottom(unit, drop > 0 ? drop : 0, 0);

    if (def->mph) {
        row->sub = label(c, &lv_font_montserrat_16, COLOR_DIM, "... km/h");
    }
    if (def->bar_max) {
        row->bar = build_bar(c, def->bar_max);
    }
    if (def->vtec) {
        /* Part of the RPM cluster: the solenoid's state, so Derek can see
         * the DID working. "--" when there's no fresh reading at all. */
        lv_obj_t *status = box(c);
        lv_obj_set_size(status, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(status, BAR_GAP, 0);
        lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(status, 8, 0);
        label(status, &lv_font_montserrat_16, COLOR_DIM, "VTEC");
        row->vtec = label(status, &lv_font_montserrat_20, COLOR_DIM, "--");
    }
}

/* A hairline under every visible slot of the list but the last. */
static void rule_lines(lv_obj_t *list)
{
    uint32_t n = lv_obj_get_child_count(list);
    lv_obj_t *last = NULL;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *slot = lv_obj_get_child(list, (int32_t)i);
        if (!lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN)) {
            last = slot;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *slot = lv_obj_get_child(list, (int32_t)i);
        bool rule = slot != last && !lv_obj_has_flag(slot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_border_width(slot, rule ? 1 : 0, 0);
    }
}

void muse_obd_cards_build(lv_obj_t *const tiles[MUSE_OBD_CARDS])
{
    int w = muse_board->width, h = muse_board->height;
    int title_y = h / 16 + (muse_board->round ? 24 : 0);
    int top = title_y + lv_font_get_line_height(&lv_font_montserrat_20) + TOP_GAP;
    int bottom = BOTTOM_INSET + (muse_board->round ? 24 : 0);
    int list_w = w - 2 * SIDE_PAD - (muse_board->round ? w / 8 : 0);
    int list_h = h - top - bottom;
    for (int i = 0; i < MUSE_OBD_CARDS; i++) {
        lv_obj_t *t = tiles[i];
        lv_obj_set_style_bg_color(t, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = label(t, &lv_font_montserrat_20, COLOR_ACCENT, TITLES[i]);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);

        s_state[i] = label(t, &lv_font_montserrat_20, COLOR_DIM, "");
        lv_obj_set_style_text_align(s_state[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(s_state[i]);

        /* The cells fill the tile between the title and the dots. */
        lv_obj_t *list = box(t);
        lv_obj_set_size(list, list_w, list_h);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, top);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        s_list[i] = list;
    }

    /* Each card's height in units: a hero takes two, a pair of halves one. */
    int units[MUSE_OBD_CARDS] = { 0 };
    for (size_t r = 0; r < ROW_COUNT; r++) {
        bool second_half = ROWS[r].half && r > 0 && ROWS[r - 1].half && ROWS[r - 1].card == ROWS[r].card;
        units[ROWS[r].card] += second_half ? 0 : ROWS[r].hero ? 2 : 1;
    }
    lv_obj_t *pair = NULL;
    for (size_t r = 0; r < ROW_COUNT; r++) {
        const row_def_t *def = &ROWS[r];
        int unit_h = list_h / (units[def->card] ? units[def->card] : 1);
        const lv_font_t *font = fit_font(def->hero ? 48 : 40, def->hero ? 2 * unit_h : unit_h, cell_extra(def));
        bool second_half = def->half && r > 0 && ROWS[r - 1].half && ROWS[r - 1].card == def->card;
        if (def->half && !second_half) {
            pair = box(s_list[def->card]);
            lv_obj_set_width(pair, lv_pct(100));
            lv_obj_set_flex_grow(pair, 1);
            lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(pair, 8, 0);
        }
        build_row(s_list[def->card], def, &s_rows[r], def->half ? pair : NULL, font);
    }
    for (int i = 0; i < MUSE_OBD_CARDS; i++) {
        uint32_t n = lv_obj_get_child_count(s_list[i]);
        for (uint32_t k = 0; k < n; k++) {
            lv_obj_t *slot = lv_obj_get_child(s_list[i], (int32_t)k);
            lv_obj_set_style_border_side(slot, LV_BORDER_SIDE_BOTTOM, 0);
            lv_obj_set_style_border_color(slot, lv_color_hex(COLOR_RULE), 0);
            lv_obj_set_style_border_opa(slot, LV_OPA_COVER, 0);
        }
        rule_lines(s_list[i]);
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

/* ---- Refreshing: only what changed ---- */

static void set_text(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
}

/* Shows or hides o; true if that changed anything. */
static bool set_hidden(lv_obj_t *o, bool hidden)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) {
        return false;
    }
    lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, hidden);
    return true;
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

/* A fresh value's colour: the glance colours, else the row's own. */
static uint32_t fresh_color(const row_def_t *def, int value)
{
    if (def->tone == TONE_COOLANT && value >= COOLANT_RED_C) {
        return COLOR_RED;
    }
    if (def->tone == TONE_COOLANT && value >= COOLANT_AMBER_C) {
        return COLOR_AMBER;
    }
    if (def->tone == TONE_TRIM && (value >= TRIM_AMBER_PCT || value <= -TRIM_AMBER_PCT)) {
        return COLOR_AMBER;
    }
    return def->hero ? COLOR_ACCENT : COLOR_TEXT;
}

/* The VTEC word under RPM: ON in the accent, OFF dim, "--" dim when the
 * car doesn't answer DID 0x2612 or the reading isn't fresh. */
static void tick_vtec(const muse_obd_status_t *st, row_t *row)
{
    bool fresh = st->vtec_sol.valid && st->vtec_sol.age_ms <= MUSE_OBD_CARD_STALE_MS;
    const char *word = !fresh ? "--" : st->vtec_sol.value ? "ON" : "OFF";
    uint32_t color = fresh && st->vtec_sol.value ? COLOR_ACCENT : COLOR_DIM;
    set_text(row->vtec, word);
    if (color != row->vtec_color) {
        lv_obj_set_style_text_color(row->vtec, lv_color_hex(color), 0);
        row->vtec_color = color;
    }
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

    bool reflow[MUSE_OBD_CARDS] = { false };
    for (size_t r = 0; r < ROW_COUNT; r++) {
        const row_def_t *def = &ROWS[r];
        row_t *row = &s_rows[r];
        int pid;
        muse_obd_reading_t v = reading(&st, def->src, &pid);

        /* Proven unsupported once the scan is in: the row goes and the card
         * reflows around it. Before the scan, it stays and reads "...". */
        bool unsupported = muse_obd_pid_unsupported(&st, pid);
        reflow[def->card] |= set_hidden(row->cell, unsupported);
        if (row->slot != row->cell) {
            /* A pair goes when both its halves have. */
            const row_t *other = r > 0 && s_rows[r - 1].slot == row->slot ? &s_rows[r - 1]
                               : r + 1 < ROW_COUNT && s_rows[r + 1].slot == row->slot ? &s_rows[r + 1] : row;
            bool both = lv_obj_has_flag(row->cell, LV_OBJ_FLAG_HIDDEN)
                     && lv_obj_has_flag(other->cell, LV_OBJ_FLAG_HIDDEN);
            reflow[def->card] |= set_hidden(row->slot, both);
        }

        char text[16], sub[24];
        uint32_t color;
        int bar = 0;
        if (v.valid) {
            int shown = def->mph ? (v.value * 621 + 500) / 1000 : v.value;
            snprintf(text, sizeof(text), def->sign ? "%+d" : "%d", shown);
            snprintf(sub, sizeof(sub), "%d km/h", v.value);
            bool stale = v.age_ms > MUSE_OBD_CARD_STALE_MS;
            color = stale ? COLOR_STALE : fresh_color(def, v.value);
            bar = v.value < 0 ? 0 : v.value > def->bar_max ? def->bar_max : v.value;
        } else {
            /* "--": the scan proved the ECU hasn't got it (hidden by now);
             * "...": not yet. */
            const char *none = unsupported ? "--" : "...";
            snprintf(text, sizeof(text), "%s", none);
            snprintf(sub, sizeof(sub), "%s km/h", none);
            color = COLOR_DIM;
        }
        set_text(row->value, text);
        if (row->sub) {
            set_text(row->sub, sub);
        }
        if (row->vtec) {
            tick_vtec(&st, row);
        }
        if (color != row->color) {
            lv_obj_set_style_text_color(row->value, lv_color_hex(color), 0);
            row->color = color;
        }
        if (row->bar) {
            uint32_t bar_color = v.valid && v.age_ms <= MUSE_OBD_CARD_STALE_MS ? COLOR_ACCENT : COLOR_STALE;
            if (bar != row->bar_value) {
                lv_bar_set_value(row->bar, bar, LV_ANIM_OFF);
                row->bar_value = bar;
            }
            if (bar_color != row->bar_color) {
                lv_obj_set_style_bg_color(row->bar, lv_color_hex(bar_color), LV_PART_INDICATOR);
                row->bar_color = bar_color;
            }
        }
    }
    for (int i = 0; i < MUSE_OBD_CARDS; i++) {
        if (reflow[i]) {
            rule_lines(s_list[i]);
        }
    }
}

