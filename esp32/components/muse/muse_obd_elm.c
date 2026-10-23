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

/* How many data bytes a Mode 01 PID's reply carries,
 * and its whole-unit value from them: the one place the conversions the
 * header lists live, shared by the single-PID parsers and the batch one. */
#define MUSE_ELM_PID_BYTES_MAX 4   /* the most data bytes pid_nbytes() says */

static int pid_nbytes(int pid)
{
    switch (pid) {
    case 0x0C: return 2;
    case 0x42: return 2;
    case 0x34: return 4;   /* lambda's two, then the sensor's two, unused */
    default: return 1;
    }
}

static int pid_value(int pid, const int *data)
{
    switch (pid) {
    case 0x0C: return (data[0] * 256 + data[1]) / 4;
    case 0x05: return data[0] - 40;
    case 0x04: return data[0] * 100 / 255;
    case 0x0B: return data[0];
    case 0x0D: return data[0];
    case 0x0E: return data[0] / 2 - 64;
    case 0x0F: return data[0] - 40;
    case 0x11: return data[0] * 100 / 255;
    case 0x06: return (data[0] - 128) * 100 / 128;
    case 0x07: return (data[0] - 128) * 100 / 128;
    case 0x46: return data[0] - 40;
    case 0x34: return data[0] * 256 + data[1];   /* raw: lambda x 32768 */
    case 0x49: return data[0] * 100 / 255;
    case 0x4A: return data[0] * 100 / 255;
    case 0x42: return data[0] * 256 + data[1];   /* mV */
    case 0x33: return data[0];
    case 0x2F: return data[0] * 100 / 255;
    default: return 0;
    }
}

