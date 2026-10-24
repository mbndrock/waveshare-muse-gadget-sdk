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
 * Settings' OBD switch (muse_obd_set_enabled) against the real OBD task:
 * muse_obd.c runs on a thread over obd_enable_fakes/, a scripted LELink^2
 * answers its ELM commands, and time is a virtual clock that moves only while
 * the task is blocked. Checks that off at boot never scans; that turning it
 * off while polling, scanning or connecting drops the link (a connection that
 * beats the cancel is taken down too), reports "off" with no readings and
 * then makes no BLE call however long it waits; that the reactions don't
 * play; and that turning it back on scans again without a restart.
 */
#define _POSIX_C_SOURCE 200809L   /* nanosleep */
#include "../components/muse/muse_obd.c"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "vtec_did_frames.h"
#include "did_cards_frames.h"

/* ---- The fakes' plumbing: one lock for everything, a virtual clock ---- */

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_crit = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_now;          /* virtual ms */
static unsigned g_notify;
static bool g_blocked;          /* the task is waiting on the clock or a notification */

void obd_fake_enter_critical(portMUX_TYPE *mux)
{
    (void)mux;
    pthread_mutex_lock(&g_crit);
}

void obd_fake_exit_critical(portMUX_TYPE *mux)
{
    (void)mux;
    pthread_mutex_unlock(&g_crit);
}

TickType_t xTaskGetTickCount(void)
{
    pthread_mutex_lock(&g_mu);
    TickType_t t = (TickType_t)g_now;
    pthread_mutex_unlock(&g_mu);
    return t;
}

static void deliver_reply(void);

static uint32_t block(TickType_t ticks, bool notify, bool clear)
{
    deliver_reply();   /* the adapter answers while the task waits, as over the air */
    pthread_mutex_lock(&g_mu);
    uint64_t deadline = ticks == portMAX_DELAY ? UINT64_MAX : g_now + ticks;
    while ((!notify || !g_notify) && g_now < deadline) {
        g_blocked = true;
        pthread_cond_broadcast(&g_cv);
        pthread_cond_wait(&g_cv, &g_mu);
    }
    g_blocked = false;
    uint32_t got = notify ? g_notify : 0;
    if (notify && got) {
        g_notify = clear ? 0 : g_notify - 1;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}

void vTaskDelay(TickType_t ticks)
{
    block(ticks, false, false);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    return block(ticks, true, clear);
}

BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    pthread_mutex_lock(&g_mu);
    g_notify++;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    return pdTRUE;
}

struct obd_fake_task { pthread_t thread; void (*fn)(void *); void *arg; };

static void *task_main(void *p)
{
    struct obd_fake_task *t = p;
    t->fn(t->arg);
    return NULL;
}

BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                               TaskHandle_t *out, uint32_t caps)
{
    (void)name, (void)stack, (void)prio, (void)caps;
    static struct obd_fake_task task;
    task.fn = fn;
    task.arg = arg;
    *out = &task;
    if (pthread_create(&task.thread, NULL, task_main, &task) != 0) {
        return pdFALSE;
    }
    pthread_detach(task.thread);
    return pdPASS;
}

struct obd_fake_queue { size_t item, cap, head, n; uint8_t *buf; };

QueueHandle_t xQueueCreateWithCaps(UBaseType_t len, UBaseType_t item, uint32_t caps)
{
    (void)caps;
    struct obd_fake_queue *q = calloc(1, sizeof(*q));
    q->item = item;
    q->cap = len;
    q->buf = calloc(len, item);
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    pthread_mutex_lock(&g_mu);
    bool ok = q->n < q->cap;
    if (ok) {
        memcpy(q->buf + ((q->head + q->n) % q->cap) * q->item, item, q->item);
        q->n++;
    }
    pthread_mutex_unlock(&g_mu);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
    (void)ticks;   /* muse_obd.c only polls */
    pthread_mutex_lock(&g_mu);
    bool ok = q->n > 0;
    if (ok) {
        memcpy(item, q->buf + q->head * q->item, q->item);
        q->head = (q->head + 1) % q->cap;
        q->n--;
    }
    pthread_mutex_unlock(&g_mu);
    return ok ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    pthread_mutex_lock(&g_mu);
    q->head = q->n = 0;
    pthread_mutex_unlock(&g_mu);
    return pdPASS;
}

struct obd_fake_stream { size_t cap, head, n; uint8_t *buf; };

StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t trigger, uint32_t caps)
{
    (void)trigger, (void)caps;
    struct obd_fake_stream *s = calloc(1, sizeof(*s));
    s->cap = size;
    s->buf = calloc(1, size);
    return s;
}

size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t ticks)
{
    (void)ticks;
    pthread_mutex_lock(&g_mu);
    size_t n = 0;
    for (; n < len && s->n < s->cap; n++, s->n++) {
        s->buf[(s->head + s->n) % s->cap] = ((const uint8_t *)data)[n];
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t ticks)
{
    (void)ticks;
    pthread_mutex_lock(&g_mu);
    size_t n = 0;
    for (; n < len && s->n; n++, s->n--) {
        ((uint8_t *)data)[n] = s->buf[s->head];
        s->head = (s->head + 1) % s->cap;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

/* ---- The rest of the firmware muse_obd.c calls ---- */

static int g_coolant_fired, g_warmup_fired;

bool muse_link_ble_started(void) { return true; }

/* The trip log: counts what the OBD task asks of it. */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_trip_starts, g_trip_ends, g_samples;
static bool g_trip_open;
static muse_obd_status_t g_last_sample;   /* the snapshot the last row came from */

void muse_obd_log_start(void) {}

void muse_obd_log_trip_start(const muse_obd_status_t *st, uint8_t did_2612, uint32_t now_ms)
{
    (void)st, (void)did_2612, (void)now_ms;
    pthread_mutex_lock(&g_log_mu);
    g_trip_starts++;
    g_trip_open = true;
    pthread_mutex_unlock(&g_log_mu);
}

void muse_obd_log_sample(const muse_obd_status_t *st, uint32_t now_ms)
{
    (void)now_ms;
    pthread_mutex_lock(&g_log_mu);
    g_samples += g_trip_open && st->polling;
    g_last_sample = *st;
    pthread_mutex_unlock(&g_log_mu);
}

void muse_obd_log_trip_end(void)
{
    pthread_mutex_lock(&g_log_mu);
    g_trip_ends += g_trip_open;
    g_trip_open = false;
    pthread_mutex_unlock(&g_log_mu);
}

void muse_obd_log_status(muse_obd_log_status_t *out)
{
    pthread_mutex_lock(&g_log_mu);
    *out = (muse_obd_log_status_t){ .flash = true, .store = "flash", .active = g_trip_open, .pending = 2 };
    pthread_mutex_unlock(&g_log_mu);
}

static int log_count(int *n)
{
    pthread_mutex_lock(&g_log_mu);
    int v = *n;
    pthread_mutex_unlock(&g_log_mu);
    return v;
}
void muse_state_start_coolant(void) { g_coolant_fired++; }
void muse_state_start_warmup(void) { g_warmup_fired++; }

/* ---- A scripted LELink^2 ---- */

#define CONN 1
#define VAL 3       /* FFE1's value handle */
#define CCCD 4

static struct {
    int disc, disc_cancel, connect, conn_cancel, terminate;   /* calls */
    int param_updates;            /* ble_gap_update_params calls */
    uint16_t itvl_min, itvl_max;  /* the last connection parameters asked for */
    uint16_t terminated;          /* the handle of the last terminate */
    bool scanning, connecting;
    bool beat_cancel;             /* the next connection completes as it is cancelled */
    uint16_t next_conn;
    ble_gap_event_fn *cb;
} g_ble = { .next_conn = CONN };

static void ble_lock(void) { pthread_mutex_lock(&g_mu); }
static void ble_unlock(void) { pthread_mutex_unlock(&g_mu); }

/* The host task's side: a GAP event to the callback muse_obd.c gave. */
static void host_event(struct ble_gap_event *ev)
{
    ble_lock();
    ble_gap_event_fn *cb = g_ble.cb;
    ble_unlock();
    cb(ev, NULL);
}

static void host_connected(int status, uint16_t conn)
{
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_CONNECT };
    ev.connect.status = status;
    ev.connect.conn_handle = conn;
    host_event(&ev);
}

uint16_t ble_uuid_u16(const ble_uuid_t *uuid) { return ((const ble_uuid16_t *)uuid)->value; }

int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b)
{
    return (int)ble_uuid_u16(a) - (int)ble_uuid_u16(b);
}

int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst)
{
    if (off + len > om->len) {
        return -1;
    }
    memcpy(dst, om->data + off, (size_t)len);
    return 0;
}

static const char ADV_NAME[] = "LELink BLE OBDII";

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *data, uint8_t len)
{
    (void)data, (void)len;
    memset(f, 0, sizeof(*f));
    f->name = (const uint8_t *)ADV_NAME;
    f->name_len = (uint8_t)strlen(ADV_NAME);
    return 0;
}

int ble_hs_id_infer_auto(int privacy, uint8_t *own)
{
    (void)privacy;
    *own = 0;
    return 0;
}

int ble_hs_synced(void) { return 1; }

int ble_gap_disc(uint8_t own, int32_t ms, const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *arg)
{
    (void)own, (void)ms, (void)params, (void)arg;
    ble_lock();
    g_ble.disc++;
    g_ble.scanning = true;
    g_ble.cb = cb;
    ble_unlock();
    return 0;
}

int ble_gap_disc_cancel(void)
{
    ble_lock();
    g_ble.disc_cancel++;
    g_ble.scanning = false;
    ble_unlock();
    return 0;
}

int ble_gap_connect(uint8_t own, const ble_addr_t *addr, int32_t ms, const void *params, ble_gap_event_fn *cb,
                    void *arg)
{
    (void)own, (void)addr, (void)ms, (void)params, (void)arg;
    ble_lock();
    g_ble.connect++;
    g_ble.connecting = true;
    g_ble.cb = cb;
    ble_unlock();
    return 0;
}

/* As NimBLE does: the cancel ends in a failed connect event, unless the
 * connection completed first (beat_cancel), which reports it up. */
