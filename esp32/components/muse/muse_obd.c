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

#include "muse_obd.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"

#include "muse_link.h"
#include "muse_obd_elm.h"
#include "muse_obd_log.h"
#include "muse_state.h"

static const char *TAG = "muse_obd";

/*
 * Link's BLE server owns the NimBLE host (main/ble_server.c) and sees only the
 * GAP events of the connections it advertised for. NimBLE hands a central's
 * events to the callback given to ble_gap_disc() and ble_gap_connect(), so
 * everything about the adapter's connection, its notifications included,
 * comes to gap_event() here and never reaches Link's or Muse's phone setup.
 *
 * The callbacks run on the host task and only post to this task (a queue for
 * events, a stream buffer for the adapter's bytes); this task starts each GAP
 * and GATT procedure and waits for its result, so nothing blocks the host.
 */

#define ADAPTER_NAME "LELink"         /* advertises as "LELink BLE OBDII" */
#define SVC_UUID 0xFFE0
#define CHR_UUID 0xFFE1               /* write and notify, both */
#define CCCD_UUID 0x2902

#define SCAN_MS 10000
#define SCAN_ITVL 160                 /* 100 ms: scan 30 ms of it, leaving the radio to Wi-Fi */
#define SCAN_WINDOW 48
#define CONNECT_MS 5000
#define GATT_MS 5000
#define ATZ_MS 3000
#define CMD_MS 1500
#define POLL_MS 1500                  /* no '>' for this long: drop and reconnect */
#define SEARCH_MS 12000               /* while the ELM may be searching for the car's protocol */
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 60000
#define RX_BYTES 512

/* Polling is response-driven and scheduled (muse_obd_elm.h): each request
 * goes out the moment the last reply is parsed, the scheduler picks the
 * most overdue signal, and due PIDs share one multi-PID Mode 01 request,
 * RPM (010C) first among them and the standing filler between targets. */
#define RPM 0x0C

typedef enum {
    EV_FOUND = 1,       /* addr: the adapter's advertisement */
    EV_SCAN_DONE,
    EV_CONNECT,         /* status, conn */
    EV_DISCONNECT,      /* conn */
    EV_SVC,             /* status, a..b: the service's handles */
    EV_CHR,             /* status, a: value handle, b: properties */
    EV_DSC,             /* status, a: the CCCD's handle (0: none found) */
    EV_WRITE,           /* status */
} ev_type_t;

#define BIT_EV(t) (1u << (t))
/* In a wait's mask: the wait takes a connection down, so it runs even when
 * turned off. Only events of the current connection pass, so it is harmless
 * when there is none. */
#define UNWIND BIT_EV(EV_DISCONNECT)

typedef struct {
    uint8_t type;
    int status;
    uint16_t conn;
    uint16_t a, b;
    ble_addr_t addr;
} obd_ev_t;

static TaskHandle_t s_task;
static QueueHandle_t s_events;
static StreamBufferHandle_t s_rx_bytes;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_val;       /* FFE1's value handle, once subscribed */
static volatile bool s_found;         /* this scan has found the adapter */
/* Settings' OBD switch (muse_obd_set_enabled). Off, every wait below gives up
 * at once, except the ones that take a connection down (UNWIND), and the
 * task sits in "off" without scanning until it is turned back on. */
static volatile bool s_enabled = true;
static uint8_t s_props;
static uint8_t s_own_addr;
static muse_elm_rx_t s_rx;
static muse_coolant_trigger_t s_coolant;
static muse_warmup_trigger_t s_warmup;
static int s_rpm = -1;                /* the latest RPM; -1 when unknown (engine off, link down) */
static muse_elm_support_t s_support;  /* this connection's support scan; all unknown until it answers */
static muse_sched_t s_sched;          /* the poll pipeline's schedule, per connection */
static muse_link_stats_t s_link;      /* the poll link's numbers (muse_obd_elm.h) */
static muse_tune_t s_tune;            /* the ATST policy's state */
static bool s_tuned;                  /* this connection has had its ATAT2 */
static uint32_t s_elm_rtt_ms;         /* the last elm_cmd exchange's round trip */
static bool s_have_protocol;          /* this connection has read ATDPN */
static int s_protocol = -1;           /* and found this protocol (7: CAN 29/500) */
/* DID 0x2612 on this connection: not asked yet, answered, or not (refused,
 * silent, garbled, or not a CAN 29-bit link: then never asked again). */
static enum { DID_UNPROBED, DID_YES, DID_NO } s_did;
static const uint8_t LOG_DID[] = { MUSE_LOG_DID_UNKNOWN, MUSE_LOG_DID_YES, MUSE_LOG_DID_NO };

/* muse_obd_status()'s snapshot: written by this task, read from any task. */
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_obd_status_t s_status = { .state = "off", .enabled = true, .protocol = -1 };
static uint32_t s_rpm_at, s_coolant_at;   /* now_ms() of each reading */
static uint32_t s_extra_at[MUSE_OBD_EXTRAS];
static uint32_t s_vtec_at;                /* now_ms() of the last DID 0x2612 reading */
static bool s_vtec_bench;                 /* ">vtecsol": the reading is the bench's, always fresh */

const muse_obd_extra_info_t MUSE_OBD_EXTRA[MUSE_OBD_EXTRAS] = {
    [MUSE_OBD_LOAD] = { 0x04, "load_pct", "load_age_ms" },
    [MUSE_OBD_MAP] = { 0x0B, "map_kpa", "map_age_ms" },
    [MUSE_OBD_SPEED] = { 0x0D, "speed_kmh", "speed_age_ms" },
    [MUSE_OBD_TIMING] = { 0x0E, "timing_deg", "timing_age_ms" },
    [MUSE_OBD_IAT] = { 0x0F, "iat_c", "iat_age_ms" },
    [MUSE_OBD_THROTTLE] = { 0x11, "throttle_pct", "throttle_age_ms" },
    [MUSE_OBD_STFT] = { 0x06, "stft_pct", "stft_age_ms" },
    [MUSE_OBD_LTFT] = { 0x07, "ltft_pct", "ltft_age_ms" },
    [MUSE_OBD_AMBIENT] = { 0x46, "ambient_c", "ambient_age_ms" },
    [MUSE_OBD_AFR] = { 0x34, "afr", "afr_age_ms", 1 },
    [MUSE_OBD_LAMBDA] = { 0x34, "lambda", "lambda_age_ms", 3 },
    [MUSE_OBD_PEDAL_D] = { 0x49, "pedal_d_pct", "pedal_d_age_ms" },
    [MUSE_OBD_PEDAL_E] = { 0x4A, "pedal_e_pct", "pedal_e_age_ms" },
    [MUSE_OBD_MODULE_V] = { 0x42, "module_v", "module_v_age_ms", 3 },
    [MUSE_OBD_BARO] = { 0x33, "baro_kpa", "baro_age_ms" },
    [MUSE_OBD_FUEL] = { 0x2F, "fuel_pct", "fuel_age_ms" },
};

