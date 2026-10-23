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

/*
 * A fabricated read-only FAT16 volume whose files are the closed OBD trip
 * logs (OBD log USB drive mode). Pure C over caller-supplied file reads, so
 * the host tests exercise the whole volume: boot sector, both FATs, the root
 * directory and INDEX.TXT are generated; trip bytes come through a callback
 * (on device, muse_obd_log_read over the snapshot's trip ids; on host, a
 * memory buffer). There is no stored image and no FAT partition.
 *
 * Snapshot semantics: the caller fixes the trip list when drive mode is
 * entered. Trips that close later are not in it and never appear until the
 * mode is re-entered; the open trip is never listed (the store only lists
 * closed trips). Closed trips are immutable, so reads never race the logger.
 */

#define MUSE_MSC_SECTOR 512u
#define MUSE_MSC_CLUSTER_SECTORS 8u /* 4 KB clusters */
#define MUSE_MSC_CLUSTER (MUSE_MSC_SECTOR * MUSE_MSC_CLUSTER_SECTORS)
#define MUSE_MSC_ROOT_SECTORS 16u /* 256 root entries */
#define MUSE_MSC_MAX_TRIPS 200
#define MUSE_MSC_LABEL "MUSE LOGS  " /* 11 bytes, space padded */
#define MUSE_MSC_INDEX_NAME "INDEX   TXT"

/* One file on the drive: a closed trip (trip_id != 0) or INDEX.TXT (index 0). */
typedef struct {
    uint32_t trip_id;
    uint32_t size;    /* bytes; for INDEX.TXT, the generated length */
    uint32_t samples;
    uint32_t crc32;
} muse_msc_file_t;

/* Reads len bytes of file file_index (0 = INDEX.TXT, 1..count-1 = trips in
 * snapshot order) at offset into buf; returns bytes read (0 past the end). */
typedef size_t (*muse_msc_read_fn)(void *ctx, int file_index, uint32_t offset, uint8_t *buf, size_t len);

typedef struct {
    muse_msc_file_t files[MUSE_MSC_MAX_TRIPS + 1]; /* [0] is INDEX.TXT */
    int count;                                     /* files, INDEX included */
    uint32_t fat_sectors;                          /* per FAT copy */
    uint32_t total_sectors;
    uint32_t data_clusters; /* clusters used by files, from cluster 2 */
} muse_msc_snapshot_t;

/* "TRIP0005.CSV" for trip 5 (minimum 4 digits, more when the id needs them;
 * ids are capped by the store at 7 digits, so 12 chars + NUL always fit). */
void muse_msc_trip_name(uint32_t id, char out[16]); /* "TRIP1234567.CSV" + NUL */

/* INDEX.TXT's content for trips[0..n): a first line identifying the gadget
 * and firmware, then one line per trip (id, bytes, samples, CRC32). LF
 * endings. Returns the length, or 0 when cap is too small. firmware may be
 * NULL ("unknown"). */
size_t muse_msc_index_build(char *buf, size_t cap, const muse_msc_file_t *trips, int n, const char *firmware);

/*
 * Fixes the snapshot: files[0] is INDEX.TXT (index_len from
 * muse_msc_index_build), then the trips in the given order. Computes the
 * geometry. False when there are too many trips.
 */
bool muse_msc_snapshot_init(muse_msc_snapshot_t *snap, const muse_msc_file_t *trips, int n, uint32_t index_len);

/* Sector lba into buf (MUSE_MSC_SECTOR bytes). File bytes come from read
 * (ctx passed through); metadata sectors are generated. False past the end. */
bool muse_msc_read_sector(const muse_msc_snapshot_t *snap, uint32_t lba, uint8_t *buf, muse_msc_read_fn read,
                          void *ctx);