int ble_gap_conn_cancel(void)
{
    ble_lock();
    g_ble.conn_cancel++;
    g_ble.connecting = false;
    bool beat = g_ble.beat_cancel;
    uint16_t conn = g_ble.next_conn;
    g_ble.beat_cancel = false;
    ble_unlock();
    host_connected(beat ? 0 : BLE_HS_EAPP, beat ? conn : BLE_HS_CONN_HANDLE_NONE);
    return 0;
}

int ble_gap_update_params(uint16_t conn, const struct ble_gap_upd_params *params)
{
    (void)conn;
    ble_lock();
    g_ble.param_updates++;
    g_ble.itvl_min = params->itvl_min;
    g_ble.itvl_max = params->itvl_max;
    ble_unlock();
    return 0;
}

int ble_gap_terminate(uint16_t conn, uint8_t reason)
{
    (void)reason;
    ble_lock();
    g_ble.terminate++;
    g_ble.terminated = conn;
    ble_unlock();
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_DISCONNECT };
    ev.disconnect.reason = 0x16;
    ev.disconnect.conn.conn_handle = conn;
    host_event(&ev);
    return 0;
}

int ble_gattc_disc_svc_by_uuid(uint16_t conn, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *arg)
{
    (void)uuid;
    struct ble_gatt_svc svc = { .start_handle = 1, .end_handle = 10 };
    cb(conn, &(struct ble_gatt_error){ 0, 0 }, &svc, arg);
    cb(conn, &(struct ble_gatt_error){ BLE_HS_EDONE, 0 }, NULL, arg);
    return 0;
}

int ble_gattc_disc_chrs_by_uuid(uint16_t conn, uint16_t start, uint16_t end, const ble_uuid_t *uuid,
                                ble_gatt_chr_fn *cb, void *arg)
{
    (void)start, (void)end, (void)uuid;
    struct ble_gatt_chr chr = { .def_handle = 2, .val_handle = VAL,
                                .properties = BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_WRITE };
    cb(conn, &(struct ble_gatt_error){ 0, 0 }, &chr, arg);
    cb(conn, &(struct ble_gatt_error){ BLE_HS_EDONE, 0 }, NULL, arg);
    return 0;
}

int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *arg)
{
    (void)start, (void)end;
    struct ble_gatt_dsc dsc = { .handle = CCCD };
    dsc.uuid.u16 = (ble_uuid16_t){ { 16 }, 0x2902 };
    cb(conn, &(struct ble_gatt_error){ 0, 0 }, VAL, &dsc, arg);
    cb(conn, &(struct ble_gatt_error){ BLE_HS_EDONE, 0 }, VAL, NULL, arg);
    return 0;
}

/* The car behind it: its protocol and how its engine ECU takes DID 0x2612.
 * The adapter keeps the ELM state the DID exchange changes, and answers
 * Mode 01 only in the state Mode 01 expects (the functional header, no
 * receive filter), so a state left behind shows as RPM going missing. */
typedef enum { DID_ON, DID_OFF, DID_REFUSE, DID_NO_DATA, DID_GARBAGE, DID_HANG, DID_SHORT } did_mode_t;
static struct {
    const char *dpn;            /* ATDPN's answer */
    did_mode_t did;
    bool physical;              /* ATSH DA10F1 is in force */
    bool filter;                /* ATCRA 18DAF110 is in force */
    int did_reqs;               /* 222612s sent */
    int bad_state_rpm;          /* 010Cs that found the DID's state left behind */
    bool s2000;                 /* Derek's 2008 S2000's support masks and its OBD-MORE-PIDS-TASK PIDs */
    /* OBD-DID-CARDS-TASK's DIDs, silent unless a test says otherwise: 0x2610
     * answers its "A/C on" fixture (DID_ON) or "off" one (DID_OFF); 0x2663
     * answers mis_frames (an OBDb fixture) or, without them, a block with
     * mis[]'s counts; DID_SHORT is a reply one byte short of its signals. */
    did_mode_t did2610, did2663;
    const char *const *mis_frames;
    uint16_t mis[4];
    int did2610_reqs, did2663_reqs;
    /* The VSA modulator at DA28 (OBD-STEERING-ANGLE-TASK): its own header
     * state (ATSH DA28F1 / ATCRA 18DAF128), scripted 4004/4005 content, and
     * a scan space where 4000 answers a short block, 4030 refuses and the
     * rest stay silent. Silent unless a test turns it on. */
    did_mode_t vsa;
    bool vsa_physical, vsa_filter;
    bool brk_sw;
    int brk_raw;
    int vsa4004_reqs, vsa4005_reqs, vsa_other_reqs;
    bool engine_off;            /* 010C goes unanswered (RPM unknown) */
    char log[8192][16];         /* every command, in order */
    int logged;
} g_car = { .dpn = "A6", .did2610 = DID_NO_DATA, .did2663 = DID_NO_DATA, .vsa = DID_NO_DATA, .brk_raw = 0x8000 };

static char g_did_reply[512];

/* A 62 26 63 reply of n bytes in all with these counts at data bytes
 * 34..41, as the ELM prints it: the length line and the frames, padded. */
static const char *misfire_block(const uint16_t mis[4], size_t n)
{
    /* <= 10 frames of <= 17 chars plus length/prompt/NUL: < 180 of 512. */
    if (n > 64) {
        abort();
    }
    uint8_t b[64] = { 0x62, 0x26, 0x63 };
    for (int c = 0; c < 4; c++) {
        b[3 + 34 + 2 * c] = (uint8_t)(mis[c] >> 8);
        b[3 + 35 + 2 * c] = (uint8_t)mis[c];
    }
    size_t at = 0, cap = sizeof(g_did_reply);
    at += (size_t)snprintf(g_did_reply + at, cap - at, "%03X\r", (unsigned)n);
    size_t i = 0;
    for (int f = 0; i < n; f++) {
        at += (size_t)snprintf(g_did_reply + at, cap - at, "%X:", (unsigned)(f & 0xF));
        for (int k = 0; k < (f == 0 ? 6 : 7); k++, i++) {
            at += (size_t)snprintf(g_did_reply + at, cap - at, "%02X", i < n ? b[i] : 0x55);
        }
        at += (size_t)snprintf(g_did_reply + at, cap - at, "\r");
    }
    snprintf(g_did_reply + at, cap - at, "\r>");
    return g_did_reply;
}

/* Any reply's bytes as the ELM prints them: the length line and the
 * frames, padded (misfire_block's formatting, for the VSA blocks). */
static const char *bytes_block(const uint8_t *b, size_t n)
{
    if (n > 64) {
        abort();
    }
    size_t at = 0, cap = sizeof(g_did_reply);
    at += (size_t)snprintf(g_did_reply + at, cap - at, "%03X\r", (unsigned)n);
    size_t i = 0;
    for (int f = 0; i < n; f++) {
        at += (size_t)snprintf(g_did_reply + at, cap - at, "%X:", (unsigned)(f & 0xF));
        for (int k = 0; k < (f == 0 ? 6 : 7); k++, i++) {
            at += (size_t)snprintf(g_did_reply + at, cap - at, "%02X", i < n ? b[i] : 0x55);
        }
        at += (size_t)snprintf(g_did_reply + at, cap - at, "\r");
    }
    snprintf(g_did_reply + at, cap - at, "\r>");
    return g_did_reply;
}

static int hex_byte(const char *p)
{
    int v = 0;
    for (int i = 0; i < 2; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= c - '0';
        } else if (c >= 'A' && c <= 'F') {
            v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }
    return v;
}

/* Does a Mode 01 request (one PID, or a batch) ask for `pid`? */
static bool mode01_has(const char *cmd, int pid)
{
    if (strncmp(cmd, "01", 2)) {
        return false;
    }
    for (int i = 2; cmd[i] && cmd[i + 1]; i += 2) {
        if (hex_byte(cmd + i) == pid) {
            return true;
        }
    }
    return false;
}

/* One Mode 01 PID's reply line, or NULL when this car doesn't answer it.
 * What it answers is its scan mask (BE 1F A8 12): 04, 05, 06, 07, 0C, 0D,
 * 0E, 0F and 11 — not 0B (MAP), and 46 is past the mask's chain bit. */
static const char *pid_line(int pid)
{
    if (g_car.s2000) {
        /* The masks read live from the car (2026-10-22), and the six PIDs
         * OBD-MORE-PIDS-TASK adds; ambient (46) is not in its masks. */
        switch (pid) {
        case 0x00: return "41 00 BE 3E A8 13";
        case 0x20: return "41 20 80 07 F0 11";
        case 0x40: return "41 40 FA D0 00 00";
        case 0x0B: return "41 0B 21";            /* 33 kPa */
        case 0x34: return "41 34 80 00 80 00";   /* lambda 1.000, AFR 14.7 */
        case 0x49: return "41 49 33";            /* 20% */
        case 0x4A: return "41 4A 1A";            /* 10% */
        case 0x42: return "41 42 36 15";         /* 13.845 V */
        case 0x33: return "41 33 65";            /* 101 kPa */
        case 0x2F: return "41 2F A0";            /* 62% */
        default: break;
        }
    }
    if (pid == 0x0C && g_car.engine_off) {
        return NULL;   /* the engine is off: no RPM */
    }
    switch (pid) {
    case 0x00: return "41 00 BE 1F A8 12";   /* no PID 20: the scan stops here */
    case 0x0C: return "41 0C 1A F8";         /* 1726 rpm */
    case 0x05: return "41 05 5A";            /* 50 C */
    case 0x04: return "41 04 66";            /* 40% */
    case 0x06: return "41 06 80";            /* 0% */
    case 0x07: return "41 07 80";            /* 0% */
    case 0x0D: return "41 0D 00";            /* parked */
    case 0x0E: return "41 0E 80";            /* 0 deg */
    case 0x0F: return "41 0F 4B";            /* 35 C */
    case 0x11: return "41 11 00";            /* throttle closed */
    default: return NULL;
    }
}

/* A Mode 01 request, one PID or a batch of up to six. One PID: its line,
 * byte for byte what the car always answered. A batch: one J1979 message,
 * as a CAN ECU answers one (the ELM327 datasheet's "Multiple PID
 * Requests"): 41 once, then each PID this car answers with its data, in the
 * order asked. Up to seven bytes it is a single line; past that the ELM
 * prints the length and the ISO-TP frames ("00A", "0:...", "1:..."), the
 * last padded with 00. NO DATA when it answers none of them. */