/* Each extra's parser, and the result that means it read a value. AFR and
 * lambda share PID 34's: it reads the raw ratio, extra_value() converts. */
static const struct {
    muse_elm_result_t (*parse)(const char *resp, int *value);
    muse_elm_result_t ok;
} EXTRA_PARSE[MUSE_OBD_EXTRAS] = {
    [MUSE_OBD_LOAD] = { muse_elm_parse_load, MUSE_ELM_LOAD },
    [MUSE_OBD_MAP] = { muse_elm_parse_map, MUSE_ELM_MAP },
    [MUSE_OBD_SPEED] = { muse_elm_parse_speed, MUSE_ELM_SPEED },
    [MUSE_OBD_TIMING] = { muse_elm_parse_timing, MUSE_ELM_TIMING },
    [MUSE_OBD_IAT] = { muse_elm_parse_iat, MUSE_ELM_IAT },
    [MUSE_OBD_THROTTLE] = { muse_elm_parse_throttle, MUSE_ELM_THROTTLE },
    [MUSE_OBD_STFT] = { muse_elm_parse_stft, MUSE_ELM_STFT },
    [MUSE_OBD_LTFT] = { muse_elm_parse_ltft, MUSE_ELM_LTFT },
    [MUSE_OBD_AMBIENT] = { muse_elm_parse_ambient, MUSE_ELM_AMBIENT },
    [MUSE_OBD_AFR] = { muse_elm_parse_afr, MUSE_ELM_AFR },
    [MUSE_OBD_LAMBDA] = { muse_elm_parse_afr, MUSE_ELM_AFR },
    [MUSE_OBD_PEDAL_D] = { muse_elm_parse_pedal_d, MUSE_ELM_PEDAL_D },
    [MUSE_OBD_PEDAL_E] = { muse_elm_parse_pedal_e, MUSE_ELM_PEDAL_E },
    [MUSE_OBD_MODULE_V] = { muse_elm_parse_module_v, MUSE_ELM_MODULE_V },
    [MUSE_OBD_BARO] = { muse_elm_parse_baro, MUSE_ELM_BARO },
    [MUSE_OBD_FUEL] = { muse_elm_parse_fuel, MUSE_ELM_FUEL },
};

/* ---- On the host task: post and wake ---- */

static void post(const obd_ev_t *ev)
{
    if (xQueueSend(s_events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event %d dropped", ev->type);
    }
    xTaskNotifyGive(s_task);
}

static bool is_adapter(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return false;
    }
    if (f.name && f.name_len >= strlen(ADAPTER_NAME)
        && memcmp(f.name, ADAPTER_NAME, strlen(ADAPTER_NAME)) == 0) {
        return true;
    }
    for (int i = 0; i < f.num_uuids16; i++) {
        if (ble_uuid_u16(&f.uuids16[i].u) == SVC_UUID) {
            return true;
        }
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    obd_ev_t ev = { 0 };
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (!s_found && is_adapter(&event->disc)) {
            s_found = true;
            ev.type = EV_FOUND;
            ev.addr = event->disc.addr;
            post(&ev);
        }
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ev.type = EV_SCAN_DONE;
        post(&ev);
        break;
    case BLE_GAP_EVENT_CONNECT:
        ev.type = EV_CONNECT;
        ev.status = event->connect.status;
        ev.conn = event->connect.conn_handle;
        post(&ev);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ev.type = EV_DISCONNECT;
        ev.status = event->disconnect.reason;
        ev.conn = event->disconnect.conn.conn_handle;
        post(&ev);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        /* The answer to connect()'s fast-parameter request, when it comes. */
        ESP_LOGI(TAG, "connection parameters update: status %d", event->conn_update.status);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (!s_val || event->notify_rx.conn_handle != s_conn || event->notify_rx.attr_handle != s_val) {
            break;
        }
        uint8_t buf[64];
        int len = OS_MBUF_PKTLEN(event->notify_rx.om);
        for (int off = 0; off < len;) {
            int n = len - off < (int)sizeof(buf) ? len - off : (int)sizeof(buf);
            if (os_mbuf_copydata(event->notify_rx.om, off, n, buf) != 0) {
                break;
            }
            if (xStreamBufferSend(s_rx_bytes, buf, n, 0) != (size_t)n) {
                ESP_LOGW(TAG, "rx overflow");
                break;
            }
            off += n;
        }
        xTaskNotifyGive(s_task);
        break;
    }
    default:
        break;
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg)
{
    obd_ev_t *found = arg;
    if (error->status == 0 && svc) {
        found->status = 0;
        found->a = svc->start_handle;
        found->b = svc->end_handle;
        return 0;
    }
    obd_ev_t ev = *found;
    ev.type = EV_SVC;
    ev.conn = conn;
    if (error->status != BLE_HS_EDONE) {
        ev.status = error->status;
    }
    post(&ev);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    obd_ev_t *found = arg;
    if (error->status == 0 && chr) {
        if (found->status) {
            found->status = 0;
            found->a = chr->val_handle;
            found->b = chr->properties;
        }
        return 0;
    }
    obd_ev_t ev = *found;
    ev.type = EV_CHR;
    ev.conn = conn;
    if (error->status != BLE_HS_EDONE) {
        ev.status = error->status;
    }
    post(&ev);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *error, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    (void)chr_val;
    obd_ev_t *found = arg;
    if (error->status == 0 && dsc) {
        if (!found->a && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(CCCD_UUID)) == 0) {
            found->a = dsc->handle;
        }
        return 0;
    }
    obd_ev_t ev = *found;
    ev.type = EV_DSC;
    ev.conn = conn;
    ev.status = error->status == BLE_HS_EDONE ? 0 : error->status;
    post(&ev);
    return 0;
}

static int on_write(uint16_t conn, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    obd_ev_t ev = { .type = EV_WRITE, .status = error->status, .conn = conn };
    post(&ev);
    return 0;
}

