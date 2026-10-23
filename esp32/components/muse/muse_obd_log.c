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

#include "muse_obd_log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "muse_build_info.h"
#include "muse_mem.h"

static const char *TAG = "muse_obd_log";

#define FLASH_BASE "/obdlog"
#define FLASH_LABEL "obdlog"
#define CARD_DIR "/sdcard/obd"
#define NVS_NS "muse"                 /* beside Settings' obd_on (muse_settings.c) */
#define NVS_NEXT_ID "obd_trip"        /* the next trip id; ids are never reused */
#define BLOCK 4096                    /* rows reach the file in blocks this big */
#define QUEUE_LEN 64                  /* rows waiting for the writer: ~8 s of polling */
#define RESERVE (64 * 1024)           /* kept free in a store: the trailer, the manifest */
#define MIN_TRIP (16 * 1024)          /* less room than RESERVE + this: the store is full */
#define LOCK_MS 2000                  /* a pull command waits this long for the writer */
#define WAKE_MS 1000                  /* the writer looks for a missed trip end this often */
#define CLOCK_SET 1700000000          /* time(): later than 2023-11, the clock was set (SNTP) */

typedef enum { STORE_SD, STORE_FLASH, STORES } store_id_t;

typedef struct {
    const char *name;     /* "sd", "flash": obd.trips' store */
    const char *dir;
    const char *base;     /* the mount point, for esp_vfs_fat_info */
    bool mounted;
    bool full;
    uint32_t trips;       /* in its manifest */
} store_t;

static store_t s_stores[STORES] = {
    [STORE_SD] = { "sd", CARD_DIR, "/sdcard" },
    [STORE_FLASH] = { "flash", FLASH_BASE, FLASH_BASE },
};

typedef enum { IT_START = 1, IT_ROW, IT_END } item_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t len;                     /* IT_ROW: bytes of row */
    uint32_t seq;                    /* the trip it belongs to (s_seq) */
    union {
        char row[MUSE_LOG_ROW_MAX];
        muse_log_meta_t meta;        /* IT_START, less the id and firmware */
    };
} item_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;     /* the stores' files: the writer and the pull commands */
static char *s_block;
static volatile bool s_ready;        /* mounted, recovered, counted */
static uint32_t s_next_id;           /* the writer's (under s_lock) */

/* The OBD task's side. */
static uint32_t s_seq;               /* this trip's number, counting since boot */
static bool s_logging;               /* this trip's rows are queued */
static uint32_t s_t0;                /* the trip's start, the OBD task's ms */
static atomic_uint s_lost;           /* rows that found the queue full, this trip */
static atomic_uint s_end_seq;        /* a trip end that didn't fit in the queue */

/* The writer's open trip (under s_lock). */
static muse_log_writer_t s_w;
static bool s_open;
static uint32_t s_open_seq;
static store_id_t s_open_store;

/* muse_obd_log_status()'s snapshot. */
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_obd_log_status_t s_status;

/* ---- The status snapshot: the writer refreshes it after each change ---- */

/* Where a trip starting now would go. */
static int pick_store(void)
{
    for (int i = 0; i < STORES; i++) {
        if (s_stores[i].mounted && !s_stores[i].full) {
            return i;
        }
    }
    return -1;
}

static void publish(void)
{
    muse_obd_log_status_t st = {
        .card = s_stores[STORE_SD].mounted,
        .flash = s_stores[STORE_FLASH].mounted,
        .active = s_open,
        .trip_valid = s_open,
        .trip = s_open ? s_w.id : 0,
        .pending = s_stores[STORE_SD].trips + s_stores[STORE_FLASH].trips,
        .dropped = s_open ? s_w.dropped : 0,
        .full = s_stores[STORE_SD].full || s_stores[STORE_FLASH].full,
    };
    int next = s_open ? (int)s_open_store : pick_store();
    st.store = next >= 0 ? s_stores[next].name : NULL;
    taskENTER_CRITICAL(&s_status_lock);
    s_status = st;
    taskEXIT_CRITICAL(&s_status_lock);
}

void muse_obd_log_status(muse_obd_log_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_status_lock);
    if (out->active) {
        out->dropped += atomic_load(&s_lost);   /* not yet handed to the writer */
    }
}

/* ---- Mounting ---- */

/* The s18's TF slot: no driver in this tree (see muse_obd_log.h). A board
 * that mounts a card at /sdcard would check for it here. */
static bool mount_card(void)
{
    return false;
}