static const char *answer_mode01(const char *cmd)
{
    static char reply[256];
    if (strlen(cmd) == 4) {
        const char *line = pid_line(hex_byte(cmd + 2));
        if (!line) {
            return "NO DATA\r\r>";
        }
        snprintf(reply, sizeof(reply), "%s\r\r>", line);
        return reply;
    }
    uint8_t msg[64];
    size_t n = 0;
    msg[n++] = 0x41;
    for (int i = 2; cmd[i] && cmd[i + 1]; i += 2) {
        const char *line = pid_line(hex_byte(cmd + i));
        if (!line) {
            continue;
        }
        /* The line's bytes after its 41: the PID and its data. */
        for (const char *c = line + 2; *c;) {
            if (*c == ' ') {
                c++;
                continue;
            }
            if (n < sizeof(msg)) {
                msg[n++] = (uint8_t)hex_byte(c);
            }
            c += 2;
        }
    }
    if (n == 1) {
        return "NO DATA\r\r>";
    }
    size_t at = 0;
    if (n <= 7) {
        for (size_t i = 0; i < n; i++) {
            at += (size_t)snprintf(reply + at, sizeof(reply) - at, "%02X", msg[i]);
        }
    } else {
        at += (size_t)snprintf(reply + at, sizeof(reply) - at, "%03X", (unsigned)n);
        size_t i = 0;
        for (int frame = 0; i < n; frame++) {
            at += (size_t)snprintf(reply + at, sizeof(reply) - at, "\r%X:", frame & 0xF);
            for (int b = 0; b < (frame == 0 ? 6 : 7); b++, i++) {
                at += (size_t)snprintf(reply + at, sizeof(reply) - at, "%02X", i < n ? msg[i] : 0);
            }
        }
    }
    snprintf(reply + at, sizeof(reply) - at, "\r\r>");
    return reply;
}

/* The adapter's answer to one ELM command. */
static const char *answer(const char *cmd)
{
    if (g_car.logged < 8192) {
        strcpy(g_car.log[g_car.logged++], cmd);
    }
    if (!strcmp(cmd, "ATZ")) {
        g_car.physical = g_car.filter = g_car.vsa_physical = g_car.vsa_filter = false;
        return "\r\rELM327 v1.5\r\r>";
    }
    if (!strcmp(cmd, "ATDPN")) return !strcmp(g_car.dpn, "A6") ? "A6\r\r>" : "A7\r\r>";
    if (!strcmp(cmd, "ATSHDA10F1")) { g_car.physical = true; g_car.vsa_physical = false; }
    if (!strcmp(cmd, "ATSHDA28F1")) { g_car.vsa_physical = true; g_car.physical = false; }
    if (!strcmp(cmd, "ATSHDB33F1")) g_car.physical = g_car.vsa_physical = false;
    if (!strcmp(cmd, "ATCRA18DAF110")) { g_car.filter = true; g_car.vsa_filter = false; }
    if (!strcmp(cmd, "ATCRA18DAF128")) { g_car.vsa_filter = true; g_car.filter = false; }
    if (!strcmp(cmd, "ATAR")) g_car.filter = g_car.vsa_filter = false;
    if (!strncmp(cmd, "AT", 2)) return "OK\r\r>";
    if (!strcmp(cmd, "222612")) {
        g_car.did_reqs++;
        if (!g_car.physical || !g_car.filter) return "NO DATA\r\r>";
        switch (g_car.did) {
        case DID_ON: return elm_from_frames(g_did_reply, sizeof(g_did_reply), S2000_2007_ON_REPORT, S2000_FRAMES, false);
        case DID_OFF: return elm_from_frames(g_did_reply, sizeof(g_did_reply), S2000_2007_OFF, S2000_FRAMES, false);
        case DID_REFUSE: return "7F2231\r\r>";
        case DID_NO_DATA: return "NO DATA\r\r>";
        case DID_GARBAGE: return "039\r0:622612FFFFC0\r2:001E00\r\r>";
        case DID_HANG: return NULL;   /* no prompt at all */
        case DID_SHORT: return "NO DATA\r\r>";   /* 0x2663's only */
        }
    }
    if (!strcmp(cmd, "222610")) {
        g_car.did2610_reqs++;
        if (!g_car.physical || !g_car.filter) return "NO DATA\r\r>";
        switch (g_car.did2610) {
        case DID_ON: return elm_from_frames(g_did_reply, sizeof(g_did_reply), S2000_2610_AC_ON, S2000_DID_FRAMES, false);
        case DID_OFF: return elm_from_frames(g_did_reply, sizeof(g_did_reply), S2000_2610_AC_OFF, S2000_DID_FRAMES, false);
        case DID_REFUSE: return "7F2231\r\r>";
        case DID_GARBAGE: return "039\r0:622610FFFDF0\r2:61E000\r\r>";
        case DID_HANG: return NULL;
        default: return "NO DATA\r\r>";
        }
    }
    if (!strcmp(cmd, "222663")) {
        g_car.did2663_reqs++;
        if (!g_car.physical || !g_car.filter) return "NO DATA\r\r>";
        switch (g_car.did2663) {
        case DID_ON:
            if (g_car.mis_frames) {
                return elm_from_frames(g_did_reply, sizeof(g_did_reply), g_car.mis_frames, S2000_DID_FRAMES, false);
            }
            return misfire_block(g_car.mis, 57);
        case DID_SHORT: return misfire_block(g_car.mis, 3 + 41);   /* one short of cylinder 4's low byte */
        case DID_REFUSE: return "7F2231\r\r>";
        case DID_GARBAGE: return "039\r0:622663F3FFFC\r2:3FF0E0\r\r>";
        case DID_HANG: return NULL;
        default: return "NO DATA\r\r>";
        }
    }
    if (!strcmp(cmd, "224004") || !strcmp(cmd, "224005")) {
        bool sw = cmd[5] == '4';
        if (sw) g_car.vsa4004_reqs++; else g_car.vsa4005_reqs++;
        if (!g_car.vsa_physical || !g_car.vsa_filter) return "NO DATA\r\r>";
        switch (g_car.vsa) {
        case DID_ON: {
            uint8_t b[64] = { 0x62, 0x40, (uint8_t)(sw ? 0x04 : 0x05) };
            if (sw) {
                b[3 + 15] = g_car.brk_sw ? 0x80 : 0x00;
                return bytes_block(b, 3 + 20);
            }
            b[3 + 7] = (uint8_t)(g_car.brk_raw >> 8);
            b[3 + 8] = (uint8_t)g_car.brk_raw;
            return bytes_block(b, 3 + 12);
        }
        case DID_REFUSE: return "7F2231\r\r>";
        case DID_GARBAGE: return "039\r0:624004FFFFC0\r2:001E00\r\r>";
        case DID_HANG: return NULL;
        default: return "NO DATA\r\r>";
        }
    }
    if (!strncmp(cmd, "22", 2) && strlen(cmd) == 6) {
        /* The scan's space: only inside a DA28 session, 4000 answering,
         * 4030 refusing, the rest silent. */
        g_car.vsa_other_reqs++;
        if (!g_car.vsa_physical || !g_car.vsa_filter || g_car.vsa != DID_ON) return "NO DATA\r\r>";
        if (!strcmp(cmd, "224030")) return "7F2231\r\r>";
        if (!strcmp(cmd, "224000")) {
            static const uint8_t B4000[] = { 0x62, 0x40, 0x00, 0x01, 0x02, 0x03, 0x04 };
            return bytes_block(B4000, sizeof(B4000));
        }
        return "NO DATA\r\r>";
    }
    if (g_car.physical || g_car.filter || g_car.vsa_physical || g_car.vsa_filter) {
        if (mode01_has(cmd, 0x0C)) g_car.bad_state_rpm++;
        return "NO DATA\r\r>";
    }
    if (!strncmp(cmd, "01", 2)) return answer_mode01(cmd);
    return "NO DATA\r\r>";
}

/* A reply waits until the task blocks for it: the write's own event comes
 * first, as over the air, so wait_for() takes both in turn. */
static const char *g_reply;
static uint16_t g_reply_conn;

static void deliver_reply(void)
{
    ble_lock();
    const char *reply = g_reply;
    uint16_t conn = g_reply_conn;
    g_reply = NULL;
    if (reply) {
        g_now += 5;   /* an answer takes a little time, like over the air */
    }
    ble_unlock();
    if (!reply) {
        return;
    }
    struct os_mbuf om = { (int)strlen(reply), (const uint8_t *)reply };
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_NOTIFY_RX };
    ev.notify_rx.conn_handle = conn;
    ev.notify_rx.attr_handle = VAL;
    ev.notify_rx.om = &om;
    host_event(&ev);
}

int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len, ble_gatt_attr_fn *cb,
                         void *arg)
{
    cb(conn, &(struct ble_gatt_error){ 0, handle }, NULL, arg);
    if (handle == VAL) {
        char cmd[16] = { 0 };
        memcpy(cmd, data, len < sizeof(cmd) ? len : sizeof(cmd) - 1);
        cmd[strcspn(cmd, "\r")] = 0;
        ble_lock();
        g_reply = answer(cmd);
        g_reply_conn = conn;
        ble_unlock();
    }
    return 0;
}

int ble_gattc_write_no_rsp_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len)
{
    (void)conn, (void)handle, (void)data, (void)len;
    return 0;
}

/* ---- The checks ---- */

static int s_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        s_fail++; \
    } \
} while (0)

static void real_nap(void)
{
    nanosleep(&(struct timespec){ 0, 20000 }, NULL);
}

/* Moves the clock on by ms, `step` at a time while the task is blocked. A
 * task that never blocks (2 s of real time) fails the run, not hangs it. */