/* ---- On this task ---- */

/* Throws away the adapter's bytes so far. The host task may be writing, so
 * read them out rather than reset the buffer under it. */
static void drain_rx(void)
{
    uint8_t buf[32];
    while (xStreamBufferReceive(s_rx_bytes, buf, sizeof(buf), 0) > 0) {
    }
    muse_elm_rx_reset(&s_rx);
}

static uint32_t now_ms(void)
{
    return (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount());
}

/* ---- The status snapshot ---- */

static void set_state(const char *state)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.state = state;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_flag(bool *flag, bool on)
{
    taskENTER_CRITICAL(&s_status_lock);
    *flag = on;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_rpm(bool valid, int rpm)
{
    uint32_t at = now_ms();
    taskENTER_CRITICAL(&s_status_lock);
    s_status.rpm_valid = valid;
    if (valid) {
        s_status.rpm = rpm;
        s_rpm_at = at;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_coolant(int coolant_c)
{
    uint32_t at = now_ms();
    taskENTER_CRITICAL(&s_status_lock);
    s_status.coolant_valid = true;
    s_status.coolant_c = coolant_c;
    s_coolant_at = at;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_extra(muse_obd_extra_t i, int value)
{
    uint32_t at = now_ms();
    taskENTER_CRITICAL(&s_status_lock);
    s_status.extra[i].valid = true;
    s_status.extra[i].value = value;
    s_extra_at[i] = at;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_protocol(int protocol)
{
    char name[sizeof(s_status.protocol_name)];
    muse_elm_protocol_name(protocol, name, sizeof(name));
    taskENTER_CRITICAL(&s_status_lock);
    s_status.protocol = protocol;
    memcpy(s_status.protocol_name, name, sizeof(name));
    taskEXIT_CRITICAL(&s_status_lock);
}

/* A scan that read the 01-20 mask: the snapshot takes all three ranges from
 * it, so a range this scan didn't reach doesn't keep an older car's mask. */
static void set_support(const muse_elm_support_t *sup)
{
    taskENTER_CRITICAL(&s_status_lock);
    for (int i = 0; i < MUSE_OBD_SUPPORT_RANGES; i++) {
        s_status.support_valid[i] = sup->valid[i];
        s_status.support[i] = sup->valid[i] ? sup->masks[i] : 0;
    }
    taskEXIT_CRITICAL(&s_status_lock);
}

/* The link is down: nothing is connected and no reading is current. The
 * protocol and the support masks stay (see muse_obd_status_t). */
static void set_link_down(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.connected = false;
    s_status.subscribed = false;
    s_status.polling = false;
    s_status.rpm_valid = false;
    s_status.coolant_valid = false;
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        s_status.extra[i].valid = false;
    }
    /* The DID's live bits go too; whether the car answers it stays. */
    s_status.vtec_sol.valid = false;
    s_status.vtec_psw.valid = false;
    s_status.did_2612_b23.valid = false;
    s_vtec_bench = false;
    /* The link's numbers were that connection's; the next one starts over. */
    s_status.link_exchanges = 0;
    s_status.link_timeouts = 0;
    s_status.link_last_rtt_ms = 0;
    s_status.link_rate_x100 = 0;
    s_status.link_batch_pct = -1;
    s_status.link_batch_off = false;
    taskEXIT_CRITICAL(&s_status_lock);
}

/* A DID 0x2612 reading (or the bench's), into the snapshot. */
static void set_vtec(const muse_vtec_did_t *v, bool bench)
{
    uint32_t at = now_ms();
    taskENTER_CRITICAL(&s_status_lock);
    s_status.vtec_sol = (muse_obd_reading_t){ .valid = true, .value = v->sol };
    s_status.vtec_psw = (muse_obd_reading_t){ .valid = true, .value = v->psw };
    s_status.did_2612_b23 = (muse_obd_reading_t){ .valid = !bench, .value = v->b23 };
    s_vtec_at = at;
    s_vtec_bench = bench;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void set_did_supported(bool yes)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.did_2612 = yes;
    taskEXIT_CRITICAL(&s_status_lock);
}

/* The poll loop's exchanges, into the link's numbers (s_link is this task's;
 * the lock is for muse_obd_status()'s copy). */
static void note_exchange(uint32_t rtt_ms, int readings, bool via_batch)
{
    taskENTER_CRITICAL(&s_status_lock);
    muse_link_stats_exchange(&s_link, now_ms(), rtt_ms, readings, via_batch);
    taskEXIT_CRITICAL(&s_status_lock);
}

static void note_timeout(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    muse_link_stats_timeout(&s_link);
    taskEXIT_CRITICAL(&s_status_lock);
}

static void note_batch_off(void)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_status.link_batch_off = true;
    taskEXIT_CRITICAL(&s_status_lock);
}

void muse_obd_status(muse_obd_status_t *out)
{
    if (!out) {
        return;
    }
    uint32_t now = now_ms();
    uint32_t rpm_at, coolant_at, extra_at[MUSE_OBD_EXTRAS], vtec_at;
    bool bench;
    muse_link_stats_t link;
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    vtec_at = s_vtec_at;
    bench = s_vtec_bench;
    rpm_at = s_rpm_at;
    coolant_at = s_coolant_at;
    memcpy(extra_at, s_extra_at, sizeof(extra_at));
    link = s_link;
    taskEXIT_CRITICAL(&s_status_lock);
    out->link_exchanges = link.exchanges;
    out->link_timeouts = link.timeouts;
    out->link_last_rtt_ms = link.last_rtt_ms;
    out->link_rate_x100 = muse_link_stats_rate_x100(&link);
    out->link_batch_pct = muse_link_stats_batch_pct(&link);
    muse_obd_log_status_t trips;
    muse_obd_log_status(&trips);
    out->log_card = trips.card;
    out->log_flash = trips.flash;
    out->log_store = trips.store;
    out->log_active = trips.active;
    out->log_trip_valid = trips.trip_valid;
    out->log_trip = trips.trip;
    out->log_pending = trips.pending;
    out->log_dropped = trips.dropped;
    out->log_full = trips.full;
    out->rpm_age_ms = out->rpm_valid ? now - rpm_at : UINT32_MAX;
    out->coolant_age_ms = out->coolant_valid ? now - coolant_at : UINT32_MAX;
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        out->extra[i].age_ms = out->extra[i].valid ? now - extra_at[i] : UINT32_MAX;
    }
    uint32_t vtec_age = bench ? 0 : now - vtec_at;
    out->vtec_sol.age_ms = out->vtec_sol.valid ? vtec_age : UINT32_MAX;
    out->vtec_psw.age_ms = out->vtec_psw.valid ? vtec_age : UINT32_MAX;
    out->did_2612_b23.age_ms = out->did_2612_b23.valid ? vtec_age : UINT32_MAX;
}

bool muse_obd_vtec_on(const muse_obd_status_t *st)
{
    return st->enabled && muse_vtec_engaged(st->vtec_sol.valid, st->vtec_sol.value != 0, st->vtec_sol.age_ms);
}

void muse_obd_bench_vtec_sol(bool on)
{
    if (s_enabled) {
        muse_vtec_did_t v = { .sol = on, .psw = !on };
        set_vtec(&v, true);
    }
}

bool muse_obd_linked(const muse_obd_status_t *st)
{
    return st->enabled && (st->subscribed || st->polling);
}

bool muse_obd_pid_unsupported(const muse_obd_status_t *st, int pid)
{
    muse_elm_support_t sup;
    for (int i = 0; i < MUSE_ELM_SUPPORT_RANGES; i++) {
        sup.valid[i] = st->support_valid[i];
        sup.masks[i] = st->support[i];
    }
    return muse_elm_pid_support(&sup, pid) == MUSE_ELM_PID_UNSUPPORTED;
}

/*
 * Waits up to timeout_ms for an event in `mask`, or with want_resp for the
 * adapter to finish a response (in s_rx). Returns the event's type, -1 for a
 * response, or 0 on a timeout, when the adapter disconnected (which always
 * ends the wait) or once Settings turns OBD off (unless mask has UNWIND).
 * Events of an earlier connection are dropped.
 */
static int wait_for(uint32_t mask, bool want_resp, uint32_t timeout_ms, obd_ev_t *out)
{
    TickType_t start = xTaskGetTickCount(), limit = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (!s_enabled && !(mask & UNWIND)) {
            return 0;
        }
        uint8_t buf[32];
        size_t n;
        while ((n = xStreamBufferReceive(s_rx_bytes, buf, sizeof(buf), 0)) > 0) {
            for (size_t i = 0; i < n; i++) {
                if (muse_elm_feed(&s_rx, buf[i]) && want_resp) {
                    return -1;   /* anything after the '>' was unasked-for: elm_cmd drains it */
                }
            }
        }
        obd_ev_t ev;
        while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
            bool conn_ev = ev.type >= EV_DISCONNECT;
            if (conn_ev && (s_conn == BLE_HS_CONN_HANDLE_NONE || ev.conn != s_conn)) {
                continue;
            }
            if (ev.type == EV_DISCONNECT) {
                ESP_LOGI(TAG, "adapter disconnected (reason 0x%x)", ev.status);
                s_val = 0;
                s_conn = BLE_HS_CONN_HANDLE_NONE;
                if (out) {
                    *out = ev;
                }
                return mask & BIT_EV(EV_DISCONNECT) ? EV_DISCONNECT : 0;
            }
            if (ev.type == EV_WRITE && want_resp && ev.status) {
                ESP_LOGW(TAG, "write failed: %d", ev.status);
                return 0;
            }
            if (mask & BIT_EV(ev.type)) {
                if (out) {
                    *out = ev;
                }
                return ev.type;
            }
        }
        TickType_t gone = xTaskGetTickCount() - start;
        if (gone >= limit) {
            return 0;
        }
        ulTaskNotifyTake(pdTRUE, limit - gone);
    }
}

