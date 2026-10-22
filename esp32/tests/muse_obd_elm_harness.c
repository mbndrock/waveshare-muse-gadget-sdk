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

/* The OBD reactions' ELM327 parser and triggers (muse_obd_elm.c: VTEC on RPM,
 * the coolant warning and the warm-up thumbs-up on PID 05), and the car data
 * cards' side (the PID support scan, the cards' PIDs, the secondary poll
 * cycle and ATDPN), fed the way the LELink^2 adapter's FFE1 notifications cut
 * its replies. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_obd_elm.h"

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

/* Feeds len bytes of `text` in notifications of `chunk` bytes; collects each
 * complete response into out[] (up to max) and returns how many completed. */
static int feed_chunks(muse_elm_rx_t *rx, const char *text, size_t len, size_t chunk,
                       char out[][MUSE_ELM_RESP_MAX], int max)
{
    int n = 0;
    for (size_t at = 0; at < len; at += chunk) {
        size_t end = at + chunk < len ? at + chunk : len;
        for (size_t i = at; i < end; i++) {
            if (muse_elm_feed(rx, (uint8_t)text[i]) && n < max) {
                strcpy(out[n++], rx->resp);
            }
        }
    }
    return n;
}

static void test_fragmented_atz(void)
{
    /* ATZ with echo still on: the echo, a blank line, the banner, the prompt. */
    const char *atz = "ATZ\r\r\rOBDII v1.5\r\r>";
    for (size_t chunk = 1; chunk <= strlen(atz); chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[2][MUSE_ELM_RESP_MAX];
        int n = feed_chunks(&rx, atz, strlen(atz), chunk, out, 2);
        CHECK(n == 1);
        CHECK(strstr(out[0], "OBDII v1.5") != NULL);
        CHECK(strchr(out[0], '>') == NULL);
        CHECK(strchr(out[0], '\r') == NULL);
        int rpm = -1;
        CHECK(muse_elm_parse_rpm(out[0], &rpm) == MUSE_ELM_NONE);
        CHECK(rpm == -1);
    }
}

static void test_searching_then_frames(void)
{
    /* The first 010C after ATSP0 searches for the protocol; NULs from some
     * clones and two responses split across one notification boundary. */
    static const char stream[] =
        "SEARCHING...\r41 0C 0B B8 \r\r>"   /* 0x0BB8 / 4 = 750 rpm, idle */
        "41 0C 55 F0 \r\r>"                 /* 0x55F0 / 4 = 5500 */
        "410C5DC0\r\r>"                     /* ATS0 form: 0x5DC0 / 4 = 6000 */
        "\x00" "41 0c 6a 00\r\r>"           /* lower case: 0x6A00 / 4 = 6784 */
        "41 0C FF FF\r\r>";                 /* the top: 16383 */
    static const int want[] = { 750, 5500, 6000, 6784, 16383 };
    for (size_t chunk = 1; chunk <= 20; chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[8][MUSE_ELM_RESP_MAX];
        int n = feed_chunks(&rx, stream, sizeof(stream) - 1, chunk, out, 8);
        CHECK(n == 5);
        for (int i = 0; i < n && i < 5; i++) {
            int rpm = -1;
            CHECK(muse_elm_parse_rpm(out[i], &rpm) == MUSE_ELM_RPM);
            CHECK(rpm == want[i]);
        }
    }
}

static void test_parse_cases(void)
{
    int rpm = -1;
    CHECK(muse_elm_parse_rpm("NO DATA\n\n", &rpm) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_rpm("SEARCHING...\nNO DATA\n", &rpm) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_rpm("?\n", &rpm) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_rpm("SEARCHING...\nUNABLE TO CONNECT\n", &rpm) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_rpm("CAN ERROR\n", &rpm) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_rpm("BUS INIT: ...ERROR\n", &rpm) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_rpm("STOPPED\n", &rpm) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_rpm("OK\n", &rpm) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_rpm("", &rpm) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_rpm("010C\n", &rpm) == MUSE_ELM_NONE);          /* an echo */
    CHECK(muse_elm_parse_rpm("41 0C 1A\n", &rpm) == MUSE_ELM_NONE);      /* cut short */
    CHECK(muse_elm_parse_rpm("41 0D 1A F8\n", &rpm) == MUSE_ELM_NONE);   /* another PID */
    CHECK(muse_elm_parse_rpm("7E8 04 41 0C 1A F8\n", &rpm) == MUSE_ELM_NONE);  /* headers: not ours */
    CHECK(muse_elm_parse_rpm("41 0C ZZ F8\n", &rpm) == MUSE_ELM_NONE);
    CHECK(rpm == -1);
    /* The echo, then the answer. */
    CHECK(muse_elm_parse_rpm("010C\n41 0C 1A F8\n", &rpm) == MUSE_ELM_RPM && rpm == 0x1AF8 / 4);
    /* Two ECUs answer: the first wins. */
    CHECK(muse_elm_parse_rpm("41 0C 0F A0\n41 0C 10 00\n", &rpm) == MUSE_ELM_RPM && rpm == 1000);
    /* A frame beats a NO DATA from another line. */
    CHECK(muse_elm_parse_rpm("NO DATA\n41 0C 0F A0\n", &rpm) == MUSE_ELM_RPM && rpm == 1000);
}