static void pass_by(uint64_t ms, uint64_t step)
{
    uint64_t until;
    pthread_mutex_lock(&g_mu);
    until = g_now + ms;
    while (g_now < until) {
        for (int spins = 0; !g_blocked; spins++) {
            pthread_mutex_unlock(&g_mu);
            if (spins > 100000) {
                fprintf(stderr, "the OBD task never blocks\n");
                exit(3);
            }
            real_nap();
            pthread_mutex_lock(&g_mu);
        }
        g_now += step;
        pthread_cond_broadcast(&g_cv);
        pthread_mutex_unlock(&g_mu);
        real_nap();
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
}

static void pass(uint64_t ms)
{
    pass_by(ms, 10);
}

typedef bool (*pred_t)(void);

/* Waits for pred, letting up to `budget_ms` of task time pass. */
static bool await(pred_t pred, uint64_t budget_ms)
{
    for (uint64_t spent = 0; spent <= budget_ms; spent += 10) {
        for (int i = 0; i < 200; i++) {
            if (pred()) {
                return true;
            }
            real_nap();
        }
        pass(10);
    }
    return pred();
}

static muse_obd_status_t status(void)
{
    muse_obd_status_t st;
    muse_obd_status(&st);
    return st;
}

static int ble(int *counter)
{
    ble_lock();
    int n = *counter;
    ble_unlock();
    return n;
}

static bool is_scanning(void) { ble_lock(); bool b = g_ble.scanning; ble_unlock(); return b; }
static bool is_connecting(void) { ble_lock(); bool b = g_ble.connecting; ble_unlock(); return b; }
static bool is_polling(void) { muse_obd_status_t st = status(); return st.polling && st.rpm_valid; }
static bool is_off(void) { return !strcmp(status().state, "off"); }
static bool has_samples(void) { return log_count(&g_samples) > 0; }

static void found_adapter(void)
{
    ble_lock();
    g_ble.scanning = false;
    ble_unlock();
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_DISC };
    host_event(&ev);
}

/* Scanning -> polling, the way the adapter would take it. */
static void link_up(uint16_t conn)
{
    CHECK(await(is_scanning, 1000), "no scan");
    found_adapter();
    CHECK(await(is_connecting, 1000), "no connect");
    ble_lock();
    g_ble.connecting = false;
    ble_unlock();
    host_connected(0, conn);
    CHECK(await(is_polling, 5000), "not polling: %s", status().state);
}

/* Off, with nothing left: no link, no readings, the snapshot says so. */
static void check_off(const char *when)
{
    muse_obd_status_t st = status();
    CHECK(!strcmp(st.state, "off"), "%s: state %s", when, st.state);
    CHECK(!st.enabled, "%s: enabled", when);
    CHECK(!st.connected && !st.subscribed && !st.polling, "%s: link flags left on", when);
    CHECK(!st.rpm_valid && !st.coolant_valid, "%s: readings left", when);
    CHECK(!st.vsa_brk_sw.valid && !st.vsa_brk_p.valid, "%s: VSA readings left", when);
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        CHECK(!st.extra[i].valid, "%s: %s left", when, MUSE_OBD_EXTRA[i].key);
    }
    CHECK(!st.adapter_found, "%s: adapter_found", when);
    CHECK(!muse_obd_linked(&st), "%s: still linked", when);
    CHECK(s_conn == BLE_HS_CONN_HANDLE_NONE, "%s: connection handle kept", when);
}

/* However long it stays off, it makes no BLE call. */
static void check_quiet(const char *when)
{
    int disc = ble(&g_ble.disc), connect = ble(&g_ble.connect);
    pass_by(5 * 60 * 1000, 250);   /* well under the shortest wait (500 ms) */
    CHECK(ble(&g_ble.disc) == disc, "%s: scanned while off", when);
    CHECK(ble(&g_ble.connect) == connect, "%s: connected while off", when);
    CHECK(is_off(), "%s: left off", when);
}

/* Waits in real time for pred while the task polls (a polling task never
 * blocks, so pass_by() can't be used); false if it never holds. */
static bool settle(pred_t pred)
{
    for (long spins = 0; spins < 5000000; spins++) {
        if (pred()) {
            return true;
        }
        real_nap();
    }
    return false;
}

#ifndef MUSE_OBD_HARNESS_DID
/* Lets ms of the task's own time go by while it polls: a response-driven
 * loop never blocks, and each reply moves the clock on. False if it stalls. */
static bool polled_for(uint64_t ms)
{
    pthread_mutex_lock(&g_mu);
    uint64_t until = g_now + ms;
    pthread_mutex_unlock(&g_mu);
    for (long spins = 0; spins < 5000000; spins++) {
        pthread_mutex_lock(&g_mu);
        bool done = g_now >= until;
        pthread_mutex_unlock(&g_mu);
        if (done) {
            return true;
        }
        real_nap();
    }
    return false;
}

#else
/* Whether pred held at any moment while ms of the task's own time went by
 * (it polls, so this watches instead of sampling). */
static bool seen_during(pred_t pred, uint64_t ms)
{
    pthread_mutex_lock(&g_mu);
    uint64_t until = g_now + ms;
    pthread_mutex_unlock(&g_mu);
    for (long spins = 0; spins < 5000000; spins++) {
        if (pred()) {
            return true;
        }
        pthread_mutex_lock(&g_mu);
        bool done = g_now >= until;
        pthread_mutex_unlock(&g_mu);
        if (done) {
            return pred();
        }
        real_nap();
    }
    return pred();
}

#endif

#ifndef MUSE_OBD_HARNESS_DID
static bool link_moving(void)
{
    muse_obd_status_t st = status();
    return st.link_exchanges > 0 && st.link_batch_pct > 0;
}