static bool scan(ble_addr_t *addr)
{
    if (ble_hs_id_infer_auto(0, &s_own_addr) != 0) {
        return false;
    }
    xQueueReset(s_events);
    s_found = false;
    struct ble_gap_disc_params dp = {
        .itvl = SCAN_ITVL,
        .window = SCAN_WINDOW,
        .filter_duplicates = 1,   /* active: the name may be in the scan response */
    };
    int rc = ble_gap_disc(s_own_addr, SCAN_MS, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan didn't start: %d", rc);
        return false;
    }
    obd_ev_t ev;
    int got = wait_for(BIT_EV(EV_FOUND) | BIT_EV(EV_SCAN_DONE), false, SCAN_MS + 2000, &ev);
    if (got != EV_SCAN_DONE) {
        ble_gap_disc_cancel();
    }
    if (got != EV_FOUND) {
        ESP_LOGD(TAG, "no adapter in sight");
        return false;
    }
    *addr = ev.addr;
    set_flag(&s_status.adapter_found, true);
    return true;
}

static bool connect(const ble_addr_t *addr)
{
    int rc = ble_gap_connect(s_own_addr, addr, CONNECT_MS, NULL, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect didn't start: %d", rc);
        return false;
    }
    obd_ev_t ev;
    if (wait_for(BIT_EV(EV_CONNECT), false, CONNECT_MS + 2000, &ev) != EV_CONNECT) {
        ble_gap_conn_cancel();
        /* The cancel ends in a failed connect event, unless the connection
         * beat it: then it is up and the caller's drop() takes it down, so
         * none is left behind (turned off too: UNWIND). */
        if (wait_for(BIT_EV(EV_CONNECT) | UNWIND, false, 1000, &ev) != EV_CONNECT || ev.status != 0) {
            return false;
        }
    }
    if (ev.status != 0) {
        ESP_LOGI(TAG, "connect failed: %d", ev.status);
        return false;
    }
    s_conn = ev.conn;
    set_flag(&s_status.connected, true);
    ESP_LOGI(TAG, "adapter connected");
    /* The poll loop keeps one request outstanding at all times, so ask for
     * the fastest connection interval on offer (7.5-15 ms): the radio
     * shouldn't be the idle party either. A refusal (the adapter's or the
     * stack's) leaves the defaults in force and changes nothing else. */
    static const struct ble_gap_upd_params FAST = {
        .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,   /* 7.5 ms, 15 ms, 4 s */
    };
    int urc = ble_gap_update_params(ev.conn, &FAST);
    ESP_LOGI(TAG, "fast connection parameters requested: %d", urc);
    return true;
}

