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
#include <stdio.h>

/*
 * The OBD trip log's files (muse_obd_log.c), as plain C over stdio with no
 * FreeRTOS, NVS or mounting in it, so the host tests can run a whole store in
 * a temporary directory. A store is one directory (the flash partition's
 * FAT, or a microSD card's) holding one CSV per trip and a manifest.
 *
 * A trip file: '#' header lines (format, firmware, trip id, protocol, the
 * supported-PID masks, the start time), the column names, one row per
 * sample, and a '#' trailer written when the trip closes:
 *
 *   # muse obd trip log v1
 *   # firmware=1a2b3c4
 *   # trip=12
 *   # protocol=CAN 29/500
 *   # supported_00=BE1FA813 supported_20=80018001 supported_40=FED08000
 *   # start=2026-10-23T18:02:11Z            (or: # t0=boot boot_ms=81234)
 *   # did_2612=yes                          (only once the probe has said, yes or no)
 *   ms,rpm,coolant_c,iat_c,...
 *   0,812,71,24,...
 *   ...
 *   # end samples=5123 dropped=0
 *
 * The manifest (MUSE_LOG_MANIFEST) has one JSON object per line, one line
 * per trip in the store, appended when a trip closes and rewritten without
 * the line when one is deleted. File names are 8.3 (FAT without long names).
 */

#define MUSE_LOG_MANIFEST "TRIPS.JSL"
#define MUSE_LOG_MANIFEST_TMP "TRIPS.TMP"
#define MUSE_LOG_NAME_MAX 13          /* "T0000012.CSV" and its NUL */
#define MUSE_LOG_PATH_MAX 48          /* a store's directory, '/', a name */
#define MUSE_LOG_DIR_MAX (MUSE_LOG_PATH_MAX - MUSE_LOG_NAME_MAX - 1)
#define MUSE_LOG_ID_MAX 9999999u      /* seven digits in the file name */

/* The columns after `ms`, in the file's order. */
typedef enum {
    MUSE_LOG_RPM,
    MUSE_LOG_COOLANT,
    MUSE_LOG_IAT,
    MUSE_LOG_TIMING,
    MUSE_LOG_MAP,
    MUSE_LOG_LOAD,
    MUSE_LOG_THROTTLE,
    MUSE_LOG_SPEED,
    MUSE_LOG_STFT,
    MUSE_LOG_LTFT,
    MUSE_LOG_AMBIENT,
    MUSE_LOG_VTEC_SOL,        /* DID 0x2612's VTEC solenoid, 1/0 */
    MUSE_LOG_VTEC_PSW,        /* and its pressure switch, 1/0 */
    MUSE_LOG_CHANNELS,
} muse_log_channel_t;

/* Each column's name, in that order ("rpm", "coolant_c", ...). */
extern const char *const MUSE_LOG_COLUMN[MUSE_LOG_CHANNELS];

/* One sample: trip-relative ms and each channel's value, if it has one. */
typedef struct {
    uint32_t ms;
    bool has[MUSE_LOG_CHANNELS];
    int value[MUSE_LOG_CHANNELS];
} muse_log_sample_t;

/* Room for any row: ten digits of ms and eleven digits a channel, commas, '\n'. */
#define MUSE_LOG_ROW_MAX (10 + MUSE_LOG_CHANNELS * 12 + 2)

/* One CSV row, '\n' included, into buf (not NUL-terminated); its length, or 0
 * when it doesn't fit in cap. A channel without a value is an empty field. */
size_t muse_log_format_row(char *buf, size_t cap, const muse_log_sample_t *s);

/* What the header says about a trip. */
typedef struct {
    uint32_t id;
    char firmware[16];        /* short commit hash; "" if not known */
    char protocol[20];        /* "CAN 29/500"; "" if not known */
    bool support_valid[3];    /* PIDs 01-20, 21-40, 41-60 */
    uint32_t support[3];
    int64_t started;          /* Unix seconds; < 0 when the clock isn't set */
    uint32_t boot_ms;         /* ms since boot at the start */
    uint8_t did_2612;         /* MUSE_LOG_DID_*: does the engine ECU answer DID 0x2612? */
} muse_log_meta_t;

#define MUSE_LOG_DID_UNKNOWN 0    /* not asked (yet): no line */
#define MUSE_LOG_DID_YES 1
#define MUSE_LOG_DID_NO 2

#define MUSE_LOG_HEADER_MAX 512

/* The header block, the column names included, into buf (not NUL-terminated);
 * its length, or 0 when it doesn't fit in cap. */
size_t muse_log_format_header(char *buf, size_t cap, const muse_log_meta_t *m);

/* A trip as the manifest lists it. */
typedef struct {
    uint32_t id;
    uint32_t bytes;           /* the file's size */
    uint32_t samples;
    uint32_t dropped;         /* samples lost to a stalled or full store */
    int64_t started, ended;   /* Unix seconds; < 0 when not known */
    uint32_t crc32;           /* of the file's bytes, muse_log_crc32 */
} muse_log_trip_t;

