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

/*
 * The OBD log doorbell's announce decision (muse_obd_doorbell.c): the exact
 * announcement line, ascending ids, the byte sum, per-connection dedupe,
 * the session-ready gate and the Settings pending text.
 */

#include <stdio.h>
#include <string.h>

#include "muse_obd_doorbell.h"

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

static muse_log_trip_t trip(uint32_t id, uint32_t bytes)
{
    muse_log_trip_t t;
    memset(&t, 0, sizeof(t));
    t.id = id;
    t.bytes = bytes;
    return t;
}

int main(void)
{
    char line[512];
    muse_doorbell_t d;
    muse_doorbell_init(&d);

    /* Nothing pending: never announce, ready or not. */
    CHECK(!muse_doorbell_announce(&d, NULL, 0, line, sizeof(line)));
    muse_doorbell_set_session_ready(&d, true);
    CHECK(!muse_doorbell_announce(&d, NULL, 0, line, sizeof(line)));

    /* The exact line: ascending ids (input is not sorted), summed bytes,
     * no thousands separators, trailing period. */
    muse_log_trip_t pending[] = { trip(6, 98203), trip(5, 100000) };
    CHECK(muse_doorbell_announce(&d, pending, 2, line, sizeof(line)));
    CHECK(strcmp(line, "Trip logs ready: trips 5, 6 (198203 bytes total).") == 0);

    /* The same trips are not announced twice on one connection. */
    CHECK(!muse_doorbell_announce(&d, pending, 2, line, sizeof(line)));
    CHECK(muse_doorbell_unannounced(&d, pending, 2) == 0);

    /* A trip that closes later is announced on its own, batched with any
     * other unannounced trip and nothing already announced. */
    muse_log_trip_t more[] = { trip(5, 100000), trip(6, 98203), trip(7, 1234567) };
    CHECK(muse_doorbell_unannounced(&d, more, 3) == 1);
    CHECK(muse_doorbell_announce(&d, more, 3, line, sizeof(line)));
    CHECK(strcmp(line, "Trip logs ready: trips 7 (1234567 bytes total).") == 0);

    /* The session dropping resets the announced set: a reconnect
     * re-announces everything still pending. */
    muse_doorbell_set_session_ready(&d, false);
    CHECK(!muse_doorbell_session_ready(&d));
    CHECK(!muse_doorbell_announce(&d, more, 3, line, sizeof(line)));
    muse_doorbell_set_session_ready(&d, true);
    CHECK(muse_doorbell_announce(&d, more, 3, line, sizeof(line)));
    CHECK(strcmp(line, "Trip logs ready: trips 5, 6, 7 (1432770 bytes total).") == 0);

    /* Not ready: no send and nothing is marked, so the ring still happens
     * when the session comes up. */
    muse_doorbell_t d2;
    muse_doorbell_init(&d2);
    muse_log_trip_t one[] = { trip(9, 2048) };
    CHECK(!muse_doorbell_announce(&d2, one, 1, line, sizeof(line)));
    CHECK(muse_doorbell_unannounced(&d2, one, 1) == 1);
    muse_doorbell_set_session_ready(&d2, true);
    CHECK(muse_doorbell_announce(&d2, one, 1, line, sizeof(line)));
    CHECK(strcmp(line, "Trip logs ready: trips 9 (2048 bytes total).") == 0);
    /* Staying ready is not a new connection: no reset. */
    muse_doorbell_set_session_ready(&d2, true);
    CHECK(!muse_doorbell_announce(&d2, one, 1, line, sizeof(line)));

    /* A buffer too small for the line sends nothing and marks nothing. */
    muse_doorbell_t d3;
    muse_doorbell_init(&d3);
    muse_doorbell_set_session_ready(&d3, true);
    char tiny[16];
    CHECK(!muse_doorbell_announce(&d3, one, 1, tiny, sizeof(tiny)));
    CHECK(muse_doorbell_unannounced(&d3, one, 1) == 1);
    CHECK(muse_doorbell_announce(&d3, one, 1, line, sizeof(line)));

    /* Deleted trips free their announced slot: a store that keeps cycling
     * trips through Muse's pull-and-delete never fills the set. */
    muse_doorbell_t d4;
    muse_doorbell_init(&d4);
    muse_doorbell_set_session_ready(&d4, true);
    for (uint32_t id = 1; id <= (uint32_t)MUSE_DOORBELL_ANNOUNCED_MAX + 40; id++) {
        muse_log_trip_t cur[] = { trip(id, 10) };
        CHECK(muse_doorbell_announce(&d4, cur, 1, line, sizeof(line)));
    }

    /* Settings' pending text. */
    char text[64];
    muse_doorbell_pending_text(0, 0, text, sizeof(text));
    CHECK(strcmp(text, "No logs waiting") == 0);
    muse_doorbell_pending_text(2, 198203, text, sizeof(text));
    CHECK(strcmp(text, "2 waiting, 193 KB") == 0);
    muse_doorbell_pending_text(1, 512, text, sizeof(text));
    CHECK(strcmp(text, "1 waiting, 0 KB") == 0);

    if (s_fail) {
        fprintf(stderr, "%d check(s) failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_doorbell\n");
    return 0;
}