static void test_overflow(void)
{
    muse_elm_rx_t rx;
    muse_elm_rx_reset(&rx);
    for (int i = 0; i < 1000; i++) {
        CHECK(!muse_elm_feed(&rx, 'A'));
    }
    CHECK(muse_elm_feed(&rx, '>'));
    CHECK(strlen(rx.resp) == MUSE_ELM_RESP_MAX - 1);
    /* The next byte starts a fresh response. */
    CHECK(!muse_elm_feed(&rx, 'O'));
    CHECK(!muse_elm_feed(&rx, 'K'));
    CHECK(muse_elm_feed(&rx, '>'));
    CHECK(strcmp(rx.resp, "OK") == 0);
}

static void test_trigger(void)
{
    muse_vtec_trigger_t t;
    muse_vtec_init(&t);
    uint32_t ms = 1000;

    /* Connected mid-pull: not armed yet, so no fire. */
    CHECK(!muse_vtec_update(&t, 6500, ms += 100));
    CHECK(!muse_vtec_update(&t, 7000, ms += 100));
    /* 5501..5999 doesn't arm. */
    CHECK(!muse_vtec_update(&t, 5800, ms += 100));
    CHECK(!muse_vtec_update(&t, 6100, ms += 100));
    /* Arms at 5500, fires once at 6000. */
    CHECK(!muse_vtec_update(&t, 5500, ms += 100));
    CHECK(!muse_vtec_update(&t, 5999, ms += 100));
    CHECK(muse_vtec_update(&t, 6000, ms += 100));
    uint32_t fired_at = ms;
    CHECK(!muse_vtec_update(&t, 7500, ms += 100));
    CHECK(!muse_vtec_update(&t, 6200, ms += 100));
    /* Hovering between the thresholds doesn't re-arm. */
    CHECK(!muse_vtec_update(&t, 5700, ms += 100));
    CHECK(!muse_vtec_update(&t, 6300, ms += 100));

    /* Re-armed inside the cooldown: the crossing is swallowed... */
    CHECK(!muse_vtec_update(&t, 3000, ms += 100));
    CHECK(!muse_vtec_update(&t, 6400, ms = fired_at + 10000));
    /* ...and it doesn't fire late when the cooldown runs out mid-pull. */
    CHECK(!muse_vtec_update(&t, 6800, ms = fired_at + 31000));
    /* After the cooldown, a fresh drop and crossing fires. */
    CHECK(!muse_vtec_update(&t, 2500, ms += 100));
    CHECK(muse_vtec_update(&t, 6050, ms += 100));
    fired_at = ms;

    /* Exactly at the cooldown edge, it fires. */
    CHECK(!muse_vtec_update(&t, 4000, ms = fired_at + MUSE_VTEC_COOLDOWN_MS - 1));
    CHECK(!muse_vtec_update(&t, 6000, ms));
    CHECK(!muse_vtec_update(&t, 4000, ms += 1));
    CHECK(muse_vtec_update(&t, 6000, ms));

    /* A dropped link disarms; the cooldown carries over. */
    muse_vtec_disarm(&t);
    CHECK(!muse_vtec_update(&t, 6500, ms += 40000));
    CHECK(!muse_vtec_update(&t, 1000, ms += 100));
    CHECK(muse_vtec_update(&t, 6500, ms += 100));

    /* A bad reading changes nothing. */
    CHECK(!muse_vtec_update(&t, 1000, ms += 100));
    CHECK(!muse_vtec_update(&t, -1, ms += 40000));
    CHECK(t.armed);

    /* The millisecond clock wrapping doesn't break the cooldown. */
    muse_vtec_init(&t);
    CHECK(!muse_vtec_update(&t, 1000, 0xFFFFF000u));
    CHECK(muse_vtec_update(&t, 6000, 0xFFFFF100u));
    CHECK(!muse_vtec_update(&t, 1000, 0x00000100u));
    CHECK(!muse_vtec_update(&t, 6000, 0x00000200u));   /* ~4.3 s later: cooling down */
    CHECK(!muse_vtec_update(&t, 1000, 0x00008000u));
    CHECK(muse_vtec_update(&t, 6000, 0x00008000u));    /* ~36 s later */
}

static void test_coolant_frames(void)
{
    /* The poll loop's mix: three 010C, then a 0105, cut anywhere. Coolant is
     * A - 40: 0x91 is 105 C, 0x3C a cold 20 C, 0x00 the bottom (-40), 0xFF the
     * top (215). */
    static const char stream[] =
        "SEARCHING...\r41 05 3C \r\r>"      /* cold start: 20 C */
        "41 0C 0B B8 \r\r>"
        "\x00" "41 05 91\r\r>"              /* 105 C, after a NUL */
        "410591\r\r>"                       /* ATS0 form */
        "41 05 8c\r\r>"                     /* lower case: 0x8C = 100 C */
        "41 05 00\r\r>"
        "41 05 FF\r\r>";
    static const int want[] = { 20, -1, 105, 105, 100, -40, 215 };
    for (size_t chunk = 1; chunk <= 24; chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[8][MUSE_ELM_RESP_MAX];
        int n = feed_chunks(&rx, stream, sizeof(stream) - 1, chunk, out, 8);
        CHECK(n == 7);
        for (int i = 0; i < n && i < 7; i++) {
            int c = -99, rpm = -1;
            if (want[i] == -1) {
                /* The RPM reply isn't a coolant reading, and vice versa. */
                CHECK(muse_elm_parse_coolant(out[i], &c) == MUSE_ELM_NONE && c == -99);
                CHECK(muse_elm_parse_rpm(out[i], &rpm) == MUSE_ELM_RPM && rpm == 750);
            } else {
                CHECK(muse_elm_parse_coolant(out[i], &c) == MUSE_ELM_COOLANT);
                CHECK(c == want[i]);
                CHECK(muse_elm_parse_rpm(out[i], &rpm) == MUSE_ELM_NONE && rpm == -1);
            }
        }
    }
}

