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

/* The OBD reactions' ELM327 parser and triggers (muse_obd_elm.c: DID 0x2612's
 * VTEC flags against real S2000 replies and the held VTEC level, the coolant
 * warning and the warm-up thumbs-up on PID 05), and the car data
 * cards' side (the PID support scan, the cards' PIDs, the poll pipeline:
 * the scheduler, the batch parser, the link stats and the ATST policy, and
 * ATDPN), fed the way the LELink^2 adapter's FFE1 notifications cut
 * its replies. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "muse_obd_elm.h"
#include "vtec_did_frames.h"
#include "did_cards_frames.h"

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

/* A reply through the byte feed, as it arrives: '\r' to '\n', no '>'. */
static const char *fed(const char *text)
{
    static muse_elm_rx_t rx;
    muse_elm_rx_reset(&rx);
    for (size_t i = 0; text[i]; i++) {
        muse_elm_feed(&rx, (uint8_t)text[i]);
    }
    return rx.resp;
}

static void test_vtec_did(void)
{
    char text[512];
    muse_vtec_did_t v;

    /* The research report's 2007 "VTEC on" sample: byte 21 = 0x01. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    CHECK(!strncmp(text, "039\r0:622612FFFFC0\r1:001E00C25F4419\r", 36));
    CHECK(strlen(fed(text)) < MUSE_ELM_RESP_MAX - 1);   /* fits, nothing cut */
    memset(&v, 0xAA, sizeof(v));
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_VTEC_DID);
    CHECK(v.sol && !v.psw && v.b23 == 0x01);

    /* Another "on" sample, and a typical "off" one: byte 21 = 0x02. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON, S2000_FRAMES, false);
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_VTEC_DID && v.sol && !v.psw && v.b23 == 1);
    elm_from_frames(text, sizeof(text), S2000_2007_OFF, S2000_FRAMES, false);
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_VTEC_DID && !v.sol && v.psw && v.b23 == 0);

    /* With spaces (ATS1) it reads the same, and still fits. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, true);
    CHECK(strlen(fed(text)) < MUSE_ELM_RESP_MAX - 1);
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_VTEC_DID && v.sol && !v.psw);

    /* In every notification size the adapter might cut it into. */
    elm_from_frames(text, sizeof(text), S2000_2007_OFF, S2000_FRAMES, false);
    for (size_t chunk = 1; chunk <= 40; chunk++) {
        muse_elm_rx_t rx;
        muse_elm_rx_reset(&rx);
        char out[2][MUSE_ELM_RESP_MAX];
        CHECK(feed_chunks(&rx, text, strlen(text), chunk, out, 2) == 1);
        CHECK(muse_elm_parse_vtec_did(out[0], &v) == MUSE_ELM_VTEC_DID && !v.sol && v.psw);
    }

    /* Truncated: frames missing, so fewer bytes than the length line says. */
    muse_vtec_did_t keep = { .sol = true, .psw = true, .b23 = 0x5A };
    for (int frames = 1; frames < S2000_FRAMES; frames++) {
        elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, frames, false);
        v = keep;
        CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
        CHECK(v.sol && v.psw && v.b23 == 0x5A);   /* untouched: never a stale "on" */
    }
    /* Cut mid-frame. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    char *last = strstr(text, "8:");
    strcpy(last + 2, "00\r\r>");
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);

    /* Wrong prefix: another DID's reply, or a positive reply to another service. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    text[9] = '1';   /* 0:622611... */
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    text[6] = '5';   /* 0:522612... */
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
    /* Another length (a different block layout): not trusted. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    text[2] = '8';   /* 038 */
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
    /* Frames out of order, or no length line. */
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    char *two = strstr(text, "\r2:");
    two[1] = '3';
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    CHECK(muse_elm_parse_vtec_did(fed(text + 4), &v) == MUSE_ELM_NONE);
    /* A short single frame that starts right. */
    CHECK(muse_elm_parse_vtec_did("622612FFFF\n", &v) == MUSE_ELM_NONE);

    /* Refused, silent, broken. */
    CHECK(muse_elm_parse_vtec_did("7F2231\n", &v) == MUSE_ELM_NEGATIVE);   /* requestOutOfRange */
    CHECK(muse_elm_parse_vtec_did("7F 22 11\n", &v) == MUSE_ELM_NEGATIVE);
    CHECK(muse_elm_parse_vtec_did("NO DATA\n", &v) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_vtec_did("CAN ERROR\n", &v) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_vtec_did("?\n", &v) == MUSE_ELM_ERROR);
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    strcpy(strstr(text, "8:"), "<RX ERROR\r\r>");
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_ERROR);
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    strcpy(strstr(text, "8:"), "BUFFER FULL\r\r>");
    CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_vtec_did("", &v) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_vtec_did("OK\n", &v) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_vtec_did("41 0C 1A F8\n", &v) == MUSE_ELM_NONE);
    /* The DID reply is nobody else's either. */
    int rpm = -1;
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    CHECK(muse_elm_parse_rpm(fed(text), &rpm) == MUSE_ELM_NONE && rpm == -1);
}

static void test_vtec_engaged(void)
{
    /* Held exactly while the solenoid reads on, freshly. */
    CHECK(muse_vtec_engaged(true, true, 0));
    CHECK(muse_vtec_engaged(true, true, MUSE_VTEC_FRESH_MS));
    CHECK(!muse_vtec_engaged(true, true, MUSE_VTEC_FRESH_MS + 1));   /* stale */
    CHECK(!muse_vtec_engaged(true, false, 0));                       /* a fresh "off" */
    CHECK(!muse_vtec_engaged(false, true, 0));                       /* no reading */
    CHECK(!muse_vtec_engaged(false, false, UINT32_MAX));
    CHECK(MUSE_VTEC_FRESH_MS == 3000u);

    /* Ticks through a pull: held across every on tick, not a one-shot; off
     * the tick an "off" lands; on again at once (no cooldown). */
    static const struct { bool valid, sol; uint32_t age; bool held; } TICKS[] = {
        { false, false, UINT32_MAX, false },
        { true, false, 100, false },
        { true, true, 50, true }, { true, true, 300, true }, { true, true, 2200, true },
        { true, true, 60, true }, { true, true, 2900, true },
        { true, false, 40, false },                      /* off */
        { true, true, 30, true },                        /* straight back on */
        { true, true, 3001, false },                     /* went stale: down */
        { true, true, 10, true },
        { false, false, UINT32_MAX, false },             /* the link dropped */
    };
    for (size_t i = 0; i < sizeof(TICKS) / sizeof(TICKS[0]); i++) {
        CHECK(muse_vtec_engaged(TICKS[i].valid, TICKS[i].sol, TICKS[i].age) == TICKS[i].held);
    }
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
    /* OBD-MORE-PIDS-TASK's one-byte PIDs (J1979: A * 100 / 255 %, A kPa). */
    { "pedal_d", 0x49, muse_elm_parse_pedal_d, MUSE_ELM_PEDAL_D,
      { 0x00, 0x80, 0xFF, 0x33, 0x1A }, { 0, 50, 100, 20, 10 } },
    { "pedal_e", 0x4A, muse_elm_parse_pedal_e, MUSE_ELM_PEDAL_E,
      { 0x00, 0x80, 0xFF, 0x33, 0x1A }, { 0, 50, 100, 20, 10 } },
    { "baro", 0x33, muse_elm_parse_baro, MUSE_ELM_BARO,
      { 0x00, 0x65, 0xFF, 0x63, 0x5A }, { 0, 101, 255, 99, 90 } },
    { "fuel", 0x2F, muse_elm_parse_fuel, MUSE_ELM_FUEL,
      { 0x00, 0x80, 0xFF, 0x33, 0x1A }, { 0, 50, 100, 20, 10 } },
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
    CHECK(muse_elm_parse_afr(resp, &v) == MUSE_ELM_NONE && v == -999);
    CHECK(muse_elm_parse_module_v(resp, &v) == MUSE_ELM_NONE && v == -999);
}

/* OBD-MORE-PIDS-TASK's wide PIDs against worked J1979 frames: 34, the
 * wideband lambda (256A + B) / 32768, its four bytes all there (C and D,
 * the sensor's, unused), and 42, the module voltage (256A + B) / 1000. */