/* Finds FFE0/FFE1 and FFE1's CCCD, and turns notifications on. */
static bool subscribe(void)
{
    static obd_ev_t found;   /* filled in by the callbacks; one procedure at a time */
    obd_ev_t ev;

    found = (obd_ev_t){ .status = BLE_HS_ENOENT };
    if (ble_gattc_disc_svc_by_uuid(s_conn, BLE_UUID16_DECLARE(SVC_UUID), on_svc, &found) != 0
        || wait_for(BIT_EV(EV_SVC), false, GATT_MS, &ev) != EV_SVC || ev.status) {
        ESP_LOGW(TAG, "no FFE0 service");
        return false;
    }
    uint16_t svc_end = ev.b;

    found = (obd_ev_t){ .status = BLE_HS_ENOENT };
    if (ble_gattc_disc_chrs_by_uuid(s_conn, ev.a, svc_end, BLE_UUID16_DECLARE(CHR_UUID), on_chr, &found) != 0
        || wait_for(BIT_EV(EV_CHR), false, GATT_MS, &ev) != EV_CHR || ev.status) {
        ESP_LOGW(TAG, "no FFE1 characteristic");
        return false;
    }
    uint16_t val = ev.a;
    s_props = (uint8_t)ev.b;
    if (!(s_props & BLE_GATT_CHR_PROP_NOTIFY)
        || !(s_props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
        ESP_LOGW(TAG, "FFE1 can't write and notify (0x%02x)", s_props);
        return false;
    }

    found = (obd_ev_t){ 0 };
    uint16_t cccd = 0;
    if (val < svc_end && ble_gattc_disc_all_dscs(s_conn, val, svc_end, on_dsc, &found) == 0
        && wait_for(BIT_EV(EV_DSC), false, GATT_MS, &ev) == EV_DSC && ev.status == 0) {
        cccd = ev.a;
    }
    if (!cccd) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        cccd = val + 1;   /* where it nearly always is */
        ESP_LOGW(TAG, "no CCCD found; trying handle %u", cccd);
    }

    s_val = val;
    static const uint8_t on[2] = { 0x01, 0x00 };
    if (ble_gattc_write_flat(s_conn, cccd, on, sizeof(on), on_write, NULL) != 0
        || wait_for(BIT_EV(EV_WRITE), false, GATT_MS, &ev) != EV_WRITE || ev.status) {
        ESP_LOGW(TAG, "couldn't subscribe to FFE1");
        return false;
    }
    set_flag(&s_status.subscribed, true);
    return true;
}

/* Sends an ELM command and waits for its '>': the response is in s_rx and
 * the exchange's round trip in s_elm_rtt_ms. */
static bool elm_cmd(const char *cmd, uint32_t timeout_ms)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_enabled) {
        return false;
    }
    /* Anything that came unasked (a prompt on connect, a late reply) goes. */
    drain_rx();

    uint32_t t0 = now_ms();
    char line[24];   /* the longest command is a six-PID request: 14 + '\r' + NUL */
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    int rc = (s_props & BLE_GATT_CHR_PROP_WRITE)
        ? ble_gattc_write_flat(s_conn, s_val, line, n, on_write, NULL)
        : ble_gattc_write_no_rsp_flat(s_conn, s_val, line, n);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s: write failed: %d", cmd, rc);
        return false;
    }
    if (wait_for(0, true, timeout_ms, NULL) != -1) {
        s_elm_rtt_ms = now_ms() - t0;
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_enabled) {
            ESP_LOGW(TAG, "%s: no '>' in %u ms", cmd, (unsigned)timeout_ms);
        }
        return false;
    }
    s_elm_rtt_ms = now_ms() - t0;
    return true;
}

static bool elm_init(void)
{
    static const char *const CMDS[] = { "ATZ", "ATE0", "ATL0", "ATH0", "ATS0", "ATSP0" };
    for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]); i++) {
        if (!elm_cmd(CMDS[i], i == 0 ? ATZ_MS : CMD_MS)) {
            return false;
        }
        if (i == 0) {
            ESP_LOGI(TAG, "adapter: %s", s_rx.resp[0] ? s_rx.resp : "(no banner)");
        }
    }
    return true;
}

/* Which protocol ATSP0 settled on ("ATDPN"). Not known (-1) until the first
 * OBD request has made the ELM search for it. */
static void read_protocol(void)
{
    if (!elm_cmd("ATDPN", CMD_MS)) {
        return;
    }
    int protocol = muse_elm_parse_protocol(s_rx.resp);
    if (protocol > 0) {
        ESP_LOGI(TAG, "protocol %d", protocol);
        set_protocol(protocol);
        s_have_protocol = true;
        s_protocol = protocol;
    }
}

/*
 * The supported-PID scan: 0100, then 0120 if it lists PID 20, then 0140 if
 * that lists PID 40. A range that doesn't answer stays unknown, and so do the
 * ones after it: the poll loop keeps asking those PIDs and lets NO DATA skip
 * them. The first 0100 after ATSP0 may search for the protocol, so it gets
 * SEARCH_MS. False only once the adapter has disconnected, or OBD was turned
 * off.
 */
static bool scan_support(void)
{
    static const char *const CMDS[MUSE_ELM_SUPPORT_RANGES] = { "0100", "0120", "0140" };
    muse_elm_support_t sup = { 0 };
    for (int i = 0; i < MUSE_ELM_SUPPORT_RANGES; i++) {
        if (i > 0 && !(sup.masks[i - 1] & 1)) {
            break;   /* the bottom bit is PID 0x20 * i: nothing past this range */
        }
        if (!elm_cmd(CMDS[i], i == 0 ? SEARCH_MS : CMD_MS)) {
            if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_enabled) {
                return false;
            }
            break;
        }
        if (muse_elm_parse_support(s_rx.resp, 0x20 * i, &sup.masks[i]) != MUSE_ELM_SUPPORT) {
            break;
        }
        sup.valid[i] = true;
        ESP_LOGI(TAG, "PIDs %02X-%02X: %08X", 0x20 * i + 1, 0x20 * (i + 1), (unsigned)sup.masks[i]);
    }
    if (sup.valid[0]) {
        s_support = sup;
        set_support(&sup);
    } else {
        ESP_LOGI(TAG, "no support scan: polling every PID");
    }
    return true;
}

void muse_obd_fire_coolant(void)
{
    if (s_enabled) {
        muse_state_start_coolant();
    }
}

void muse_obd_fire_warmup(void)
{
    if (s_enabled) {
        muse_state_start_warmup();
    }
}

static void fire_coolant(int coolant_c)
{
    ESP_LOGW(TAG, "coolant hot: %d C at %d rpm", coolant_c, s_rpm);
    muse_obd_fire_coolant();
}