static void test_coolant_parse_cases(void)
{
    int c = -99;
    CHECK(muse_elm_parse_coolant("41 05 91\n", &c) == MUSE_ELM_COOLANT && c == 105);
    CHECK(muse_elm_parse_coolant("41 05 3C\n", &c) == MUSE_ELM_COOLANT && c == 20);
    CHECK(muse_elm_parse_coolant("4 1 0 5 9 1\n", &c) == MUSE_ELM_COOLANT && c == 105);   /* odd spacing */
    c = -99;
    CHECK(muse_elm_parse_coolant("NO DATA\n\n", &c) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_coolant("SEARCHING...\nNO DATA\n", &c) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_coolant("?\n", &c) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_coolant("SEARCHING...\nUNABLE TO CONNECT\n", &c) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_coolant("BUS INIT: ...ERROR\n", &c) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_coolant("STOPPED\n", &c) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_coolant("OK\n", &c) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_coolant("", &c) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_coolant("0105\n", &c) == MUSE_ELM_NONE);              /* an echo */
    CHECK(muse_elm_parse_coolant("41 05\n", &c) == MUSE_ELM_NONE);             /* cut short */
    CHECK(muse_elm_parse_coolant("41 05 9\n", &c) == MUSE_ELM_NONE);           /* half a byte */
    CHECK(muse_elm_parse_coolant("41 0D 91\n", &c) == MUSE_ELM_NONE);          /* another PID */
    CHECK(muse_elm_parse_coolant("41 0C 1A F8\n", &c) == MUSE_ELM_NONE);       /* RPM */
    CHECK(muse_elm_parse_coolant("7E8 03 41 05 91\n", &c) == MUSE_ELM_NONE);   /* headers: not ours */
    CHECK(muse_elm_parse_coolant("41 05 G1\n", &c) == MUSE_ELM_NONE);
    CHECK(c == -99);
    /* The echo, then the answer; two ECUs: the first wins; a frame beats NO DATA. */
    CHECK(muse_elm_parse_coolant("0105\n41 05 7B\n", &c) == MUSE_ELM_COOLANT && c == 83);
    CHECK(muse_elm_parse_coolant("41 05 91\n41 05 3C\n", &c) == MUSE_ELM_COOLANT && c == 105);
    CHECK(muse_elm_parse_coolant("NO DATA\n41 05 92\n", &c) == MUSE_ELM_COOLANT && c == 106);
    /* Trailing bytes from a clone are ignored, as for RPM. */
    CHECK(muse_elm_parse_coolant("41 05 91 00 00\n", &c) == MUSE_ELM_COOLANT && c == 105);
}