muse_elm_result_t muse_elm_parse_rpm(const char *resp, int *rpm)
{
    int ab[2];
    muse_elm_result_t res = parse_pid(resp, 0x0C, 2, ab, MUSE_ELM_RPM);
    if (res == MUSE_ELM_RPM) {
        *rpm = pid_value(0x0C, ab);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_coolant(const char *resp, int *coolant_c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x05, 1, &a, MUSE_ELM_COOLANT);
    if (res == MUSE_ELM_COOLANT) {
        *coolant_c = pid_value(0x05, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_load(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x04, 1, &a, MUSE_ELM_LOAD);
    if (res == MUSE_ELM_LOAD) {
        *pct = pid_value(0x04, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_map(const char *resp, int *kpa)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0B, 1, &a, MUSE_ELM_MAP);
    if (res == MUSE_ELM_MAP) {
        *kpa = pid_value(0x0B, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_speed(const char *resp, int *kmh)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0D, 1, &a, MUSE_ELM_SPEED);
    if (res == MUSE_ELM_SPEED) {
        *kmh = pid_value(0x0D, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_timing(const char *resp, int *deg)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0E, 1, &a, MUSE_ELM_TIMING);
    if (res == MUSE_ELM_TIMING) {
        *deg = pid_value(0x0E, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_iat(const char *resp, int *c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x0F, 1, &a, MUSE_ELM_IAT);
    if (res == MUSE_ELM_IAT) {
        *c = pid_value(0x0F, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_throttle(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x11, 1, &a, MUSE_ELM_THROTTLE);
    if (res == MUSE_ELM_THROTTLE) {
        *pct = pid_value(0x11, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_stft(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x06, 1, &a, MUSE_ELM_STFT);
    if (res == MUSE_ELM_STFT) {
        *pct = pid_value(0x06, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_ltft(const char *resp, int *pct)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x07, 1, &a, MUSE_ELM_LTFT);
    if (res == MUSE_ELM_LTFT) {
        *pct = pid_value(0x07, &a);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_ambient(const char *resp, int *c)
{
    int a;
    muse_elm_result_t res = parse_pid(resp, 0x46, 1, &a, MUSE_ELM_AMBIENT);
    if (res == MUSE_ELM_AMBIENT) {
        *c = pid_value(0x46, &a);
    }
    return res;
}

/* A one-PID parser for `pid`, whose reading is `ok`. */
static muse_elm_result_t parse_one(const char *resp, int pid, muse_elm_result_t ok, int *value)
{
    int data[MUSE_ELM_PID_BYTES_MAX];
    muse_elm_result_t res = parse_pid(resp, pid, pid_nbytes(pid), data, ok);
    if (res == ok) {
        *value = pid_value(pid, data);
    }
    return res;
}

muse_elm_result_t muse_elm_parse_afr(const char *resp, int *raw)
{
    return parse_one(resp, 0x34, MUSE_ELM_AFR, raw);
}

muse_elm_result_t muse_elm_parse_pedal_d(const char *resp, int *pct)
{
    return parse_one(resp, 0x49, MUSE_ELM_PEDAL_D, pct);
}

muse_elm_result_t muse_elm_parse_pedal_e(const char *resp, int *pct)
{
    return parse_one(resp, 0x4A, MUSE_ELM_PEDAL_E, pct);
}

muse_elm_result_t muse_elm_parse_module_v(const char *resp, int *mv)
{
    return parse_one(resp, 0x42, MUSE_ELM_MODULE_V, mv);
}

muse_elm_result_t muse_elm_parse_baro(const char *resp, int *kpa)
{
    return parse_one(resp, 0x33, MUSE_ELM_BARO, kpa);
}

muse_elm_result_t muse_elm_parse_fuel(const char *resp, int *pct)
{
    return parse_one(resp, 0x2F, MUSE_ELM_FUEL, pct);
}

int muse_elm_lambda_x1000(int raw)
{
    return (raw * 1000 + 16384) / 32768;
}

int muse_elm_afr_x10(int raw)
{
    return (raw * 147 + 16384) / 32768;
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

/* The scheduler's items: RPM and the DID, then the secondaries in tier
 * order. rank breaks overdue ties (the lower rank asks first); within a
 * rank the table's order does. -1 is the DID: not a Mode 01 PID. */
static const struct {
    int pid;
    uint32_t target_ms;
    int rank;
} SCHED[MUSE_SCHED_ITEMS] = {
    { 0x0C, 0, 0 },        /* RPM: always due, the standing filler */
    { -1, 1000, 1 },       /* DID 0x2612 */
    { 0x0B, 1000, 2 },     /* fast: MAP */
    { 0x11, 1000, 2 },     /* fast: throttle */
    { 0x04, 1000, 2 },     /* fast: load */
    { 0x0D, 1000, 2 },     /* fast: speed */
    { 0x34, 1000, 2 },     /* fast: AFR (wideband lambda) */
    { 0x49, 1000, 2 },     /* fast: pedal D */
    { 0x4A, 1000, 2 },     /* fast: pedal E */
    { 0x06, 2000, 3 },     /* mid: STFT */
    { 0x07, 2000, 3 },     /* mid: LTFT */
    { 0x0E, 2000, 3 },     /* mid: timing */
    { 0x2F, 2000, 3 },     /* mid: fuel level (off the cards' 3 s dimming edge) */
    { 0x05, 3000, 4 },     /* slow: coolant */
    { 0x0F, 3000, 4 },     /* slow: IAT */
    { 0x46, 3000, 4 },     /* slow: ambient */
    { 0x42, 3000, 4 },     /* slow: module voltage */
    { 0x33, 3000, 4 },     /* slow: barometric pressure */
};

#define SCHED_DID 1   /* SCHED's index of the DID item */

static bool sched_eligible(const muse_sched_t *s, int i, uint32_t now_ms)
{
    if (!s->present[i] || s->evicted[i]) {
        return false;
    }
    if (SCHED[i].pid < 0) {
        /* The DID's floor: at most one read per MUSE_SCHED_DID_MIN_MS. */
        return s->did_on && (!s->did_have || now_ms - s->did_last_ms >= MUSE_SCHED_DID_MIN_MS);
    }
    return true;
}

/* How far past its freshness target an item is; never attempted counts as
 * as overdue as it gets, so a new connection sweeps everything at once. */
static int32_t sched_overdue(const muse_sched_t *s, int i, uint32_t now_ms)
{
    if (!s->have[i]) {
        return INT32_MAX / 2;
    }
    return (int32_t)(now_ms - s->last_ms[i]) - (int32_t)SCHED[i].target_ms;
}

void muse_sched_init(muse_sched_t *s, const muse_elm_support_t *sup, bool did, uint32_t now_ms)
{
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < MUSE_SCHED_ITEMS; i++) {
        s->present[i] = SCHED[i].pid < 0
            || muse_elm_pid_support(sup, SCHED[i].pid) != MUSE_ELM_PID_UNSUPPORTED;
    }
    s->did_on = did;
    if (did) {
        /* The probe just read it: its floor, and its freshness, run from now. */
        s->did_have = true;
        s->did_last_ms = now_ms;
        s->have[SCHED_DID] = true;
        s->last_ms[SCHED_DID] = now_ms;
    }
}

void muse_sched_apply_support(muse_sched_t *s, const muse_elm_support_t *sup)
{
    for (int i = 0; i < MUSE_SCHED_ITEMS; i++) {
        if (SCHED[i].pid >= 0) {
            s->present[i] = muse_elm_pid_support(sup, SCHED[i].pid) != MUSE_ELM_PID_UNSUPPORTED;
        }
    }
}

void muse_sched_set_did(muse_sched_t *s, bool did, uint32_t now_ms)
{
    s->did_on = did;
    if (did && !s->did_have) {
        s->did_have = true;
        s->did_last_ms = now_ms;
        s->have[SCHED_DID] = true;
        s->last_ms[SCHED_DID] = now_ms;
    }
}

void muse_sched_pick(const muse_sched_t *s, uint32_t now_ms, muse_sched_req_t *req)
{
    req->did = false;
    req->n = 0;
    /* The most overdue eligible item; ties by rank, then by table order. */
    int best = -1;
    int32_t best_over = 0;
    for (int i = 0; i < MUSE_SCHED_ITEMS; i++) {
        if (!sched_eligible(s, i, now_ms)) {
            continue;
        }
        int32_t over = sched_overdue(s, i, now_ms);
        if (best < 0 || over > best_over || (over == best_over && SCHED[i].rank < SCHED[best].rank)) {
            best = i;
            best_over = over;
        }
    }
    if (best < 0) {
        req->pids[0] = 0x0C;   /* nothing eligible: RPM on its own */
        req->n = 1;
        return;
    }
    if (SCHED[best].pid < 0) {
        req->did = true;
        return;
    }
    bool in[MUSE_SCHED_ITEMS] = { 0 };
    req->pids[req->n++] = (uint8_t)SCHED[best].pid;
    in[best] = true;
    /* Fill the batch with the most overdue of what's due (RPM's target is
     * 0, so it rides along whenever there is room). */
    while (!s->batch_off && req->n < MUSE_ELM_BATCH_MAX) {
        int bi = -1;
        int32_t bo = 0;
        for (int i = 0; i < MUSE_SCHED_ITEMS; i++) {
            if (in[i] || SCHED[i].pid < 0 || !sched_eligible(s, i, now_ms)) {
                continue;
            }
            int32_t over = sched_overdue(s, i, now_ms);
            if (over < 0) {
                continue;   /* not due: a batch carries due items only */
            }
            if (bi < 0 || over > bo || (over == bo && SCHED[i].rank < SCHED[bi].rank)) {
                bi = i;
                bo = over;
            }
        }
        if (bi < 0) {
            break;
        }
        req->pids[req->n++] = (uint8_t)SCHED[bi].pid;
        in[bi] = true;
    }
    /* RPM leads the request when it is in it. */
    for (int i = 1; i < req->n; i++) {
        if (req->pids[i] == 0x0C) {
            memmove(&req->pids[1], &req->pids[0], (size_t)i);
            req->pids[0] = 0x0C;
            break;
        }
    }
}

void muse_sched_done_pid(muse_sched_t *s, int pid, bool got, bool ecu_live, uint32_t now_ms)
{
    for (int i = 0; i < MUSE_SCHED_ITEMS; i++) {
        if (SCHED[i].pid != pid) {
            continue;
        }
        s->last_ms[i] = now_ms;
        s->have[i] = true;
        if (got) {
            s->misses[i] = 0;
        } else if (ecu_live && pid != 0x0C && ++s->misses[i] >= 2) {
            s->evicted[i] = true;   /* asked twice against a live ECU, never answered */
        }
        return;
    }
}

void muse_sched_done_did(muse_sched_t *s, uint32_t now_ms)
{
    s->did_have = true;
    s->did_last_ms = now_ms;
    s->have[SCHED_DID] = true;
    s->last_ms[SCHED_DID] = now_ms;
}

void muse_sched_latch_batch_off(muse_sched_t *s)
{
    s->batch_off = true;
}

void muse_link_stats_reset(muse_link_stats_t *s)
{
    memset(s, 0, sizeof(*s));
}

void muse_link_stats_exchange(muse_link_stats_t *s, uint32_t now_ms, uint32_t rtt_ms, int readings, bool via_batch)
{
    s->exchanges++;
    s->last_rtt_ms = rtt_ms;
    s->times[s->t_head] = now_ms;
    s->t_head = (s->t_head + 1) % MUSE_LINK_TIMES;
    if (s->t_count < MUSE_LINK_TIMES) {
        s->t_count++;
    }
    if (readings > 0) {
        s->readings += (uint32_t)readings;
        if (via_batch) {
            s->batch_readings += (uint32_t)readings;
        }
    }
}

void muse_link_stats_timeout(muse_link_stats_t *s)
{
    s->timeouts++;
}

uint32_t muse_link_stats_rate_x100(const muse_link_stats_t *s)
{
    if (s->t_count < 2) {
        return 0;
    }
    uint32_t newest = s->times[(s->t_head + MUSE_LINK_TIMES - 1) % MUSE_LINK_TIMES];
    uint32_t oldest = s->times[(s->t_head + MUSE_LINK_TIMES - s->t_count) % MUSE_LINK_TIMES];
    uint32_t span = newest - oldest;
    return span ? (uint32_t)((uint64_t)(s->t_count - 1) * 100000 / span) : 0;
}

int muse_link_stats_batch_pct(const muse_link_stats_t *s)
{
    return s->readings ? (int)(s->batch_readings * 100 / s->readings) : -1;
}

void muse_tune_init(muse_tune_t *t)
{
    memset(t, 0, sizeof(*t));
    t->atst = MUSE_TUNE_ATST_MAX;   /* the ELM's default is what's in force */
}

int muse_tune_observe_value(muse_tune_t *t, uint32_t rtt_ms)
{
    t->consec = 0;
    if (t->trimmed) {
        return -1;
    }
    t->rtt_sum += rtt_ms;
    if (++t->rtt_n < MUSE_TUNE_MEASURE) {
        return -1;
    }
    t->trimmed = true;
    /* Twice the mean round trip, in the byte's 4 ms units, clamped. */
    uint32_t byte = (t->rtt_sum / (uint32_t)t->rtt_n * 2 + 3) / 4;
    if (byte < MUSE_TUNE_ATST_MIN) {
        byte = MUSE_TUNE_ATST_MIN;
    }
    if (byte > MUSE_TUNE_ATST_MAX) {
        byte = MUSE_TUNE_ATST_MAX;
    }
    if (byte == t->atst) {
        return -1;
    }
    t->atst = (uint8_t)byte;
    return (int)byte;
}

int muse_tune_observe_timeout(muse_tune_t *t)
{
    if (++t->consec < MUSE_TUNE_TIMEOUTS) {
        return -1;
    }
    t->consec = 0;
    if (t->atst >= MUSE_TUNE_ATST_MAX) {
        return -1;
    }
    uint32_t byte = (uint32_t)t->atst + MUSE_TUNE_ATST_STEP;
    t->atst = (uint8_t)(byte > MUSE_TUNE_ATST_MAX ? MUSE_TUNE_ATST_MAX : byte);
    return t->atst;
}

/* Hex digits of a line, spaces left out, as bytes; -1 for anything else (or
 * an odd count, or more than cap). */
static int line_bytes(const char *line, size_t n, uint8_t *out, size_t cap)
{
    size_t k = 0;
    int hi = -1;
    for (size_t i = 0; i < n; i++) {
        if (line[i] == ' ') {
            continue;
        }
        int v = hex_val(line[i]);
        if (v < 0) {
            return -1;
        }
        if (hi < 0) {
            hi = v;
            continue;
        }
        if (k == cap) {
            return -1;
        }
        out[k++] = (uint8_t)(hi << 4 | v);
        hi = -1;
    }
    return hi < 0 ? (int)k : -1;
}

/* How many hex digits the line has, spaces left out; -1 if it has anything else. */
static int hex_digits(const char *line, size_t n)
{
    int d = 0;
    for (size_t i = 0; i < n; i++) {
        if (line[i] == ' ') {
            continue;
        }
        if (hex_val(line[i]) < 0) {
            return -1;
        }
        d++;
    }
    return d;
}

/*
 * The bytes of a CAN reply with headers off and CAN formatting on: one line
 * for a single frame ("7F2231"), or a length line of three hex digits and
 * then frames "0:" .. "F:" (and round again) for a longer one, cut at that
 * length. Lines that aren't data ("SEARCHING...") are skipped. -1 when the
 * data doesn't add up: frames out of order or without a length, fewer bytes
 * than the length says, or more than cap.
 */
static int can_reply_bytes(const char *resp, uint8_t *out, size_t cap)
{
    int total = -1;     /* the length line's, once read */
    size_t n = 0;
    unsigned next = 0;  /* the frame index expected next */
    bool single = false;
    const char *p = resp;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon) {
            /* A frame: its index, then its bytes. */
            int idx = -1;
            for (const char *c = p; c < colon; c++) {
                if (*c == ' ') {
                    continue;
                }
                if (idx >= 0 || hex_val(*c) < 0) {
                    return -1;
                }
                idx = hex_val(*c);
            }
            if (total < 0 || single || idx != (int)(next & 0xF)) {
                return -1;
            }
            next++;
            int got = line_bytes(colon + 1, len - (size_t)(colon + 1 - p), out + n, cap - n);
            if (got < 0) {
                return -1;
            }
            n += (size_t)got;
        } else {
            int digits = hex_digits(p, len);
            if (digits == 3 && total < 0 && n == 0 && !single) {
                total = hex_val(p[strspn(p, " ")]);
                for (const char *c = p + strspn(p, " ") + 1; c < p + len; c++) {
                    if (*c != ' ') {
                        total = total << 4 | hex_val(*c);
                    }
                }
            } else if (digits > 0) {
                if (total >= 0 || single || n) {
                    return -1;
                }
                int got = line_bytes(p, len, out, cap);
                if (got < 0) {
                    return -1;
                }
                n = (size_t)got;
                single = true;
            }
        }
        p += len;
        if (*p == '\n') {
            p++;
        }
    }
    if (total >= 0) {
        if (n < (size_t)total) {
            return -1;   /* frames missing */
        }
        n = (size_t)total;   /* the last frame's padding */
    }
    return (int)n;
}

/*
 * One Mode 01 response message's bytes, as J1979 shapes a multi-PID answer
 * on CAN (ISO 15765-4; the ELM327 datasheet's "Multiple PID Requests"): 41
 * once, then each answered PID followed by its data bytes, in any order.
 * Each asked PID found takes its value (the first answer wins when several
 * ECUs answer). It stops at a PID it didn't ask for, or one cut short: past
 * that it can't know where the next PID starts. Returns how many it read.
 */
static int batch_message(const uint8_t *b, size_t n, const uint8_t *pids, int np, int *values, bool *got)
{
    if (n < 1 || b[0] != 0x41) {
        return 0;
    }
    int found = 0;
    size_t i = 1;
    while (i < n) {
        int k = -1;
        for (int j = 0; j < np; j++) {
            if (pids[j] == b[i]) {
                k = j;
                break;
            }
        }
        if (k < 0) {
            break;
        }
        size_t nb = (size_t)pid_nbytes(b[i]);
        if (i + 1 + nb > n) {
            break;
        }
        if (!got[k]) {
            int data[MUSE_ELM_PID_BYTES_MAX];
            for (size_t d = 0; d < nb; d++) {
                data[d] = b[i + 1 + d];
            }
            values[k] = pid_value(b[i], data);
            got[k] = true;
            found++;
        }
        i += 1 + nb;
    }
    return found;
}

muse_elm_result_t muse_elm_parse_batch(const char *resp, const uint8_t *pids, int n, int *values, bool *got)
{
    if (n > MUSE_ELM_BATCH_MAX) {
        n = MUSE_ELM_BATCH_MAX;
    }
    for (int i = 0; i < n; i++) {
        got[i] = false;
    }
    /* With headers off and CAN formatting on, each message is either a
     * single-frame line ("410C1AF80D00") or a length line ("00A") and its
     * frames ("0:410C1AF80D00", "1:0544..."), cut at that length. Lines that
     * aren't data ("SEARCHING...", a garbled line) are skipped; a frame out
     * of order loses its message only. */
    uint8_t msg[64];
    size_t have = 0;
    int total = -1;     /* the pending multi-frame message's length; -1: none */
    unsigned next = 0;  /* its frame index expected next */
    int found = 0;
    const char *p = resp;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char *colon = memchr(p, ':', len);
        if (colon) {
            int idx = -1;
            bool ok = true;
            for (const char *c = p; c < colon && ok; c++) {
                if (*c == ' ') {
                    continue;
                }
                ok = idx < 0 && hex_val(*c) >= 0;
                idx = hex_val(*c);
            }
            int got_n = -1;
            if (ok && total >= 0 && idx == (int)(next & 0xF)) {
                got_n = line_bytes(colon + 1, len - (size_t)(colon + 1 - p), msg + have, sizeof(msg) - have);
            }
            if (got_n < 0) {
                total = -1;   /* stray, out of order or garbled: the message is lost */
            } else {
                have += (size_t)got_n;
                next++;
                if (have >= (size_t)total) {
                    found += batch_message(msg, (size_t)total, pids, n, values, got);   /* padding cut */
                    total = -1;
                }
            }
        } else {
            int digits = hex_digits(p, len);
            if (digits == 3) {
                total = 0;
                for (const char *c = p; c < p + len; c++) {
                    if (*c != ' ') {
                        total = total << 4 | hex_val(*c);
                    }
                }
                if ((size_t)total > sizeof(msg)) {
                    total = -1;   /* longer than any batch's answer: not ours */
                }
                have = 0;
                next = 0;
            } else if (digits > 0) {
                uint8_t one[8];
                int bytes = line_bytes(p, len, one, sizeof(one));
                if (bytes > 0) {
                    found += batch_message(one, (size_t)bytes, pids, n, values, got);
                }
            }
        }
        p += len;
        if (*p == '\n') {
            p++;
        }
    }
    if (found) {
        return MUSE_ELM_BATCH;
    }
    /* Nothing read a value: the request as a whole was refused (NO DATA or
     * an error), or no message answered any asked PID (all garbled). */
    return parse_pid(resp, -1, 0, NULL, MUSE_ELM_NONE);
}

muse_elm_result_t muse_elm_parse_vtec_did(const char *resp, muse_vtec_did_t *out)
{
    /* An error anywhere spoils the whole reply; NO DATA is the ECU's silence. */
    muse_elm_result_t lines = parse_pid(resp, -1, 0, NULL, MUSE_ELM_NONE);
    const char *p = resp;
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len && p[strspn(p, " ")] == '<') {
            return MUSE_ELM_ERROR;   /* "<RX ERROR", "<DATA ERROR": a broken frame */
        }
        p += len;
        if (*p == '\n') {
            p++;
        }
    }
    if (lines == MUSE_ELM_ERROR) {
        return MUSE_ELM_ERROR;
    }
    uint8_t b[64];
    int n = can_reply_bytes(resp, b, sizeof(b));
    if (n >= 3 && b[0] == 0x7F && b[1] == 0x22) {
        return MUSE_ELM_NEGATIVE;
    }
    if (n == MUSE_VTEC_DID_LEN && b[0] == 0x62 && b[1] == 0x26 && b[2] == 0x12) {
        const uint8_t *data = b + 3;
        out->sol = data[MUSE_VTEC_DID_FLAGS] & MUSE_VTEC_DID_SOL;
        out->psw = data[MUSE_VTEC_DID_FLAGS] & MUSE_VTEC_DID_PSW;
        out->b23 = data[MUSE_VTEC_DID_B23];
        return MUSE_ELM_VTEC_DID;
    }
    return n <= 0 && lines == MUSE_ELM_NO_DATA ? MUSE_ELM_NO_DATA : MUSE_ELM_NONE;
}

bool muse_vtec_engaged(bool valid, bool sol, uint32_t age_ms)
{
    return valid && sol && age_ms <= MUSE_VTEC_FRESH_MS;
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
