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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* opendir, on the host */
#endif

#include "muse_obd_log_store.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

const char *const MUSE_LOG_COLUMN[MUSE_LOG_CHANNELS] = {
    [MUSE_LOG_RPM] = "rpm",
    [MUSE_LOG_COOLANT] = "coolant_c",
    [MUSE_LOG_IAT] = "iat_c",
    [MUSE_LOG_TIMING] = "timing_deg",
    [MUSE_LOG_MAP] = "map_kpa",
    [MUSE_LOG_LOAD] = "load_pct",
    [MUSE_LOG_THROTTLE] = "throttle_pct",
    [MUSE_LOG_SPEED] = "speed_kmh",
    [MUSE_LOG_STFT] = "stft_pct",
    [MUSE_LOG_LTFT] = "ltft_pct",
    [MUSE_LOG_AMBIENT] = "ambient_c",
    [MUSE_LOG_VTEC_SOL] = "vtec_sol",
    [MUSE_LOG_VTEC_PSW] = "vtec_psw",
    [MUSE_LOG_AFR] = "afr",
    [MUSE_LOG_FUEL] = "fuel_pct",
    [MUSE_LOG_PEDAL_D] = "pedal_d_pct",
    [MUSE_LOG_PEDAL_E] = "pedal_e_pct",
    [MUSE_LOG_MODULE_V] = "module_v",
    [MUSE_LOG_BARO] = "baro_kpa",
};

const uint8_t MUSE_LOG_DECIMALS[MUSE_LOG_CHANNELS] = {
    [MUSE_LOG_AFR] = 1,
    [MUSE_LOG_MODULE_V] = 3,
};

/* ---- Text into a bounded buffer, without printf: every length is checked
 * here, so no format can overflow or be cut short unnoticed. ---- */

typedef struct {
    char *p;
    size_t cap, len;
    bool over;    /* something didn't fit */
} out_t;

