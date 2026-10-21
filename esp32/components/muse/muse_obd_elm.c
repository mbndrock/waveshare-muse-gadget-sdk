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

/* A line reading 41 0C A B: the hex digits, spaces left out, start "410C"
 * and go on for at least two more bytes. */
static bool line_rpm(const char *line, size_t n, int *rpm)
{
    int nib[8];
    int k = 0;
    for (size_t i = 0; i < n && k < 8; i++) {
        if (line[i] == ' ') {
            continue;
        }
        int v = hex_val(line[i]);
        if (v < 0) {
            return false;
        }
        nib[k++] = v;
    }
    if (k < 8 || nib[0] != 4 || nib[1] != 1 || nib[2] != 0 || nib[3] != 0xC) {
        return false;
    }
    int a = nib[4] << 4 | nib[5];
    int b = nib[6] << 4 | nib[7];
    *rpm = (a * 256 + b) / 4;
    return true;
}

muse_elm_result_t muse_elm_parse_rpm(const char *resp, int *rpm)
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
        if (line_rpm(p, n, rpm)) {
            return MUSE_ELM_RPM;
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