/* The obdlog partition, wear-levelled FAT. Formatted only when it holds no
 * filesystem yet (the first boot after the table gained it) or a broken
 * one: a good one, and the trips in it, survive reflashing the app. */
static bool mount_flash(void)
{
    if (!esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, FLASH_LABEL)) {
        ESP_LOGI(TAG, "no %s partition: no flash store", FLASH_LABEL);
        return false;
    }
    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = true,   /* FatFs: no filesystem, or a corrupt one, only */
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_BASE, FLASH_LABEL, &config, &wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flash store: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Free bytes in the store's filesystem, or 0 if unknown. */
static uint64_t free_bytes(const store_t *s)
{
    uint64_t total = 0, free = 0;
    return esp_vfs_fat_info(s->base, &total, &free) == ESP_OK ? free : 0;
}

/* The store has room for a trip (and clears or sets full to say so). */
static bool has_room(store_t *s)
{
    s->full = free_bytes(s) < RESERVE + MIN_TRIP;
    return !s->full;
}

static void count_trips(store_t *s)
{
    int n = muse_log_store_list(s->dir, NULL, 0);
    s->trips = n > 0 ? (uint32_t)n : 0;
}

static void open_store(store_t *s)
{
    int added = muse_log_store_recover(s->dir);
    if (added) {
        ESP_LOGW(TAG, "%s: %d trip(s) cut short by a reset, listed", s->name, added);
    }
    count_trips(s);
    has_room(s);
    ESP_LOGI(TAG, "%s store: %u trip(s) waiting to be pulled%s", s->name, (unsigned)s->trips,
             s->full ? ", full" : "");
}

/* ---- The id counter, in NVS ---- */

static uint32_t load_next_id(void)
{
    nvs_handle_t h;
    uint32_t next = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_NEXT_ID, &next);
        nvs_close(h);
    }
    return next;
}

static void save_next_id(uint32_t next)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_NEXT_ID, next);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---- The writer task ---- */

static void take_lost(void)
{
    unsigned lost = atomic_exchange(&s_lost, 0);
    if (lost && s_open) {
        muse_log_trip_drop(&s_w, lost);
    }
}

static void close_trip(void)
{
    if (!s_open) {
        return;
    }
    take_lost();
    store_t *s = &s_stores[s_open_store];
    bool full = s_w.full;
    time_t now = time(NULL);
    muse_log_trip_t t;
    if (muse_log_trip_close(&s_w, now >= CLOCK_SET ? (int64_t)now : -1, &t)) {
        ESP_LOGI(TAG, "trip %u logged to %s: %u samples (%u dropped), %u bytes, crc32 %08x", (unsigned)t.id,
                 s->name, (unsigned)t.samples, (unsigned)t.dropped, (unsigned)t.bytes, (unsigned)t.crc32);
    } else {
        ESP_LOGW(TAG, "trip %u: couldn't list it in the %s manifest", (unsigned)t.id, s->name);
    }
    count_trips(s);
    if (full) {
        s->full = true;
        ESP_LOGW(TAG, "%s store full: no more trips there until some are pulled and deleted", s->name);
    }
    s_open = false;
}

static void start_trip(const item_t *it)
{
    close_trip();   /* the last one's end went missing */
    for (int i = 0; i < STORES; i++) {
        if (s_stores[i].mounted && !s_stores[i].full) {
            has_room(&s_stores[i]);
        }
    }
    int pick = pick_store();
    if (pick < 0) {
        ESP_LOGI(TAG, "no store with room: this trip isn't logged");
        return;
    }
    store_t *s = &s_stores[pick];
    muse_log_meta_t m = it->meta;
    m.id = s_next_id;
    strlcpy(m.firmware, muse_build_commit(), sizeof(m.firmware));
    uint64_t room = free_bytes(s) - RESERVE;
    uint32_t limit = room > UINT32_MAX ? UINT32_MAX : (uint32_t)room;
    if (!muse_log_trip_open(&s_w, s->dir, &m, s_block, BLOCK, limit)) {
        ESP_LOGW(TAG, "couldn't start trip %u in the %s store", (unsigned)m.id, s->name);
        return;
    }
    /* Taken for good once the file exists: a reset can't hand it out again. */
    s_next_id = m.id + 1;
    save_next_id(s_next_id);
    s_open = true;
    s_open_seq = it->seq;
    s_open_store = (store_id_t)pick;
    atomic_store(&s_lost, 0);
    ESP_LOGI(TAG, "trip %u: logging to %s", (unsigned)m.id, s->name);
}