static void put_n(out_t *o, const char *s, size_t n)
{
    if (o->over || n > o->cap - o->len) {
        o->over = true;
        return;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
}

static void put(out_t *o, const char *s)
{
    put_n(o, s, strlen(s));
}

static void put_u(out_t *o, unsigned long long v)
{
    char d[20];
    size_t n = 0;
    do {
        d[sizeof(d) - 1 - n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put_n(o, d + sizeof(d) - n, n);
}

static void put_i(out_t *o, long long v)
{
    if (v < 0) {
        put(o, "-");
        put_u(o, 0ull - (unsigned long long)v);
    } else {
        put_u(o, (unsigned long long)v);
    }
}

/* v in 10^-decimals of its unit, with the point: 147 with 1 is "14.7",
 * 845 with 3 is "0.845". No point when decimals is 0. */
static void put_fixed(out_t *o, long long v, int decimals)
{
    if (decimals <= 0) {
        put_i(o, v);
        return;
    }
    if (decimals > 9) {
        decimals = 9;   /* d[] below holds them */
    }
    unsigned long long m = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
    unsigned long long scale = 1;
    for (int i = 0; i < decimals; i++) {
        scale *= 10;
    }
    if (v < 0) {
        put(o, "-");
    }
    put_u(o, m / scale);
    put(o, ".");
    char d[20];
    unsigned long long part = m % scale;
    for (int i = decimals - 1; i >= 0; i--) {
        d[i] = (char)('0' + part % 10);
        part /= 10;
    }
    put_n(o, d, (size_t)decimals);
}

/* Zero-padded to `width` digits. */
static void put_u_pad(out_t *o, unsigned v, int width)
{
    char d[10];
    for (int i = width - 1; i >= 0; i--) {
        d[i] = (char)('0' + v % 10);
        v /= 10;
    }
    put_n(o, d, (size_t)width);
}

static void put_hex8(out_t *o, uint32_t v)
{
    static const char HEX[] = "0123456789ABCDEF";
    char d[8];
    for (int i = 7; i >= 0; i--) {
        d[i] = HEX[v & 0xF];
        v >>= 4;
    }
    put_n(o, d, 8);
}

/* "2026-10-23T18:02:11Z" (days to the civil date: H. Hinnant's algorithm). */
static void put_utc(out_t *o, int64_t t)
{
    int64_t days = t / 86400, secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        days--;
    }
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    put_i(o, y);
    put(o, "-");
    put_u_pad(o, m, 2);
    put(o, "-");
    put_u_pad(o, d, 2);
    put(o, "T");
    put_u_pad(o, (unsigned)(secs / 3600), 2);
    put(o, ":");
    put_u_pad(o, (unsigned)(secs / 60 % 60), 2);
    put(o, ":");
    put_u_pad(o, (unsigned)(secs % 60), 2);
    put(o, "Z");
}

size_t muse_log_format_row(char *buf, size_t cap, const muse_log_sample_t *s)
{
    out_t o = { buf, cap, 0, false };
    put_u(&o, s->ms);
    for (int c = 0; c < MUSE_LOG_CHANNELS; c++) {
        put(&o, ",");
        if (s->has[c]) {
            put_fixed(&o, s->value[c], MUSE_LOG_DECIMALS[c]);
        }
    }
    put(&o, "\n");
    return o.over ? 0 : o.len;
}

size_t muse_log_format_header(char *buf, size_t cap, const muse_log_meta_t *m)
{
    out_t o = { buf, cap, 0, false };
    put(&o, "# muse obd trip log v1\n# firmware=");
    put(&o, m->firmware[0] ? m->firmware : "unknown");
    put(&o, "\n# trip=");
    put_u(&o, m->id);
    put(&o, "\n# protocol=");
    put(&o, m->protocol[0] ? m->protocol : "unknown");
    put(&o, "\n#");
    for (int i = 0; i < 3; i++) {
        put(&o, i == 0 ? " supported_00=" : i == 1 ? " supported_20=" : " supported_40=");
        if (m->support_valid[i]) {
            put_hex8(&o, m->support[i]);
        } else {
            put(&o, "unknown");
        }
    }
    if (m->started >= 0) {
        put(&o, "\n# start=");
        put_utc(&o, m->started);
    } else {
        put(&o, "\n# t0=boot boot_ms=");
        put_u(&o, m->boot_ms);
    }
    if (m->did_2612 == MUSE_LOG_DID_YES || m->did_2612 == MUSE_LOG_DID_NO) {
        put(&o, m->did_2612 == MUSE_LOG_DID_YES ? "\n# did_2612=yes" : "\n# did_2612=no");
    }
    put(&o, "\nms");
    for (int c = 0; c < MUSE_LOG_CHANNELS; c++) {
        put(&o, ",");
        put(&o, MUSE_LOG_COLUMN[c]);
    }
    put(&o, "\n");
    return o.over ? 0 : o.len;
}

/* ---- The manifest ---- */

static void put_time(out_t *o, int64_t t)
{
    if (t < 0) {
        put(o, "null");
    } else {
        put_i(o, t);
    }
}

size_t muse_log_manifest_line(char *buf, size_t cap, const muse_log_trip_t *t)
{
    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(t->id, name);
    out_t o = { buf, cap, 0, false };
    put(&o, "{\"id\":");
    put_u(&o, t->id);
    put(&o, ",\"file\":\"");
    put(&o, name);
    put(&o, "\",\"bytes\":");
    put_u(&o, t->bytes);
    put(&o, ",\"samples\":");
    put_u(&o, t->samples);
    put(&o, ",\"dropped\":");
    put_u(&o, t->dropped);
    put(&o, ",\"started\":");
    put_time(&o, t->started);
    put(&o, ",\"ended\":");
    put_time(&o, t->ended);
    put(&o, ",\"crc32\":\"");
    put_hex8(&o, t->crc32);
    put(&o, "\"}\n");
    if (o.over || o.len >= cap) {
        return 0;
    }
    buf[o.len] = '\0';
    return o.len;
}

/* The text after `"key":` in line, or NULL. */
static const char *field(const char *line, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = strchr(line, '"'); p; p = strchr(p + 1, '"')) {
        if (!strncmp(p + 1, key, n) && p[1 + n] == '"' && p[2 + n] == ':') {
            return p + 3 + n;
        }
    }
    return NULL;
}

static bool field_u32(const char *line, const char *key, uint32_t *out)
{
    const char *p = field(line, key);
    if (!p || *p < '0' || *p > '9') {
        return false;
    }
    char *end;
    unsigned long long v = strtoull(p, &end, 10);
    if (v > UINT32_MAX || (*end != ',' && *end != '}')) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool field_time(const char *line, const char *key, int64_t *out)
{
    const char *p = field(line, key);
    if (!p) {
        return false;
    }
    if (!strncmp(p, "null", 4)) {
        *out = -1;
        return true;
    }
    if (*p < '0' || *p > '9') {
        return false;
    }
    char *end;
    long long v = strtoll(p, &end, 10);
    if (*end != ',' && *end != '}') {
        return false;
    }
    *out = v;
    return true;
}

bool muse_log_manifest_parse(const char *line, muse_log_trip_t *t)
{
    muse_log_trip_t r;
    const char *crc = field(line, "crc32");
    if (line[0] != '{' || !field_u32(line, "id", &r.id) || !field_u32(line, "bytes", &r.bytes)
        || !field_u32(line, "samples", &r.samples) || !field_u32(line, "dropped", &r.dropped)
        || !field_time(line, "started", &r.started) || !field_time(line, "ended", &r.ended) || !crc
        || crc[0] != '"') {
        return false;
    }
    char *end;
    unsigned long v = strtoul(crc + 1, &end, 16);
    if (end != crc + 9 || *end != '"' || r.id == 0) {
        return false;
    }
    r.crc32 = (uint32_t)v;
    *t = r;
    return true;
}

/* ---- CRC-32, base64, names ---- */

uint32_t muse_log_crc32(uint32_t crc, const void *data, size_t len)
{
    static const uint32_t NIBBLE[16] = {
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
        0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C, 0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
    };
    const uint8_t *p = data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ NIBBLE[crc & 0xF];
        crc = (crc >> 4) ^ NIBBLE[crc & 0xF];
    }
    return ~crc;
}

size_t muse_log_base64(char *out, size_t cap, const uint8_t *in, size_t len)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = MUSE_LOG_B64_LEN(len);
    if (need >= cap) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[o++] = B64[v >> 18 & 63];
        out[o++] = B64[v >> 12 & 63];
        out[o++] = i + 1 < len ? B64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

void muse_log_file_name(uint32_t id, char out[MUSE_LOG_NAME_MAX])
{
    out_t o = { out, MUSE_LOG_NAME_MAX - 1, 0, false };
    put(&o, "T");
    put_u_pad(&o, id > MUSE_LOG_ID_MAX ? MUSE_LOG_ID_MAX : id, 7);
    put(&o, ".CSV");
    out[o.len] = '\0';
}

/* dir "/" name into path; false when it doesn't fit. */
static bool join(char path[MUSE_LOG_PATH_MAX], const char *dir, const char *name)
{
    out_t o = { path, MUSE_LOG_PATH_MAX - 1, 0, false };
    put(&o, dir);
    put(&o, "/");
    put(&o, name);
    if (o.over) {
        return false;
    }
    path[o.len] = '\0';
    return true;
}

static FILE *open_in(const char *dir, const char *name, const char *mode)
{
    char path[MUSE_LOG_PATH_MAX];
    return join(path, dir, name) ? fopen(path, mode) : NULL;
}

static bool remove_in(const char *dir, const char *name)
{
    char path[MUSE_LOG_PATH_MAX];
    return join(path, dir, name) && remove(path) == 0;
}

/* ---- A store ---- */

/* Calls fn for each manifest line that parses; false stops. -1 if the
 * manifest exists but can't be read, else how many lines parsed. */
typedef bool (*each_fn)(const muse_log_trip_t *t, void *ctx);

static int each_trip(const char *dir, each_fn fn, void *ctx)
{
    FILE *f = open_in(dir, MUSE_LOG_MANIFEST, "r");
    if (!f) {
        return 0;   /* no manifest yet: no trips */
    }
    char line[MUSE_LOG_LINE_MAX];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        muse_log_trip_t t;
        if (!muse_log_manifest_parse(line, &t)) {
            continue;
        }
        n++;
        if (fn && !fn(&t, ctx)) {
            break;
        }
    }
    bool err = ferror(f);
    fclose(f);
    return err ? -1 : n;
}

