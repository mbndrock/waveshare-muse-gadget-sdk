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
#define NO_RPM_PAUSE_MS 500           /* after NO DATA or an error, before asking again */
#define BACKOFF_MIN_MS 2000
#define BACKOFF_MAX_MS 60000
#define RX_BYTES 512

/* Requests alternate RPM (010C) with one secondary PID, in turn from
 * MUSE_ELM_SECONDARY (muse_obd_elm.c): coolant (0105) first, then the cards'. */
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
static uint8_t s_props;
static uint8_t s_own_addr;
static muse_elm_rx_t s_rx;
static muse_vtec_trigger_t s_trigger;
static muse_coolant_trigger_t s_coolant;
static muse_warmup_trigger_t s_warmup;
static int s_rpm = -1;                /* the latest RPM; -1 when unknown (engine off, link down) */
static muse_elm_support_t s_support;  /* this connection's support scan; all unknown until it answers */
static unsigned s_cursor;             /* the next secondary PID in MUSE_ELM_SECONDARY */
static bool s_have_protocol;          /* this connection has read ATDPN */

/* muse_obd_status()'s snapshot: written by this task, read from any task. */
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static muse_obd_status_t s_status = { .state = "off", .protocol = -1 };
static uint32_t s_rpm_at, s_coolant_at;   /* now_ms() of each reading */
static uint32_t s_extra_at[MUSE_OBD_EXTRAS];

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
};

/* Each extra's parser, and the result that means it read a value. */
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
    taskEXIT_CRITICAL(&s_status_lock);
}

void muse_obd_status(muse_obd_status_t *out)
{
    if (!out) {
        return;
    }
    uint32_t now = now_ms();
    uint32_t rpm_at, coolant_at, extra_at[MUSE_OBD_EXTRAS];
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    rpm_at = s_rpm_at;
    coolant_at = s_coolant_at;
    memcpy(extra_at, s_extra_at, sizeof(extra_at));
    taskEXIT_CRITICAL(&s_status_lock);
    out->rpm_age_ms = out->rpm_valid ? now - rpm_at : UINT32_MAX;
    out->coolant_age_ms = out->coolant_valid ? now - coolant_at : UINT32_MAX;
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        out->extra[i].age_ms = out->extra[i].valid ? now - extra_at[i] : UINT32_MAX;
    }
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
 * response, or 0 on a timeout or when the adapter disconnected (which always
 * ends the wait). Events of an earlier connection are dropped.
 */
static int wait_for(uint32_t mask, bool want_resp, uint32_t timeout_ms, obd_ev_t *out)
{
    TickType_t start = xTaskGetTickCount(), limit = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
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
        return false;
    }
    if (ev.status != 0) {
        ESP_LOGI(TAG, "connect failed: %d", ev.status);
        return false;
    }
    s_conn = ev.conn;
    set_flag(&s_status.connected, true);
    ESP_LOGI(TAG, "adapter connected");
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

/* Sends an ELM command and waits for its '>': the response is in s_rx. */
static bool elm_cmd(const char *cmd, uint32_t timeout_ms)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    /* Anything that came unasked (a prompt on connect, a late reply) goes. */
    drain_rx();

    char line[16];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    int rc = (s_props & BLE_GATT_CHR_PROP_WRITE)
        ? ble_gattc_write_flat(s_conn, s_val, line, n, on_write, NULL)
        : ble_gattc_write_no_rsp_flat(s_conn, s_val, line, n);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s: write failed: %d", cmd, rc);
        return false;
    }
    if (wait_for(0, true, timeout_ms, NULL) != -1) {
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ESP_LOGW(TAG, "%s: no '>' in %u ms", cmd, (unsigned)timeout_ms);
        }
        return false;
    }
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
    }
}

/*
 * The supported-PID scan: 0100, then 0120 if it lists PID 20, then 0140 if
 * that lists PID 40. A range that doesn't answer stays unknown, and so do the
 * ones after it: the poll loop keeps asking those PIDs and lets NO DATA skip
 * them. The first 0100 after ATSP0 may search for the protocol, so it gets
 * SEARCH_MS. False only once the adapter has disconnected.
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
            if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
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

void muse_obd_fire_vtec(void)
{
    muse_state_start_vtec();
}

void muse_obd_fire_coolant(void)
{
    muse_state_start_coolant();
}

void muse_obd_fire_warmup(void)
{
    muse_state_start_warmup();
}

static void fire(int rpm)
{
    ESP_LOGI(TAG, "VTEC! %d rpm", rpm);
    muse_obd_fire_vtec();
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

/* A coolant reply: feeds the warning and the warm-up. Anything else (NO DATA,
 * an ECU without PID 05) is skipped without slowing the RPM polling. */
static void got_coolant(void)
{
    int coolant_c = 0;
    if (muse_elm_parse_coolant(s_rx.resp, &coolant_c) != MUSE_ELM_COOLANT) {
        ESP_LOGD(TAG, "no coolant reading");
        return;
    }
    ESP_LOGD(TAG, "coolant %d C", coolant_c);
    set_coolant(coolant_c);
    if (muse_coolant_update(&s_coolant, coolant_c, s_rpm, now_ms())) {
        fire_coolant(coolant_c);
    }
    if (muse_warmup_update(&s_warmup, coolant_c)) {
        fire_warmup(coolant_c);
    }
}