/* OBD-MORE-PIDS-TASK: every new extra has a reading. */
static bool more_pids_read(void)
{
    static const muse_obd_extra_t NEW[] = { MUSE_OBD_AFR, MUSE_OBD_LAMBDA, MUSE_OBD_PEDAL_D, MUSE_OBD_PEDAL_E,
                                            MUSE_OBD_MODULE_V, MUSE_OBD_BARO, MUSE_OBD_FUEL };
    muse_obd_status_t st = status();
    for (size_t i = 0; i < sizeof(NEW) / sizeof(NEW[0]); i++) {
        if (!st.extra[NEW[i]].valid) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    /* Linked: subscribed or polling, and never while turned off. */
    CHECK(muse_obd_linked(&(muse_obd_status_t){ .enabled = true, .subscribed = true }), "subscribed");
    CHECK(muse_obd_linked(&(muse_obd_status_t){ .enabled = true, .polling = true }), "polling");
    CHECK(!muse_obd_linked(&(muse_obd_status_t){ .enabled = true, .connected = true }), "connected only");
    CHECK(!muse_obd_linked(&(muse_obd_status_t){ .subscribed = true, .polling = true }), "turned off");

    /* Off at boot: the task starts in "off" and never scans. */
    muse_obd_set_enabled(false);
    muse_obd_start();
    CHECK(await(is_off, 100), "not off at boot");
    check_off("boot");
    check_quiet("boot");
    CHECK(!muse_obd_enabled(), "enabled at boot");

    /* On: scans, connects and polls, without a restart. */
    muse_obd_set_enabled(true);
    link_up(CONN);
    muse_obd_status_t st = status();
    CHECK(st.enabled && muse_obd_linked(&st), "on and polling, but not linked");
    CHECK(st.rpm == 1726, "rpm %d", st.rpm);
    /* The connection asked for the fast parameters (7.5-15 ms). */
    CHECK(ble(&g_ble.param_updates) == 1 && g_ble.itvl_min == 6 && g_ble.itvl_max == 12,
          "connection parameters: %d updates, %u-%u", ble(&g_ble.param_updates), g_ble.itvl_min, g_ble.itvl_max);
    /* And the link's numbers are moving: exchanges, a rate, batched readings
     * (counted just after the reply that made it "polling": wait for them). */
    CHECK(settle(link_moving), "link stats never moved");
    st = status();
    CHECK(st.link_exchanges > 0 && st.link_last_rtt_ms > 0 && st.link_batch_pct > 0 && !st.link_batch_off,
          "link stats: %u exchanges, rtt %u, batch %d%%", (unsigned)st.link_exchanges,
          (unsigned)st.link_last_rtt_ms, st.link_batch_pct);
    /* Polling started a trip, and its samples are going in. */
    CHECK(log_count(&g_trip_starts) == 1, "trips started: %d", log_count(&g_trip_starts));
    CHECK(await(has_samples, 1000), "no samples logged");
    CHECK(st.log_flash && st.log_active && st.log_pending == 2 && !strcmp(st.log_store, "flash"),
          "the snapshot lacks the log's status");

    /* Off while polling: dropped at once, with the BLE link terminated, and
     * the trip flushed and closed with it. */
    int terms = ble(&g_ble.terminate);
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 200), "polling: not off: %s", status().state);
    CHECK(ble(&g_ble.terminate) == terms + 1, "polling: link not terminated");
    CHECK(g_ble.terminated == CONN, "polling: terminated %u", g_ble.terminated);
    check_off("polling");
    CHECK(log_count(&g_trip_ends) == 1, "the trip wasn't closed when OBD went off");
    int samples = log_count(&g_samples);
    check_quiet("polling");
    CHECK(log_count(&g_samples) == samples, "logged while off");
    CHECK(log_count(&g_trip_starts) == 1, "a trip started while off");
    /* Off, the log still reports (its trips can be pulled). */
    st = status();
    CHECK(st.log_flash && st.log_pending == 2 && !st.log_active, "off: the log's status is gone");

    /* The reactions don't play while off, and the bench can't hold VTEC up. */
    muse_obd_fire_coolant();
    muse_obd_fire_warmup();
    CHECK(!g_coolant_fired && !g_warmup_fired, "a reaction played while off");
    muse_obd_bench_vtec_sol(true);
    st = status();
    CHECK(!st.vtec_sol.valid && !muse_obd_vtec_on(&st), "the bench's VTEC while off");

    /* Off while scanning: the scan is cancelled. */
    muse_obd_set_enabled(true);
    CHECK(await(is_scanning, 1000), "no scan after turning on");
    int cancels = ble(&g_ble.disc_cancel);
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 200), "scanning: not off: %s", status().state);
    CHECK(ble(&g_ble.disc_cancel) == cancels + 1, "scanning: scan not cancelled");
    check_off("scanning");
    check_quiet("scanning");

    /* Off while connecting: cancelled, and nothing left connected. */
    muse_obd_set_enabled(true);
    CHECK(await(is_scanning, 1000), "no scan");
    found_adapter();
    CHECK(await(is_connecting, 1000), "no connect");
    cancels = ble(&g_ble.conn_cancel);
    terms = ble(&g_ble.terminate);
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 1500), "connecting: not off: %s", status().state);
    CHECK(ble(&g_ble.conn_cancel) == cancels + 1, "connecting: not cancelled");
    CHECK(ble(&g_ble.terminate) == terms, "connecting: terminated a link that never came up");
    check_off("connecting");

    /* The same, with the connection beating the cancel: it is taken down. */
    muse_obd_set_enabled(true);
    CHECK(await(is_scanning, 1000), "no scan");
    found_adapter();
    CHECK(await(is_connecting, 1000), "no connect");
    ble_lock();
    g_ble.beat_cancel = true;
    g_ble.next_conn = 7;
    ble_unlock();
    terms = ble(&g_ble.terminate);
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "raced connect: not off: %s", status().state);
    CHECK(ble(&g_ble.terminate) == terms + 1, "raced connect: the connection was left up");
    CHECK(g_ble.terminated == 7, "raced connect: terminated %u", g_ble.terminated);
    check_off("raced connect");
    check_quiet("raced connect");

    /* And back on: the next link polls as before; the reactions play again. */
    muse_obd_set_enabled(true);
    link_up(CONN);
    st = status();
    CHECK(!strcmp(st.state, "polling") && st.enabled, "back on: %s", st.state);
    muse_obd_fire_coolant();
    CHECK(g_coolant_fired == 1, "back on: no reaction");
    /* ">vtecsol 1" holds VTEC up, fresh however long, until ">vtecsol 0". */
    muse_obd_bench_vtec_sol(true);
    st = status();
    CHECK(st.vtec_sol.valid && st.vtec_sol.value == 1 && muse_obd_vtec_on(&st), "bench: not held");
    muse_obd_bench_vtec_sol(false);
    st = status();
    CHECK(st.vtec_sol.valid && st.vtec_sol.value == 0 && !muse_obd_vtec_on(&st), "bench: not let go");

    /* OBD-MORE-PIDS-TASK on Derek's S2000: its masks list all six new PIDs,
     * so the real task schedules them, they ride the J1979 batches, and
     * each lands in the snapshot (obd.read's source) and in the log's row. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 1500), "s2000: not off: %s", status().state);
    g_car.s2000 = true;
    muse_obd_set_enabled(true);
    link_up(CONN);
    CHECK(await(more_pids_read, 5000), "s2000: the new PIDs never all read");
    st = status();
    CHECK(st.support_valid[0] && st.support_valid[1] && st.support_valid[2]
          && st.support[0] == 0xBE3EA813u && st.support[1] == 0x8007F011u && st.support[2] == 0xFAD00000u,
          "s2000: masks %08X %08X %08X", (unsigned)st.support[0], (unsigned)st.support[1], (unsigned)st.support[2]);
    CHECK(st.extra[MUSE_OBD_AFR].value == 147, "s2000: afr x10 %d", st.extra[MUSE_OBD_AFR].value);
    CHECK(st.extra[MUSE_OBD_LAMBDA].value == 1000, "s2000: lambda x1000 %d", st.extra[MUSE_OBD_LAMBDA].value);
    CHECK(st.extra[MUSE_OBD_PEDAL_D].value == 20, "s2000: pedal d %d", st.extra[MUSE_OBD_PEDAL_D].value);
    CHECK(st.extra[MUSE_OBD_PEDAL_E].value == 10, "s2000: pedal e %d", st.extra[MUSE_OBD_PEDAL_E].value);
    CHECK(st.extra[MUSE_OBD_MODULE_V].value == 13845, "s2000: module mV %d", st.extra[MUSE_OBD_MODULE_V].value);
    CHECK(st.extra[MUSE_OBD_BARO].value == 101, "s2000: baro %d", st.extra[MUSE_OBD_BARO].value);
    CHECK(st.extra[MUSE_OBD_FUEL].value == 62, "s2000: fuel %d", st.extra[MUSE_OBD_FUEL].value);
    CHECK(st.extra[MUSE_OBD_MAP].valid && st.extra[MUSE_OBD_MAP].value == 33, "s2000: map");
    /* Ambient isn't in the car's masks: never asked, never a value. */
    CHECK(!st.extra[MUSE_OBD_AMBIENT].valid && muse_obd_pid_unsupported(&st, 0x46), "s2000: ambient");
    CHECK(st.link_batch_pct > 0 && !st.link_batch_off, "s2000: batches %d%% off %d", st.link_batch_pct,
          st.link_batch_off);
    /* Fresh: each is read again within its tier's target (and the cards'
     * 3 s dimming rule), fuel included. The loop never idles while it
     * polls (each reply moves the clock on), so let 4 s of its time go by. */
    CHECK(polled_for(4000), "s2000: the poll loop stalled");
    st = status();
    static const muse_obd_extra_t FRESH[] = { MUSE_OBD_AFR, MUSE_OBD_PEDAL_D, MUSE_OBD_PEDAL_E, MUSE_OBD_FUEL,
                                              MUSE_OBD_MODULE_V, MUSE_OBD_BARO };
    for (size_t i = 0; i < sizeof(FRESH) / sizeof(FRESH[0]); i++) {
        CHECK(st.extra[FRESH[i]].valid && st.extra[FRESH[i]].age_ms <= 3000u, "s2000: %s %u ms old",
              MUSE_OBD_EXTRA[FRESH[i]].key, (unsigned)st.extra[FRESH[i]].age_ms);
    }
    pthread_mutex_lock(&g_log_mu);
    muse_obd_status_t row = g_last_sample;
    pthread_mutex_unlock(&g_log_mu);
    CHECK(row.extra[MUSE_OBD_AFR].valid && row.extra[MUSE_OBD_AFR].value == 147
          && row.extra[MUSE_OBD_FUEL].valid && row.extra[MUSE_OBD_MODULE_V].value == 13845,
          "s2000: the log's rows lack the new values");
    g_car.s2000 = false;

    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_enable\n");
    return 0;
}
#else   /* MUSE_OBD_HARNESS_DID */

/*
 * DID 0x2612 against the real task: the exchange is state-safe (Mode 01's
 * header and receive filter are back for every RPM request, whatever the DID
 * did), the scheduler reads it about once a second between Mode 01 batches,
 * a failed or hung exchange neither drops the link nor upsets RPM, the probe
 * runs once per connection and only on CAN 29-bit, and the VTEC level
 * follows the solenoid: held while fresh "on", down on "off" or stale,
 * never from RPM.
 */

static int car(int *n)
{
    ble_lock();
    int v = *n;
    ble_unlock();
    return v;
}

static void set_car(did_mode_t did, const char *dpn)
{
    ble_lock();
    g_car.did = did;
    g_car.dpn = dpn;
    ble_unlock();
}

static int s_want;   /* await()'s threshold for the predicates below */
static bool did_reqs_reach(void) { return car(&g_car.did_reqs) >= s_want; }
static bool log_reach(void) { return car(&g_car.logged) >= s_want; }
static bool sol_is_on(void) { muse_obd_status_t st = status(); return st.vtec_sol.valid && st.vtec_sol.value == 1; }
static bool sol_is_off(void) { muse_obd_status_t st = status(); return st.vtec_sol.valid && st.vtec_sol.value == 0; }
static bool vtec_down(void) { muse_obd_status_t st = status(); return !muse_obd_vtec_on(&st); }

/* The commands since the log was cleared: every 222612 inside the whole
 * choreography (the header and filter set before it, Mode 01's state put
 * back straight after), Mode 01 requests throughout, RPM in most of them,
 * and, over the whole span (four DID windows at least: every tier's target
 * expires inside it), each secondary the scan left asked at least once,
 * coolant among them. A single window is only about a second of link time,
 * shorter than the slow tier's target, so coverage is judged on the span. */
static void check_cadence(const char *when)
{
    static const int WANT[] = { 0x04, 0x05, 0x06, 0x07, 0x0D, 0x0E, 0x0F, 0x11 };
    ble_lock();
    int n = g_car.logged, prev = -1, dids = 0, reqs = 0, rpm_reqs = 0;
    bool seen[256] = { 0 };
    for (int i = 0; i < n; i++) {
        if (!strncmp(g_car.log[i], "01", 2)) {
            reqs++;
            bool has_rpm = false;
            for (int j = 2; g_car.log[i][j] && g_car.log[i][j + 1]; j += 2) {
                int pid = hex_byte(g_car.log[i] + j);
                if (pid == 0x0C) {
                    has_rpm = true;
                } else if (pid > 0) {
                    seen[pid] = true;
                }
            }
            rpm_reqs += has_rpm;
        }
        if (strcmp(g_car.log[i], "222612")) {
            continue;
        }
        dids++;
        if (i >= 2) {
            CHECK(!strcmp(g_car.log[i - 2], "ATSHDA10F1") && !strcmp(g_car.log[i - 1], "ATCRA18DAF110"),
                  "%s: request %d without its header and filter", when, i);
        }
        if (i + 2 < n) {
            CHECK(!strcmp(g_car.log[i + 1], "ATAR") && !strcmp(g_car.log[i + 2], "ATSHDB33F1"),
                  "%s: request %d not put back", when, i);
        }
        if (prev >= 0) {
            int wreqs = 0;
            for (int k = prev + 3; k < i - 2; k++) {
                wreqs += !strncmp(g_car.log[k], "01", 2);
            }
            CHECK(wreqs >= 4, "%s: window %d..%d has %d Mode 01 requests", when, prev, i, wreqs);
        }
        prev = i;
    }
    ble_unlock();
    CHECK(dids >= 2, "%s: only %d DID reads", when, dids);
    CHECK(reqs >= 8 && rpm_reqs >= reqs / 2, "%s: %d Mode 01 requests, %d with RPM", when, reqs, rpm_reqs);
    for (size_t w = 0; w < sizeof(WANT) / sizeof(WANT[0]); w++) {
        CHECK(seen[WANT[w]], "%s: the span never asked PID %02X", when, WANT[w]);
    }
}

static void clear_log(void)
{
    ble_lock();
    g_car.logged = 0;
    ble_unlock();
}

/* ---- OBD-DID-CARDS-TASK ---- */