typedef struct {
    muse_log_trip_t *out;
    int max, n;
} list_ctx_t;

static bool list_one(const muse_log_trip_t *t, void *ctx)
{
    list_ctx_t *c = ctx;
    if (c->n < c->max) {
        c->out[c->n] = *t;
    }
    c->n++;
    return true;
}

int muse_log_store_list(const char *dir, muse_log_trip_t *out, int max)
{
    list_ctx_t c = { out, out ? max : 0, 0 };
    int n = each_trip(dir, list_one, &c);
    return n < 0 ? -1 : c.n;
}

typedef struct {
    uint32_t id;
    muse_log_trip_t *out;
    bool found;
} find_ctx_t;

static bool find_one(const muse_log_trip_t *t, void *ctx)
{
    find_ctx_t *c = ctx;
    if (t->id != c->id) {
        return true;
    }
    if (c->out) {
        *c->out = *t;
    }
    c->found = true;
    return false;
}

bool muse_log_store_find(const char *dir, uint32_t id, muse_log_trip_t *out)
{
    find_ctx_t c = { id, out, false };
    each_trip(dir, find_one, &c);
    return c.found;
}

static bool max_one(const muse_log_trip_t *t, void *ctx)
{
    uint32_t *max = ctx;
    if (t->id > *max) {
        *max = t->id;
    }
    return true;
}