static void test_more_pids_wide(void)
{
    static const struct {
        const char *resp;
        int raw, lambda_x1000, afr_x10;
    } AFR[] = {
        { "41 34 80 00 80 00\n", 0x8000, 1000, 147 },   /* stoich: lambda 1.000, AFR 14.7 */
        { "41 34 00 00 00 00\n", 0x0000, 0, 0 },        /* the bottom byte extreme */
        { "41 34 FF FF FF FF\n", 0xFFFF, 2000, 294 },   /* the top: 1.99997, 29.39955 */
        { "41346CCC7FFF\n", 0x6CCC, 850, 125 },         /* rich: 0.84998, 12.4946; ATS0 */
        { "SEARCHING...\n41 34 8C CD 80 00\n", 0x8CCD, 1100, 162 },   /* lean: 1.10001, 16.1701 */
        { "0134\n41 34 7a e1 80 00\n", 0x7AE1, 960, 141 },            /* the echo; lower case */
    };
    for (size_t i = 0; i < sizeof(AFR) / sizeof(AFR[0]); i++) {
        int raw = -999;
        CHECK(muse_elm_parse_afr(AFR[i].resp, &raw) == MUSE_ELM_AFR && raw == AFR[i].raw);
        CHECK(muse_elm_lambda_x1000(raw) == AFR[i].lambda_x1000);
        CHECK(muse_elm_afr_x10(raw) == AFR[i].afr_x10);
        int v = -999;
        CHECK(muse_elm_parse_module_v(AFR[i].resp, &v) == MUSE_ELM_NONE && v == -999);
        CHECK(muse_elm_parse_rpm(AFR[i].resp, &v) == MUSE_ELM_NONE && v == -999);
    }
    /* A miss is never a value: cut short, garbled, refused, someone else's. */
    static const char *const AFR_NONE[] = {
        "41 34 80 00\n", "41 34 80 00 80\n", "41 34 80 0G 80 00\n", "41 24 80 00 80 00\n",
        "7E8 06 41 34 80 00 80 00\n", "0134\n", "OK\n", "",
    };
    for (size_t i = 0; i < sizeof(AFR_NONE) / sizeof(AFR_NONE[0]); i++) {
        int raw = -999;
        CHECK(muse_elm_parse_afr(AFR_NONE[i], &raw) == MUSE_ELM_NONE && raw == -999);
    }
    int raw = -999;
    CHECK(muse_elm_parse_afr("NO DATA\n", &raw) == MUSE_ELM_NO_DATA && raw == -999);
    CHECK(muse_elm_parse_afr("?\n", &raw) == MUSE_ELM_ERROR && raw == -999);

    static const struct {
        const char *resp;
        int mv;
    } VOLTS[] = {
        { "41 42 36 15\n", 13845 },   /* 13.845 V, running */
        { "41 42 30 D4\n", 12500 },   /* 12.500 V, key on */
        { "41 42 00 00\n", 0 },
        { "41 42 FF FF\n", 65535 },   /* the top: 65.535 V */
        { "41423615\n", 13845 },
    };
    for (size_t i = 0; i < sizeof(VOLTS) / sizeof(VOLTS[0]); i++) {
        int mv = -999;
        CHECK(muse_elm_parse_module_v(VOLTS[i].resp, &mv) == MUSE_ELM_MODULE_V && mv == VOLTS[i].mv);
        CHECK(muse_elm_parse_afr(VOLTS[i].resp, &raw) == MUSE_ELM_NONE);
    }
    int mv = -999;
    CHECK(muse_elm_parse_module_v("41 42 36\n", &mv) == MUSE_ELM_NONE && mv == -999);   /* cut short */
    CHECK(muse_elm_parse_module_v("41 42 3G 15\n", &mv) == MUSE_ELM_NONE && mv == -999);
    CHECK(muse_elm_parse_module_v("NO DATA\n", &mv) == MUSE_ELM_NO_DATA && mv == -999);

    /* All six ride a batch like any Mode 01 PID: one J1979 message, the
     * wide ones' bytes counted (sixteen bytes, three frames). */
    static const uint8_t pids[6] = { 0x0C, 0x34, 0x42, 0x49, 0x2F, 0x33 };
    int values[6];
    bool got[6];
    CHECK(muse_elm_parse_batch("010\n0:410C1AF83480\n1:00800042361549\n2:332F8000000000\n", pids, 6, values, got)
          == MUSE_ELM_BATCH);
    CHECK(got[0] && values[0] == 1726);
    CHECK(got[1] && values[1] == 0x8000);
    CHECK(got[2] && values[2] == 13845);
    CHECK(got[3] && values[3] == 20);
    CHECK(got[4] && values[4] == 50);
    CHECK(!got[5]);   /* baro left out: its miss only */
    /* AFR cut short at the message's end: a miss, never a value. */
    CHECK(muse_elm_parse_batch("410D3C348000\n", (const uint8_t[]){ 0x0D, 0x34 }, 2, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && !got[1]);
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


static void test_batch_parse(void)
{
    /* A full six-PID reply, as the feed hands it over (\n between
     * lines): every value, converted like the single parsers. */
    static const uint8_t pids[6] = { 0x0C, 0x0B, 0x11, 0x04, 0x0D, 0x06 };
    static const char *full =
        "41 0C 1A F8\n41 0B 62\n41 11 33\n41 04 66\n41 0D 3C\n41 06 90\n\n>";
    int values[6];
    bool got[6];
    CHECK(muse_elm_parse_batch(full, pids, 6, values, got) == MUSE_ELM_BATCH);
    for (int i = 0; i < 6; i++) {
        CHECK(got[i]);
    }
    CHECK(values[0] == 1726);         /* (0x1A * 256 + 0xF8) / 4 */
    CHECK(values[1] == 0x62);         /* MAP, kPa as read */
    CHECK(values[2] == 0x33 * 100 / 255);
    CHECK(values[3] == 0x66 * 100 / 255);
    CHECK(values[4] == 0x3C);
    CHECK(values[5] == (0x90 - 128) * 100 / 128);
    /* The batch values agree with the single-PID parsers on the same reply. */
    int one;
    CHECK(muse_elm_parse_rpm(full, &one) == MUSE_ELM_RPM && one == values[0]);
    CHECK(muse_elm_parse_map(full, &one) == MUSE_ELM_MAP && one == values[1]);
    CHECK(muse_elm_parse_stft(full, &one) == MUSE_ELM_STFT && one == values[5]);

    /* A partial reply: speed's line is missing, the rest still read. */
    static const char *partial = "41 0C 1A F8\n41 0B 62\n41 11 33\n41 04 66\n41 06 90\n\n>";
    CHECK(muse_elm_parse_batch(partial, pids, 6, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && got[1] && got[2] && got[3] && !got[4] && got[5]);

    /* A malformed line is a miss for its PID only. */
    static const char *garbled = "41 0C 1A F8\n41 0B ZZ\n41 11 33\n41 04 66\n41 0D 3C\n41 06 90\n\n>";
    CHECK(muse_elm_parse_batch(garbled, pids, 6, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && !got[1] && got[2] && got[3] && got[4] && got[5]);

    /* The whole batch refused: NO DATA, or the ELM's "?", nothing read. */
    CHECK(muse_elm_parse_batch("NO DATA\r\r>", pids, 6, values, got) == MUSE_ELM_NO_DATA);
    for (int i = 0; i < 6; i++) {
        CHECK(!got[i]);
    }
    CHECK(muse_elm_parse_batch("?\r\r>", pids, 6, values, got) == MUSE_ELM_ERROR);
    /* Nothing that answers and nothing that refuses: NONE. */
    CHECK(muse_elm_parse_batch("GARBAGE\r\r>", pids, 6, values, got) == MUSE_ELM_NONE);

    /* What a CAN ECU really sends (the ELM327 datasheet's "Multiple PID
     * Requests", verbatim): one message, 41 once, then each PID and its
     * data; ten bytes, so a length line and two ISO-TP frames, the last
     * padded. Asked 01 04 05 0B 0C. */
    static const uint8_t ds_pids[4] = { 0x04, 0x05, 0x0B, 0x0C };
    static const char *const DS[] = {
        "00A\n0: 41 04 3F 05 44 0B\n1: 21 0C 17 B8 00 00 00\n",
        "00A\n0: 41 0B 21 04 3F 0C\n1: 17 B8 05 44 00 00 00\n",   /* answered in another order */
        "00A\n0:41043F05440B\n1:210C17B8000000\n",                /* ATS0: no spaces */
        "SEARCHING...\n00A\n0:41043F05440B\n1:210C17B8000000\n\n>",
    };
    for (size_t k = 0; k < sizeof(DS) / sizeof(DS[0]); k++) {
        CHECK(muse_elm_parse_batch(DS[k], ds_pids, 4, values, got) == MUSE_ELM_BATCH);
        CHECK(got[0] && got[1] && got[2] && got[3]);
        CHECK(values[0] == 0x3F * 100 / 255);   /* load 24% */
        CHECK(values[1] == 0x44 - 40);          /* coolant 28 C */
        CHECK(values[2] == 0x21);               /* MAP 33 kPa */
        CHECK(values[3] == 0x17B8 / 4);         /* 1518 rpm */
    }
    /* A short answer fits one frame: a single line, no length. */
    static const uint8_t two[3] = { 0x0C, 0x0D, 0x11 };
    CHECK(muse_elm_parse_batch("410C1AF80D3C\n", two, 3, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && values[0] == 1726 && got[1] && values[1] == 0x3C && !got[2]);   /* 11 left out: its miss only */
    /* Two ECUs answering apart: each message counts, the first answer wins. */
    CHECK(muse_elm_parse_batch("410D3C\n410C1AF80D50\n", two, 3, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && values[0] == 1726 && got[1] && values[1] == 0x3C && !got[2]);
    /* A frame missing, or out of order: that message is lost, nothing guessed. */
    CHECK(muse_elm_parse_batch("00A\n0:41043F05440B\n", ds_pids, 4, values, got) == MUSE_ELM_NONE);
    CHECK(!got[0] && !got[1] && !got[2] && !got[3]);
    CHECK(muse_elm_parse_batch("00A\n1:210C17B8000000\n0:41043F05440B\n", ds_pids, 4, values, got) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_batch("0:41043F05440B\n1:210C17B8000000\n", ds_pids, 4, values, got) == MUSE_ELM_NONE);
    /* A PID that wasn't asked for ends the walk: its length can't be trusted. */
    CHECK(muse_elm_parse_batch("410C1AF8330D3C\n", two, 3, values, got) == MUSE_ELM_BATCH);
    CHECK(got[0] && !got[1]);
    /* Cut short inside a PID's data: what came before it still reads. */
    CHECK(muse_elm_parse_batch("410D3C0C1A\n", two, 3, values, got) == MUSE_ELM_BATCH);
    CHECK(!got[0] && got[1]);
    /* A message that isn't a Mode 01 answer reads nothing. */
    CHECK(muse_elm_parse_batch("7F0112\n", two, 3, values, got) == MUSE_ELM_NONE);
}

static bool req_has(const muse_sched_req_t *r, int pid)
{
    for (int i = 0; i < r->n; i++) {
        if (r->pids[i] == pid) {
            return true;
        }
    }
    return false;
}

/* Serves one pick at t: every PID in it read a value. */
static void serve(muse_sched_t *s, const muse_sched_req_t *r, uint32_t t)
{
    for (int i = 0; i < r->n; i++) {
        muse_sched_done_pid(s, r->pids[i], true, true, t);
    }
}

/* Picks and serves at t until only the filler is left (RPM alone), adding
 * each PID asked to asks[]; the first pick in *first. Returns the picks. */
static int sweep(muse_sched_t *s, uint32_t t, int asks[256], muse_sched_req_t *first)
{
    muse_sched_req_t r;
    for (int k = 0; k < 16; k++) {
        muse_sched_pick(s, t, &r);
        if (k == 0 && first) {
            *first = r;
        }
        if (r.did || (r.n == 1 && r.pids[0] == 0x0C)) {
            if (!r.did) {
                serve(s, &r, t);
            }
            return k + 1;
        }
        for (int i = 0; i < r.n; i++) {
            asks[r.pids[i]]++;
        }
        serve(s, &r, t);
    }
    return -1;
}

/* The schedule's secondaries by tier (OBD-PIPELINE-TASK, OBD-MORE-PIDS-TASK). */
static const uint8_t FAST[] = { 0x0B, 0x11, 0x04, 0x0D, 0x34, 0x49, 0x4A };
static const uint8_t MID[] = { 0x06, 0x07, 0x0E, 0x2F };
static const uint8_t SLOW[] = { 0x05, 0x0F, 0x46, 0x42, 0x33 };
#define TIER_N(t) (int)(sizeof(t) / sizeof(t[0]))

static void test_scheduler(void)
{
    muse_elm_support_t sup = { 0 };   /* nothing scanned: everything unknown, all kept */
    muse_sched_t s;
    muse_sched_req_t r;

    /* A new connection sweeps: RPM leads a full batch of what's due, then
     * the rest, each signal asked once at the same instant (dedupe), until
     * only the filler is left. */
    muse_sched_init(&s, &sup, true, 1000);
    int asks[256] = { 0 };
    /* Sixteen secondaries and RPM: RPM and five, six, then the last five
     * with RPM riding along, then the filler alone. */
    for (int k = 0; k < 4; k++) {
        muse_sched_pick(&s, 1000, &r);
        CHECK(!r.did);
        if (k == 0) {
            CHECK(r.n == MUSE_ELM_BATCH_MAX && r.pids[0] == 0x0C);
            CHECK(req_has(&r, 0x0B) && req_has(&r, 0x11));   /* the fast tier rides first */
        }
        if (k < 3) {
            CHECK(r.n == MUSE_ELM_BATCH_MAX);
        }
        for (int i = 0; i < r.n; i++) {
            for (int j = i + 1; j < r.n; j++) {
                CHECK(r.pids[i] != r.pids[j]);   /* never twice in one request */
            }
            asks[r.pids[i]]++;
        }
        serve(&s, &r, 1000);
    }
    CHECK(r.n == 1 && r.pids[0] == 0x0C);   /* the last of the sweep: RPM alone */
    for (int pid = 0x01; pid <= 0x4A; pid++) {
        if (pid == 0x0C) {
            continue;
        }
        CHECK(asks[pid] <= 1);   /* no secondary asked twice in the sweep */
    }
    for (int i = 0; i < TIER_N(FAST); i++) CHECK(asks[FAST[i]] == 1);
    for (int i = 0; i < TIER_N(MID); i++) CHECK(asks[MID[i]] == 1);
    for (int i = 0; i < TIER_N(SLOW); i++) CHECK(asks[SLOW[i]] == 1);

    /* The DID's floor: never inside 1 s of the probe, however overdue;
     * eligible after it, picked once it is the most overdue, and the floor
     * runs from that read too. */
    for (uint32_t t = 1100; t < 2000; t += 100) {
        muse_sched_pick(&s, t, &r);
        CHECK(!r.did);
        serve(&s, &r, t);
    }
    bool saw_did = false;
    for (uint32_t t = 2000; t <= 2400 && !saw_did; t += 50) {
        muse_sched_pick(&s, t, &r);
        if (r.did) {
            saw_did = true;
            CHECK(r.which == MUSE_DID_2612);   /* the only one this connection reads */
            muse_sched_done_did(&s, (muse_did_t)r.which, t);
            break;
        }
        serve(&s, &r, t);
    }
    CHECK(saw_did);
    muse_sched_pick(&s, 2500, &r);
    CHECK(!r.did);

    /* Tiers by target, each PID at its own (OBD-MORE-PIDS-TASK: AFR and
     * the pedals fast, fuel mid, volts and baro slow). All swept at 0. */
    muse_sched_init(&s, &sup, false, 0);
    int t_asks[256] = { 0 };
    CHECK(sweep(&s, 0, t_asks, NULL) > 0);
    /* +1500 ms: only the fast tier (1 s) is due; all of it is asked. */
    memset(t_asks, 0, sizeof(t_asks));
    CHECK(sweep(&s, 1500, t_asks, NULL) > 0);
    for (int i = 0; i < TIER_N(FAST); i++) CHECK(t_asks[FAST[i]] == 1);
    for (int i = 0; i < TIER_N(MID); i++) CHECK(t_asks[MID[i]] == 0);
    for (int i = 0; i < TIER_N(SLOW); i++) CHECK(t_asks[SLOW[i]] == 0);
    /* +2100 ms: the mid tier (2 s) is due; the fast one was just read. */
    memset(t_asks, 0, sizeof(t_asks));
    CHECK(sweep(&s, 2100, t_asks, NULL) > 0);
    for (int i = 0; i < TIER_N(FAST); i++) CHECK(t_asks[FAST[i]] == 0);
    for (int i = 0; i < TIER_N(MID); i++) CHECK(t_asks[MID[i]] == 1);
    for (int i = 0; i < TIER_N(SLOW); i++) CHECK(t_asks[SLOW[i]] == 0);
    /* +3050 ms: fast (550 ms over) and slow (50 ms over) are both due: the
     * more overdue fast tier fills the first request, then the slow one. */
    memset(t_asks, 0, sizeof(t_asks));
    muse_sched_req_t first;
    CHECK(sweep(&s, 3050, t_asks, &first) > 0);
    for (int i = 0; i < TIER_N(FAST); i++) CHECK(t_asks[FAST[i]] == 1);
    for (int i = 0; i < TIER_N(MID); i++) CHECK(t_asks[MID[i]] == 0);
    for (int i = 0; i < TIER_N(SLOW); i++) CHECK(t_asks[SLOW[i]] == 1);
    for (int i = 0; i < TIER_N(SLOW); i++) CHECK(!req_has(&first, SLOW[i]));
    /* Misses evict the new PIDs as any other: twice against a live ECU. */
    muse_sched_init(&s, &sup, false, 0);
    muse_sched_done_pid(&s, 0x34, false, true, 0);
    muse_sched_done_pid(&s, 0x34, false, true, 0);
    muse_sched_done_pid(&s, 0x42, false, false, 0);   /* silent: proves nothing */
    muse_sched_done_pid(&s, 0x42, false, false, 0);
    memset(t_asks, 0, sizeof(t_asks));
    CHECK(sweep(&s, 10000, t_asks, NULL) > 0);
    CHECK(t_asks[0x34] == 0 && t_asks[0x42] == 1 && t_asks[0x49] == 1);

    /* Misses: twice against a live ECU and a PID leaves the schedule;
     * against a silent one it stays. RPM never leaves. */
    muse_sched_init(&s, &sup, false, 0);
    muse_sched_done_pid(&s, 0x0B, false, true, 0);
    muse_sched_done_pid(&s, 0x0B, false, true, 0);
    bool map_asked = false;
    for (int k = 0; k < 40 && !map_asked; k++) {
        muse_sched_pick(&s, 10000, &r);
        map_asked = req_has(&r, 0x0B);
        serve(&s, &r, 10000);
    }
    CHECK(!map_asked);
    muse_sched_done_pid(&s, 0x11, false, false, 0);   /* a silent ECU proves nothing */
    muse_sched_done_pid(&s, 0x11, false, false, 0);
    bool throttle_asked = false;
    for (int k = 0; k < 40 && !throttle_asked; k++) {
        muse_sched_pick(&s, 20000, &r);
        throttle_asked = req_has(&r, 0x11);
        serve(&s, &r, 20000);
    }
    CHECK(throttle_asked);
    muse_sched_done_pid(&s, 0x0C, false, true, 0);
    muse_sched_done_pid(&s, 0x0C, false, true, 0);
    muse_sched_pick(&s, 30000, &r);
    CHECK(req_has(&r, 0x0C));

    /* The latch: single-PID picks, however much is due. */
    muse_sched_init(&s, &sup, false, 0);
    muse_sched_latch_batch_off(&s);
    muse_sched_pick(&s, 0, &r);
    CHECK(!r.did && r.n == 1 && r.pids[0] == 0x0C);

    /* A scan-proven unsupported PID is never picked. */
    sup.valid[0] = true;
    sup.masks[0] = 0xBE1FA812u;   /* no MAP; the chain bit ends the ranges */
    muse_sched_init(&s, &sup, false, 0);
    for (int k = 0; k < 12; k++) {
        muse_sched_pick(&s, (uint32_t)k * 1000, &r);
        CHECK(!req_has(&r, 0x0B) && !req_has(&r, 0x46));
        /* 21-60 proven unsupported too: none of the new PIDs either. */
        CHECK(!req_has(&r, 0x2F) && !req_has(&r, 0x33) && !req_has(&r, 0x34) && !req_has(&r, 0x42)
              && !req_has(&r, 0x49) && !req_has(&r, 0x4A));
        serve(&s, &r, (uint32_t)k * 1000);
    }
    /* Derek's 2008 S2000 (its masks, read live 2026-10-22): all six new
     * PIDs supported, so all six are scheduled. */
    sup.valid[0] = sup.valid[1] = sup.valid[2] = true;
    sup.masks[0] = 0xBE3EA813u;
    sup.masks[1] = 0x8007F011u;
    sup.masks[2] = 0xFAD00000u;
    static const uint8_t NEW[] = { 0x34, 0x49, 0x4A, 0x2F, 0x42, 0x33 };
    for (int i = 0; i < 6; i++) {
        CHECK(muse_elm_pid_support(&sup, NEW[i]) == MUSE_ELM_PID_SUPPORTED);
    }
    muse_sched_init(&s, &sup, false, 0);
    memset(t_asks, 0, sizeof(t_asks));
    CHECK(sweep(&s, 0, t_asks, NULL) > 0);
    for (int i = 0; i < 6; i++) {
        CHECK(t_asks[NEW[i]] == 1);
    }
    /* Each one alone proven unsupported (its bit cleared) leaves the schedule. */
    for (int i = 0; i < 6; i++) {
        muse_elm_support_t one = sup;
        int pid = NEW[i];
        one.masks[(pid - 1) / 32] &= ~(1u << (31 - (pid - 1) % 32));
        CHECK(muse_elm_pid_support(&one, pid) == MUSE_ELM_PID_UNSUPPORTED);
        muse_sched_init(&s, &one, false, 0);
        memset(t_asks, 0, sizeof(t_asks));
        CHECK(sweep(&s, 0, t_asks, NULL) > 0);
        CHECK(t_asks[pid] == 0);
    }
}

static void test_link_stats(void)
{
    muse_link_stats_t st;
    muse_link_stats_reset(&st);
    CHECK(st.exchanges == 0 && muse_link_stats_rate_x100(&st) == 0 && muse_link_stats_batch_pct(&st) == -1);
    /* Four exchanges 100 ms apart, two of them batches of two readings. */
    muse_link_stats_exchange(&st, 1000, 40, 2, true);
    muse_link_stats_exchange(&st, 1100, 50, 2, true);
    muse_link_stats_exchange(&st, 1200, 60, 1, false);
    muse_link_stats_exchange(&st, 1300, 70, 1, false);
    CHECK(st.exchanges == 4 && st.last_rtt_ms == 70);
    CHECK(st.readings == 6 && st.batch_readings == 4);
    CHECK(muse_link_stats_batch_pct(&st) == 66);   /* 4 of 6 readings */
    CHECK(muse_link_stats_rate_x100(&st) == 1000); /* 3 spans of 100 ms: 10.00/s */
    muse_link_stats_timeout(&st);
    muse_link_stats_timeout(&st);
    CHECK(st.timeouts == 2 && st.exchanges == 4);
    /* The ring wraps: 20 exchanges 10 ms apart read as 100.00/s. */
    muse_link_stats_reset(&st);
    for (int i = 0; i < 20; i++) {
        muse_link_stats_exchange(&st, (uint32_t)i * 10, 5, 0, false);
    }
    CHECK(st.exchanges == 20 && muse_link_stats_rate_x100(&st) == 10000);
}

static void test_tune(void)
{
    muse_tune_t t;
    muse_tune_init(&t);
    CHECK(t.atst == MUSE_TUNE_ATST_MAX);
    /* The trim lands after MUSE_TUNE_MEASURE valued exchanges: twice the
     * mean round trip (40 ms of it is 80 ms, ATST 0x14). */
    for (int i = 0; i < MUSE_TUNE_MEASURE - 1; i++) {
        CHECK(muse_tune_observe_value(&t, 40) == -1);
    }
    CHECK(muse_tune_observe_value(&t, 40) == 0x14);
    CHECK(t.atst == 0x14);
    CHECK(muse_tune_observe_value(&t, 40) == -1);   /* the trim happens once */
    /* Three answerless in a row step it back a notch; a value resets the run. */
    CHECK(muse_tune_observe_timeout(&t) == -1);
    CHECK(muse_tune_observe_timeout(&t) == -1);
    CHECK(muse_tune_observe_value(&t, 40) == -1);
    CHECK(muse_tune_observe_timeout(&t) == -1);
    CHECK(muse_tune_observe_timeout(&t) == -1);
    CHECK(muse_tune_observe_timeout(&t) == 0x14 + MUSE_TUNE_ATST_STEP);
    /* The steps stop at the ELM's default: bounded. */
    for (int i = 0; i < 40; i++) {
        muse_tune_observe_timeout(&t);
    }
    CHECK(t.atst == MUSE_TUNE_ATST_MAX);
    CHECK(muse_tune_observe_timeout(&t) == -1);
    /* A slow car trims to nothing: the default stands. */
    muse_tune_init(&t);
    for (int i = 0; i < MUSE_TUNE_MEASURE - 1; i++) {
        CHECK(muse_tune_observe_value(&t, 300) == -1);
    }
    CHECK(muse_tune_observe_value(&t, 300) == -1);
    CHECK(t.atst == MUSE_TUNE_ATST_MAX);
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

/* ---- OBD-DID-CARDS-TASK: DIDs 0x2610 and 0x2663 ---- */

/* A UDS reply's bytes as the ELM prints them with headers off: the length
 * line, "0:" with six bytes, then seven a frame, the last padded with 55
 * as the S2000's ECU pads. */
static char *elm_bytes(char *out, size_t cap, const uint8_t *b, size_t n)
{
    /* At most 128 bytes: 19 frames, <= 17 chars each, plus the length
     * line, the prompt and the NUL. */
    size_t frames = n <= 6 ? 1 : 1 + (n - 6 + 6) / 7;
    if (n > 128 || cap < 4 + frames * 17 + 3) {
        abort();
    }
    size_t at = 0;
    at += (size_t)snprintf(out + at, cap - at, "%03X\r", (unsigned)n);
    size_t i = 0;
    for (int f = 0; i < n && at < cap; f++) {
        at += (size_t)snprintf(out + at, cap - at, "%X:", (unsigned)(f & 0xF));
        for (int k = 0; k < (f == 0 ? 6 : 7) && at < cap; k++, i++) {
            at += (size_t)snprintf(out + at, cap - at, "%02X", i < n ? b[i] : 0x55);
        }
        at += (size_t)snprintf(out + at, cap - at, "\r");
    }
    if (at < cap) {
        snprintf(out + at, cap - at, "\r>");
    }
    return out;
}

/* A 62 26 xx reply of n bytes in all, its data zeroed. */
static void did_block(uint8_t *b, int did, size_t n)
{
    memset(b, 0, n);
    b[0] = 0x62;
    b[1] = (uint8_t)(did >> 8);
    b[2] = (uint8_t)did;
}

static void test_did_cards_parse(void)
{
    char text[512];
    muse_did_2610_t a;
    muse_did_2663_t m;

    /* The table: requests, the bytes each needs, the floors. */
    CHECK(MUSE_DID_INFO[MUSE_DID_2612].did == 0x2612 && !strcmp(MUSE_DID_INFO[MUSE_DID_2612].req, "222612"));
    CHECK(MUSE_DID_INFO[MUSE_DID_2610].did == 0x2610 && !strcmp(MUSE_DID_INFO[MUSE_DID_2610].req, "222610"));
    CHECK(MUSE_DID_INFO[MUSE_DID_2663].did == 0x2663 && !strcmp(MUSE_DID_INFO[MUSE_DID_2663].req, "222663"));
    CHECK(MUSE_DID_INFO[MUSE_DID_2612].min_data == 54 && MUSE_DID_INFO[MUSE_DID_2610].min_data == 33
          && MUSE_DID_INFO[MUSE_DID_2663].min_data == 42);
    CHECK(MUSE_DID_INFO[MUSE_DID_2612].min_ms == 1000 && MUSE_DID_INFO[MUSE_DID_2610].min_ms == 3000
          && MUSE_DID_INFO[MUSE_DID_2663].min_ms == 3000);

    /* OBDb's 2007 replies, byte for byte: the clutch off and on, the
     * generator's duty (x 100 / 255, cut to whole percent). */
    elm_from_frames(text, sizeof(text), S2000_2610_AC_OFF, S2000_DID_FRAMES, false);
    CHECK(!strncmp(text, "039\r0:622610FFFDF0\r1:61E0000000003D\r", 36));
    memset(&a, 0xAA, sizeof(a));
    CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_DID_2610 && !a.ac && a.generator_pct == 23);
    elm_from_frames(text, sizeof(text), S2000_2610_AC_ON, S2000_DID_FRAMES, false);
    CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_DID_2610 && a.ac && a.generator_pct == 45);
    elm_from_frames(text, sizeof(text), S2000_2610_AC_ON, S2000_DID_FRAMES, true);   /* ATS1's spaces */
    CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_DID_2610 && a.ac && a.generator_pct == 45);

    /* Each cylinder's count at its own two bytes, from OBDb's own cases. */
    static const struct {
        const char *const *raw;
        uint16_t want[MUSE_MISFIRE_CYLS];
    } MIS[] = {
        { S2000_2663_NONE, { 0, 0, 0, 0 } }, { S2000_2663_CYL1, { 1, 0, 0, 0 } },
        { S2000_2663_CYL2, { 0, 1, 0, 0 } }, { S2000_2663_CYL3, { 0, 0, 1, 0 } },
        { S2000_2663_CYL4, { 0, 0, 0, 1 } },
    };
    for (size_t i = 0; i < sizeof(MIS) / sizeof(MIS[0]); i++) {
        elm_from_frames(text, sizeof(text), MIS[i].raw, S2000_DID_FRAMES, i % 2 == 1);
        memset(&m, 0xAA, sizeof(m));
        CHECK(muse_elm_parse_did_2663(fed(text), &m) == MUSE_ELM_DID_2663);
        CHECK(!memcmp(m.count, MIS[i].want, sizeof(m.count)));
        /* and it is nobody else's */
        muse_vtec_did_t v;
        CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_NONE);
        CHECK(muse_elm_parse_vtec_did(fed(text), &v) == MUSE_ELM_NONE);
    }
    elm_from_frames(text, sizeof(text), S2000_2007_ON_REPORT, S2000_FRAMES, false);
    CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_did_2663(fed(text), &m) == MUSE_ELM_NONE);
    elm_from_frames(text, sizeof(text), S2000_2610_AC_ON, S2000_DID_FRAMES, false);
    CHECK(muse_elm_parse_did_2663(fed(text), &m) == MUSE_ELM_NONE);

    /* Cut short (frames missing against the 039 length): no reading, and
     * the caller's last one is left alone. */
    a = (muse_did_2610_t){ .ac = true, .generator_pct = 77 };
    elm_from_frames(text, sizeof(text), S2000_2610_AC_OFF, 5, false);
    CHECK(muse_elm_parse_did_2610(fed(text), &a) == MUSE_ELM_NONE && a.ac && a.generator_pct == 77);
    m = (muse_did_2663_t){ { 9, 9, 9, 9 } };
    elm_from_frames(text, sizeof(text), S2000_2663_CYL1, 6, false);
    CHECK(muse_elm_parse_did_2663(fed(text), &m) == MUSE_ELM_NONE && m.count[0] == 9 && m.count[3] == 9);

    /* The clutch is data byte 32's 0x02, and only that bit. */
    uint8_t b[64];
    did_block(b, 0x2610, 57);
    b[3 + 32] = 0x02;
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 57)), &a) == MUSE_ELM_DID_2610 && a.ac);
    b[3 + 32] = 0xFD;
    b[3 + 31] = b[3 + 33] = 0x02;
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 57)), &a) == MUSE_ELM_DID_2610 && !a.ac);
    /* The generator is data byte 23, whole percent. */
    static const struct { uint8_t raw; int pct; } GEN[] = { { 0x00, 0 }, { 0x80, 50 }, { 0xFF, 100 }, { 0x3C, 23 } };
    for (size_t i = 0; i < sizeof(GEN) / sizeof(GEN[0]); i++) {
        did_block(b, 0x2610, 57);
        b[3 + 23] = GEN[i].raw;
        CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 57)), &a) == MUSE_ELM_DID_2610
              && a.generator_pct == GEN[i].pct);
    }
    /* Lengths: 33 data bytes are enough for 0x2610, 32 are not. */
    did_block(b, 0x2610, 3 + 33);
    b[3 + 32] = 0x02;
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 3 + 33)), &a) == MUSE_ELM_DID_2610 && a.ac);
    a.ac = false;
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 3 + 32)), &a) == MUSE_ELM_NONE && !a.ac);

    /* The counts: big-endian, cylinder n at data byte 34 + 2 (n - 1);
     * the bytes either side play no part. */
    did_block(b, 0x2663, 57);
    static const uint8_t COUNTS[] = { 0x12, 0x34, 0x00, 0xFF, 0xAB, 0xCD, 0x00, 0x07 };
    memcpy(b + 3 + 34, COUNTS, sizeof(COUNTS));
    b[3 + 33] = b[3 + 42] = 0xEE;
    CHECK(muse_elm_parse_did_2663(fed(elm_bytes(text, sizeof(text), b, 57)), &m) == MUSE_ELM_DID_2663);
    CHECK(m.count[0] == 0x1234 && m.count[1] == 0x00FF && m.count[2] == 0xABCD && m.count[3] == 0x0007);
    /* 42 data bytes are enough, 41 are not. */
    CHECK(muse_elm_parse_did_2663(fed(elm_bytes(text, sizeof(text), b, 3 + 42)), &m) == MUSE_ELM_DID_2663
          && m.count[3] == 7);
    m.count[3] = 99;
    CHECK(muse_elm_parse_did_2663(fed(elm_bytes(text, sizeof(text), b, 3 + 41)), &m) == MUSE_ELM_NONE
          && m.count[3] == 99);

    /* A garbled header: another DID, not a positive answer, frames out of
     * order. */
    did_block(b, 0x2611, 57);
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 57)), &a) == MUSE_ELM_NONE);
    did_block(b, 0x2610, 57);
    b[0] = 0x63;
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 57)), &a) == MUSE_ELM_NONE);
    elm_from_frames(text, sizeof(text), S2000_2663_CYL2, S2000_DID_FRAMES, false);
    char *two = strstr(text, "\r2:");
    CHECK(two != NULL);
    if (two) {
        two[1] = '3';   /* frame 2 claims to be frame 3 */
    }
    CHECK(muse_elm_parse_did_2663(fed(text), &m) == MUSE_ELM_NONE);

    /* Refused, silent, the ELM's errors. */
    CHECK(muse_elm_parse_did_2610("7F2231\n", &a) == MUSE_ELM_NEGATIVE);
    CHECK(muse_elm_parse_did_2663("7F 22 31\n", &m) == MUSE_ELM_NEGATIVE);
    CHECK(muse_elm_parse_did_2610("NO DATA\n", &a) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_did_2663("NO DATA\n", &m) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_did_2663("CAN ERROR\n", &m) == MUSE_ELM_ERROR);
    elm_from_frames(text, sizeof(text), S2000_2663_CYL1, S2000_DID_FRAMES, false);
    char broken[600];
    snprintf(broken, sizeof(broken), "<RX ERROR\r%s", text);
    CHECK(muse_elm_parse_did_2663(fed(broken), &m) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_did_2610("", &a) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_did_2663("41 0C 1A F8\n", &m) == MUSE_ELM_NONE);
}

