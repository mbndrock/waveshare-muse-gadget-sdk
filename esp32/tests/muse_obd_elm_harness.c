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

/* The OBD VTEC reaction's ELM327 parser and trigger (muse_obd_elm.c), fed the
 * way the LELink^2 adapter's FFE1 notifications cut its replies. */

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

int main(void)
{
    test_fragmented_atz();
    test_searching_then_frames();
    test_parse_cases();
    test_overflow();
    test_trigger();
    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_elm\n");
    return 0;
}