uint32_t muse_log_store_max_id(const char *dir)
{
    uint32_t max = 0;
    each_trip(dir, max_one, &max);
    return max;
}

int muse_log_store_read(const char *dir, uint32_t id, uint32_t offset, uint8_t *buf, size_t cap, uint32_t *total)
{
    muse_log_trip_t t;
    if (!muse_log_store_find(dir, id, &t)) {
        return MUSE_LOG_READ_UNKNOWN;
    }
    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(id, name);
    FILE *f = open_in(dir, name, "rb");
    if (!f) {
        return MUSE_LOG_READ_IO;
    }
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        size = ftell(f);
    }
    if (size < 0) {
        fclose(f);
        return MUSE_LOG_READ_IO;
    }
    *total = (uint32_t)size;
    if (offset > (uint32_t)size) {
        fclose(f);
        return MUSE_LOG_READ_PAST_END;
    }
    size_t want = (uint32_t)size - offset < cap ? (uint32_t)size - offset : cap;
    size_t got = 0;
    if (want && fseek(f, (long)offset, SEEK_SET) == 0) {
        got = fread(buf, 1, want, f);
    }
    fclose(f);
    return got == want ? (int)got : MUSE_LOG_READ_IO;
}

typedef struct {
    uint32_t id;
    FILE *out;
    bool found, failed;
} rewrite_ctx_t;

static bool keep_one(const muse_log_trip_t *t, void *ctx)
{
    rewrite_ctx_t *c = ctx;
    if (t->id == c->id) {
        c->found = true;
        return true;
    }
    char line[MUSE_LOG_LINE_MAX];
    size_t n = muse_log_manifest_line(line, sizeof(line), t);
    if (!n || fwrite(line, 1, n, c->out) != n) {
        c->failed = true;
        return false;
    }
    return true;
}