/* OBD-STEERING-ANGLE-TASK Phase 1: the VSA (DA28) decodes, the raw reply
 * the scan stores, its DID list, and the 500 ms schedule tier. The 4004 /
 * 4005 layouts are OBDb's for other Hondas — provisional until the car —
 * so these blocks are built from the spec's offsets, not captured frames. */
static void test_vsa_parse(void)
{
    char text[512];
    muse_vsa_4004_t s;
    muse_vsa_4005_t p;

    /* The generalised table: five DIDs, two header sessions, the floors. */
    CHECK(MUSE_DID_INFO[MUSE_DID_4004].did == 0x4004 && !strcmp(MUSE_DID_INFO[MUSE_DID_4004].req, "224004"));
    CHECK(MUSE_DID_INFO[MUSE_DID_4005].did == 0x4005 && !strcmp(MUSE_DID_INFO[MUSE_DID_4005].req, "224005"));
    CHECK(MUSE_DID_INFO[MUSE_DID_4004].min_data == 16 && MUSE_DID_INFO[MUSE_DID_4005].min_data == 9);
    CHECK(MUSE_DID_INFO[MUSE_DID_4004].min_ms == 500 && MUSE_DID_INFO[MUSE_DID_4005].min_ms == 500);
    for (int d = 0; d < MUSE_DID_4004; d++) {
        CHECK(!strcmp(MUSE_DID_INFO[d].atsh, "ATSHDA10F1") && !strcmp(MUSE_DID_INFO[d].atcra, "ATCRA18DAF110"));
    }
    for (int d = MUSE_DID_4004; d < MUSE_DIDS; d++) {
        CHECK(!strcmp(MUSE_DID_INFO[d].atsh, "ATSHDA28F1") && !strcmp(MUSE_DID_INFO[d].atcra, "ATCRA18DAF128"));
    }
    /* Every request in the table is a UDS 0x22 read, nothing else. */
    for (int d = 0; d < MUSE_DIDS; d++) {
        CHECK(!strncmp(MUSE_DID_INFO[d].req, "22", 2) && strlen(MUSE_DID_INFO[d].req) == 6);
    }

    /* The brake switch is data byte 15's 0x80, and only that bit. */
    uint8_t b[96];
    did_block(b, 0x4004, 3 + 20);
    b[3 + 15] = 0x80;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &s) == MUSE_ELM_DID_4004 && s.brk);
    b[3 + 15] = 0x7F;
    b[3 + 14] = b[3 + 16] = 0x80;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &s) == MUSE_ELM_DID_4004 && !s.brk);
    /* 16 data bytes are enough, 15 are not (and the last reading stands). */
    did_block(b, 0x4004, 3 + 16);
    b[3 + 15] = 0x80;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 16)), &s) == MUSE_ELM_DID_4004 && s.brk);
    s.brk = false;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 15)), &s) == MUSE_ELM_NONE && !s.brk);
    /* The VSA block sizes are unknown (the scan stores up to 128 reply
     * bytes): a block longer than the DA10 parsers' 64 still reads. */
    did_block(b, 0x4004, 3 + 80);
    b[3 + 15] = 0x80;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 80)), &s) == MUSE_ELM_DID_4004 && s.brk);
    did_block(b, 0x4005, 3 + 80);
    b[3 + 7] = 0x80;
    b[3 + 8] = 0x00;
    CHECK(muse_elm_parse_vsa_4005(fed(elm_bytes(text, sizeof(text), b, 3 + 80)), &p) == MUSE_ELM_DID_4005
          && p.brake_p == 4750);

    /* The pressure scale: -25..120 bar linear over the raw count. */
    CHECK(muse_vsa_brake_centibar(0) == -2500);
    CHECK(muse_vsa_brake_centibar(0xFFFF) == 12000);
    CHECK(muse_vsa_brake_centibar(0x8000) == 4750);
    CHECK(muse_vsa_brake_centibar(11299) == 0 || muse_vsa_brake_centibar(11299) == 1);
    /* Big-endian at data bytes 7-8; the bytes either side play no part. */
    did_block(b, 0x4005, 3 + 12);
    b[3 + 7] = 0x80;
    b[3 + 8] = 0x00;
    b[3 + 6] = b[3 + 9] = 0xEE;
    CHECK(muse_elm_parse_vsa_4005(fed(elm_bytes(text, sizeof(text), b, 3 + 12)), &p) == MUSE_ELM_DID_4005
          && p.brake_p == 4750);
    /* 9 data bytes are enough, 8 are not. */
    CHECK(muse_elm_parse_vsa_4005(fed(elm_bytes(text, sizeof(text), b, 3 + 9)), &p) == MUSE_ELM_DID_4005
          && p.brake_p == 4750);
    p.brake_p = 999;
    CHECK(muse_elm_parse_vsa_4005(fed(elm_bytes(text, sizeof(text), b, 3 + 8)), &p) == MUSE_ELM_NONE
          && p.brake_p == 999);
    /* Each block is nobody else's. */
    muse_did_2610_t a;
    muse_did_2663_t m;
    muse_vtec_did_t v;
    CHECK(muse_elm_parse_vsa_4004(fed(elm_bytes(text, sizeof(text), b, 3 + 12)), &s) == MUSE_ELM_NONE);
    did_block(b, 0x4004, 3 + 20);
    CHECK(muse_elm_parse_vsa_4005(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &p) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_vtec_did(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &v) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_did_2610(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &a) == MUSE_ELM_NONE);
    CHECK(muse_elm_parse_did_2663(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), &m) == MUSE_ELM_NONE);

    /* Refused, silent, garbled, as for the DA10 blocks. */
    CHECK(muse_elm_parse_vsa_4004("7F2231\n", &s) == MUSE_ELM_NEGATIVE);
    CHECK(muse_elm_parse_vsa_4005("7F 22 14\n", &p) == MUSE_ELM_NEGATIVE);
    CHECK(muse_elm_parse_vsa_4004("NO DATA\n", &s) == MUSE_ELM_NO_DATA);
    CHECK(muse_elm_parse_vsa_4005("CAN ERROR\n", &p) == MUSE_ELM_ERROR);
    CHECK(muse_elm_parse_vsa_4004("", &s) == MUSE_ELM_NONE);

    /* The scan's raw read: the reply's bytes out, the refusal's code out. */
    uint8_t raw[128];
    int n = 0, nrc = 99;
    did_block(b, 0x4031, 3 + 5);
    b[3] = 0x12, b[4] = 0x34, b[5] = 0x56, b[6] = 0x78, b[7] = 0x9A;
    CHECK(muse_elm_parse_did_raw(fed(elm_bytes(text, sizeof(text), b, 3 + 5)), 0x4031, raw, sizeof(raw), &n, &nrc)
          == MUSE_ELM_DID_RAW);
    CHECK(n == 8 && raw[0] == 0x62 && raw[1] == 0x40 && raw[2] == 0x31 && raw[3] == 0x12 && raw[7] == 0x9A);
    CHECK(muse_elm_parse_did_raw(fed(elm_bytes(text, sizeof(text), b, 3 + 5)), 0x4032, raw, sizeof(raw), &n, &nrc)
          == MUSE_ELM_NONE);
    nrc = -1;
    CHECK(muse_elm_parse_did_raw("7F2231\n", 0x4031, raw, sizeof(raw), &n, &nrc) == MUSE_ELM_NEGATIVE && nrc == 0x31);
    CHECK(muse_elm_parse_did_raw("NO DATA\n", 0x4031, raw, sizeof(raw), &n, &nrc) == MUSE_ELM_NO_DATA && nrc == -1);
    CHECK(muse_elm_parse_did_raw("CAN ERROR\n", 0x4031, raw, sizeof(raw), &n, &nrc) == MUSE_ELM_ERROR);
    /* A reply longer than the slot does not add up: no reading. */
    did_block(b, 0x4031, 3 + 20);
    CHECK(muse_elm_parse_did_raw(fed(elm_bytes(text, sizeof(text), b, 3 + 20)), 0x4031, raw, 10, &n, &nrc)
          == MUSE_ELM_NONE);

    /* The scan list: OBDb's DA28 space, in order, nothing twice. */
    static const uint16_t WANT[] = { 0x4000, 0x4001, 0x4002, 0x4003, 0x4004, 0x4005, 0x4006, 0x4007, 0x4008, 0x4009,
                                     0x4030, 0x4031, 0x4032, 0x4033, 0x4034, 0x4035, 0x4036, 0x4037, 0x4038, 0x4039,
                                     0x4070, 0x4071, 0x4072, 0x4073, 0xE600, 0xE602 };
    CHECK(MUSE_VSA_SCAN_DIDS_N == (int)(sizeof(WANT) / sizeof(WANT[0])));
    CHECK(!memcmp(MUSE_VSA_SCAN_DIDS, WANT, sizeof(WANT)));

    /* The 500 ms tier: with only the VSA pair on, each is picked on its
     * floor (never inside it, the probe counting as a read), the pair is
     * due together for its grouped session, and no DA10 DID ever is. */
    muse_elm_support_t sup = { 0 };
    muse_sched_t sc;
    muse_sched_init(&sc, &sup, (1u << MUSE_DID_4004) | (1u << MUSE_DID_4005), 1000);
    CHECK(!muse_sched_did_due(&sc, MUSE_DID_4004, 1499));
    CHECK(muse_sched_did_due(&sc, MUSE_DID_4004, 1500) && muse_sched_did_due(&sc, MUSE_DID_4005, 1500));
    CHECK(!muse_sched_did_due(&sc, MUSE_DID_2612, 100000));   /* never probed on: never due */
    int picks[MUSE_DIDS] = { 0 };
    uint32_t done[MUSE_DIDS] = { 0 };
    for (int d = 0; d < MUSE_DIDS; d++) {
        done[d] = 1000;   /* each DID's floor runs from its probe at 1000 */
    }
    muse_sched_req_t r;
    for (uint32_t t = 1000; t < 11000;) {
        muse_sched_pick(&sc, t, &r);
        if (r.did) {
            CHECK(r.which == MUSE_DID_4004 || r.which == MUSE_DID_4005);
            CHECK(t - done[r.which] >= 500);   /* the floor holds */
            picks[r.which]++;
            t += 100;
            muse_sched_done_did(&sc, (muse_did_t)r.which, t);
            done[r.which] = t;
        } else {
            t += 50;
            serve(&sc, &r, t);
        }
    }
    CHECK(picks[MUSE_DID_4004] >= 15 && picks[MUSE_DID_4005] >= 15);
    CHECK(picks[MUSE_DID_2612] == 0 && picks[MUSE_DID_2610] == 0 && picks[MUSE_DID_2663] == 0);
    /* Off in the schedule (probe failed): never picked, however late. */
    muse_sched_init(&sc, &sup, 0, 1000);
    muse_sched_pick(&sc, 100000, &r);
    CHECK(!r.did);
}