static void handle(const item_t *it)
{
    switch (it->kind) {
    case IT_START:
        start_trip(it);
        break;
    case IT_ROW:
        if (s_open && it->seq == s_open_seq) {
            take_lost();
            muse_log_trip_row(&s_w, it->row, it->len);
        }
        break;
    case IT_END:
        if (s_open && it->seq == s_open_seq) {
            close_trip();
        }
        break;
    default:
        break;
    }
}

static void log_task(void *arg)
{
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stores[STORE_SD].mounted = mount_card();
    s_stores[STORE_FLASH].mounted = mount_flash();
    uint32_t max_seen = 0;
    for (int i = 0; i < STORES; i++) {
        if (s_stores[i].mounted) {
            open_store(&s_stores[i]);
            uint32_t m = muse_log_store_max_id(s_stores[i].dir);
            max_seen = m > max_seen ? m : max_seen;
        }
    }
    s_next_id = muse_log_next_id(load_next_id(), max_seen);
    publish();
    xSemaphoreGive(s_lock);
    s_ready = true;

    for (;;) {
        item_t it;
        bool got = xQueueReceive(s_queue, &it, pdMS_TO_TICKS(WAKE_MS)) == pdTRUE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (got) {
            handle(&it);
            /* Everything already waiting, in one go. */
            while (xQueueReceive(s_queue, &it, 0) == pdTRUE) {
                handle(&it);
            }
        }
        unsigned end = atomic_load(&s_end_seq);
        if (s_open && end == s_open_seq) {
            close_trip();
        }
        publish();
        xSemaphoreGive(s_lock);
    }
}

