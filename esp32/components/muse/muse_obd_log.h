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

#include "muse_obd.h"
#include "muse_obd_log_store.h"

/*
 * The OBD trip log (CONFIG_MUSE_OBD_VTEC): while the OBD link polls, each
 * sample goes to a CSV per trip, in one of two stores: a microSD card when
 * one is mounted, else the internal flash's "obdlog" FAT partition (present
 * in partitions_muse.csv, the 16 MB boards' table). A trip starts when
 * polling starts and ends when the link drops; the file is then closed and
 * listed in its store's manifest (muse_obd_log_store.h has the formats).
 * Muse pulls trips over Link (obd.trips, obd.trip_read) and deletes each
 * after checking it (obd.trip_delete): the gadget never deletes on its own,
 * and a full store stops taking trips rather than overwrite one.
 *
 * Logging never holds up polling: the OBD task only formats a row and queues
 * it; a low-priority writer task does the file work, in blocks, and a row
 * that finds the queue full is counted as dropped. With no store, nothing is
 * logged and the OBD work carries on as before.
 *
 * The s18's TF slot has no driver in this tree (its card select sits on the
 * IO expander, held high), so the card store is a stub that never mounts.
 */

/* Mounts the stores and starts the writer task. Once, at boot (muse_obd_start). */
void muse_obd_log_start(void);

/* The OBD task's side, which never blocks: polling has started (a trip
 * begins, if a store can take it; did_2612 is this connection's DID probe,
 * MUSE_LOG_DID_*, for the header); a sample, after each RPM reply (now_ms on
 * the OBD task's clock); the link dropped (the trip is closed). */
void muse_obd_log_trip_start(const muse_obd_status_t *st, uint8_t did_2612, uint32_t now_ms);
void muse_obd_log_sample(const muse_obd_status_t *st, uint32_t now_ms);
void muse_obd_log_trip_end(void);

/* The row muse_obd_log_sample() logs for a snapshot: each channel the
 * snapshot has a current reading for. */
void muse_obd_log_sample_of(const muse_obd_status_t *st, uint32_t ms, muse_log_sample_t *out);

typedef struct {
    bool card;              /* a microSD card is mounted */
    bool flash;             /* the obdlog partition is mounted */
    const char *store;      /* where the current or next trip goes: "sd", "flash" or NULL */
    bool active;            /* a trip file is open */
    bool trip_valid;        /* trip is the open trip's id */
    uint32_t trip;
    uint32_t pending;       /* completed trips not yet deleted, both stores */
    uint32_t dropped;       /* samples dropped this trip */
    bool full;              /* a store filled up: it takes no more trips until one is deleted */
} muse_obd_log_status_t;

/* Never blocks; any task. */
void muse_obd_log_status(muse_obd_log_status_t *out);

/* ---- The pull commands' side (obd.trips, obd.trip_read, obd.trip_delete),
 * from any task; they work with OBD turned off too. Each may wait briefly
 * for the writer to finish a block. ---- */

#define MUSE_OBD_LOG_CHUNK 49152   /* the most obd.trip_read returns at once (65536 base64 chars) */

typedef struct {
    muse_log_trip_t trip;
    const char *store;      /* "sd" or "flash" */
} muse_obd_log_entry_t;

/* Up to max completed trips into out, card first, oldest first in each;
 * returns how many there are in all (may be more than max), or -1 if the
 * stores are busy. */
int muse_obd_log_list(muse_obd_log_entry_t *out, int max);

/* Up to cap bytes of trip id from offset (as muse_log_store_read, which
 * gives the errors), whichever store has it. MUSE_LOG_READ_IO also when the
 * stores are busy. */
int muse_obd_log_read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *total);

/* Removes trip id from whichever store has it: MUSE_LOG_READ_OK, or
 * MUSE_LOG_READ_UNKNOWN for no such trip, MUSE_LOG_READ_IO when the stores
 * are busy or the manifest couldn't be rewritten. The open trip isn't listed
 * until it closes, so it can't be deleted. */
int muse_obd_log_delete(uint32_t id);