/* The misfire warning's trigger, read by read. */
static void test_misfire_trigger(void)
{
    const uint32_t COOL = MUSE_MISFIRE_COOLDOWN_MS;
    CHECK(COOL == MUSE_COOLANT_COOLDOWN_MS);
    muse_misfire_trigger_t t;
    muse_misfire_init(&t);
    uint16_t c[MUSE_MISFIRE_CYLS] = { 3, 0, 0, 0 };

    /* The first read is the baseline: it never fires, however high. */
    CHECK(!muse_misfire_update(&t, c, 800, 0) && t.rose == 0);
    CHECK(!muse_misfire_update(&t, c, 800, 3000) && t.rose == 0);   /* unchanged */
    /* An increase with the engine running fires; the cylinder is marked. */
    c[0] = 4;
    CHECK(muse_misfire_update(&t, c, 800, 6000) && t.rose == 0x1);
    /* Counted once: the next read without an increase is quiet and unmarks. */
    CHECK(!muse_misfire_update(&t, c, 800, 9000) && t.rose == 0);
    /* A second increase inside the cooldown is marked, not fired. */
    c[2] = 1;
    CHECK(!muse_misfire_update(&t, c, 800, 12000) && t.rose == 0x4);
    CHECK(!muse_misfire_update(&t, c, 800, 6000 + COOL - 1) && t.rose == 0);
    /* After it, a new increase fires again. */
    c[3] = 1;
    CHECK(muse_misfire_update(&t, c, 800, 6000 + COOL) && t.rose == 0x8);
    uint32_t t0 = 6000 + 2 * COOL + 1000;   /* past that fire's cooldown */
    /* With the engine off (under 400 rpm, or unknown) an increase is let go,
     * and doesn't fire later either. */
    c[1] = 5;
    CHECK(!muse_misfire_update(&t, c, MUSE_COOLANT_RUNNING_RPM - 1, t0) && t.rose == 0x2);
    CHECK(!muse_misfire_update(&t, c, 800, t0 + 3000));
    c[1] = 6;
    CHECK(!muse_misfire_update(&t, c, -1, t0 + 6000));
    /* Exactly the running threshold counts as running. */
    c[1] = 7;
    CHECK(muse_misfire_update(&t, c, MUSE_COOLANT_RUNNING_RPM, t0 + 9000) && t.rose == 0x2);
    uint32_t t1 = t0 + 9000 + COOL;   /* that fire's cooldown is over */
    /* Cleared counters (any one going down) are a new baseline, never a
     * fire, even with another one up in the same read. */
    uint16_t z[MUSE_MISFIRE_CYLS] = { 0, 0, 0, 0 };
    CHECK(!muse_misfire_update(&t, z, 800, t1) && t.rose == 0);
    uint16_t mixed[MUSE_MISFIRE_CYLS] = { 0, 0, 0, 2 };
    CHECK(muse_misfire_update(&t, mixed, 800, t1 + 3000) && t.rose == 0x8);   /* up from the new baseline */
    uint32_t t2 = t1 + 3000 + COOL;
    uint16_t down_up[MUSE_MISFIRE_CYLS] = { 5, 0, 0, 1 };   /* 1 up, 4 down */
    CHECK(!muse_misfire_update(&t, down_up, 800, t2) && t.rose == 0);
    /* A counter that wraps (65535 -> 0) is a decrease too. */
    uint16_t top[MUSE_MISFIRE_CYLS] = { 5, 0, 0, UINT16_MAX };
    CHECK(muse_misfire_update(&t, top, 800, t2 + 3000));   /* up from 1 */
    uint32_t t3 = t2 + 3000 + COOL;
    uint16_t wrapped[MUSE_MISFIRE_CYLS] = { 5, 0, 0, 0 };
    CHECK(!muse_misfire_update(&t, wrapped, 800, t3) && t.rose == 0);
    /* A dropped link: the next read is a baseline again, the cooldown holds. */
    uint16_t more[MUSE_MISFIRE_CYLS] = { 9, 9, 9, 9 };
    muse_misfire_reset(&t);
    CHECK(!t.have && t.rose == 0);
    CHECK(!muse_misfire_update(&t, more, 800, t3 + 1000) && t.rose == 0);   /* baseline, not an increase */
    more[0] = 10;
    CHECK(muse_misfire_update(&t, more, 800, t3 + 4000));
    muse_misfire_reset(&t);
    more[0] = 11;
    CHECK(!muse_misfire_update(&t, more, 800, t3 + 7000));
    more[0] = 12;
    CHECK(!muse_misfire_update(&t, more, 800, t3 + 10000) && t.rose == 0x1);   /* the cooldown survived the drop */
    /* The clock wraps like the others'. */
    muse_misfire_init(&t);
    uint16_t w[MUSE_MISFIRE_CYLS] = { 0, 0, 0, 0 };
    CHECK(!muse_misfire_update(&t, w, 800, UINT32_MAX - 1000));
    w[0] = 1;
    CHECK(muse_misfire_update(&t, w, 800, UINT32_MAX - 500));
    w[0] = 2;
    CHECK(!muse_misfire_update(&t, w, 800, 1000));                  /* 1.5 s later, past the wrap */
    w[0] = 3;
    CHECK(muse_misfire_update(&t, w, 800, COOL - 500 + 1));        /* 120 s after the fire */
}

