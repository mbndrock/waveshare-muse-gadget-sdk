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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_obd_log_store.h"

/*
 * The OBD trip log's doorbell (OBD-LOG-DOORBELL-TASK): the gadget tells Muse
 * finished trips are waiting, as one text turn,
 *
 *   Trip logs ready: trips 5, 6 (198203 bytes total).
 *
 * and Muse pulls them (obd.trips / obd.trip_read) and deletes the copies.
 * This unit is the decision, as plain C over the store listing
 * (muse_log_trip_t) so the host tests can run it: the pending trips, the
 * per-connection announced set and the session-ready flag go in, and whether
 * to send, with the exact line, comes out. The IDF side (the watcher and
 * Settings' button, in muse_obd_log.c) lists the stores, feeds this and hands
 * a sent line to muse_hatch_text_turn. Ids in a line are ascending; its bytes
 * are the sum of those trips' manifest byte counts. Trips an announcement
 * covered are not announced again until the session drops and reconnects.
 */

#define MUSE_DOORBELL_ANNOUNCED_MAX 128   /* announced ids kept per connection */

/* Room for any line this unit writes: the prefix, MUSE_DOORBELL_ANNOUNCED_MAX
 * seven-digit ids with separators, and the byte total. */
#define MUSE_DOORBELL_LINE_MAX 1408

typedef struct {
    uint32_t announced[MUSE_DOORBELL_ANNOUNCED_MAX];
    int announced_count;
    bool session_ready;
} muse_doorbell_t;

void muse_doorbell_init(muse_doorbell_t *d);

/* The chat session's ready flag. Losing it ends the connection: the announced
 * set is forgotten, so a later reconnect re-announces what is still pending. */
void muse_doorbell_set_session_ready(muse_doorbell_t *d, bool ready);
bool muse_doorbell_session_ready(const muse_doorbell_t *d);

/* How many of pending[0..n) have not been announced on this connection. */
int muse_doorbell_unannounced(const muse_doorbell_t *d, const muse_log_trip_t *pending, int n);

/*
 * The announcement, if there is one to make: the session is ready and at
 * least one pending trip is unannounced. Writes the line (NUL-terminated)
 * into out, records the trips it names as announced, and returns true.
 * Otherwise writes nothing and returns false; in particular a line that
 * would not fit in cap announces and marks nothing. One call names as many
 * unannounced trips (ascending) as the announced set has room for after ids
 * no longer pending are dropped; the caller rings again for any rest.
 */
bool muse_doorbell_announce(muse_doorbell_t *d, const muse_log_trip_t *pending, int n, char *out, size_t cap);

/* Settings' row text for count pending trips of total_bytes in all:
 * "2 waiting, 193 KB" (total_bytes / 1024), or "No logs waiting". */
void muse_doorbell_pending_text(int count, uint32_t total_bytes, char *out, size_t cap);