#define MUSE_LOG_LINE_MAX 192

/* The trip's manifest line, '\n' included, NUL-terminated; its length, or 0
 * when it doesn't fit in cap. */
size_t muse_log_manifest_line(char *buf, size_t cap, const muse_log_trip_t *t);
/* Reads a line written by muse_log_manifest_line; false for anything else. */
bool muse_log_manifest_parse(const char *line, muse_log_trip_t *t);

/* CRC-32 as zlib's crc32() and Python's zlib.crc32(): start with 0 and pass
 * the result back in for the next piece. */
uint32_t muse_log_crc32(uint32_t crc, const void *data, size_t len);

/* Standard base64 with padding into out, NUL-terminated; its length, or 0
 * when it doesn't fit (cap must hold 4 * ceil(len / 3) + 1). */
#define MUSE_LOG_B64_LEN(n) (((n) + 2) / 3 * 4)
size_t muse_log_base64(char *out, size_t cap, const uint8_t *in, size_t len);

/* "T0000012.CSV" for trip 12. */
void muse_log_file_name(uint32_t id, char out[MUSE_LOG_NAME_MAX]);

/* ---- A store: a directory ---- */

/*
 * Up to max of the store's trips into out (in manifest order: oldest first);
 * returns how many the store has (which may be more than max), or -1 when
 * the manifest can't be read. A store without a manifest has none.
 */
int muse_log_store_list(const char *dir, muse_log_trip_t *out, int max);
/* The manifest's entry for id. */
bool muse_log_store_find(const char *dir, uint32_t id, muse_log_trip_t *out);
/* The highest id in the manifest; 0 for none. */
uint32_t muse_log_store_max_id(const char *dir);

typedef enum {
    MUSE_LOG_READ_OK = 0,
    MUSE_LOG_READ_UNKNOWN = -1,     /* no such trip in this store */
    MUSE_LOG_READ_PAST_END = -2,    /* offset > the file's size */
    MUSE_LOG_READ_IO = -3,
} muse_log_read_err_t;

/*
 * Up to cap bytes of trip id's file from offset into buf; returns how many
 * (0 at offset == size: the end), or a muse_log_read_err_t. *total is the
 * file's size.
 */
int muse_log_store_read(const char *dir, uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *total);

/* Removes trip id: its manifest line first, then its file. False when the
 * store has no such trip or the manifest couldn't be rewritten. */
bool muse_log_store_delete(const char *dir, uint32_t id);

/*
 * After a reset mid-trip or mid-delete: a manifest that only exists as
 * MUSE_LOG_MANIFEST_TMP is put back, and a trip file with no manifest line
 * gets one (its size and CRC, its rows counted, times unknown). Returns how
 * many trips were added.
 */
int muse_log_store_recover(const char *dir);

/* ---- Writing a trip ---- */

typedef struct {
    FILE *f;
    char dir[MUSE_LOG_DIR_MAX];
    uint32_t id;
    int64_t started;
    char *buf;                /* the caller's block buffer */
    size_t cap, len;
    uint32_t buf_rows;        /* rows in buf, not yet written */
    uint32_t samples;         /* rows written or in buf */
    uint32_t dropped;
    uint32_t written;         /* bytes in the file */
    uint32_t limit;           /* the file may not grow past this */
    bool full;                /* hit the limit or a short write: no more rows */
} muse_log_writer_t;

/*
 * Creates trip m->id's file in dir and writes its header. buf (cap bytes) is
 * where rows wait to be written in one block; limit caps the file's size
 * (the store's free space less a reserve), and a trip whose header doesn't
 * fit under it doesn't open. False if the file can't be made.
 */
bool muse_log_trip_open(muse_log_writer_t *w, const char *dir, const muse_log_meta_t *m, char *buf, size_t cap,
                        uint32_t limit);
/* Adds a row (from muse_log_format_row); writes the block when it fills.
 * Once full, rows are counted as dropped. */
void muse_log_trip_row(muse_log_writer_t *w, const char *row, size_t len);
/* Rows lost before reaching the writer (a stalled queue). */
void muse_log_trip_drop(muse_log_writer_t *w, uint32_t n);
/* Writes what is waiting. False on a short write: the store is full. */
bool muse_log_trip_flush(muse_log_writer_t *w);
/*
 * Writes the rest and the trailer, closes the file and appends the trip's
 * manifest line (its size and CRC read back from the file). ended: Unix
 * seconds or < 0. *out (if not NULL) gets the entry. False if the manifest
 * line couldn't be written; the file is closed either way.
 */
bool muse_log_trip_close(muse_log_writer_t *w, int64_t ended, muse_log_trip_t *out);

/*
 * The next trip id to hand out: the persisted counter (the next id), or one
 * past the highest id in any store if that is further, so a counter that was
 * lost or reset never hands out an id again. Never 0.
 */
uint32_t muse_log_next_id(uint32_t stored_next, uint32_t max_seen);
