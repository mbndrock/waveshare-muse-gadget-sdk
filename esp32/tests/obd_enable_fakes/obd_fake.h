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
 * Just enough ESP-IDF, FreeRTOS and NimBLE for muse_obd.c on the host
 * (muse_obd_enable_harness.c implements them): the task runs on a thread,
 * time is a virtual clock the harness moves, and the adapter is scripted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* sdkconfig */
#define CONFIG_MUSE_OBD_VTEC 1
#define CONFIG_MUSE_HATCH 0

/* esp_err */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1

/* esp_log: nothing printed, but the formats are still checked. */
#define OBD_FAKE_LOG(tag, fmt, ...) do { if (0) printf("%s " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE OBD_FAKE_LOG
#define ESP_LOGW OBD_FAKE_LOG
#define ESP_LOGI OBD_FAKE_LOG
#define ESP_LOGD OBD_FAKE_LOG

/* esp_heap_caps */
#define MALLOC_CAP_SPIRAM 1

/* FreeRTOS */
typedef uint32_t TickType_t;
typedef long BaseType_t;
typedef unsigned long UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(t) ((uint32_t)(t))
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void obd_fake_enter_critical(portMUX_TYPE *mux);
void obd_fake_exit_critical(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) obd_fake_enter_critical(mux)
#define taskEXIT_CRITICAL(mux) obd_fake_exit_critical(mux)

typedef struct obd_fake_task *TaskHandle_t;
typedef struct obd_fake_queue *QueueHandle_t;
typedef struct obd_fake_stream *StreamBufferHandle_t;
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
BaseType_t xTaskNotifyGive(TaskHandle_t task);
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                               TaskHandle_t *out, uint32_t caps);
QueueHandle_t xQueueCreateWithCaps(UBaseType_t len, UBaseType_t item, uint32_t caps);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t q);
StreamBufferHandle_t xStreamBufferCreateWithCaps(size_t size, size_t trigger, uint32_t caps);
size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t len, TickType_t ticks);
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *data, size_t len, TickType_t ticks);

/* NimBLE */
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EDONE 14
#define BLE_HS_ENOENT 5
#define BLE_HS_EAPP 9
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 0x04
#define BLE_GATT_CHR_PROP_WRITE 0x08
#define BLE_GATT_CHR_PROP_NOTIFY 0x10

typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; } ble_uuid_any_t;
#define BLE_UUID16_DECLARE(v) (&(const ble_uuid16_t){ { 16 }, (v) }.u)
uint16_t ble_uuid_u16(const ble_uuid_t *uuid);
int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b);

typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
struct os_mbuf { int len; const uint8_t *data; };
#define OS_MBUF_PKTLEN(om) ((om)->len)
int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst);

struct ble_gap_disc_desc { ble_addr_t addr; const uint8_t *data; uint8_t length_data; };
struct ble_gap_conn_desc { uint16_t conn_handle; };
enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_DISC, BLE_GAP_EVENT_DISC_COMPLETE,
       BLE_GAP_EVENT_NOTIFY_RX, BLE_GAP_EVENT_CONN_UPDATE };
struct ble_gap_event {
    int type;
    union {
        struct ble_gap_disc_desc disc;
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct ble_gap_conn_desc conn; } disconnect;
        struct { uint16_t conn_handle; uint16_t attr_handle; struct os_mbuf *om; } notify_rx;
        struct { int status; uint16_t conn_handle; } conn_update;
    };
};
struct ble_gap_conn_params { uint16_t itvl_min, itvl_max, latency, supervision_timeout, min_ce, max_ce; };
int ble_gap_update_params(uint16_t conn, const struct ble_gap_conn_params *params);
struct ble_gap_disc_params { uint16_t itvl, window; uint8_t filter_policy, limited, passive, filter_duplicates; };
typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
int ble_gap_disc(uint8_t own, int32_t ms, const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *arg);
int ble_gap_disc_cancel(void);
int ble_gap_connect(uint8_t own, const ble_addr_t *addr, int32_t ms, const void *params, ble_gap_event_fn *cb,
                    void *arg);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t conn, uint8_t reason);

struct ble_hs_adv_fields { const uint8_t *name; uint8_t name_len; const ble_uuid16_t *uuids16; uint8_t num_uuids16; };
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *data, uint8_t len);
int ble_hs_id_infer_auto(int privacy, uint8_t *own);
int ble_hs_synced(void);

struct ble_gatt_error { uint16_t status; uint16_t att_handle; };
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_attr { uint16_t handle; };
typedef int ble_gatt_disc_svc_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
typedef int ble_gatt_chr_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_chr *, void *);
typedef int ble_gatt_dsc_fn(uint16_t, const struct ble_gatt_error *, uint16_t, const struct ble_gatt_dsc *, void *);
typedef int ble_gatt_attr_fn(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
int ble_gattc_disc_svc_by_uuid(uint16_t conn, const ble_uuid_t *uuid, ble_gatt_disc_svc_fn *cb, void *arg);
int ble_gattc_disc_chrs_by_uuid(uint16_t conn, uint16_t start, uint16_t end, const ble_uuid_t *uuid,
                                ble_gatt_chr_fn *cb, void *arg);
int ble_gattc_disc_all_dscs(uint16_t conn, uint16_t start, uint16_t end, ble_gatt_dsc_fn *cb, void *arg);
int ble_gattc_write_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len, ble_gatt_attr_fn *cb,
                         void *arg);
int ble_gattc_write_no_rsp_flat(uint16_t conn, uint16_t handle, const void *data, uint16_t len);