static void fire_warmup(int coolant_c)
{
    ESP_LOGI(TAG, "warmed up: %d C", coolant_c);
    muse_obd_fire_warmup();
}

/* The last response was the ELM's "OK" (an AT command took). */
static bool elm_ok(void)
{
    return strstr(s_rx.resp, "OK") != NULL;
}

/* The ATST byte the tune policy picked, sent to the adapter (the ELM's
 * response timeout, byte x 4 ms). Tuning only ever sends this one command
 * and only ever logs a step: it never wedges or reconnects the session,
 * which stays the init/retry machinery's business. */
static void apply_atst(int byte)
{
    static const char HEX[] = "0123456789ABCDEF";
    char cmd[8] = { 'A', 'T', 'S', 'T', HEX[(byte >> 4) & 0xF], HEX[byte & 0xF], '\0' };
    if (elm_cmd(cmd, CMD_MS) && elm_ok()) {
        ESP_LOGI(TAG, "ATST %02X: response timeout %d ms", byte, byte * 4);
    }
}

/* Once per connection, once the protocol is known: aggressive adaptive
 * timing (ATAT2) and a fresh ATST policy to trim from the exchanges. */
static void tune_link(void)
{
    if (s_tuned || !s_have_protocol) {
        return;
    }
    s_tuned = true;
    muse_tune_init(&s_tune);
    if (elm_cmd("ATAT2", CMD_MS) && elm_ok()) {
        ESP_LOGI(TAG, "adaptive timing: ATAT2");
    }
}

/* One Mode 01 exchange's outcome, fed to the tune policy: a valued one may
 * trim ATST (once, from the measured round trips); an answerless one may
 * step it back. Anything it decides is sent at once. */
static void tune_observe(bool valued, uint32_t rtt_ms)
{
    int byte = valued ? muse_tune_observe_value(&s_tune, rtt_ms) : muse_tune_observe_timeout(&s_tune);
    if (byte >= 0) {
        apply_atst(byte);
    }
}

/* Mode 01's state back: automatic receive (no CRA filter) and the
 * functional header (18 DB 33 F1, the ELM's own default on a 29-bit link).
 * Tried twice: a late reply to a slow request can take the first one's
 * prompt. False only when it can't be done, which drops the link and goes
 * through the usual reconnect and ELM init. */
static bool did_restore(void)
{
    for (int i = 0; i < 2; i++) {
        if (elm_cmd("ATAR", CMD_MS) && elm_ok() && elm_cmd("ATSHDB33F1", CMD_MS) && elm_ok()) {
            return true;
        }
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_enabled) {
            return false;
        }
    }
    return false;
}

/*
 * One read of DID 0x2612, state-safe: the header and the receive filter
 * point at the engine ECU (ATSH DA10F1, ATCRA 18DAF110) for the request
 * only, and did_restore() puts Mode 01's back afterwards, whatever happened
 * in between. Read only: service 0x22 in the default session, nothing is
 * written to the car. *link is false once the link is gone (or the ELM's
 * state couldn't be put back, so it must be dropped and initialised again).
 */
static muse_elm_result_t did_read(muse_vtec_did_t *out, bool *link)
{
    muse_elm_result_t res = MUSE_ELM_NONE;
    if (elm_cmd("ATSHDA10F1", CMD_MS) && elm_ok() && elm_cmd("ATCRA18DAF110", CMD_MS) && elm_ok()
        && elm_cmd(MUSE_VTEC_DID_REQ, CMD_MS)) {
        res = muse_elm_parse_vtec_did(s_rx.resp, out);
        if (res == MUSE_ELM_NONE) {
            ESP_LOGW(TAG, "DID 2612: unexpected reply: %s", s_rx.resp);
        }
    }
    *link = did_restore();
    return res;
}

/* A good DID 0x2612 reading: the VTEC flags into the snapshot. */
static void got_did(const muse_vtec_did_t *v)
{
    ESP_LOGD(TAG, "VTEC solenoid %s, pressure switch %s, byte 23 %02X", v->sol ? "on" : "off",
             v->psw ? "on" : "off", v->b23);
    set_vtec(v, false);
}

/*
 * Once per connection, after the support scan, on a CAN 29-bit link only:
 * does the engine ECU answer DID 0x2612? Refused, silent or garbled, it is
 * never asked again on this connection. Nothing on any other protocol, and
 * nothing until the protocol is known. False only when the link went down.
 */
static bool probe_did(void)
{
    if (s_did != DID_UNPROBED || s_protocol != 7) {
        return true;
    }
    muse_vtec_did_t v;
    bool link;
    muse_elm_result_t res = did_read(&v, &link);
    s_did = res == MUSE_ELM_VTEC_DID ? DID_YES : DID_NO;
    set_did_supported(s_did == DID_YES);
    if (s_did == DID_YES) {
        ESP_LOGI(TAG, "DID 2612 answers: reading the VTEC solenoid");
        got_did(&v);
    } else {
        ESP_LOGI(TAG, "DID 2612 not answered (%s): no VTEC reading on this connection",
                 res == MUSE_ELM_NEGATIVE ? "refused" : res == MUSE_ELM_NO_DATA ? "no data" : "no reply");
    }
    return link;
}

/* A coolant reading, from a single-PID reply or out of a batch: into the
 * snapshot, and to the warning and the warm-up. */
static void coolant_reading(int coolant_c)
{
    ESP_LOGD(TAG, "coolant %d C", coolant_c);
    set_coolant(coolant_c);
    if (muse_coolant_update(&s_coolant, coolant_c, s_rpm, now_ms())) {
        fire_coolant(coolant_c);
    }
    if (muse_warmup_update(&s_warmup, coolant_c)) {
        fire_warmup(coolant_c);
    }
}

/* A coolant reply: feeds the warning and the warm-up. Anything else (NO DATA,
 * an ECU without PID 05) is skipped without slowing the RPM polling. True
 * when a reading came in. */
static bool got_coolant(void)
{
    int coolant_c = 0;
    if (muse_elm_parse_coolant(s_rx.resp, &coolant_c) != MUSE_ELM_COOLANT) {
        ESP_LOGD(TAG, "no coolant reading");
        return false;
    }
    coolant_reading(coolant_c);
    return true;
}

/* A PID's value as its extra holds it: PID 34's raw ratio becomes the AFR
 * and the lambda; every other extra keeps the parser's value. */