bool muse_log_store_delete(const char *dir, uint32_t id)
{
    if (!muse_log_store_find(dir, id, NULL)) {
        return false;
    }
    /* The manifest without the line goes beside the old one, then takes its
     * place: a reset in between leaves one or the other (recover). FAT's
     * rename doesn't replace, so the old one goes first. */
    FILE *out = open_in(dir, MUSE_LOG_MANIFEST_TMP, "w");
    if (!out) {
        return false;
    }
    rewrite_ctx_t c = { id, out, false, false };
    int n = each_trip(dir, keep_one, &c);
    if (fclose(out) != 0 || n < 0 || c.failed || !c.found) {
        remove_in(dir, MUSE_LOG_MANIFEST_TMP);
        return false;
    }
    char from[MUSE_LOG_PATH_MAX], to[MUSE_LOG_PATH_MAX];
    if (!join(from, dir, MUSE_LOG_MANIFEST_TMP) || !join(to, dir, MUSE_LOG_MANIFEST)) {
        return false;
    }
    remove(to);
    if (rename(from, to) != 0) {
        return false;
    }
    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(id, name);
    remove_in(dir, name);   /* gone from the manifest is what counts: an orphan comes back (recover) */
    return true;
}

/* The trip id of a file named like muse_log_file_name's, or 0. */
static uint32_t trip_file_id(const char *name)
{
    if ((name[0] != 'T' && name[0] != 't') || strlen(name) != MUSE_LOG_NAME_MAX - 1) {
        return 0;
    }
    uint32_t id = 0;
    for (int i = 1; i <= 7; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return 0;
        }
        id = id * 10 + (uint32_t)(name[i] - '0');
    }
    const char *ext = name + 8;
    if (ext[0] != '.' || (ext[1] | 0x20) != 'c' || (ext[2] | 0x20) != 's' || (ext[3] | 0x20) != 'v') {
        return 0;
    }
    return id;
}

/* The file's size, CRC and rows (lines not starting '#', less the column
 * names). False if it can't be read. */
static bool scan_file(FILE *f, uint32_t *bytes, uint32_t *crc, uint32_t *rows)
{
    uint8_t buf[512];
    uint32_t n = 0, c = 0, lines = 0;
    bool line_start = true, comment = false;
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        c = muse_log_crc32(c, buf, got);
        n += (uint32_t)got;
        for (size_t i = 0; i < got; i++) {
            if (line_start) {
                comment = buf[i] == '#';
                line_start = false;
            }
            if (buf[i] == '\n') {
                lines += !comment;
                line_start = true;
            }
        }
    }
    if (ferror(f)) {
        return false;
    }
    *bytes = n;
    *crc = c;
    if (rows) {
        *rows = lines ? lines - 1 : 0;
    }
    return true;
}

/* Does the manifest end mid-line (an append cut off by a reset)? */
static bool ends_mid_line(const char *dir)
{
    FILE *f = open_in(dir, MUSE_LOG_MANIFEST, "rb");
    if (!f) {
        return false;
    }
    bool mid = fseek(f, -1, SEEK_END) == 0 && fgetc(f) != '\n';
    fclose(f);
    return mid;
}

static bool append_line(const char *dir, const muse_log_trip_t *t)
{
    char line[MUSE_LOG_LINE_MAX];
    size_t n = muse_log_manifest_line(line, sizeof(line), t);
    bool mid = ends_mid_line(dir);   /* then the cut-off line ends here, alone */
    FILE *f = open_in(dir, MUSE_LOG_MANIFEST, "a");
    if (!f) {
        return false;
    }
    bool ok = n && (!mid || fputc('\n', f) == '\n') && fwrite(line, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

int muse_log_store_recover(const char *dir)
{
    char path[MUSE_LOG_PATH_MAX], tmp[MUSE_LOG_PATH_MAX];
    if (!join(path, dir, MUSE_LOG_MANIFEST) || !join(tmp, dir, MUSE_LOG_MANIFEST_TMP)) {
        return 0;
    }
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        remove(tmp);   /* a delete that never got to the rename: the old one stands */
    } else {
        rename(tmp, path);
    }

    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int added = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t id = trip_file_id(e->d_name);
        if (!id || muse_log_store_find(dir, id, NULL)) {
            continue;
        }
        muse_log_trip_t t = { .id = id, .started = -1, .ended = -1 };
        FILE *trip = open_in(dir, e->d_name, "rb");
        if (!trip) {
            continue;
        }
        bool ok = scan_file(trip, &t.bytes, &t.crc32, &t.samples);
        fclose(trip);
        if (ok && append_line(dir, &t)) {
            added++;
        }
    }
    closedir(d);
    return added;
}