/* Each DID on its own floor in the schedule, over a minute of link time
 * with DID exchanges four times a Mode 01 one: only answered DIDs are read,
 * never inside their floors, never together, and 0x2612 is never starved by
 * the 3 s pair. */
static void test_did_schedule(void)
{
    muse_elm_support_t sup = { 0 };
    muse_sched_t s;
    muse_sched_req_t r;
    const uint32_t MODE01_MS = 50, DID_MS = 200;

    /* Only 0x2612 answered: the pair is never asked. */
    muse_sched_init(&s, &sup, 1u << MUSE_DID_2612, 0);
    int picks[MUSE_DIDS] = { 0 };
    for (uint32_t t = 0; t < 20000;) {
        muse_sched_pick(&s, t, &r);
        if (r.did) {
            CHECK(r.which < MUSE_DIDS && r.n == 0);
            picks[r.which]++;
            t += DID_MS;
            muse_sched_done_did(&s, (muse_did_t)r.which, t);
        } else {
            t += MODE01_MS;
            serve(&s, &r, t);
        }
    }
    CHECK(picks[MUSE_DID_2612] >= 15 && picks[MUSE_DID_2610] == 0 && picks[MUSE_DID_2663] == 0);

    /* All three: each at its floor, 0x2612 still about once a second. */
    muse_sched_init(&s, &sup, 1u << MUSE_DID_2612 | 1u << MUSE_DID_2610 | 1u << MUSE_DID_2663, 0);
    uint32_t done[MUSE_DIDS] = { 0, 0, 0 }, worst[MUSE_DIDS] = { 0 };
    int n[MUSE_DIDS] = { 0 };
    for (uint32_t t = 0; t < 60000;) {
        muse_sched_pick(&s, t, &r);
        if (!r.did) {
            t += MODE01_MS;
            serve(&s, &r, t);
            continue;
        }
        muse_did_t d = (muse_did_t)r.which;
        uint32_t since = t - done[d];   /* from the last exchange's end */
        CHECK(since >= MUSE_DID_INFO[d].min_ms);   /* the floor holds */
        if (since > worst[d]) {
            worst[d] = since;
        }
        n[d]++;
        t += DID_MS;
        muse_sched_done_did(&s, d, t);
        done[d] = t;
    }
    /* Late by at most what can be ahead of it: the other two DIDs' exchanges
     * and a few Mode 01 batches already overdue. */
    uint32_t slack = 2 * DID_MS + 4 * MODE01_MS;
    CHECK(worst[MUSE_DID_2612] <= MUSE_SCHED_DID_MIN_MS + slack);
    CHECK(worst[MUSE_DID_2610] <= MUSE_DID_2610_MIN_MS + slack);
    CHECK(worst[MUSE_DID_2663] <= MUSE_DID_2663_MIN_MS + slack);
    CHECK(n[MUSE_DID_2612] >= 45 && n[MUSE_DID_2610] >= 17 && n[MUSE_DID_2663] >= 17);
    if (worst[MUSE_DID_2612] > MUSE_SCHED_DID_MIN_MS + slack) {
        fprintf(stderr, "2612 waited %u ms\n", (unsigned)worst[MUSE_DID_2612]);
    }

    /* A DID answered after polling started joins then, from its probe. */
    muse_sched_init(&s, &sup, 1u << MUSE_DID_2612, 0);
    muse_sched_set_did(&s, MUSE_DID_2663, true, 5000);
    for (uint32_t t = 5000; t < 7900; t += MODE01_MS) {
        muse_sched_pick(&s, t, &r);
        CHECK(!(r.did && r.which == MUSE_DID_2663));   /* inside its floor from the probe */
        if (r.did) {
            muse_sched_done_did(&s, (muse_did_t)r.which, t);
        } else {
            serve(&s, &r, t);
        }
    }
    bool seen = false;
    for (uint32_t t = 8000; t < 9000 && !seen; t += MODE01_MS) {
        muse_sched_pick(&s, t, &r);
        seen = r.did && r.which == MUSE_DID_2663;
        if (r.did) {
            muse_sched_done_did(&s, (muse_did_t)r.which, t);
        } else {
            serve(&s, &r, t);
        }
    }
    CHECK(seen);
}

int main(void)
{
    test_fragmented_atz();
    test_searching_then_frames();
    test_parse_cases();
    test_overflow();
    test_vtec_did();
    test_vtec_engaged();
    test_coolant_frames();
    test_coolant_parse_cases();
    test_coolant_trigger();
    test_warmup_trigger();
    test_extra_parse_cases();
    test_extra_frames();
    test_more_pids_wide();
    test_support_parse();
    test_pid_support();
    test_batch_parse();
    test_scheduler();
    test_did_cards_parse();
    test_vsa_parse();
    test_misfire_trigger();
    test_did_schedule();
    test_link_stats();
    test_tune();
    test_protocol();
    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_elm\n");
    return 0;
}