static int extra_value(muse_obd_extra_t i, int value)
{
    switch (i) {
    case MUSE_OBD_AFR: return muse_elm_afr_x10(value);
    case MUSE_OBD_LAMBDA: return muse_elm_lambda_x1000(value);
    default: return value;
    }
}

/* One reading of a cards' PID (from a single-PID reply or out of a batch):
 * into every extra it feeds. */
static void extra_reading(int pid, int value)
{
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        if (MUSE_OBD_EXTRA[i].pid == pid) {
            int v = extra_value((muse_obd_extra_t)i, value);
            ESP_LOGD(TAG, "%s %d", MUSE_OBD_EXTRA[i].key, v);
            set_extra((muse_obd_extra_t)i, v);
        }
    }
}

/* A reply to one of the cards' PIDs: into the snapshot. NO DATA (an ECU
 * without it, or nothing yet) is skipped like coolant's, keeping the last
 * reading, which ages. True when a reading came in. */
static bool got_extra(int pid)
{
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        if (MUSE_OBD_EXTRA[i].pid != pid) {
            continue;
        }
        int value = 0;
        if (EXTRA_PARSE[i].parse(s_rx.resp, &value) == EXTRA_PARSE[i].ok) {
            extra_reading(pid, value);
            return true;
        }
        return false;
    }
    return false;
}

/* One reading out of a batch reply: where a single-PID reply's would go. */
static void batch_reading(int pid, int value)
{
    if (pid == 0x05) {
        coolant_reading(value);
        return;
    }
    extra_reading(pid, value);
}

/* One row of the trip log: the snapshot as it stands after an RPM reply.
 * The logger only queues it; it never waits on the store. */
static void log_sample(void)
{
    muse_obd_status_t st;
    muse_obd_status(&st);
    muse_obd_log_sample(&st, now_ms());
}

/*
 * The poll loop: response-driven and scheduled (muse_obd_elm.h). Each
 * request is picked and sent the moment the last reply is parsed; there is
 * no delay anywhere in the loop, so the link idles only while the adapter
 * is still answering. RPM is read for the cards and the trip log only:
 * nothing fires on it. While frames come, a reply that takes over POLL_MS
 * drops the link. Until the first RPM reply (the first request, or after
 * NO DATA or an error) the ELM may be searching for the car's protocol,
 * which takes seconds, so requests get SEARCH_MS; only RPM replies decide
 * that. The first RPM reply also redoes whatever init couldn't learn with
 * the engine still off: the support scan, the protocol, the tuning and the
 * DID probe. Returns once the link is lost; true if the adapter answered
 * at all.
 */
static bool poll_rpm(void)
{
    bool answered = false, locked = false, retried = false;
    muse_sched_init(&s_sched, &s_support, s_did == DID_YES, now_ms());
    taskENTER_CRITICAL(&s_status_lock);
    muse_link_stats_reset(&s_link);
    taskEXIT_CRITICAL(&s_status_lock);
    for (;;) {
        muse_sched_req_t req;
        muse_sched_pick(&s_sched, now_ms(), &req);
        if (req.did) {
            /* One DID read. A failed one leaves the last reading to age
             * out, never a stale "on". */
            muse_vtec_did_t v;
            bool link;
            uint32_t t0 = now_ms();
            muse_elm_result_t res = did_read(&v, &link);
            if (res == MUSE_ELM_VTEC_DID) {
                got_did(&v);
                answered = true;
            } else if (res == MUSE_ELM_NO_DATA) {
                note_timeout();
            }
            if (link) {
                note_exchange(now_ms() - t0, 0, false);
            }
            muse_sched_done_did(&s_sched, now_ms());
            if (!link) {
                return answered;
            }
            continue;
        }
        /* The Mode 01 request: "01" and the batch's PIDs, two hex digits
         * each, straight into the buffer (its size is the batch's). */
        static const char HEX[] = "0123456789ABCDEF";
        char cmd[2 + 2 * MUSE_ELM_BATCH_MAX + 1];
        int len = 0;
        cmd[len++] = '0';
        cmd[len++] = '1';
        bool rpm_in = false;
        for (int i = 0; i < req.n; i++) {
            cmd[len++] = HEX[(req.pids[i] >> 4) & 0xF];
            cmd[len++] = HEX[req.pids[i] & 0xF];
            rpm_in = rpm_in || req.pids[i] == RPM;
        }
        cmd[len] = '\0';
        if (!elm_cmd(cmd, locked ? POLL_MS : SEARCH_MS)) {
            note_timeout();
            tune_observe(false, 0);
            return answered;
        }
        answered = true;
        uint32_t rtt = s_elm_rtt_ms, now = now_ms();
        int readings = 0;
        bool rpm_got = false, no_data = strstr(s_rx.resp, "NO DATA") != NULL;
        if (req.n == 1) {
            int pid = req.pids[0];
            bool got;
            if (pid == RPM) {
                int rpm = 0;
                muse_elm_result_t res = muse_elm_parse_rpm(s_rx.resp, &rpm);
                locked = res == MUSE_ELM_RPM;
                s_rpm = locked ? rpm : -1;
                set_rpm(locked, rpm);
                got = locked;
                if (got) {
                    ESP_LOGD(TAG, "%d rpm", rpm);
                    rpm_got = true;
                }
            } else if (pid == 0x05) {
                got = got_coolant();
            } else {
                got = got_extra(pid);
            }
            readings = got ? 1 : 0;
            muse_sched_done_pid(&s_sched, pid, got, locked || got, now);
        } else {
            int values[MUSE_ELM_BATCH_MAX];
            bool got[MUSE_ELM_BATCH_MAX];
            muse_elm_result_t res = muse_elm_parse_batch(s_rx.resp, req.pids, req.n, values, got);
            if (res == MUSE_ELM_NO_DATA || res == MUSE_ELM_ERROR) {
                /* The batch as a whole was refused: latch batching off for
                 * the connection and carry on single-PID, never retrying
                 * the batch. Its PIDs weren't answered, but weren't
                 * individually refused either: no misses. */
                ESP_LOGI(TAG, "batch refused (%s): single-PID for this connection",
                         res == MUSE_ELM_NO_DATA ? "NO DATA" : "error");
                muse_sched_latch_batch_off(&s_sched);
                note_batch_off();
                for (int i = 0; i < req.n; i++) {
                    muse_sched_done_pid(&s_sched, req.pids[i], false, false, now);
                }
            } else {
                for (int i = 0; i < req.n; i++) {
                    if (!got[i]) {
                        continue;
                    }
                    readings++;
                    if (req.pids[i] == RPM) {
                        s_rpm = values[i];
                        set_rpm(true, values[i]);
                        ESP_LOGD(TAG, "%d rpm", values[i]);
                        rpm_got = true;
                    } else {
                        batch_reading(req.pids[i], values[i]);
                    }
                }
                if (rpm_in) {
                    locked = rpm_got;
                    if (!rpm_got) {
                        s_rpm = -1;
                        set_rpm(false, 0);
                    }
                }
                /* A PID missing from the reply is a miss for that PID only,
                 * against an ECU this exchange proved is answering. */
                for (int i = 0; i < req.n; i++) {
                    muse_sched_done_pid(&s_sched, req.pids[i], got[i], readings > 0 || locked, now);
                }
            }
        }
        if (rpm_in) {
            log_sample();
        }
        note_exchange(rtt, readings, req.n > 1);
        if (readings > 0) {
            tune_observe(true, rtt);
        } else if (no_data) {
            note_timeout();
            tune_observe(false, 0);
        }
        if (rpm_got && !retried) {
            retried = true;
            if (!s_support.valid[0] && !scan_support()) {
                return answered;
            }
            muse_sched_apply_support(&s_sched, &s_support);
            if (!s_have_protocol) {
                read_protocol();
                tune_link();
            }
            if (!probe_did()) {   /* if the protocol only turned up now */
                return answered;
            }
            if (s_did == DID_YES) {
                muse_sched_set_did(&s_sched, true, now_ms());
            }
        }
    }
}