/* ---- Writing a trip ---- */

static bool write_out(muse_log_writer_t *w, const void *data, size_t n)
{
    if (w->full || n > w->limit - w->written) {
        w->full = true;
        return false;
    }
    size_t put = fwrite(data, 1, n, w->f);
    w->written += (uint32_t)put;
    if (put != n) {
        w->full = true;
        return false;
    }
    return true;
}

bool muse_log_trip_open(muse_log_writer_t *w, const char *dir, const muse_log_meta_t *m, char *buf, size_t cap,
                        uint32_t limit)
{
    memset(w, 0, sizeof(*w));
    if (strlen(dir) >= sizeof(w->dir) || cap < MUSE_LOG_ROW_MAX || m->id == 0) {
        return false;
    }
    char header[MUSE_LOG_HEADER_MAX];
    size_t n = muse_log_format_header(header, sizeof(header), m);
    if (!n || n > limit) {
        return false;
    }
    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(m->id, name);
    w->f = open_in(dir, name, "wb");
    if (!w->f) {
        return false;
    }
    memcpy(w->dir, dir, strlen(dir) + 1);
    w->id = m->id;
    w->started = m->started;
    w->buf = buf;
    w->cap = cap;
    w->limit = limit;
    if (!write_out(w, header, n)) {
        fclose(w->f);
        w->f = NULL;
        remove_in(dir, name);
        return false;
    }
    return true;
}

bool muse_log_trip_flush(muse_log_writer_t *w)
{
    if (!w->len) {
        return !w->full;
    }
    bool ok = write_out(w, w->buf, w->len);
    if (!ok) {
        /* What didn't fit is lost: those rows count as dropped. */
        w->samples -= w->buf_rows;
        w->dropped += w->buf_rows;
    }
    w->len = 0;
    w->buf_rows = 0;
    return ok;
}

void muse_log_trip_row(muse_log_writer_t *w, const char *row, size_t len)
{
    if (!w->f || w->full || len > w->cap) {
        w->dropped++;
        return;
    }
    if (len > w->cap - w->len && !muse_log_trip_flush(w)) {
        w->dropped++;
        return;
    }
    memcpy(w->buf + w->len, row, len);
    w->len += len;
    w->buf_rows++;
    w->samples++;
}

void muse_log_trip_drop(muse_log_writer_t *w, uint32_t n)
{
    w->dropped += n;
}

bool muse_log_trip_close(muse_log_writer_t *w, int64_t ended, muse_log_trip_t *out)
{
    if (!w->f) {
        return false;
    }
    muse_log_trip_flush(w);
    /* The trailer; it fits in the limit's reserve if the rows didn't. */
    char trailer[64];
    out_t o = { trailer, sizeof(trailer), 0, false };
    put(&o, "# end samples=");
    put_u(&o, w->samples);
    put(&o, " dropped=");
    put_u(&o, w->dropped);
    put(&o, "\n");
    if (!o.over) {
        w->full = false;
        w->limit += (uint32_t)o.len;
        write_out(w, trailer, o.len);
    }
    bool closed = fclose(w->f) == 0;
    w->f = NULL;

    muse_log_trip_t t = {
        .id = w->id, .samples = w->samples, .dropped = w->dropped, .started = w->started, .ended = ended,
    };
    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(w->id, name);
    FILE *f = open_in(w->dir, name, "rb");
    bool ok = f && scan_file(f, &t.bytes, &t.crc32, NULL);
    if (f) {
        fclose(f);
    }
    ok = ok && closed && append_line(w->dir, &t);
    if (out) {
        *out = t;
    }
    return ok;
}

uint32_t muse_log_next_id(uint32_t stored_next, uint32_t max_seen)
{
    uint32_t next = stored_next ? stored_next : 1;
    return max_seen >= next ? max_seen + 1 : next;
}
