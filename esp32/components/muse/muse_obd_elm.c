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

#include "muse_obd_elm.h"

#include <stdio.h>
#include <string.h>

void muse_elm_rx_reset(muse_elm_rx_t *rx)
{
    rx->len = 0;
    rx->done = false;
    rx->resp[0] = '\0';
}

bool muse_elm_feed(muse_elm_rx_t *rx, uint8_t byte)
{
    if (rx->done) {
        muse_elm_rx_reset(rx);
    }
    if (byte == '>') {
        rx->resp[rx->len] = '\0';
        rx->done = true;
        return true;
    }
    if (byte == '\r') {
        byte = '\n';
    }
    if (byte != '\n' && (byte < 0x20 || byte > 0x7e)) {
        return false;
    }
    if (rx->len < MUSE_ELM_RESP_MAX - 1) {
        rx->resp[rx->len++] = (char)byte;
    }
    return false;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Does the line, spaces left out, start with `word`? (case-insensitive) */
static bool line_starts(const char *line, size_t n, const char *word)
{
    size_t w = 0;
    for (size_t i = 0; i < n && word[w]; i++) {
        char c = line[i];
        if (c == ' ') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c != word[w]) {
            return false;
        }
        w++;
    }
    return word[w] == '\0';
}

/* A line reading 41 <pid> and then `nbytes` data bytes: the hex digits, spaces
 * left out, start "41" and the PID and go on for at least nbytes more bytes
 * (into data[]); anything after them is ignored. */
static bool line_pid(const char *line, size_t n, int pid, int nbytes, int *data)
{
    int nib[12];   /* 41, the PID and up to four bytes */
    int want = 4 + 2 * nbytes;
    int k = 0;
    for (size_t i = 0; i < n && k < want; i++) {
        if (line[i] == ' ') {
            continue;
        }
        int v = hex_val(line[i]);
        if (v < 0) {
            return false;
        }
        nib[k++] = v;
    }
    if (k < want || nib[0] != 4 || nib[1] != 1 || nib[2] != pid >> 4 || nib[3] != (pid & 0xF)) {
        return false;
    }
    for (int b = 0; b < nbytes; b++) {
        data[b] = nib[4 + 2 * b] << 4 | nib[5 + 2 * b];
    }
    return true;
}

/* The first line that answers `pid` with nbytes of data wins (`found`); else
 * NO DATA, then an error, if any line says so. */
static muse_elm_result_t parse_pid(const char *resp, int pid, int nbytes, int *data, muse_elm_result_t found)
{
    static const char *const ERRORS[] = {
        "?", "UNABLETOCONNECT", "CANERROR", "BUSERROR", "BUSBUSY", "BUSINIT:...ERROR",
        "FBERROR", "DATAERROR", "STOPPED", "ERR", "LVRESET", "BUFFERFULL",
    };
    muse_elm_result_t res = MUSE_ELM_NONE;
    const char *p = resp;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (line_pid(p, n, pid, nbytes, data)) {
            return found;
        }
        if (line_starts(p, n, "NODATA")) {
            res = MUSE_ELM_NO_DATA;
        } else if (res == MUSE_ELM_NONE) {
            for (size_t e = 0; e < sizeof(ERRORS) / sizeof(ERRORS[0]); e++) {
                if (line_starts(p, n, ERRORS[e])) {
                    res = MUSE_ELM_ERROR;
                    break;
                }
            }
        }
        p += n;
        if (*p == '\n') {
            p++;
        }
    }
    return res;
}