static int fired(void) { ble_lock(); int n = g_coolant_fired; ble_unlock(); return n; }
static int s_want_fired;
static bool fired_reach(void) { return fired() >= s_want_fired; }
static bool reqs_2663_reach(void) { return car(&g_car.did2663_reqs) >= s_want; }
static bool reqs_2610_reach(void) { return car(&g_car.did2610_reqs) >= s_want; }
static bool cards_dids_read(void)
{
    muse_obd_status_t st = status();
    return st.did_2610 && st.did_2663 && st.ac_clutch.valid && st.misfire[0].valid;
}
static bool rpm_gone(void) { return !status().rpm_valid; }
static bool rose_set(void) { return status().misfire_rose != 0; }
static bool rpm_back(void) { return status().rpm_valid; }

static void set_mis(const char *const *frames, uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4)
{
    ble_lock();
    g_car.mis_frames = frames;
    g_car.mis[0] = c1, g_car.mis[1] = c2, g_car.mis[2] = c3, g_car.mis[3] = c4;
    ble_unlock();
}

static void set_dids(did_mode_t d2610, did_mode_t d2663)
{
    ble_lock();
    g_car.did2610 = d2610;
    g_car.did2663 = d2663;
    ble_unlock();
}

/* Every DID request in the log since it was cleared sits inside a header
 * session (ATSH/ATCRA set at its start, Mode 01's state put back at its
 * end: ATAR then ATSHDB33F1), with only DID requests in between: no Mode
 * 01 inside a session. One session may carry several DID reads (the poll
 * loop groups the due ones), so the session, not each read, owns the
 * choreography. Returns how many reads of each DID it saw. */
static void check_did_exchanges(const char *when, int reads[3])
{
    static const char *const REQ[3] = { "222612", "222610", "222663" };
    reads[0] = reads[1] = reads[2] = 0;
    ble_lock();
    int n = g_car.logged;
    bool inside = false, filtered = false;
    for (int i = 0; i < n; i++) {
        const char *c = g_car.log[i];
        if (!strcmp(c, "ATSHDA10F1")) {
            CHECK(!inside, "%s: a DID exchange inside another at %d", when, i);
            inside = true;
            filtered = false;
            continue;
        }
        if (!strcmp(c, "ATCRA18DAF110")) {
            filtered = true;
            continue;
        }
        if (!strcmp(c, "ATSHDB33F1")) {
            CHECK(!inside || (i >= 1 && !strcmp(g_car.log[i - 1], "ATAR")), "%s: session at %d not put back", when, i);
            inside = false;
            filtered = false;
            continue;
        }
        int d = -1;
        for (int k = 0; k < 3; k++) {
            if (!strcmp(c, REQ[k])) {
                d = k;
            }
        }
        if (d >= 0) {
            reads[d]++;
            CHECK(inside && filtered, "%s: %s at %d without its header", when, c, i);
            continue;
        }
        CHECK(!inside || !strcmp(c, "ATAR"),
              "%s: %s inside a DID exchange at %d", when, c, i);
    }
    ble_unlock();
}

/* ---- OBD-STEERING-ANGLE-TASK Phase 1 ---- */

static void set_vsa(did_mode_t mode, bool brk_sw, int brk_raw)
{
    ble_lock();
    g_car.vsa = mode;
    g_car.brk_sw = brk_sw;
    g_car.brk_raw = brk_raw;
    ble_unlock();
}

static bool vsa_read(void)
{
    muse_obd_status_t st = status();
    return st.vsa == 1 && st.vsa_brk_sw.valid && st.vsa_brk_p.valid;
}
static bool brk_released(void)
{
    muse_obd_status_t st = status();
    return st.vsa_brk_sw.valid && st.vsa_brk_sw.value == 0 && st.vsa_brk_p.valid && st.vsa_brk_p.value == -2500;
}
static bool reqs_4005_reach(void) { return car(&g_car.vsa4005_reqs) >= s_want; }
static bool scan_done(void) { return !strcmp(status().vsa_scan, "done"); }

/* Every VSA request in the log sits inside a DA28 session (its ATSH/ATCRA
 * at the start, Mode 01's state put back at the end), with only VSA
 * requests in between, and no VSA request inside a DA10 session or the
 * other way round. Returns how many DA28 sessions carried both DIDs. */
static int check_vsa_exchanges(const char *when)
{
    int grouped = 0, in4004 = 0, in4005 = 0;
    ble_lock();
    int n = g_car.logged;
    bool da28 = false, da28_filtered = false, da10 = false;
    for (int i = 0; i < n; i++) {
        const char *c = g_car.log[i];
        if (!strcmp(c, "ATSHDA28F1")) {
            CHECK(!da28 && !da10, "%s: a DA28 session inside another at %d", when, i);
            da28 = true;
            da28_filtered = false;
            in4004 = in4005 = 0;
            continue;
        }
        if (!strcmp(c, "ATSHDA10F1")) {
            CHECK(!da28, "%s: a DA10 session inside a DA28 one at %d", when, i);
            da10 = true;
            continue;
        }
        if (!strcmp(c, "ATCRA18DAF128")) {
            da28_filtered = true;
            continue;
        }
        if (!strcmp(c, "ATSHDB33F1")) {
            if (da28) {
                CHECK(i >= 1 && !strcmp(g_car.log[i - 1], "ATAR"), "%s: DA28 session at %d not put back", when, i);
                grouped += in4004 > 0 && in4005 > 0;
            }
            da28 = da10 = false;
            da28_filtered = false;
            continue;
        }
        bool is_da10 = !strcmp(c, "222612") || !strcmp(c, "222610") || !strcmp(c, "222663");
        bool is_vsa = !is_da10 && !strncmp(c, "22", 2);   /* the pair, and the scan's DIDs */
        if (is_vsa) {
            CHECK(da28 && da28_filtered, "%s: %s at %d without its DA28 header", when, c, i);
            if (!strcmp(c, "224004")) in4004++; else in4005++;
            continue;
        }
        if (is_da10) {
            CHECK(!da28, "%s: %s inside a DA28 session at %d", when, c, i);
            continue;
        }
        CHECK(!da28 || !strcmp(c, "ATAR"), "%s: %s inside a DA28 session at %d", when, c, i);
    }
    ble_unlock();
    return grouped;
}