static void test_coolant_trigger(void)
{
    muse_coolant_trigger_t t;
    muse_coolant_init(&t);
    uint32_t ms = 1000;
    const int run = 2500;

    /* Normal running, then it climbs: 105 C must hold for 5 s. */
    CHECK(!muse_coolant_update(&t, 88, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 104, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 105, run, ms += 1000));
    uint32_t hot_at = ms;
    CHECK(!muse_coolant_update(&t, 107, run, ms = hot_at + 2000));
    CHECK(!muse_coolant_update(&t, 106, run, ms = hot_at + MUSE_COOLANT_SUSTAIN_MS - 1));
    CHECK(muse_coolant_update(&t, 105, run, ms = hot_at + MUSE_COOLANT_SUSTAIN_MS));   /* exactly 5 s */
    uint32_t fired_at = ms;
    /* Fires once: still hot, no repeat. */
    CHECK(!muse_coolant_update(&t, 110, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 112, run, ms += 10000));

    /* After the cooldown, still not re-armed (never dropped to 100): quiet. */
    CHECK(!muse_coolant_update(&t, 101, run, ms = fired_at + MUSE_COOLANT_COOLDOWN_MS + 1000));
    CHECK(!muse_coolant_update(&t, 108, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 108, run, ms += 10000));
    /* Re-arms at <= 100 (101 didn't), then a new sustained 5 s fires. */
    CHECK(!muse_coolant_update(&t, 100, run, ms += 1000));
    CHECK(t.armed);
    CHECK(!muse_coolant_update(&t, 106, run, ms += 1000));
    hot_at = ms;
    CHECK(!muse_coolant_update(&t, 106, run, ms = hot_at + 4000));
    CHECK(muse_coolant_update(&t, 106, run, ms = hot_at + 5000));
    fired_at = ms;

    /* Re-armed inside the cooldown: no fire until it runs out... */
    CHECK(!muse_coolant_update(&t, 95, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 105, run, ms += 1000));
    hot_at = ms;
    CHECK(!muse_coolant_update(&t, 105, run, ms = hot_at + 6000));
    CHECK(!muse_coolant_update(&t, 109, run, ms = fired_at + MUSE_COOLANT_COOLDOWN_MS - 1));
    /* ...then, still hot all along, it does. */
    CHECK(muse_coolant_update(&t, 109, run, ms = fired_at + MUSE_COOLANT_COOLDOWN_MS));
    fired_at = ms;

    /* Sustain resets on any reading below 105. */
    muse_coolant_init(&t);
    ms = 500000;
    CHECK(!muse_coolant_update(&t, 106, run, ms));
    CHECK(!muse_coolant_update(&t, 106, run, ms += 2000));
    CHECK(!muse_coolant_update(&t, 104, run, ms += 1000));   /* dips: starts over */
    CHECK(!muse_coolant_update(&t, 106, run, ms += 1000));
    hot_at = ms;
    CHECK(!muse_coolant_update(&t, 106, run, ms += 3000));   /* 7 s since the first: not enough */
    CHECK(!muse_coolant_update(&t, 106, run, ms = hot_at + 4999));
    CHECK(muse_coolant_update(&t, 106, run, ms = hot_at + 5000));

    /* Engine off: a hot soak with the key on never builds sustain or fires. */
    muse_coolant_init(&t);
    ms = 900000;
    for (int i = 0; i < 60; i++) {
        CHECK(!muse_coolant_update(&t, 112, 0, ms += 1000));
        CHECK(!muse_coolant_update(&t, 112, -1, ms += 1000));    /* RPM unknown (NO DATA) */
        CHECK(!muse_coolant_update(&t, 112, MUSE_COOLANT_RUNNING_RPM - 1, ms += 1000));
    }
    /* The engine starts: sustain only from the first running reading. */
    CHECK(!muse_coolant_update(&t, 112, MUSE_COOLANT_RUNNING_RPM, ms += 1000));
    hot_at = ms;
    CHECK(!muse_coolant_update(&t, 112, 750, ms = hot_at + 3000));
    /* Stalls mid-sustain: starts over. */
    CHECK(!muse_coolant_update(&t, 112, 0, ms = hot_at + 4000));
    CHECK(!muse_coolant_update(&t, 112, 750, ms = hot_at + 6000));
    CHECK(!muse_coolant_update(&t, 112, 750, ms = hot_at + 10999));
    CHECK(muse_coolant_update(&t, 112, 750, ms = hot_at + 11000));

    /* A dropped link restarts the sustain; arming and the cooldown hold. */
    muse_coolant_init(&t);
    ms = 2000000;
    CHECK(!muse_coolant_update(&t, 106, run, ms));
    CHECK(!muse_coolant_update(&t, 106, run, ms += 4000));
    muse_coolant_reset(&t);
    CHECK(!muse_coolant_update(&t, 106, run, ms += 1000));
    CHECK(!muse_coolant_update(&t, 106, run, ms += 4999));
    CHECK(muse_coolant_update(&t, 106, run, ms += 1));
    muse_coolant_reset(&t);
    CHECK(!t.armed);
    CHECK(!muse_coolant_update(&t, 106, run, ms += 200000));
    CHECK(!muse_coolant_update(&t, 106, run, ms += 6000));   /* not re-armed: quiet */

    /* Readings out of the PID's range change nothing. */
    muse_coolant_init(&t);
    CHECK(!muse_coolant_update(&t, 106, run, 1000));
    CHECK(!muse_coolant_update(&t, 216, run, 3000));
    CHECK(!muse_coolant_update(&t, -41, run, 4000));
    CHECK(t.hot);
    CHECK(muse_coolant_update(&t, 106, run, 6000));

    /* The millisecond clock wrapping doesn't break the sustain or the cooldown. */
    muse_coolant_init(&t);
    CHECK(!muse_coolant_update(&t, 106, run, 0xFFFFF000u));
    CHECK(!muse_coolant_update(&t, 106, run, 0x00000100u));            /* ~4.3 s */
    CHECK(muse_coolant_update(&t, 106, run, 0x00000400u));             /* ~5.1 s */
    CHECK(!muse_coolant_update(&t, 90, run, 0x00001000u));
    CHECK(!muse_coolant_update(&t, 106, run, 0x00002000u));
    CHECK(!muse_coolant_update(&t, 106, run, 0x00010000u));            /* ~66 s after the fire */
    CHECK(!muse_coolant_update(&t, 106, run, 0x00000400u + MUSE_COOLANT_COOLDOWN_MS - 1));
    CHECK(muse_coolant_update(&t, 106, run, 0x00000400u + MUSE_COOLANT_COOLDOWN_MS));
}

static void test_warmup_trigger(void)
{
    muse_warmup_trigger_t t;

    /* A cold start: armed at <= 70, fires once at 82, then quiet while hot. */
    muse_warmup_init(&t);
    CHECK(!t.armed);
    CHECK(!muse_warmup_update(&t, 20));
    CHECK(t.armed);
    CHECK(!muse_warmup_update(&t, 55));
    CHECK(!muse_warmup_update(&t, 71));
    CHECK(!muse_warmup_update(&t, 81));
    CHECK(muse_warmup_update(&t, MUSE_WARMUP_FIRE_C));
    CHECK(!t.armed);
    for (int c = 82; c <= 110; c++) {
        CHECK(!muse_warmup_update(&t, c));
    }
    /* Cooling off but not cold (71..81, back to 90) doesn't re-arm. */
    CHECK(!muse_warmup_update(&t, 71));
    CHECK(!muse_warmup_update(&t, 90));
    /* Back to 70 re-arms it, and the next warm-up fires again, once. */
    CHECK(!muse_warmup_update(&t, MUSE_WARMUP_COLD_C));
    CHECK(t.armed);
    CHECK(!muse_warmup_update(&t, 76));
    CHECK(muse_warmup_update(&t, 88));
    CHECK(!muse_warmup_update(&t, 88));

    /* Already warm on the first reading (hot restart, reconnect mid-drive):
     * never fires, however long it stays warm. */
    muse_warmup_init(&t);
    CHECK(!muse_warmup_update(&t, 90));
    CHECK(!muse_warmup_update(&t, 82));
    CHECK(!muse_warmup_update(&t, 75));
    CHECK(!muse_warmup_update(&t, 95));
    CHECK(!t.armed);

    /* Invalid readings change nothing: they neither arm nor fire. */
    muse_warmup_init(&t);
    CHECK(!muse_warmup_update(&t, -41));
    CHECK(!muse_warmup_update(&t, -1000));
    CHECK(!t.armed);
    CHECK(!muse_warmup_update(&t, 50));
    CHECK(!muse_warmup_update(&t, 216));
    CHECK(!muse_warmup_update(&t, 9999));
    CHECK(t.armed);
    CHECK(muse_warmup_update(&t, 84));
    /* The PID's range ends are real readings: -40 arms, 215 fires. */
    muse_warmup_init(&t);
    CHECK(!muse_warmup_update(&t, -40));
    CHECK(muse_warmup_update(&t, 215));

    /* A link drop re-initialises it: armed before the drop, the new
     * connection must see cold again. */
    muse_warmup_init(&t);
    CHECK(!muse_warmup_update(&t, 40));
    muse_warmup_init(&t);
    CHECK(!muse_warmup_update(&t, 85));

    /* Fed from parsed replies, the way muse_obd.c does it. */
    muse_warmup_init(&t);
    static const char *const REPLIES[] = { "41 05 64", "NO DATA", "41 05 6E", "?", "41 05 7A", "41 05 7B" };
    int fires = 0;
    for (size_t i = 0; i < sizeof(REPLIES) / sizeof(REPLIES[0]); i++) {
        int c = 0;
        if (muse_elm_parse_coolant(REPLIES[i], &c) == MUSE_ELM_COOLANT) {
            fires += muse_warmup_update(&t, c);   /* 60, 70, 82, 83 */
        }
    }
    CHECK(fires == 1);
}

