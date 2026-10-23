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

#include "muse_usb_drive.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tusb.h"

#include "muse_build_info.h"
#include "muse_msc_fat.h"
#include "muse_obd_log.h"

static const char *TAG = "muse.usbdrive";
static bool s_active;
static muse_msc_snapshot_t s_snap;
static char s_index[4096];
static muse_msc_file_t s_trips[MUSE_MSC_MAX_TRIPS];

bool muse_usb_drive_active(void) { return s_active; }

bool muse_usb_drive_requested(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("muse", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u8(h, "usb_drv", &v);
        nvs_close(h);
    }
    s_active = v != 0;
    return s_active;
}

void muse_usb_drive_set(bool on)
{
    nvs_handle_t h;
    if (nvs_open("muse", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_set_u8(h, "usb_drv", on ? 1 : 0);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    s_active = on;
}

/* File bytes for the FAT layer: INDEX.TXT from memory, trips from the log
 * store by id (closed trips are immutable, so reads never race the logger;
 * muse_obd_log_read already refuses the open trip, which is never listed). */
static size_t drive_read(void *ctx, int idx, uint32_t off, uint8_t *buf, size_t len)
{
    (void)ctx;
    if (idx == 0) {
        size_t total = s_snap.files[0].size;
        if (off >= total) return 0;
        size_t n = total - off;
        if (n > len) n = len;
        memcpy(buf, s_index + off, n);
        return n;
    }
    uint32_t total = 0;
    int got = muse_obd_log_read(s_snap.files[idx].trip_id, off, buf, len, &total);
    return got > 0 ? (size_t)got : 0;
}

static void usb_task(void *arg)
{
    (void)arg;
    tusb_init();
    for (;;) {
        tud_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void muse_usb_drive_start(void)
{
    muse_obd_log_entry_t entries[MUSE_MSC_MAX_TRIPS];
    int total = muse_obd_log_list(entries, MUSE_MSC_MAX_TRIPS);
    int n = total > MUSE_MSC_MAX_TRIPS ? MUSE_MSC_MAX_TRIPS : total;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        s_trips[i].trip_id = entries[i].trip.id;
        s_trips[i].size = entries[i].trip.bytes;
        s_trips[i].samples = entries[i].trip.samples;
        s_trips[i].crc32 = entries[i].trip.crc32;
    }
    size_t ilen = muse_msc_index_build(s_index, sizeof(s_index), s_trips, n, muse_build_commit());
    if (ilen == 0) { /* too many trips for the buffer: say so in the file */
        ilen = (size_t)snprintf(s_index, sizeof(s_index), "Muse trip logs (firmware %s)\n",
                                muse_build_commit());
    }
    if (!muse_msc_snapshot_init(&s_snap, s_trips, n, (uint32_t)ilen)) {
        ESP_LOGE(TAG, "snapshot failed for %d trips", n);
        return;
    }
    ESP_LOGI(TAG, "drive: %d trips, %lu sectors", n, (unsigned long)s_snap.total_sectors);
    if (xTaskCreate(usb_task, "usb_msc", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "USB task failed");
    }
}

/* ---- TinyUSB device descriptors (MSC only) ---- */

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(s_device), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, .idProduct = 0x4001, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&s_device; }

enum { ITF_MSC, ITF_TOTAL };
#define CONFIG_TOTAL (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t s_config[CONFIG_TOTAL] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_TOTAL, 0, CONFIG_TOTAL, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 4, 0x01, 0x81, 64),
};
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_config;
}

static const char *s_strings[] = { "\x09\x04", "Meta", "Muse Logs", "MUSELOGS" };
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    if (index == 0) {
        buf[0] = (uint16_t)(TUSB_DESC_STRING << 8) | (2 * 2 + 2);
        buf[1] = 0x0409;
        return buf;
    }
    if (index >= sizeof(s_strings) / sizeof(s_strings[0])) return NULL;
    const char *s = s_strings[index];
    size_t len = strlen(s);
    if (len > 31) len = 31;
    buf[0] = (uint16_t)(TUSB_DESC_STRING << 8) | (uint16_t)(2 * len + 2);
    for (size_t i = 0; i < len; i++) buf[1 + i] = (uint8_t)s[i];
    return buf;
}

/* ---- TinyUSB MSC callbacks: read-only media ---- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "Meta    ", 8);
    memcpy(product_id, "Muse Logs       ", 16);
    memcpy(product_rev, "1.00", 4);
}
bool tud_msc_test_unit_ready_cb(uint8_t lun) { (void)lun; return true; }
void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_snap.total_sectors;
    *block_size = (uint16_t)MUSE_MSC_SECTOR;
}
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start; (void)load_eject;
    return true;
}
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (offset != 0 || bufsize % MUSE_MSC_SECTOR != 0) return -1;
    uint8_t *out = (uint8_t *)buffer;
    for (uint32_t i = 0; i < bufsize / MUSE_MSC_SECTOR; i++) {
        if (!muse_msc_read_sector(&s_snap, lba + i, out + i * MUSE_MSC_SECTOR, drive_read, NULL)) return -1;
    }
    return (int32_t)bufsize;
}
/* Read-only: writes are refused (the host also sees is_writable = false). */
bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return false; }
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun; (void)lba; (void)offset; (void)buffer; (void)bufsize;
    return -1; /* write-protected */
}
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)lun; (void)scsi_cmd; (void)buffer; (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