int main(void)
{
    /* A reply received just before disconnect must not fire after header
     * restoration observes the lost link. Exercise the production dispatch
     * before starting the worker, so this boundary is deterministic. */
    muse_misfire_init(&s_misfire);
    const uint16_t baseline[4] = { 0 };
    muse_misfire_update(&s_misfire, baseline, 800, 0);
    did_reading_t late = { .misfire = { .count = { 1, 0, 0, 0 } } };
    s_rpm = 800;
    got_did(MUSE_DID_2663, &late);
    CHECK(g_coolant_fired == 0 && s_misfire.base[0] == 0, "disconnected reply fired or moved baseline");
    s_conn = CONN;
    s_enabled = false;
    got_did(MUSE_DID_2663, &late);
    CHECK(g_coolant_fired == 0 && s_misfire.base[0] == 0, "disabled reply moved baseline");
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_enabled = true;
    s_rpm = -1;
    muse_misfire_init(&s_misfire);

    /* The level: a fresh "on" solenoid only, and only while OBD is on. RPM
     * plays no part, at any value. */
    muse_obd_status_t st = { .enabled = true, .rpm_valid = true, .rpm = 9000, .rpm_age_ms = 10 };
    CHECK(!muse_obd_vtec_on(&st), "9000 rpm, no solenoid reading");
    st.vtec_sol = (muse_obd_reading_t){ true, 0, 10 };
    CHECK(!muse_obd_vtec_on(&st), "9000 rpm, solenoid off");
    st.vtec_sol = (muse_obd_reading_t){ true, 1, MUSE_VTEC_FRESH_MS + 1 };
    CHECK(!muse_obd_vtec_on(&st), "9000 rpm, solenoid on but stale");
    st.vtec_sol = (muse_obd_reading_t){ true, 1, 10 };
    CHECK(muse_obd_vtec_on(&st), "solenoid on, fresh");
    st.enabled = false;
    CHECK(!muse_obd_vtec_on(&st), "OBD turned off");
    st = (muse_obd_status_t){ .enabled = true, .vtec_sol = { true, 1, 10 } };
    CHECK(muse_obd_vtec_on(&st), "solenoid on with no RPM at all");

    /* CAN 29-bit and an ECU that answers: probed, then read every rotation. */
    set_car(DID_ON, "A7");
    muse_obd_start();
    link_up(CONN);
    CHECK(await(sol_is_on, 2000), "no solenoid reading");
    st = status();
    CHECK(st.did_2612, "did_2612 false");
    CHECK(st.vtec_sol.value == 1 && st.vtec_psw.valid && st.vtec_psw.value == 0, "flags %d/%d", st.vtec_sol.value,
          st.vtec_psw.value);
    CHECK(st.did_2612_b23.valid && st.did_2612_b23.value == 1, "byte 23");
    CHECK(muse_obd_vtec_on(&st), "the solenoid is on, VTEC isn't");
    CHECK(st.rpm_valid && st.rpm == 1726, "RPM after the probe: %d", st.rpm);
    clear_log();
    s_want = car(&g_car.did_reqs) + 4;
    CHECK(await(did_reqs_reach, 5000), "the DID isn't read every rotation");
    check_cadence("answering");
    CHECK(car(&g_car.bad_state_rpm) == 0, "Mode 01 found the DID's state %d times", car(&g_car.bad_state_rpm));

    /* Off, and straight back on: the level follows, no cooldown. */
    set_car(DID_OFF, "A7");
    CHECK(await(sol_is_off, 2000), "the solenoid didn't go off");
    CHECK(vtec_down(), "VTEC still up with the solenoid off");
    set_car(DID_ON, "A7");
    CHECK(await(sol_is_on, 2000), "the solenoid didn't come back on");
    st = status();
    CHECK(muse_obd_vtec_on(&st), "VTEC didn't come straight back");

    /* A garbled reply mid-connection: no reading, RPM carries on, no drop. */
    int terms = ble(&g_ble.terminate);
    set_car(DID_GARBAGE, "A7");
    s_want = car(&g_car.did_reqs) + 3;
    CHECK(await(did_reqs_reach, 5000), "garbage: stopped asking");
    st = status();
    CHECK(st.polling && st.rpm_valid && st.rpm == 1726, "garbage: RPM gone");
    CHECK(st.did_2612, "garbage mid-connection unsupported the DID");
    CHECK(ble(&g_ble.terminate) == terms, "garbage dropped the link");

    /* No prompt at all: each read times out, the state is put back, and
     * polling goes on; the last "on" ages out instead of sticking. */
    set_car(DID_ON, "A7");
    CHECK(await(sol_is_on, 2000), "on again");
    set_car(DID_HANG, "A7");
    CHECK(await(vtec_down, 20000), "a hung DID left VTEC up (a stale on)");
    st = status();
    CHECK(st.vtec_sol.valid && st.vtec_sol.age_ms > MUSE_VTEC_FRESH_MS, "the reading didn't age");
    clear_log();
    s_want = car(&g_car.did_reqs) + 2;
    CHECK(await(did_reqs_reach, 20000), "hang: stopped asking");
    st = status();
    CHECK(st.polling && st.rpm_valid && st.rpm == 1726, "hang: RPM gone");
    CHECK(ble(&g_ble.terminate) == terms, "a hung DID dropped the link");
    CHECK(car(&g_car.bad_state_rpm) == 0, "hang: Mode 01 found the DID's state");

    /* The link drops: the readings go, whether the car answers stays. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "not off");
    st = status();
    CHECK(st.did_2612 && !st.vtec_sol.valid && !st.vtec_psw.valid && !st.did_2612_b23.valid,
          "after the drop: did_2612 %d, sol %d", st.did_2612, st.vtec_sol.valid);
    CHECK(!muse_obd_vtec_on(&st), "VTEC up after the drop");
    check_off("DID drop");
    check_quiet("DID drop");
    CHECK(has_samples(), "the DID's link logged nothing");

    /* A new connection to an ECU that refuses it: asked once, never again. */
    set_car(DID_REFUSE, "A7");
    ble_lock();
    g_car.did_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    clear_log();
    s_want = 300;
    CHECK(await(log_reach, 5000), "refused: not polling");
    CHECK(car(&g_car.did_reqs) == 1, "refused: asked %d times", car(&g_car.did_reqs));
    st = status();
    CHECK(!st.did_2612 && !st.vtec_sol.valid && !muse_obd_vtec_on(&st), "refused: still supported");
    CHECK(st.rpm_valid && car(&g_car.bad_state_rpm) == 0, "refused: RPM");

    /* Silent (NO DATA): the same. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "not off");
    set_car(DID_NO_DATA, "A7");
    ble_lock();
    g_car.did_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    clear_log();
    CHECK(await(log_reach, 5000), "silent: not polling");
    CHECK(car(&g_car.did_reqs) == 1 && !status().did_2612, "silent: asked %d times", car(&g_car.did_reqs));

    /* CAN 11-bit: never asked at all, and no AT change. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "not off");
    set_car(DID_ON, "A6");
    ble_lock();
    g_car.did_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    clear_log();
    CHECK(await(log_reach, 5000), "CAN 11: not polling");
    CHECK(car(&g_car.did_reqs) == 0, "CAN 11-bit: asked the DID");
    ble_lock();
    for (int i = 0; i < g_car.logged; i++) {
        CHECK(strncmp(g_car.log[i], "ATSH", 4) && strncmp(g_car.log[i], "ATCRA", 5), "CAN 11-bit: %s",
              g_car.log[i]);
    }
    ble_unlock();
    st = status();
    CHECK(!muse_obd_vtec_on(&st) && st.rpm_valid, "CAN 11-bit");

    /* OBD-DID-CARDS-TASK: DIDs 0x2610 and 0x2663 against the real task. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "cards DIDs: not off");
    /* Off, the bench's ">misfire" does nothing at all. */
    int base_fired = fired();
    CHECK(muse_obd_bench_misfire() == -1 && fired() == base_fired, "bench while off");
    set_car(DID_ON, "A7");
    set_dids(DID_ON, DID_ON);
    set_mis(S2000_2663_NONE, 0, 0, 0, 0);
    ble_lock();
    g_car.did_reqs = g_car.did2610_reqs = g_car.did2663_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    CHECK(settle(cards_dids_read), "cards DIDs: no readings");
    st = status();
    CHECK(st.did_2612 && st.did_2610 && st.did_2663, "answered: %d %d %d", st.did_2612, st.did_2610, st.did_2663);
    CHECK(st.ac_clutch.value == 1 && st.generator_pct.valid && st.generator_pct.value == 45, "2610: ac %d gen %d",
          st.ac_clutch.value, st.generator_pct.value);
    for (int c = 0; c < 4; c++) {
        CHECK(st.misfire[c].valid && st.misfire[c].value == 0, "2663: cylinder %d %d", c + 1, st.misfire[c].value);
    }
    CHECK(st.misfire_rose == 0 && fired() == base_fired, "the baseline read fired");

    /* The schedule: every DID exchange whole and alone, 0x2612 the most
     * often (1 s against the pair's 3 s), each probe once, Mode 01 safe. */
    clear_log();
    s_want = car(&g_car.did2663_reqs) + 3;
    CHECK(settle(reqs_2663_reach), "2663 not read again");
    int reads[3];
    check_did_exchanges("cards DIDs", reads);
    CHECK(reads[0] >= 2 * reads[1] && reads[0] >= 2 * reads[2] && reads[1] >= 2 && reads[2] >= 3,
          "reads 2612 %d, 2610 %d, 2663 %d", reads[0], reads[1], reads[2]);
    CHECK(car(&g_car.bad_state_rpm) == 0, "cards DIDs: Mode 01 found a DID's state");
    st = status();
    CHECK(st.polling && st.rpm_valid && st.rpm == 1726 && muse_obd_vtec_on(&st), "cards DIDs: RPM or VTEC lost");

    /* A cylinder 1 increase on OBDb's own frames, the engine running: one
     * warning, through the coolant warning's plumbing, and the amber mark. */
    s_want_fired = base_fired + 1;
    set_mis(S2000_2663_CYL1, 0, 0, 0, 0);
    CHECK(settle(fired_reach), "an increase didn't fire");
    /* (The amber mark lasts one read, which the free-running task can be
     * past by now: the trigger's own test checks it read by read.) */
    CHECK(status().misfire[0].value == 1, "after the increase: %d", status().misfire[0].value);
    /* Counted once: the next reads leave it and clear the mark. */
    s_want = car(&g_car.did2663_reqs) + 2;
    CHECK(settle(reqs_2663_reach), "2663 stopped");
    CHECK(fired() == base_fired + 1 && status().misfire_rose == 0, "fired %d, rose %x", fired() - base_fired,
          status().misfire_rose);
    /* The bench inside the cooldown: the trigger lets it go... */
    CHECK(muse_obd_bench_misfire() == 0 && fired() == base_fired + 1, "bench inside the cooldown fired");
    /* ...and the car's baseline is untouched by it: the same counts again
     * fire nothing, and the card isn't marked. */
    s_want = car(&g_car.did2663_reqs) + 2;
    CHECK(settle(reqs_2663_reach), "2663 stopped after the bench");
    CHECK(fired() == base_fired + 1 && status().misfire_rose == 0, "the bench moved the baseline");
    /* A cylinder 2 increase inside the cooldown: marked, not fired. */
    set_mis(NULL, 1, 1, 0, 0);
    s_want = car(&g_car.did2663_reqs) + 1;
    CHECK(settle(reqs_2663_reach), "2663 stopped (cylinder 2)");
    CHECK(fired() == base_fired + 1, "fired inside the cooldown");

    /* A short read mid-connection: no reading, the last one ages, nothing
     * made up, still asked. Garbled the same. */
    set_dids(DID_ON, DID_SHORT);
    set_mis(NULL, 50, 50, 50, 50);
    s_want = car(&g_car.did2663_reqs) + 2;
    CHECK(settle(reqs_2663_reach), "short: stopped asking");
    st = status();
    CHECK(st.did_2663 && st.misfire[0].valid && st.misfire[0].value == 1 && st.misfire[1].value == 1,
          "short: %d %d", st.misfire[0].value, st.misfire[1].value);
    CHECK(st.misfire[0].age_ms > 0, "short: the reading didn't age");
    set_dids(DID_GARBAGE, DID_GARBAGE);
    s_want = car(&g_car.did2610_reqs) + 2;
    CHECK(settle(reqs_2610_reach), "garbled 2610: stopped asking");
    st = status();
    CHECK(st.did_2610 && st.ac_clutch.valid && st.ac_clutch.value == 1 && st.polling && st.rpm_valid,
          "garbled 2610: readings %d/%d", st.ac_clutch.valid, st.ac_clutch.value);
    CHECK(fired() == base_fired + 1 && ble(&g_ble.terminate) >= 0, "garbled: fired");

    /* Off: no DID read at all, the readings go, the answers stay. Five
     * quiet minutes pass, so the cooldown is over when it comes back. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "cards DIDs: not off (2)");
    st = status();
    CHECK(st.did_2610 && st.did_2663 && !st.ac_clutch.valid && !st.generator_pct.valid && !st.misfire[0].valid
          && st.misfire_rose == 0, "off: the readings stayed");
    int r2610 = car(&g_car.did2610_reqs), r2663 = car(&g_car.did2663_reqs);
    check_off("cards DIDs");
    check_quiet("cards DIDs");
    CHECK(car(&g_car.did2610_reqs) == r2610 && car(&g_car.did2663_reqs) == r2663, "a DID read while off");
    CHECK(fired() == base_fired + 1, "a warning while off");

    /* A new connection: its first read is a baseline, however high. */
    set_dids(DID_ON, DID_ON);
    set_mis(NULL, 7, 7, 7, 7);
    muse_obd_set_enabled(true);
    link_up(CONN);
    CHECK(settle(cards_dids_read), "reconnect: no readings");
    s_want = car(&g_car.did2663_reqs) + 1;
    CHECK(settle(reqs_2663_reach), "reconnect: 2663");
    CHECK(fired() == base_fired + 1, "the reconnect's baseline fired");
    /* The engine off: an increase is let go, and stays let go once it runs. */
    ble_lock();
    g_car.engine_off = true;
    ble_unlock();
    CHECK(settle(rpm_gone), "RPM didn't go");
    set_mis(NULL, 8, 7, 7, 7);
    s_want = car(&g_car.did2663_reqs) + 2;
    CHECK(settle(reqs_2663_reach), "engine off: 2663");
    CHECK(fired() == base_fired + 1 && status().misfire[0].value == 8, "engine off: fired");
    ble_lock();
    g_car.engine_off = false;
    ble_unlock();
    CHECK(settle(rpm_back), "RPM didn't come back");
    s_want = car(&g_car.did2663_reqs) + 2;
    CHECK(settle(reqs_2663_reach), "engine on: 2663");
    CHECK(fired() == base_fired + 1, "an increase seen with the engine off fired later");
    /* Running, past the cooldown: an increase fires; then the bench, within
     * the new cooldown, doesn't. */
    s_want_fired = base_fired + 2;
    set_mis(NULL, 8, 7, 7, 9);
    CHECK(settle(fired_reach), "running, past the cooldown: no fire");
    CHECK(muse_obd_bench_misfire() == 0, "bench right after a fire");

    /* An ECU that refuses both new DIDs: each asked once on the connection. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "refuse: not off");
    check_quiet("before refuse");
    set_dids(DID_REFUSE, DID_REFUSE);
    ble_lock();
    g_car.did_reqs = g_car.did2610_reqs = g_car.did2663_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    s_want = 6;
    CHECK(settle(did_reqs_reach), "refuse: 2612 not polled");
    st = status();
    CHECK(car(&g_car.did2610_reqs) == 1 && car(&g_car.did2663_reqs) == 1, "refused: asked %d and %d times",
          car(&g_car.did2610_reqs), car(&g_car.did2663_reqs));
    CHECK(!st.did_2610 && !st.did_2663 && st.did_2612 && !st.ac_clutch.valid && !st.misfire[0].valid,
          "refused: still answered");
    /* With the trigger idle the bench fires through it, past the cooldown
     * (the quiet minutes above). */
    s_want_fired = fired() + 1;
    CHECK(muse_obd_bench_misfire() == 1 && fired_reach(), "bench: no fire");
    CHECK(muse_obd_bench_misfire() == 0, "bench: fired twice inside the cooldown");

    /* The bench before a connection's first read leaves no baseline of its
     * own behind: that read is still the car's baseline, unmarked. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "bench baseline: not off");
    check_quiet("bench baseline");   /* past the bench's cooldown */
    set_dids(DID_ON, DID_ON);
    set_mis(NULL, 4, 4, 4, 4);
    muse_obd_set_enabled(true);
    int before = fired();
    CHECK(muse_obd_bench_misfire() == 1 && fired() == before + 1, "bench before the link: no fire");
    link_up(CONN);
    CHECK(settle(cards_dids_read), "bench baseline: no readings");
    /* Watched the whole time, not sampled: a leaked count would mark
     * every cylinder for a read. */
    CHECK(!seen_during(rose_set, 8000), "the bench's count leaked into the car's baseline");
    st = status();
    CHECK(st.misfire[0].value == 4 && fired() == before + 1, "bench baseline: %d, fired %d", st.misfire[0].value,
          fired() - before);

    /* OBD-STEERING-ANGLE-TASK Phase 1: the VSA at DA28 against the task. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "vsa: not off");
    /* Off, a scan can't even be asked for: no VSA requests, ever. */
    CHECK(!muse_obd_vsa_scan_start(), "a scan started while off");
    set_vsa(DID_ON, true, 0x8000);   /* pedal pressed, 47.50 bar */
    ble_lock();
    g_car.vsa4004_reqs = g_car.vsa4005_reqs = g_car.vsa_other_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    CHECK(settle(vsa_read), "vsa: no readings from the probe or the schedule");
    st = status();
    CHECK(st.vsa == 1, "vsa verdict %d", st.vsa);
    CHECK(st.vsa_brk_sw.value == 1 && st.vsa_brk_p.value == 4750, "vsa: sw %d, p %d", st.vsa_brk_sw.value,
          st.vsa_brk_p.value);
    CHECK(st.vsa_brk_sw.age_ms != UINT32_MAX && st.vsa_brk_p.age_ms != UINT32_MAX, "vsa: ages");
    CHECK(st.rpm_valid && st.rpm == 1726 && car(&g_car.bad_state_rpm) == 0, "vsa: RPM");
    /* The pedal comes up: the grouped 500 ms tier follows it. */
    set_vsa(DID_ON, false, 0);
    CHECK(settle(brk_released), "vsa: the release never read");
    set_vsa(DID_ON, true, 0x8000);
    CHECK(settle(vsa_read), "vsa: the press never read");

    /* Choreography: VSA only inside DA28 sessions, the pair grouped in
     * one session, Mode 01's state back after each, DA10 untouched. */
    clear_log();
    s_want = car(&g_car.vsa4005_reqs) + 3;
    CHECK(settle(reqs_4005_reach), "vsa: 4005 not read again");
    CHECK(check_vsa_exchanges("vsa") >= 1, "vsa: 4004 and 4005 never shared a session");
    CHECK(car(&g_car.bad_state_rpm) == 0, "vsa: Mode 01 found a session's state");

    /* The raw scan: every DID's outcome stored, driving polls throughout. */
    int rpm_rows = log_count(&g_samples);
    CHECK(muse_obd_vsa_scan_start(), "vsa scan: not accepted");
    CHECK(!strcmp(status().vsa_scan, "running"), "vsa scan: state %s", status().vsa_scan);
    CHECK(settle(scan_done), "vsa scan: never done");
    st = status();
    CHECK(st.vsa_scan_answered == 3, "vsa scan: %d answered", st.vsa_scan_answered);
    CHECK(st.rpm_valid && log_count(&g_samples) > rpm_rows, "vsa scan: driving polls starved");
    muse_vsa_raw_t raw;
    CHECK(muse_obd_vsa_raw(0x4004, &raw) == 0 && raw.kind == MUSE_VSA_RAW_POSITIVE && raw.len == 23
          && raw.bytes[0] == 0x62 && raw.bytes[1] == 0x40 && raw.bytes[2] == 0x04 && (raw.bytes[3 + 15] & 0x80),
          "vsa scan: 4004 kind %d len %u", raw.kind, (unsigned)raw.len);
    CHECK(muse_obd_vsa_raw(0x4000, &raw) == 0 && raw.kind == MUSE_VSA_RAW_POSITIVE && raw.len == 7
          && raw.bytes[3] == 0x01 && raw.bytes[6] == 0x04, "vsa scan: 4000");
    CHECK(muse_obd_vsa_raw(0x4030, &raw) == 0 && raw.kind == MUSE_VSA_RAW_NEGATIVE && raw.nrc == 0x31,
          "vsa scan: 4030 kind %d nrc %02X", raw.kind, raw.nrc);
    CHECK(muse_obd_vsa_raw(0x4001, &raw) == 0 && raw.kind == MUSE_VSA_RAW_TIMEOUT, "vsa scan: 4001 silent");
    CHECK(muse_obd_vsa_raw(0xE602, &raw) == 0 && raw.kind == MUSE_VSA_RAW_TIMEOUT, "vsa scan: E602 silent");
    CHECK(muse_obd_vsa_raw(0x2612, &raw) == -1 && muse_obd_vsa_raw(0x9999, &raw) == -1, "vsa scan: foreign DIDs");
    /* The scan's own exchanges obey the same choreography. */
    CHECK(check_vsa_exchanges("vsa scan") >= 0, "vsa scan: choreography");
    CHECK(car(&g_car.bad_state_rpm) == 0, "vsa scan: Mode 01 found a session's state");

    /* Off: no VSA read at all, the readings go, the verdict stays, and the
     * scan's results were that connection's. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "vsa: not off (2)");
    st = status();
    CHECK(st.vsa == 1 && !st.vsa_brk_sw.valid && !st.vsa_brk_p.valid, "vsa off: verdict %d, readings left", st.vsa);
    CHECK(!strcmp(st.vsa_scan, "idle") && muse_obd_vsa_raw(0x4004, &raw) == 0 && raw.kind == MUSE_VSA_RAW_NONE,
          "vsa off: scan results kept");
    int v4 = car(&g_car.vsa4004_reqs), v5 = car(&g_car.vsa4005_reqs), vo = car(&g_car.vsa_other_reqs);
    check_quiet("vsa");
    CHECK(car(&g_car.vsa4004_reqs) == v4 && car(&g_car.vsa4005_reqs) == v5 && car(&g_car.vsa_other_reqs) == vo,
          "a VSA read while off");

    /* A VSA that refuses the probe: asked once, 4005 never, no retry storm. */
    set_vsa(DID_REFUSE, true, 0x8000);
    ble_lock();
    g_car.vsa4004_reqs = g_car.vsa4005_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    clear_log();
    s_want = 300;
    CHECK(await(log_reach, 5000), "vsa refused: not polling");
    st = status();
    CHECK(car(&g_car.vsa4004_reqs) == 1 && car(&g_car.vsa4005_reqs) == 0, "vsa refused: asked %d/%d times",
          car(&g_car.vsa4004_reqs), car(&g_car.vsa4005_reqs));
    CHECK(st.vsa == 0 && !st.vsa_brk_sw.valid && !st.vsa_brk_p.valid && st.rpm_valid, "vsa refused: verdict %d",
          st.vsa);

    /* Silent (a dead address): the same one timed-out exchange per
     * connection, polling unaffected. */
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 3000), "vsa silent: not off");
    set_vsa(DID_NO_DATA, true, 0x8000);
    ble_lock();
    g_car.vsa4004_reqs = g_car.vsa4005_reqs = 0;
    ble_unlock();
    muse_obd_set_enabled(true);
    link_up(CONN);
    clear_log();
    CHECK(await(log_reach, 5000), "vsa silent: not polling");
    CHECK(car(&g_car.vsa4004_reqs) == 1 && car(&g_car.vsa4005_reqs) == 0 && status().vsa == 0,
          "vsa silent: asked %d/%d times", car(&g_car.vsa4004_reqs), car(&g_car.vsa4005_reqs));
    set_vsa(DID_NO_DATA, false, 0x8000);

    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_did\n");
    return 0;
}
#endif