/* ---- The car data cards ---- */

typedef muse_elm_result_t (*parse_fn)(const char *resp, int *value);

/* Each card PID's parser, with raw bytes A and the value they must read as. */
static const struct {
    const char *name;
    int pid;
    parse_fn parse;
    muse_elm_result_t ok;
    int a[5], want[5];
} EXTRAS[] = {
    { "load", 0x04, muse_elm_parse_load, MUSE_ELM_LOAD,
      { 0x00, 0x80, 0xFF, 0x33, 0x1A }, { 0, 50, 100, 20, 10 } },
    { "map", 0x0B, muse_elm_parse_map, MUSE_ELM_MAP,
      { 0x00, 0x1E, 0x65, 0xFF, 0x64 }, { 0, 30, 101, 255, 100 } },
    { "speed", 0x0D, muse_elm_parse_speed, MUSE_ELM_SPEED,
      { 0x00, 0x58, 0xFF, 0x01, 0x64 }, { 0, 88, 255, 1, 100 } },
    { "timing", 0x0E, muse_elm_parse_timing, MUSE_ELM_TIMING,
      { 0x00, 0x8C, 0xFF, 0x75, 0x80 }, { -64, 6, 63, -6, 0 } },
    { "iat", 0x0F, muse_elm_parse_iat, MUSE_ELM_IAT,
      { 0x00, 0x46, 0xFF, 0x28, 0x3C }, { -40, 30, 215, 0, 20 } },
    { "throttle", 0x11, muse_elm_parse_throttle, MUSE_ELM_THROTTLE,
      { 0x00, 0x33, 0xFF, 0x80, 0x26 }, { 0, 20, 100, 50, 14 } },
    { "stft", 0x06, muse_elm_parse_stft, MUSE_ELM_STFT,
      { 0x80, 0x00, 0xFF, 0x84, 0x7C }, { 0, -100, 99, 3, -3 } },
    { "ltft", 0x07, muse_elm_parse_ltft, MUSE_ELM_LTFT,
      { 0x80, 0x00, 0xFF, 0x8A, 0x70 }, { 0, -100, 99, 7, -12 } },
    { "ambient", 0x46, muse_elm_parse_ambient, MUSE_ELM_AMBIENT,
      { 0x00, 0x3C, 0xFF, 0x28, 0x14 }, { -40, 20, 215, 0, -20 } },
};
#define EXTRA_COUNT (int)(sizeof(EXTRAS) / sizeof(EXTRAS[0]))

/* Every other parser, RPM and coolant included, finds nothing in `resp`. */
static void check_nobody_else(const char *resp, int except)
{
    for (int j = 0; j < EXTRA_COUNT; j++) {
        if (j == except) {
            continue;
        }
        int v = -999;
        if (EXTRAS[j].parse(resp, &v) != MUSE_ELM_NONE || v != -999) {
            fprintf(stderr, "%s read \"%s\"\n", EXTRAS[j].name, resp);
            s_fail++;
        }
    }
    int v = -999;
    CHECK(muse_elm_parse_rpm(resp, &v) == MUSE_ELM_NONE && v == -999);
    CHECK(muse_elm_parse_coolant(resp, &v) == MUSE_ELM_NONE && v == -999);
}

