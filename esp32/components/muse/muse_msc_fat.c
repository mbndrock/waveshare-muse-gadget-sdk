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

#include "muse_msc_fat.h"

#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t clusters_for(uint32_t size) { return (size + MUSE_MSC_CLUSTER - 1) / MUSE_MSC_CLUSTER; }

void muse_msc_trip_name(uint32_t id, char out[16])
{
    /* Manual formatting: snprintf("%04lu") into 13 bytes trips
     * -Werror=format-truncation even though the store caps ids at 7 digits. */
    char digits[11];
    int nd = 0;
    uint32_t v = id;
    do { digits[nd++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && nd < 10);
    int width = nd < 4 ? 4 : nd;
    memcpy(out, "TRIP", 4);
    for (int i = 0; i < width - nd; i++) out[4 + i] = '0';
    for (int i = 0; i < nd; i++) out[4 + (width - nd) + i] = digits[nd - 1 - i];
    memcpy(out + 4 + width, ".CSV", 5); /* includes the NUL */
}

/* Appends s to buf at *len (cap total, NUL room kept); false on overflow,
 * in which case nothing is appended. No snprintf: sizes are provable. */
static bool append_str(char *buf, size_t cap, size_t *len, const char *s)
{
    size_t n = strlen(s);
    if (*len + n >= cap) return false;
    memcpy(buf + *len, s, n);
    *len += n;
    buf[*len] = '\0';
    return true;
}
static bool append_u32(char *buf, size_t cap, size_t *len, uint32_t v)
{
    char tmp[11];
    int nd = 0;
    do { tmp[nd++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
    if (*len + (size_t)nd >= cap) return false;
    for (int i = 0; i < nd; i++) buf[*len + i] = tmp[nd - 1 - i];
    *len += (size_t)nd;
    buf[*len] = '\0';
    return true;
}
static bool append_crc(char *buf, size_t cap, size_t *len, uint32_t v)
{
    static const char HEX[] = "0123456789abcdef";
    if (*len + 8 >= cap) return false;
    for (int i = 0; i < 8; i++) buf[*len + i] = HEX[(v >> (28 - 4 * i)) & 0xF];
    *len += 8;
    buf[*len] = '\0';
    return true;
}

size_t muse_msc_index_build(char *buf, size_t cap, const muse_msc_file_t *trips, int n, const char *firmware)
{
    if (!buf || cap == 0) return 0;
    size_t len = 0;
    buf[0] = '\0';
    if (!append_str(buf, cap, &len, "Muse trip logs (firmware ")
        || !append_str(buf, cap, &len, firmware ? firmware : "unknown")
        || !append_str(buf, cap, &len, ")\n")) return 0;
    for (int i = 0; i < n; i++) {
        char name[16];
        muse_msc_trip_name(trips[i].trip_id, name);
        if (!append_str(buf, cap, &len, name)
            || !append_str(buf, cap, &len, " id=") || !append_u32(buf, cap, &len, trips[i].trip_id)
            || !append_str(buf, cap, &len, " bytes=") || !append_u32(buf, cap, &len, trips[i].size)
            || !append_str(buf, cap, &len, " samples=") || !append_u32(buf, cap, &len, trips[i].samples)
            || !append_str(buf, cap, &len, " crc32=") || !append_crc(buf, cap, &len, trips[i].crc32)
            || !append_str(buf, cap, &len, "\n")) return 0;
    }
    return len;
}

bool muse_msc_snapshot_init(muse_msc_snapshot_t *snap, const muse_msc_file_t *trips, int n, uint32_t index_len)
{
    if (!snap || n < 0 || n > MUSE_MSC_MAX_TRIPS) return false;
    memset(snap, 0, sizeof(*snap));
    snap->files[0].size = index_len;
    snap->count = n + 1;
    uint32_t clusters = clusters_for(index_len);
    for (int i = 0; i < n; i++) {
        snap->files[i + 1] = trips[i];
        clusters += clusters_for(trips[i].size);
    }
    snap->data_clusters = clusters;
    /* FAT16: each FAT holds (clusters + 2) 16-bit entries; iterate to a
     * fixed point because the FAT's own size feeds the total. */
    uint32_t fat = 1;
    for (;;) {
        uint32_t entries = clusters + 2;
        uint32_t need = (entries * 2 + MUSE_MSC_SECTOR - 1) / MUSE_MSC_SECTOR;
        if (need == fat) break;
        fat = need;
    }
    snap->fat_sectors = fat;
    snap->total_sectors = 1 + 2 * fat + MUSE_MSC_ROOT_SECTORS + clusters * MUSE_MSC_CLUSTER_SECTORS;
    return true;
}

/* First cluster of file i. */
static uint32_t first_cluster(const muse_msc_snapshot_t *snap, int i)
{
    uint32_t c = 2;
    for (int k = 0; k < i; k++) c += clusters_for(snap->files[k].size);
    return c;
}

static void boot_sector(const muse_msc_snapshot_t *snap, uint8_t *buf)
{
    memset(buf, 0, MUSE_MSC_SECTOR);
    buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;
    memcpy(buf + 3, "MSDOS5.0", 8);
    put16(buf + 11, (uint16_t)MUSE_MSC_SECTOR);
    buf[13] = (uint8_t)MUSE_MSC_CLUSTER_SECTORS;
    put16(buf + 14, 1); /* reserved */
    buf[16] = 2;        /* FATs */
    put16(buf + 17, (uint16_t)(MUSE_MSC_ROOT_SECTORS * MUSE_MSC_SECTOR / 32));
    if (snap->total_sectors < 65536) {
        put16(buf + 19, (uint16_t)snap->total_sectors);
    } else {
        put32(buf + 32, snap->total_sectors);
    }
    buf[21] = 0xF8; /* fixed disk */
    put16(buf + 22, (uint16_t)snap->fat_sectors);
    put16(buf + 24, 63); put16(buf + 26, 255); /* spt / heads: cosmetic */
    buf[36] = 0x80; buf[38] = 0x29;             /* drive, boot signature */
    put32(buf + 39, 0x4D534301);                /* serial "MS\x03\x01" */
    memcpy(buf + 43, MUSE_MSC_LABEL, 11);
    memcpy(buf + 54, "FAT16   ", 8);
    buf[510] = 0x55; buf[511] = 0xAA;
}

static void fat_sector(const muse_msc_snapshot_t *snap, uint32_t idx, uint8_t *buf)
{
    memset(buf, 0, MUSE_MSC_SECTOR);
    for (uint32_t e = 0; e < MUSE_MSC_SECTOR / 2; e++) {
        uint32_t entry = idx * (MUSE_MSC_SECTOR / 2) + e;
        uint16_t v = 0;
        if (entry == 0) v = 0xFF00 | 0xF8; /* media + reserved */
        else if (entry == 1) v = 0xFFFF;
        else {
            /* Find the file owning this cluster and its position in it. */
            uint32_t c = 2, acc = 0;
            v = 0;
            for (int i = 0; i < snap->count; i++) {
                uint32_t n = clusters_for(snap->files[i].size);
                if (entry >= c && entry < c + n) {
                    v = (entry - c + 1 < n) ? (uint16_t)(entry + 1) : 0xFFFF;
                    break;
                }
                c += n;
            }
            (void)acc;
        }
        put16(buf + e * 2, v);
    }
}

static void dir_sector(const muse_msc_snapshot_t *snap, uint32_t idx, uint8_t *buf)
{
    memset(buf, 0, MUSE_MSC_SECTOR);
    for (uint32_t s = 0; s < MUSE_MSC_SECTOR / 32; s++) {
        uint32_t entry = idx * (MUSE_MSC_SECTOR / 32) + s;
        uint8_t *d = buf + s * 32;
        if (entry == 0) {
            memcpy(d, MUSE_MSC_LABEL, 11);
            d[11] = 0x08; /* volume label */
        } else if (entry - 1 < (uint32_t)snap->count) {
            int i = (int)(entry - 1);
            if (i == 0) {
                memcpy(d, MUSE_MSC_INDEX_NAME, 11);
            } else {
                char name[16];
                muse_msc_trip_name(snap->files[i].trip_id, name);
                /* 8.3: "TRIP0005" + "CSV", space padded. An id over 9999
                 * makes a longer base name; the field holds its first 8
                 * chars (documented in the Results: ids stay 4-digit for
                 * the first 9999 trips). */
                memset(d, ' ', 11);
                size_t nl = strlen(name) - 4;
                if (nl > 8) nl = 8;
                memcpy(d, name, nl);
                memcpy(d + 8, "CSV", 3);
            }
            d[11] = 0x20; /* archive, read-only intent */
            put16(d + 26, (uint16_t)first_cluster(snap, i));
            put32(d + 28, snap->files[i].size);
        }
    }
}

bool muse_msc_read_sector(const muse_msc_snapshot_t *snap, uint32_t lba, uint8_t *buf, muse_msc_read_fn read,
                          void *ctx)
{
    if (!snap || !buf || lba >= snap->total_sectors) return false;
    if (lba == 0) { boot_sector(snap, buf); return true; }
    uint32_t fat_end = 1 + 2 * snap->fat_sectors;
    if (lba < fat_end) { fat_sector(snap, (lba - 1) % snap->fat_sectors, buf); return true; }
    if (lba < fat_end + MUSE_MSC_ROOT_SECTORS) { dir_sector(snap, lba - fat_end, buf); return true; }
    uint32_t rel = (lba - fat_end - MUSE_MSC_ROOT_SECTORS);
    uint32_t cluster = 2 + rel / MUSE_MSC_CLUSTER_SECTORS;
    uint32_t in_cluster = (rel % MUSE_MSC_CLUSTER_SECTORS) * MUSE_MSC_SECTOR;
    memset(buf, 0, MUSE_MSC_SECTOR);
    for (int i = 0; i < snap->count; i++) {
        uint32_t fc = first_cluster(snap, i), n = clusters_for(snap->files[i].size);
        if (cluster >= fc && cluster < fc + n) {
            uint32_t off = (cluster - fc) * MUSE_MSC_CLUSTER + in_cluster;
            if (off < snap->files[i].size && read) {
                size_t want = snap->files[i].size - off;
                if (want > MUSE_MSC_SECTOR) want = MUSE_MSC_SECTOR;
                (void)read(ctx, i, off, buf, want);
            }
            return true;
        }
    }
    return true; /* unused data area: zeros */
}