static void drop(void)
{
    muse_obd_log_trip_end();   /* the trip ends with the link: flushed and closed */
    uint16_t conn = s_conn;
    s_val = 0;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait_for(BIT_EV(EV_DISCONNECT), false, 2000, NULL);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    muse_coolant_reset(&s_coolant);
    muse_warmup_init(&s_warmup);   /* a new connection must see the engine cold again */
    s_rpm = -1;
    s_support = (muse_elm_support_t){ 0 };   /* the next connection scans again */
    s_tuned = false;   /* the next connection tunes its link again */
    s_have_protocol = false;
    s_protocol = -1;
    s_did = DID_UNPROBED;   /* the next connection asks again; did_2612 keeps this one's answer */
    set_link_down();
}

/* Sleeps for ms, or less once turned off. */
static void nap(uint32_t ms)
{
    TickType_t start = xTaskGetTickCount(), limit = pdMS_TO_TICKS(ms);
    for (TickType_t gone = 0; s_enabled && gone < limit; gone = xTaskGetTickCount() - start) {
        ulTaskNotifyTake(pdTRUE, limit - gone);
    }
}

static void obd_task(void *arg)
{
    (void)arg;
    muse_coolant_init(&s_coolant);
    muse_warmup_init(&s_warmup);
    uint32_t backoff = BACKOFF_MIN_MS;
    for (;;) {
        /* Turned off in Settings: no scan and no connection, which leaves
         * the adapter free for a phone, until it is turned back on. */
        if (!s_enabled) {
            set_state("off");
            set_flag(&s_status.adapter_found, false);
            ESP_LOGI(TAG, "off");
            while (!s_enabled) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            ESP_LOGI(TAG, "on: looking for the adapter");
            backoff = BACKOFF_MIN_MS;
        }
        /* Link brings the host up (at boot on PSRAM boards) and may restart it. */
        set_state("waiting");
        while (s_enabled && (!muse_link_ble_started() || !ble_hs_synced())) {
            nap(1000);
        }
        if (!s_enabled) {
            continue;
        }
        ble_addr_t addr;
        bool polled = false;
        set_state("scanning");
        set_flag(&s_status.adapter_found, false);
        if (scan(&addr)) {
            set_state("connecting");
            if (connect(&addr)) {
                set_state("discovering");
                if (subscribe()) {
                    set_state("initializing");
                    /* ATDPN after the scan: the scan's 0100 is what makes
                     * ATSP0 search for the protocol. */
                    if (elm_init() && scan_support()) {
                        read_protocol();
                        tune_link();   /* ATAT2 + the ATST policy, now the protocol is known */
                        /* DID 0x2612 before polling, so the trip's header
                         * can say; its exchange failing badly drops the link. */
                        if (probe_did()) {
                            ESP_LOGI(TAG, "polling RPM, coolant and the cards' PIDs");
                            set_state("polling");
                            set_flag(&s_status.polling, true);
                            muse_obd_status_t st;
                            muse_obd_status(&st);
                            muse_obd_log_trip_start(&st, LOG_DID[s_did], now_ms());   /* a trip per polling link */
                            polled = poll_rpm();
                            set_flag(&s_status.polling, false);
                        }
                    }
                }
                drop();
            }
        }
        if (!s_enabled) {
            continue;
        }
        set_state("backoff");
        backoff = polled ? BACKOFF_MIN_MS : backoff;
        nap(backoff);
        backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }
}

void muse_obd_set_enabled(bool on)
{
    taskENTER_CRITICAL(&s_status_lock);
    s_enabled = on;
    s_status.enabled = on;
    taskEXIT_CRITICAL(&s_status_lock);
    if (s_task) {
        xTaskNotifyGive(s_task);   /* out of whatever it waits on, to see it */
    }
}

bool muse_obd_enabled(void)
{
    return s_enabled;
}

void muse_obd_start(void)
{
    if (s_task) {
        return;
    }
    muse_obd_log_start();   /* the trip log's stores, with OBD on or off */
    /* Before the task exists, so it can't run ahead of this. */
    taskENTER_CRITICAL(&s_status_lock);
    s_status.started = true;
    s_status.state = s_enabled ? "waiting" : "off";
    taskEXIT_CRITICAL(&s_status_lock);
    s_events = xQueueCreateWithCaps(16, sizeof(obd_ev_t), MALLOC_CAP_SPIRAM);
    s_rx_bytes = xStreamBufferCreateWithCaps(RX_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_events || !s_rx_bytes
        || xTaskCreateWithCaps(obd_task, "muse_obd", 6144, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start");
        taskENTER_CRITICAL(&s_status_lock);
        s_status.started = false;
        s_status.state = "off";
        taskEXIT_CRITICAL(&s_status_lock);
        return;
    }
    ESP_LOGI(TAG, "looking for an OBD adapter (%s*)", ADAPTER_NAME);
}