static void test_extra_parse_cases(void)
{
    for (int i = 0; i < EXTRA_COUNT; i++) {
        int pid = EXTRAS[i].pid, other = EXTRAS[(i + 1) % EXTRA_COUNT].pid;
        parse_fn parse = EXTRAS[i].parse;
        muse_elm_result_t ok = EXTRAS[i].ok;
        char buf[96];
        int v;
        for (int k = 0; k < 5; k++) {
            int a = EXTRAS[i].a[k], want = EXTRAS[i].want[k];
            /* Nominal, ATS0 form, lower case, trailing clone bytes. */
            snprintf(buf, sizeof(buf), "41 %02X %02X\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            check_nobody_else(buf, i);
            snprintf(buf, sizeof(buf), "41%02X%02X\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            snprintf(buf, sizeof(buf), "41 %02x %02x\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            snprintf(buf, sizeof(buf), "41 %02X %02X 00 00\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            /* The echo, then the answer; SEARCHING... first. */
            snprintf(buf, sizeof(buf), "01%02X\n41 %02X %02X\n", pid, pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            snprintf(buf, sizeof(buf), "SEARCHING...\n41 %02X %02X\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            /* Two ECUs: the first wins; a frame beats NO DATA. */
            snprintf(buf, sizeof(buf), "41 %02X %02X\n41 %02X %02X\n", pid, a, pid, a ^ 0x55);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
            snprintf(buf, sizeof(buf), "NO DATA\n41 %02X %02X\n", pid, a);
            v = -999;
            CHECK(parse(buf, &v) == ok && v == want);
        }

        /* Nothing that answers: the value is left alone. */
        v = -999;
        snprintf(buf, sizeof(buf), "01%02X\n", pid);                  /* the echo alone */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "41 %02X 50\n", other);            /* another PID */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "41 %02X\n", pid);                 /* cut short */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "41 %02X 5\n", pid);               /* half a byte */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "7E8 03 41 %02X 50\n", pid);       /* headers on */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "7E803 41%02X50\n", pid);          /* headers, no spaces */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "41 %02X G0\n", pid);              /* not hex */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        snprintf(buf, sizeof(buf), "42 %02X 50\n", pid);              /* not mode 01's answer */
        CHECK(parse(buf, &v) == MUSE_ELM_NONE);
        CHECK(parse("41 0C 1A F8\n", &v) == MUSE_ELM_NONE);           /* RPM */
        CHECK(parse("41 05 91\n", &v) == MUSE_ELM_NONE);              /* coolant */
        CHECK(parse("OK\n", &v) == MUSE_ELM_NONE);
        CHECK(parse("", &v) == MUSE_ELM_NONE);
        CHECK(parse("NO DATA\n\n", &v) == MUSE_ELM_NO_DATA);
        CHECK(parse("SEARCHING...\nNO DATA\n", &v) == MUSE_ELM_NO_DATA);
        CHECK(parse("?\n", &v) == MUSE_ELM_ERROR);
        CHECK(parse("CAN ERROR\n", &v) == MUSE_ELM_ERROR);
        CHECK(parse("SEARCHING...\nUNABLE TO CONNECT\n", &v) == MUSE_ELM_ERROR);
        CHECK(parse("BUS INIT: ...ERROR\n", &v) == MUSE_ELM_ERROR);
        CHECK(parse("STOPPED\n", &v) == MUSE_ELM_ERROR);
        CHECK(v == -999);
    }
}

/* All the cards' replies in one stream, NULs and all, cut at every size. */
static void test_extra_frames(void)
{
    char stream[1024];
    size_t len = 0;
    for (int i = 0; i < EXTRA_COUNT; i++) {
        len += (size_t)snprintf(stream + len, sizeof(stream) - len, "%s41 %02X %02X \r\r>", i % 3 ? "" : "\r",
                                EXTRAS[i].pid, EXTRAS[i].a[1]);
        stream[len++] = '\0';   /* a clone's NUL between replies */
    }
    for (size_t chunk = 1; chunk <= 24; chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[EXTRA_COUNT + 1][MUSE_ELM_RESP_MAX];
        int n = feed_chunks(&rx, stream, len, chunk, out, EXTRA_COUNT + 1);
        CHECK(n == EXTRA_COUNT);
        for (int i = 0; i < n && i < EXTRA_COUNT; i++) {
            int v = -999;
            CHECK(EXTRAS[i].parse(out[i], &v) == EXTRAS[i].ok && v == EXTRAS[i].want[1]);
            check_nobody_else(out[i], i);
        }
    }
}