void muse_obd_log_start(void)
{
    if (s_queue) {
        return;
    }
    s_queue = xQueueCreateWithCaps(QUEUE_LEN, sizeof(item_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    s_block = heap_caps_malloc(BLOCK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* An internal-RAM stack: this task is the one that writes the flash. */
    if (!s_queue || !s_lock || !s_block
        || xTaskCreate(log_task, "muse_obd_log", 5120, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start: no trip logging");
        return;
    }
}

/* ---- The OBD task's side ---- */

void muse_obd_log_sample_of(const muse_obd_status_t *st, uint32_t ms, muse_log_sample_t *out)
{
    static const struct {
        muse_log_channel_t ch;
        muse_obd_extra_t extra;
    } EXTRAS[] = {
        { MUSE_LOG_IAT, MUSE_OBD_IAT },       { MUSE_LOG_TIMING, MUSE_OBD_TIMING },
        { MUSE_LOG_MAP, MUSE_OBD_MAP },       { MUSE_LOG_LOAD, MUSE_OBD_LOAD },
        { MUSE_LOG_THROTTLE, MUSE_OBD_THROTTLE }, { MUSE_LOG_SPEED, MUSE_OBD_SPEED },
        { MUSE_LOG_STFT, MUSE_OBD_STFT },     { MUSE_LOG_LTFT, MUSE_OBD_LTFT },
        { MUSE_LOG_AMBIENT, MUSE_OBD_AMBIENT },
    };
    memset(out, 0, sizeof(*out));
    out->ms = ms;
    out->has[MUSE_LOG_RPM] = st->rpm_valid;
    out->value[MUSE_LOG_RPM] = st->rpm;
    out->has[MUSE_LOG_COOLANT] = st->coolant_valid;
    out->value[MUSE_LOG_COOLANT] = st->coolant_c;
    for (size_t i = 0; i < sizeof(EXTRAS) / sizeof(EXTRAS[0]); i++) {
        out->has[EXTRAS[i].ch] = st->extra[EXTRAS[i].extra].valid;
        out->value[EXTRAS[i].ch] = st->extra[EXTRAS[i].extra].value;
    }
    out->has[MUSE_LOG_VTEC_SOL] = st->vtec_sol.valid;
    out->value[MUSE_LOG_VTEC_SOL] = st->vtec_sol.value != 0;
    out->has[MUSE_LOG_VTEC_PSW] = st->vtec_psw.valid;
    out->value[MUSE_LOG_VTEC_PSW] = st->vtec_psw.value != 0;
}

void muse_obd_log_trip_start(const muse_obd_status_t *st, uint8_t did_2612, uint32_t now_ms)
{
    muse_obd_log_trip_end();   /* never two at once */
    muse_obd_log_status_t ls;
    muse_obd_log_status(&ls);
    if (!s_ready || !ls.store) {
        return;   /* no store: the OBD work carries on, unlogged */
    }
    item_t it = { .kind = IT_START, .seq = ++s_seq };
    muse_log_meta_t *m = &it.meta;
    if (st->protocol > 0) {
        strlcpy(m->protocol, st->protocol_name, sizeof(m->protocol));
    }
    for (int i = 0; i < 3 && i < MUSE_OBD_SUPPORT_RANGES; i++) {
        m->support_valid[i] = st->support_valid[i];
        m->support[i] = st->support[i];
    }
    time_t now = time(NULL);
    m->started = now >= CLOCK_SET ? (int64_t)now : -1;
    m->boot_ms = now_ms;
    m->did_2612 = did_2612;
    atomic_store(&s_lost, 0);
    s_t0 = now_ms;
    s_logging = xQueueSend(s_queue, &it, 0) == pdTRUE;
}

void muse_obd_log_sample(const muse_obd_status_t *st, uint32_t now_ms)
{
    if (!s_logging) {
        return;
    }
    muse_log_sample_t s;
    muse_obd_log_sample_of(st, now_ms - s_t0, &s);
    item_t it = { .kind = IT_ROW, .seq = s_seq };
    size_t n = muse_log_format_row(it.row, sizeof(it.row), &s);
    it.len = (uint8_t)n;
    if (!n || xQueueSend(s_queue, &it, 0) != pdTRUE) {
        atomic_fetch_add(&s_lost, 1);   /* the writer is behind: dropped, counted */
    }
}

void muse_obd_log_trip_end(void)
{
    if (!s_logging) {
        return;
    }
    s_logging = false;
    item_t it = { .kind = IT_END, .seq = s_seq };
    if (xQueueSend(s_queue, &it, 0) != pdTRUE) {
        atomic_store(&s_end_seq, s_seq);   /* the writer closes it once it catches up */
    }
}

/* ---- The pull commands ---- */

static bool lock(void)
{
    return s_ready && xSemaphoreTake(s_lock, pdMS_TO_TICKS(LOCK_MS)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

int muse_obd_log_list(muse_obd_log_entry_t *out, int max)
{
    if (!s_ready) {
        return 0;   /* still mounting: nothing to list yet */
    }
    if (!lock()) {
        return -1;
    }
    int total = 0;
    for (int i = 0; i < STORES && total >= 0; i++) {
        if (!s_stores[i].mounted) {
            continue;
        }
        int n = muse_log_store_list(s_stores[i].dir, NULL, 0);
        muse_log_trip_t *trips = n > 0 ? heap_caps_malloc(sizeof(*trips) * (size_t)n, MUSE_BIG_CAPS) : NULL;
        if (n > 0 && !trips) {
            total = -1;
            break;
        }
        int got = n > 0 ? muse_log_store_list(s_stores[i].dir, trips, n) : 0;
        for (int j = 0; j < got && j < n; j++) {
            if (out && total < max) {
                out[total].trip = trips[j];
                out[total].store = s_stores[i].name;
            }
            total++;
        }
        free(trips);
    }
    unlock();
    return total;
}

static int find_store(uint32_t id)
{
    for (int i = 0; i < STORES; i++) {
        if (s_stores[i].mounted && muse_log_store_find(s_stores[i].dir, id, NULL)) {
            return i;
        }
    }
    return -1;
}

int muse_obd_log_read(uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *total)
{
    if (!lock()) {
        return s_ready ? MUSE_LOG_READ_IO : MUSE_LOG_READ_UNKNOWN;
    }
    int i = find_store(id);
    int n = i < 0 ? MUSE_LOG_READ_UNKNOWN : muse_log_store_read(s_stores[i].dir, id, offset, buf, cap, total);
    unlock();
    return n;
}

int muse_obd_log_delete(uint32_t id)
{
    if (!lock()) {
        return s_ready ? MUSE_LOG_READ_IO : MUSE_LOG_READ_UNKNOWN;
    }
    int i = find_store(id);
    int res = i < 0 ? MUSE_LOG_READ_UNKNOWN
                    : muse_log_store_delete(s_stores[i].dir, id) ? MUSE_LOG_READ_OK : MUSE_LOG_READ_IO;
    if (res == MUSE_LOG_READ_OK) {
        store_t *s = &s_stores[i];
        count_trips(s);
        if (s->full) {
            has_room(s);   /* space came back: it can take trips again */
        }
        publish();
        ESP_LOGI(TAG, "trip %u deleted from %s", (unsigned)id, s->name);
    }
    unlock();
    return res;
}