/* A reply to one of the cards' PIDs: into the snapshot. NO DATA (an ECU
 * without it, or nothing yet) is skipped like coolant's, keeping the last
 * reading, which ages. */
static void got_extra(int pid)
{
    for (int i = 0; i < MUSE_OBD_EXTRAS; i++) {
        if (MUSE_OBD_EXTRA[i].pid != pid) {
            continue;
        }
        int value = 0;
        if (EXTRA_PARSE[i].parse(s_rx.resp, &value) == EXTRA_PARSE[i].ok) {
            ESP_LOGD(TAG, "%s %d", MUSE_OBD_EXTRA[i].key, value);
            set_extra((muse_obd_extra_t)i, value);
        }
        return;
    }
}

/*
 * Asks for RPM and a secondary PID by turns, each request after the last '>':
 * RPM, coolant, RPM, the next card PID, ... (muse_elm_next_secondary). While
 * frames come, a reply that takes over POLL_MS drops the link. Until then (the
 * first request, or after NO DATA or an error) the ELM may be searching for
 * the car's protocol, which takes seconds, so it gets SEARCH_MS. Only RPM
 * replies decide that, and the pause after a non-answer. The first RPM reply
 * also redoes whatever init couldn't learn with the engine still off: the
 * support scan and the protocol. Returns once the link is lost; true if the
 * adapter answered at all.
 */
static bool poll_rpm(void)
{
    bool answered = false, locked = false, retried = false;
    for (unsigned n = 0;; n++) {
        int pid = n % 2 ? muse_elm_next_secondary(&s_support, &s_cursor) : RPM;
        if (pid < 0) {
            continue;   /* every secondary is unsupported: RPM only */
        }
        char cmd[8];
        snprintf(cmd, sizeof(cmd), "01%02X", pid);
        if (!elm_cmd(cmd, locked ? POLL_MS : SEARCH_MS)) {
            return answered;
        }
        answered = true;
        if (pid == 0x05) {
            got_coolant();
            continue;
        }
        if (pid != RPM) {
            got_extra(pid);
            continue;
        }
        int rpm = 0;
        muse_elm_result_t res = muse_elm_parse_rpm(s_rx.resp, &rpm);
        locked = res == MUSE_ELM_RPM;
        s_rpm = locked ? rpm : -1;
        set_rpm(locked, rpm);
        if (locked) {
            ESP_LOGD(TAG, "%d rpm", rpm);
            if (muse_vtec_update(&s_trigger, rpm, now_ms())) {
                fire(rpm);
            }
            if (!retried) {
                retried = true;
                if (!s_support.valid[0] && !scan_support()) {
                    return answered;
                }
                if (!s_have_protocol) {
                    read_protocol();
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(NO_RPM_PAUSE_MS));
        }
    }
}

static void drop(void)
{
    uint16_t conn = s_conn;
    s_val = 0;
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        wait_for(BIT_EV(EV_DISCONNECT), false, 2000, NULL);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    muse_vtec_disarm(&s_trigger);
    muse_coolant_reset(&s_coolant);
    muse_warmup_init(&s_warmup);   /* a new connection must see the engine cold again */
    s_rpm = -1;
    s_support = (muse_elm_support_t){ 0 };   /* the next connection scans again */
    s_cursor = 0;
    s_have_protocol = false;
    set_link_down();
}

static void obd_task(void *arg)
{
    (void)arg;
    muse_vtec_init(&s_trigger);
    muse_coolant_init(&s_coolant);
    muse_warmup_init(&s_warmup);
    uint32_t backoff = BACKOFF_MIN_MS;
    for (;;) {
        /* Link brings the host up (at boot on PSRAM boards) and may restart it. */
        set_state("waiting");
        while (!muse_link_ble_started() || !ble_hs_synced()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
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
                        ESP_LOGI(TAG, "polling RPM, coolant and the cards' PIDs");
                        set_state("polling");
                        set_flag(&s_status.polling, true);
                        polled = poll_rpm();
                        set_flag(&s_status.polling, false);
                    }
                }
                drop();
            }
        }
        set_state("backoff");
        backoff = polled ? BACKOFF_MIN_MS : backoff;
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }
}

void muse_obd_start(void)
{
    if (s_task) {
        return;
    }
    /* Before the task exists, so it can't run ahead of this. */
    taskENTER_CRITICAL(&s_status_lock);
    s_status.started = true;
    s_status.state = "waiting";
    taskEXIT_CRITICAL(&s_status_lock);
    s_events = xQueueCreateWithCaps(16, sizeof(obd_ev_t), MALLOC_CAP_SPIRAM);
    s_rx_bytes = xStreamBufferCreateWithCaps(RX_BYTES, 1, MALLOC_CAP_SPIRAM);
    if (!s_events || !s_rx_bytes
        || xTaskCreateWithCaps(obd_task, "muse_obd", 4096, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start");
        taskENTER_CRITICAL(&s_status_lock);
        s_status.started = false;
        s_status.state = "off";
        taskEXIT_CRITICAL(&s_status_lock);
        return;
    }
    ESP_LOGI(TAG, "looking for an OBD adapter (%s*)", ADAPTER_NAME);
}
