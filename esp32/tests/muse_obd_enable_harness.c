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

static int g_vtec_fired, g_coolant_fired, g_warmup_fired;

bool muse_link_ble_started(void) { return true; }
void muse_state_start_vtec(void) { g_vtec_fired++; }
void muse_state_start_coolant(void) { g_coolant_fired++; }
void muse_state_start_warmup(void) { g_warmup_fired++; }

/* ---- A scripted LELink^2 ---- */

#define CONN 1
#define VAL 3       /* FFE1's value handle */
#define CCCD 4

static struct {
    int disc, disc_cancel, connect, conn_cancel, terminate;   /* calls */
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

/* The adapter's answer to one ELM command. */
static const char *answer(const char *cmd)
{
    if (!strcmp(cmd, "ATZ")) return "\r\rELM327 v1.5\r\r>";
    if (!strcmp(cmd, "ATDPN")) return "A6\r\r>";
    if (!strncmp(cmd, "AT", 2)) return "OK\r\r>";
    if (!strcmp(cmd, "0100")) return "41 00 BE 1F A8 12\r\r>";   /* no PID 20: the scan stops here */
    if (!strcmp(cmd, "010C")) return "41 0C 1A F8\r\r>";         /* 1726 rpm */
    if (!strcmp(cmd, "0105")) return "41 05 5A\r\r>";            /* 50 C */
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

    /* Off while polling: dropped at once, with the BLE link terminated. */
    int terms = ble(&g_ble.terminate);
    muse_obd_set_enabled(false);
    CHECK(await(is_off, 200), "polling: not off: %s", status().state);
    CHECK(ble(&g_ble.terminate) == terms + 1, "polling: link not terminated");
    CHECK(g_ble.terminated == CONN, "polling: terminated %u", g_ble.terminated);
    check_off("polling");
    check_quiet("polling");

    /* The reactions don't play while off. */
    muse_obd_fire_vtec();
    muse_obd_fire_coolant();
    muse_obd_fire_warmup();
    CHECK(!g_vtec_fired && !g_coolant_fired && !g_warmup_fired, "a reaction played while off");

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
    muse_obd_fire_vtec();
    CHECK(g_vtec_fired == 1, "back on: no reaction");

    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_enable\n");
    return 0;
}
