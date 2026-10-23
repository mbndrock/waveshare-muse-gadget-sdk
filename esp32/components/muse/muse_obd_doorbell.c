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

#include "muse_obd_doorbell.h"

#include <stdio.h>
#include <string.h>

void muse_doorbell_init(muse_doorbell_t *d)
{
    memset(d, 0, sizeof(*d));
}

void muse_doorbell_set_session_ready(muse_doorbell_t *d, bool ready)
{
    if (d->session_ready && !ready) {
        d->announced_count = 0;   /* a new connection re-announces */
    }
    d->session_ready = ready;
}

bool muse_doorbell_session_ready(const muse_doorbell_t *d)
{
    return d->session_ready;
}

static bool is_announced(const muse_doorbell_t *d, uint32_t id)
{
    for (int i = 0; i < d->announced_count; i++) {
        if (d->announced[i] == id) {
            return true;
        }
    }
    return false;
}

static bool is_pending(const muse_log_trip_t *pending, int n, uint32_t id)
{
    for (int i = 0; i < n; i++) {
        if (pending[i].id == id) {
            return true;
        }
    }
    return false;
}

int muse_doorbell_unannounced(const muse_doorbell_t *d, const muse_log_trip_t *pending, int n)
{
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (!is_announced(d, pending[i].id)) {
            count++;
        }
    }
    return count;
}

bool muse_doorbell_announce(muse_doorbell_t *d, const muse_log_trip_t *pending, int n, char *out, size_t cap)
{
    if (!d->session_ready || n <= 0 || !pending || cap == 0) {
        return false;
    }
    /* Announced trips Muse has since pulled and deleted free their slots;
     * trip ids are never reused, so a dropped id can never come back. */
    int kept = 0;
    for (int i = 0; i < d->announced_count; i++) {
        if (is_pending(pending, n, d->announced[i])) {
            d->announced[kept++] = d->announced[i];
        }
    }
    d->announced_count = kept;

    int room = MUSE_DOORBELL_ANNOUNCED_MAX - d->announced_count;
    if (room <= 0 || muse_doorbell_unannounced(d, pending, n) == 0) {
        return false;
    }

    /* The batch, ascending: repeatedly the smallest unannounced id past the
     * last one taken, at most room of them, so what is marked is exactly
     * what the line names. */
    uint32_t ids[MUSE_DOORBELL_ANNOUNCED_MAX];
    int batch = 0;
    uint64_t bytes = 0;
    uint32_t after = 0;
    while (batch < room) {
        const muse_log_trip_t *best = NULL;
        for (int i = 0; i < n; i++) {
            if (pending[i].id > after && !is_announced(d, pending[i].id)
                && (!best || pending[i].id < best->id)) {
                best = &pending[i];
            }
        }
        if (!best) {
            break;
        }
        ids[batch++] = best->id;
        bytes += best->bytes;
        after = best->id;
    }
    if (batch == 0) {
        return false;
    }

    /* Assembled with memcpy after the whole line is known to fit: no
     * snprintf into a caller-sized buffer for the format-truncation gate. */
    static const char PREFIX[] = "Trip logs ready: trips ";
    static const char SUFFIX[] = " bytes total).";
    char total_text[24];
    int total_len = snprintf(total_text, sizeof(total_text), "%llu", (unsigned long long)bytes);
    if (total_len < 0 || (size_t)total_len >= sizeof(total_text)) {
        return false;
    }
    size_t need = (sizeof(PREFIX) - 1) + 2 + (size_t)total_len + (sizeof(SUFFIX) - 1) + 1;
    for (int i = 0; i < batch; i++) {
        char id_text[16];
        int id_len = snprintf(id_text, sizeof(id_text), "%u", (unsigned)ids[i]);
        if (id_len < 0 || (size_t)id_len >= sizeof(id_text)) {
            return false;
        }
        need += (size_t)id_len + (i ? 2 : 0);
    }
    if (need > cap) {
        return false;
    }
    char *p = out;
    memcpy(p, PREFIX, sizeof(PREFIX) - 1);
    p += sizeof(PREFIX) - 1;
    for (int i = 0; i < batch; i++) {
        if (i) {
            memcpy(p, ", ", 2);
            p += 2;
        }
        int id_len = snprintf(p, 16, "%u", (unsigned)ids[i]);
        p += id_len;
    }
    memcpy(p, " (", 2);
    p += 2;
    memcpy(p, total_text, (size_t)total_len);
    p += total_len;
    memcpy(p, SUFFIX, sizeof(SUFFIX) - 1);
    p += sizeof(SUFFIX) - 1;
    *p = '\0';

    for (int i = 0; i < batch; i++) {
        d->announced[d->announced_count++] = ids[i];
    }
    return true;
}

void muse_doorbell_pending_text(int count, uint32_t total_bytes, char *out, size_t cap)
{
    if (count <= 0) {
        snprintf(out, cap, "No logs waiting");
    } else {
        snprintf(out, cap, "%d waiting, %u KB", count, (unsigned)(total_bytes / 1024));
    }
}