muse_elm_result_t muse_elm_parse_rpm(const char *resp, int *rpm)
{
    int ab[2];
    muse_elm_result_t res = parse_pid(resp, 0x0C, 2, ab, MUSE_ELM_RPM);
    if (res == MUSE_ELM_RPM) {
        *rpm = (ab[0] * 256 + ab[1]) / 4;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_coolant(const char *resp, int *coolant_c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x05, 1, &a, MUSE_ELM_COOLANT);
    if (res == MUSE_ELM_COOLANT) {
        *coolant_c = a - 40;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_load(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x04, 1, &a, MUSE_ELM_LOAD);
    if (res == MUSE_ELM_LOAD) {
        *pct = a * 100 / 255;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_map(const char *resp, int *kpa)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0B, 1, &a, MUSE_ELM_MAP);
    if (res == MUSE_ELM_MAP) {
        *kpa = a;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_speed(const char *resp, int *kmh)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0D, 1, &a, MUSE_ELM_SPEED);
    if (res == MUSE_ELM_SPEED) {
        *kmh = a;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_timing(const char *resp, int *deg)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0E, 1, &a, MUSE_ELM_TIMING);
    if (res == MUSE_ELM_TIMING) {
        *deg = a / 2 - 64;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_iat(const char *resp, int *c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0F, 1, &a, MUSE_ELM_IAT);
    if (res == MUSE_ELM_IAT) {
        *c = a - 40;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_throttle(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x11, 1, &a, MUSE_ELM_THROTTLE);
    if (res == MUSE_ELM_THROTTLE) {
        *pct = a * 100 / 255;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_stft(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x06, 1, &a, MUSE_ELM_STFT);
    if (res == MUSE_ELM_STFT) {
        *pct = (a - 128) * 100 / 128;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_ltft(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x07, 1, &a, MUSE_ELM_LTFT);
    if (res == MUSE_ELM_LTFT) {
        *pct = (a - 128) * 100 / 128;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_ambient(const char *resp, int *c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x46, 1, &a, MUSE_ELM_AMBIENT);
    if (res == MUSE_ELM_AMBIENT) {
        *c = a - 40;
    }
    return res;
}

muse_elm_result_t muse_elm_parse_support(const char *resp, int base, uint32_t *mask)
{
    int abcd[4];
    muse_elm_result_t res = parse_pid(resp, base, 4, abcd, MUSE_ELM_SUPPORT);
    if (res == MUSE_ELM_SUPPORT) {
        *mask = (uint32_t)abcd[0] << 24 | (uint32_t)abcd[1] << 16 | (uint32_t)abcd[2] << 8 | (uint32_t)abcd[3];
    }
    return res;
}

/* Is `pid` (base + 1 .. base + 0x20) set in its range's mask? */
static bool mask_has(uint32_t mask, int pid)
{
    return mask >> (31 - (pid - 1) % 32) & 1;
}

muse_elm_pid_support_t muse_elm_pid_support(const muse_elm_support_t *s, int pid)
{
    if (pid < 1 || pid > 0x20 * MUSE_ELM_SUPPORT_RANGES) {
        return MUSE_ELM_PID_UNKNOWN;
    }
    int range = (pid - 1) / 32;
    if (s->valid[range]) {
        return mask_has(s->masks[range], pid) ? MUSE_ELM_PID_SUPPORTED : MUSE_ELM_PID_UNSUPPORTED;
    }
    /* Not read: the nearest range below that was says whether the chain went
     * on past it. Clear, nothing above it exists; set, a later read failed. */
    for (int k = range - 1; k >= 0; k--) {
        if (s->valid[k]) {
            return mask_has(s->masks[k], 0x20 * (k + 1)) ? MUSE_ELM_PID_UNKNOWN : MUSE_ELM_PID_UNSUPPORTED;
        }
    }
    return MUSE_ELM_PID_UNKNOWN;
}

int muse_elm_parse_protocol(const char *resp)
{
    /* The first line with anything on it: "A6", "6", "a 6". */
    const char *p = resp;
    while (*p == '\n' || *p == ' ') {
        p++;
    }
    if (*p == 'A' || *p == 'a') {
        p++;
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || *p == '\n') {
            return 10;   /* "A" on its own is protocol A (SAE J1939), set by hand */
        }
    }
    int n = hex_val(*p);
    if (n <= 0) {
        return -1;
    }
    p++;
    while (*p == ' ') {
        p++;
    }
    return *p == '\0' || *p == '\n' ? n : -1;
}

void muse_elm_protocol_name(int protocol, char *buf, size_t cap)
{
    static const char *const NAMES[] = {
        [1] = "SAE J1850 PWM",
        [2] = "SAE J1850 VPW",
        [3] = "ISO 9141-2",
        [4] = "KWP2000 5-baud",
        [5] = "KWP2000 fast",
        [6] = "CAN 11/500",
        [7] = "CAN 29/500",
        [8] = "CAN 11/250",
        [9] = "CAN 29/250",
        [10] = "SAE J1939",
        [11] = "USER1 CAN",
        [12] = "USER2 CAN",
    };
    if (!cap) {
        return;
    }
    if (protocol > 0 && protocol < (int)(sizeof(NAMES) / sizeof(NAMES[0])) && NAMES[protocol]) {
        snprintf(buf, cap, "%s", NAMES[protocol]);
    } else {
        snprintf(buf, cap, "proto %d", protocol);
    }
}

const uint8_t MUSE_ELM_SECONDARY[MUSE_ELM_SECONDARIES] = {
    0x05, 0x0F, 0x0E, 0x0B, 0x04, 0x11, 0x0D, 0x06, 0x07, 0x46,
};

int muse_elm_next_secondary(const muse_elm_support_t *s, unsigned *cursor)
{
    for (unsigned tried = 0; tried < MUSE_ELM_SECONDARIES; tried++) {
        int pid = MUSE_ELM_SECONDARY[*cursor % MUSE_ELM_SECONDARIES];
        *cursor = (*cursor + 1) % MUSE_ELM_SECONDARIES;
        if (muse_elm_pid_support(s, pid) != MUSE_ELM_PID_UNSUPPORTED) {
            return pid;
        }
    }
    return -1;
}

void muse_vtec_init(muse_vtec_trigger_t *t)
{
    memset(t, 0, sizeof(*t));
}

void muse_vtec_disarm(muse_vtec_trigger_t *t)
{
    t->armed = false;
}

bool muse_vtec_update(muse_vtec_trigger_t *t, int rpm, uint32_t now_ms)
{
    if (rpm < 0) {
        return false;
    }
    if (rpm <= MUSE_VTEC_ARM_RPM) {
        t->armed = true;
        return false;
    }
    if (rpm < MUSE_VTEC_FIRE_RPM || !t->armed) {
        return false;
    }
    t->armed = false;
    if (t->fired && now_ms - t->last_fire_ms < MUSE_VTEC_COOLDOWN_MS) {
        return false;   /* too soon: swallowed */
    }
    t->fired = true;
    t->last_fire_ms = now_ms;
    return true;
}

void muse_coolant_init(muse_coolant_trigger_t *t)
{
    memset(t, 0, sizeof(*t));
    t->armed = true;
}

void muse_coolant_reset(muse_coolant_trigger_t *t)
{
    t->hot = false;
}

bool muse_coolant_update(muse_coolant_trigger_t *t, int coolant_c, int rpm, uint32_t now_ms)
{
    if (coolant_c < -40 || coolant_c > 215) {
        return false;   /* not a reading */
    }
    if (coolant_c <= MUSE_COOLANT_REARM_C) {
        t->armed = true;
    }
    if (coolant_c < MUSE_COOLANT_FIRE_C || rpm < MUSE_COOLANT_RUNNING_RPM) {
        t->hot = false;   /* cooler, or the engine is off: the sustain starts over */
        return false;
    }
    if (!t->hot) {
        t->hot = true;
        t->hot_since_ms = now_ms;
    }
    if (!t->armed || now_ms - t->hot_since_ms < MUSE_COOLANT_SUSTAIN_MS) {
        return false;
    }
    if (t->fired && now_ms - t->last_fire_ms < MUSE_COOLANT_COOLDOWN_MS) {
        return false;   /* cooling down: fires when it runs out, if still hot */
    }
    t->armed = false;
    t->fired = true;
    t->last_fire_ms = now_ms;
    return true;
}

void muse_warmup_init(muse_warmup_trigger_t *t)
{
    memset(t, 0, sizeof(*t));
}

bool muse_warmup_update(muse_warmup_trigger_t *t, int coolant_c)
{
    if (coolant_c < -40 || coolant_c > 215) {
        return false;   /* not a reading */
    }
    if (coolant_c <= MUSE_WARMUP_COLD_C) {
        t->armed = true;
        return false;
    }
    if (coolant_c < MUSE_WARMUP_FIRE_C || !t->armed) {
        return false;
    }
    t->armed = false;
    return true;
}