static void test_support_parse(void)
{
    uint32_t m = 0;
    CHECK(muse_elm_parse_support("41 00 BE 1F A8 13\n", 0x00, &m) == MUSE_ELM_SUPPORT && m == 0xBE1FA813u);
    CHECK(muse_elm_parse_support("SEARCHING...\n41 00 BE 3E B8 11\n", 0x00, &m) == MUSE_ELM_SUPPORT
          && m == 0xBE3EB811u);
    CHECK(muse_elm_parse_support("4100be1fa813\n", 0x00, &m) == MUSE_ELM_SUPPORT && m == 0xBE1FA813u);
    CHECK(muse_elm_parse_support("0100\n41 00 98 18 80 01\n", 0x00, &m) == MUSE_ELM_SUPPORT && m == 0x98188001u);
    CHECK(muse_elm_parse_support("41 00 FF FF FF FF 00\n", 0x00, &m) == MUSE_ELM_SUPPORT && m == 0xFFFFFFFFu);
    CHECK(muse_elm_parse_support("41 20 80 01 80 01\n", 0x20, &m) == MUSE_ELM_SUPPORT && m == 0x80018001u);
    CHECK(muse_elm_parse_support("41 40 44 00 00 00\n", 0x40, &m) == MUSE_ELM_SUPPORT && m == 0x44000000u);
    CHECK(muse_elm_parse_support("41 00 00 00 00 00\n", 0x00, &m) == MUSE_ELM_SUPPORT && m == 0);
    m = 0x12345678u;
    CHECK(muse_elm_parse_support("41 20 80 01 80 01\n", 0x00, &m) == MUSE_ELM_NONE);   /* the wrong range */
    CHECK(muse_elm_parse_support("41 00 BE 1F A8 13\n", 0x20, &m) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_support("41 00 BE 1F A8\n", 0x00, &m) == MUSE_ELM_NONE);      /* cut short */
    CHECK(muse_elm_parse_support("41 00 BE 1F A8 1\n", 0x00, &m) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_support("7E8 06 41 00 BE 1F A8 13\n", 0x00, &m) == MUSE_ELM_NONE);   /* headers */
    CHECK(muse_elm_parse_support("0100\n", 0x00, &m) == MUSE_ELM_NONE);                /* the echo */
    CHECK(muse_elm_parse_support("41 0C 1A F8\n", 0x00, &m) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_support("NO DATA\n", 0x00, &m) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_support("SEARCHING...\nUNABLE TO CONNECT\n", 0x00, &m) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_support("NO DATA\n", 0x40, &m) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_support("", 0x00, &m) == MUSE_ELM_NONE);
    CHECK(m == 0x12345678u);
    /* A support reply is no reading, nor a reading a support reply. */
    check_nobody_else("41 00 BE 1F A8 13\n", -1);
    CHECK(muse_elm_parse_support("41 0D 58\n", 0x00, &m) == MUSE_ELM_NONE && m == 0x12345678u);

    /* Fragmented: SEARCHING..., then the chain 0100 / 0120 / 0140. */
    static const char stream[] = "SEARCHING...\r41 00 BE 1F A8 13 \r\r>"
                                 "\x00" "41 20 80 01 80 01\r\r>"
                                 "41 40 44 00 00 00\r\r>";
    static const uint32_t want[3] = { 0xBE1FA813u, 0x80018001u, 0x44000000u };
    for (size_t chunk = 1; chunk <= 24; chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[4][MUSE_ELM_RESP_MAX];
        int n = feed_chunks(&rx, stream, sizeof(stream) - 1, chunk, out, 4);
        CHECK(n == 3);
        for (int i = 0; i < n && i < 3; i++) {
            m = 0;
            CHECK(muse_elm_parse_support(out[i], 0x20 * i, &m) == MUSE_ELM_SUPPORT && m == want[i]);
        }
    }
}

static void test_pid_support(void)
{
    muse_elm_support_t s = { 0 };
    /* No scan: everything unknown, so the poll loop still asks. */
    for (int pid = 0; pid <= 0x70; pid++) {
        CHECK(muse_elm_pid_support(&s, pid) == MUSE_ELM_PID_UNKNOWN);
    }

    /* 01-20 read: BE1FA813. PID 01 is the top bit, PID 20 the bottom. */
    s.valid[0] = true;
    s.masks[0] = 0xBE1FA813u;
    static const int yes[] = { 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
                               0x11, 0x13, 0x15, 0x1C, 0x1F, 0x20 };
    for (int pid = 1; pid <= 0x20; pid++) {
        bool want = false;
        for (size_t i = 0; i < sizeof(yes) / sizeof(yes[0]); i++) {
            want |= yes[i] == pid;
        }
        CHECK(muse_elm_pid_support(&s, pid) == (want ? MUSE_ELM_PID_SUPPORTED : MUSE_ELM_PID_UNSUPPORTED));
    }
    CHECK(muse_elm_pid_support(&s, 0x0B) == MUSE_ELM_PID_UNSUPPORTED);   /* no MAP */
    /* PID 20 set but 0120 not read (it failed): 21-60 unknown. */
    CHECK(muse_elm_pid_support(&s, 0x21) == MUSE_ELM_PID_UNKNOWN);
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_UNKNOWN);

    /* PID 20 clear: nothing above 20, so 21-60 are unsupported unread. */
    s.masks[0] = 0xBE1FA812u;
    CHECK(muse_elm_pid_support(&s, 0x20) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x21) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x40) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x60) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x0C) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x61) == MUSE_ELM_PID_UNKNOWN);   /* past what's scanned */
    CHECK(muse_elm_pid_support(&s, 0x00) == MUSE_ELM_PID_UNKNOWN);

    /* The whole chain: 21-40 says PID 40 (bottom bit), 41-60 has 46. */
    s.masks[0] = 0xBE1FA813u;
    s.valid[1] = true;
    s.masks[1] = 0x80018001u;   /* 21, 30, 31, 40 */
    CHECK(muse_elm_pid_support(&s, 0x21) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x22) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x30) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x31) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x40) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_UNKNOWN);   /* 0140 not read */
    s.valid[2] = true;
    s.masks[2] = 0x44000000u;   /* 42 and 46 */
    CHECK(muse_elm_pid_support(&s, 0x42) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x45) == MUSE_ELM_PID_UNSUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_SUPPORTED);
    CHECK(muse_elm_pid_support(&s, 0x47) == MUSE_ELM_PID_UNSUPPORTED);
    s.masks[2] = 0x40000000u;   /* 42 only */
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_UNSUPPORTED);
    /* 21-40 says no PID 40: 41-60 unsupported without reading 0140. */
    s.valid[2] = false;
    s.masks[1] = 0x80018000u;
    CHECK(muse_elm_pid_support(&s, 0x46) == MUSE_ELM_PID_UNSUPPORTED);
}

static void test_secondary_cycle(void)
{
    static const int ORDER[MUSE_ELM_SECONDARIES] = { 0x05, 0x0F, 0x0E, 0x0B, 0x04, 0x11, 0x0D, 0x06, 0x07, 0x46 };
    for (int i = 0; i < MUSE_ELM_SECONDARIES; i++) {
        CHECK(MUSE_ELM_SECONDARY[i] == ORDER[i]);
    }

    /* Support unknown: all ten in order, coolant first, round and round. */
    muse_elm_support_t s = { 0 };
    unsigned cursor = 0;
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < MUSE_ELM_SECONDARIES; i++) {
            CHECK(muse_elm_next_secondary(&s, &cursor) == ORDER[i]);
        }
    }

    /* The poll loop: RPM on every even request, a secondary on every odd one. */
    cursor = 0;
    int seq[40];
    for (unsigned n = 0; n < 40; n++) {
        seq[n] = n % 2 ? muse_elm_next_secondary(&s, &cursor) : 0x0C;
    }
    for (int n = 0; n < 40; n++) {
        CHECK(seq[n] == (n % 2 ? ORDER[(n / 2) % MUSE_ELM_SECONDARIES] : 0x0C));
    }

    /* Scanned: BE1FA812 has no MAP (0B), and nothing past 20 (no 46). */
    s.valid[0] = true;
    s.masks[0] = 0xBE1FA812u;
    static const int scanned[] = { 0x05, 0x0F, 0x0E, 0x04, 0x11, 0x0D, 0x06, 0x07 };
    cursor = 0;
    for (int round = 0; round < 3; round++) {
        for (size_t i = 0; i < sizeof(scanned) / sizeof(scanned[0]); i++) {
            CHECK(muse_elm_next_secondary(&s, &cursor) == scanned[i]);
        }
    }

    /* No coolant either: the cycle starts at intake air. */
    s.masks[0] &= ~(1u << (31 - (0x05 - 1)));
    cursor = 0;
    CHECK(muse_elm_next_secondary(&s, &cursor) == 0x0F);

    /* 21-40 and 41-60 read, with 46: ambient joins at the end. */
    s.masks[0] = 0xBE1FA813u;
    s.valid[1] = true;
    s.masks[1] = 0x00000001u;
    s.valid[2] = true;
    s.masks[2] = 0x04000000u;
    cursor = 9;
    CHECK(muse_elm_next_secondary(&s, &cursor) == 0x46);
    CHECK(muse_elm_next_secondary(&s, &cursor) == 0x05);

    /* Nothing supported: -1, so the loop polls RPM alone. */
    s = (muse_elm_support_t){ 0 };
    s.valid[0] = true;
    s.masks[0] = 1u << (31 - (0x0C - 1));   /* only RPM */
    cursor = 3;
    CHECK(muse_elm_next_secondary(&s, &cursor) == -1);
    CHECK(muse_elm_next_secondary(&s, &cursor) == -1);
}

static void test_protocol(void)
{
    CHECK(muse_elm_parse_protocol("A6\n") == 6);
    CHECK(muse_elm_parse_protocol("A6") == 6);
    CHECK(muse_elm_parse_protocol("6\n") == 6);
    CHECK(muse_elm_parse_protocol("\nA7\n\n") == 7);
    CHECK(muse_elm_parse_protocol("a 3\n") == 3);
    CHECK(muse_elm_parse_protocol("A5\n") == 5);
    CHECK(muse_elm_parse_protocol("4\n") == 4);
    CHECK(muse_elm_parse_protocol("AC\n") == 12);
    CHECK(muse_elm_parse_protocol("AA\n") == 10);
    CHECK(muse_elm_parse_protocol("A\n") == 10);    /* protocol A set by hand */
    CHECK(muse_elm_parse_protocol("A0\n") == -1);   /* auto, nothing found yet */
    CHECK(muse_elm_parse_protocol("0\n") == -1);
    CHECK(muse_elm_parse_protocol("?\n") == -1);
    CHECK(muse_elm_parse_protocol("") == -1);
    CHECK(muse_elm_parse_protocol("A6 X\n") == -1);
    CHECK(muse_elm_parse_protocol("A67\n") == -1);
    CHECK(muse_elm_parse_protocol("ATDPN\n") == -1);

    char name[24];
    static const struct { int n; const char *name; } NAMES[] = {
        { 6, "CAN 11/500" }, { 7, "CAN 29/500" }, { 3, "ISO 9141-2" }, { 4, "KWP2000 5-baud" },
        { 5, "KWP2000 fast" }, { 8, "CAN 11/250" }, { 9, "CAN 29/250" }, { 1, "SAE J1850 PWM" },
        { 2, "SAE J1850 VPW" }, { 10, "SAE J1939" }, { 13, "proto 13" }, { 0, "proto 0" }, { -1, "proto -1" },
    };
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        muse_elm_protocol_name(NAMES[i].n, name, sizeof(name));
        CHECK(strcmp(name, NAMES[i].name) == 0);
    }
    char tiny[4];
    muse_elm_protocol_name(6, tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "CAN") == 0);
    muse_elm_protocol_name(6, tiny, 0);   /* no room: nothing written */
    CHECK(strcmp(tiny, "CAN") == 0);
    /* The name fits the snapshot's 20 bytes. */
    for (int n = -1; n <= 16; n++) {
        muse_elm_protocol_name(n, name, sizeof(name));
        CHECK(strlen(name) < 20);
    }
}

int main(void)
{
    test_fragmented_atz();
    test_searching_then_frames();
    test_parse_cases();
    test_overflow();
    test_trigger();
    test_coolant_frames();
    test_coolant_parse_cases();
    test_coolant_trigger();
    test_warmup_trigger();
    test_extra_parse_cases();
    test_extra_frames();
    test_support_parse();
    test_pid_support();
    test_secondary_cycle();
    test_protocol();
    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_elm\n");
    return 0;
}
