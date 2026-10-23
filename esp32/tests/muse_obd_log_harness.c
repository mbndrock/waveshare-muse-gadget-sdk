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
 * The OBD trip log's files (muse_obd_log_store.c) in a temporary directory,
 * standing in for the flash partition's FAT: the row, header and manifest
 * formats, CRC-32 and base64; a whole trip's bytes back through chunked
 * reads (start, middle, the exact end, past it); delete removing exactly one
 * trip; the manifest and the id counter across a simulated reboot; recovery
 * after a reset mid-trip or mid-delete; and a store that fills up.
 */
#define _DEFAULT_SOURCE   /* mkdtemp */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "muse_obd_log_store.h"

static int s_fail;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        s_fail++; \
    } \
} while (0)

static char s_dir[64];

static void path_of(char *out, size_t cap, const char *name)
{
    int n = snprintf(out, cap, "%s/%s", s_dir, name);
    if (n < 0 || (size_t)n >= cap) {
        abort();
    }
}

static bool exists(const char *name)
{
    char p[128];
    path_of(p, sizeof(p), name);
    return access(p, F_OK) == 0;
}

static long file_size(const char *name)
{
    char p[128];
    path_of(p, sizeof(p), name);
    FILE *f = fopen(p, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static void test_row(void)
{
    muse_log_sample_t s = { .ms = 1234 };
    char row[MUSE_LOG_ROW_MAX];
    size_t n = muse_log_format_row(row, sizeof(row), &s);
    CHECK(n == strlen("1234,,,,,,,,,,,\n"));
    CHECK(!memcmp(row, "1234,,,,,,,,,,,\n", n));

    s.ms = 0;
    for (int c = 0; c < MUSE_LOG_CHANNELS; c++) {
        s.has[c] = true;
        s.value[c] = c * 10 - 20;
    }
    s.has[MUSE_LOG_AMBIENT] = false;
    n = muse_log_format_row(row, sizeof(row), &s);
    const char *want = "0,-20,-10,0,10,20,30,40,50,60,70,\n";
    CHECK(n == strlen(want) && !memcmp(row, want, n));

    /* The widest row there can be fits MUSE_LOG_ROW_MAX. */
    s.ms = UINT32_MAX;
    for (int c = 0; c < MUSE_LOG_CHANNELS; c++) {
        s.has[c] = true;
        s.value[c] = INT32_MIN;
    }
    n = muse_log_format_row(row, sizeof(row), &s);
    CHECK(n > 0 && n <= MUSE_LOG_ROW_MAX);
    CHECK(muse_log_format_row(row, n - 1, &s) == 0);   /* one short: refused, not cut */
    CHECK(muse_log_format_row(row, n, &s) == n);

    CHECK(MUSE_LOG_CHANNELS == 11);
    static const char *const NAMES[] = {
        "rpm", "coolant_c", "iat_c", "timing_deg", "map_kpa", "load_pct",
        "throttle_pct", "speed_kmh", "stft_pct", "ltft_pct", "ambient_c",
    };
    for (int c = 0; c < MUSE_LOG_CHANNELS; c++) {
        CHECK(!strcmp(MUSE_LOG_COLUMN[c], NAMES[c]));
    }
}

static void test_header(void)
{
    muse_log_meta_t m = {
        .id = 12, .firmware = "1a2b3c4", .protocol = "CAN 29/500",
        .support_valid = { true, true, false }, .support = { 0xBE1FA813u, 0x80018001u, 0 },
        .started = 1729700000, .boot_ms = 81234,
    };
    char h[MUSE_LOG_HEADER_MAX + 1];
    size_t n = muse_log_format_header(h, MUSE_LOG_HEADER_MAX, &m);
    CHECK(n > 0);
    h[n] = '\0';
    const char *want =
        "# muse obd trip log v1\n"
        "# firmware=1a2b3c4\n"
        "# trip=12\n"
        "# protocol=CAN 29/500\n"
        "# supported_00=BE1FA813 supported_20=80018001 supported_40=unknown\n"
        "# start=2024-10-23T16:13:20Z\n"
        "ms,rpm,coolant_c,iat_c,timing_deg,map_kpa,load_pct,throttle_pct,speed_kmh,stft_pct,ltft_pct,ambient_c\n";
    CHECK(!strcmp(h, want));
    if (strcmp(h, want)) {
        fprintf(stderr, "got:\n%s", h);
    }

    /* No clock: t0 is boot. Nothing known: "unknown". */
    m = (muse_log_meta_t){ .id = 1, .started = -1, .boot_ms = 5000 };
    n = muse_log_format_header(h, MUSE_LOG_HEADER_MAX, &m);
    h[n] = '\0';
    CHECK(strstr(h, "# t0=boot boot_ms=5000\n") != NULL);
    CHECK(strstr(h, "# start=") == NULL);
    CHECK(strstr(h, "# firmware=unknown\n# trip=1\n# protocol=unknown\n") != NULL);
    CHECK(strstr(h, "supported_00=unknown supported_20=unknown supported_40=unknown") != NULL);
    CHECK(muse_log_format_header(h, 40, &m) == 0);

    /* A leap day and the year 2038's far side. */
    m.started = 1709208000;   /* 2024-02-29T12:00:00Z */
    n = muse_log_format_header(h, MUSE_LOG_HEADER_MAX, &m);
    h[n] = '\0';
    CHECK(strstr(h, "# start=2024-02-29T12:00:00Z\n") != NULL);
    m.started = 4102444799LL;   /* 2099-12-31T23:59:59Z */
    n = muse_log_format_header(h, MUSE_LOG_HEADER_MAX, &m);
    h[n] = '\0';
    CHECK(strstr(h, "# start=2099-12-31T23:59:59Z\n") != NULL);
}

static void test_manifest_line(void)
{
    muse_log_trip_t t = { .id = 42, .bytes = 123456, .samples = 789, .dropped = 3,
                          .started = 1729700000, .ended = -1, .crc32 = 0x0BADF00Du };
    char line[MUSE_LOG_LINE_MAX];
    size_t n = muse_log_manifest_line(line, sizeof(line), &t);
    const char *want = "{\"id\":42,\"file\":\"T0000042.CSV\",\"bytes\":123456,\"samples\":789,\"dropped\":3,"
                       "\"started\":1729700000,\"ended\":null,\"crc32\":\"0BADF00D\"}\n";
    CHECK(n == strlen(want) && !strcmp(line, want));
    muse_log_trip_t r;
    CHECK(muse_log_manifest_parse(line, &r));
    CHECK(r.id == 42 && r.bytes == 123456 && r.samples == 789 && r.dropped == 3);
    CHECK(r.started == 1729700000 && r.ended == -1 && r.crc32 == 0x0BADF00Du);

    /* The longest line fits. */
    t = (muse_log_trip_t){ .id = MUSE_LOG_ID_MAX, .bytes = UINT32_MAX, .samples = UINT32_MAX,
                           .dropped = UINT32_MAX, .started = 4102444799LL, .ended = 4102444799LL,
                           .crc32 = UINT32_MAX };
    n = muse_log_manifest_line(line, sizeof(line), &t);
    CHECK(n > 0 && n < MUSE_LOG_LINE_MAX);
    CHECK(muse_log_manifest_parse(line, &r) && r.bytes == UINT32_MAX && r.crc32 == UINT32_MAX);
    CHECK(muse_log_manifest_line(line, n, &t) == 0);   /* no room for the NUL: refused */

    CHECK(!muse_log_manifest_parse("", &r));
    CHECK(!muse_log_manifest_parse("garbage\n", &r));
    CHECK(!muse_log_manifest_parse("{\"id\":0,\"bytes\":1,\"samples\":1,\"dropped\":0,\"started\":null,"
                                   "\"ended\":null,\"crc32\":\"00000000\"}", &r));   /* id 0 */
    CHECK(!muse_log_manifest_parse("{\"id\":5,\"bytes\":1,\"samples\":1,\"dropped\":0,\"started\":null,"
                                   "\"ended\":null,\"crc32\":\"0000\"}", &r));       /* short CRC */
    CHECK(!muse_log_manifest_parse("{\"id\":5,\"bytes\":-1,\"samples\":1,\"dropped\":0,\"started\":null,"
                                   "\"ended\":null,\"crc32\":\"00000000\"}", &r));
    CHECK(!muse_log_manifest_parse("{\"id\":5,\"bytes\":99999999999,\"samples\":1,\"dropped\":0,"
                                   "\"started\":null,\"ended\":null,\"crc32\":\"00000000\"}", &r));
    /* Cut off mid-line (a reset during the append). */
    CHECK(!muse_log_manifest_parse("{\"id\":5,\"bytes\":10,\"samp", &r));
}

static void test_crc_base64_names(void)
{
    CHECK(muse_log_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(muse_log_crc32(0, "", 0) == 0);
    /* In pieces, as the store reads a file back. */
    CHECK(muse_log_crc32(muse_log_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK(muse_log_crc32(0, "The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);

    static const struct { const char *in, *out; } B64[] = {
        { "", "" }, { "f", "Zg==" }, { "fo", "Zm8=" }, { "foo", "Zm9v" },
        { "foob", "Zm9vYg==" }, { "fooba", "Zm9vYmE=" }, { "foobar", "Zm9vYmFy" },
    };
    for (size_t i = 0; i < sizeof(B64) / sizeof(B64[0]); i++) {
        char out[16];
        size_t n = muse_log_base64(out, sizeof(out), (const uint8_t *)B64[i].in, strlen(B64[i].in));
        CHECK(n == strlen(B64[i].out) && !strcmp(out, B64[i].out));
    }
    uint8_t all[256];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    char big[MUSE_LOG_B64_LEN(256) + 1];
    CHECK(muse_log_base64(big, sizeof(big), all, 256) == MUSE_LOG_B64_LEN(256));
    CHECK(!strncmp(big, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8gISIjJCUmJygpKissLS4v", 64));
    CHECK(!strcmp(big + MUSE_LOG_B64_LEN(256) - 8, "/P3+/w=="));
    CHECK(muse_log_base64(big, MUSE_LOG_B64_LEN(256), all, 256) == 0);   /* no room for the NUL */
    /* trip_read's chunk: 6144 bytes are exactly 8192 characters. */
    CHECK(MUSE_LOG_B64_LEN(6144) == 8192);

    char name[MUSE_LOG_NAME_MAX];
    muse_log_file_name(12, name);
    CHECK(!strcmp(name, "T0000012.CSV"));
    muse_log_file_name(MUSE_LOG_ID_MAX, name);
    CHECK(!strcmp(name, "T9999999.CSV"));
}

/* ---- A store ---- */

#define BLOCK 256   /* small, so a trip spans many blocks */

static char s_expect[1 << 16];
static size_t s_expect_len;

static muse_log_meta_t meta(uint32_t id)
{
    return (muse_log_meta_t){ .id = id, .firmware = "abc1234", .protocol = "CAN 29/500",
                              .support_valid = { true }, .support = { 0xBE1FA813u }, .started = -1,
                              .boot_ms = 1000 * id };
}

/* Writes trip id with `rows` samples; s_expect holds the bytes it should have. */
static bool write_trip(uint32_t id, int rows, uint32_t limit, muse_log_trip_t *out)
{
    static char block[BLOCK];
    muse_log_meta_t m = meta(id);
    muse_log_writer_t w;
    if (!muse_log_trip_open(&w, s_dir, &m, block, sizeof(block), limit)) {
        return false;
    }
    s_expect_len = muse_log_format_header(s_expect, sizeof(s_expect), &m);
    for (int i = 0; i < rows; i++) {
        muse_log_sample_t s = { .ms = (uint32_t)i * 125 };
        s.has[MUSE_LOG_RPM] = true;
        s.value[MUSE_LOG_RPM] = 800 + i * 37 % 6000;
        s.has[MUSE_LOG_COOLANT] = i > 3;
        s.value[MUSE_LOG_COOLANT] = 40 + i / 10;
        s.has[MUSE_LOG_STFT] = i % 2;
        s.value[MUSE_LOG_STFT] = -3 + i % 7;
        char row[MUSE_LOG_ROW_MAX];
        size_t n = muse_log_format_row(row, sizeof(row), &s);
        muse_log_trip_row(&w, row, n);
        if (!w.full && s_expect_len + n <= sizeof(s_expect)) {
            memcpy(s_expect + s_expect_len, row, n);
            s_expect_len += n;
        }
    }
    bool ok = muse_log_trip_close(&w, 1729703600, out);
    char trailer[64];
    int n = snprintf(trailer, sizeof(trailer), "# end samples=%u dropped=%u\n", (unsigned)out->samples,
                     (unsigned)out->dropped);
    if (n > 0 && (size_t)n < sizeof(trailer) && s_expect_len + (size_t)n <= sizeof(s_expect)) {
        memcpy(s_expect + s_expect_len, trailer, (size_t)n);
        s_expect_len += (size_t)n;
    }
    return ok;
}

/* Reads trip id back the way obd.trip_read does, chunk by chunk. */
static size_t read_all(uint32_t id, size_t chunk, uint8_t *out, size_t cap)
{
    size_t at = 0;
    for (;;) {
        uint32_t total = 0;
        int n = muse_log_store_read(s_dir, id, (uint32_t)at, out + at, chunk < cap - at ? chunk : cap - at, &total);
        if (n < 0) {
            return (size_t)-1;
        }
        at += (size_t)n;
        if (at == total) {
            return at;
        }
        if (n == 0) {
            return (size_t)-1;
        }
    }
}

static void test_store(void)
{
    /* Empty: no manifest, no trips. */
    CHECK(muse_log_store_list(s_dir, NULL, 0) == 0);
    CHECK(muse_log_store_max_id(s_dir) == 0);
    CHECK(!muse_log_store_find(s_dir, 1, NULL));
    uint32_t total = 0;
    uint8_t buf[8192];
    CHECK(muse_log_store_read(s_dir, 1, 0, buf, sizeof(buf), &total) == MUSE_LOG_READ_UNKNOWN);

    /* A trip, round trip: every byte back, in chunks of every size. */
    muse_log_trip_t t1;
    CHECK(write_trip(1, 500, 1u << 20, &t1));
    CHECK(t1.id == 1 && t1.samples == 500 && t1.dropped == 0);
    CHECK(t1.ended == 1729703600 && t1.started == -1);
    CHECK(t1.bytes == s_expect_len);
    CHECK((long)t1.bytes == file_size("T0000001.CSV"));
    CHECK(t1.crc32 == muse_log_crc32(0, s_expect, s_expect_len));
    static uint8_t back[1 << 16];
    static const size_t CHUNKS[] = { 6144, 1, 7, 255, 256, 4096 };
    for (size_t c = 0; c < sizeof(CHUNKS) / sizeof(CHUNKS[0]); c++) {
        memset(back, 0, sizeof(back));
        size_t n = read_all(1, CHUNKS[c], back, sizeof(back));
        CHECK(n == s_expect_len && !memcmp(back, s_expect, n));
        CHECK(muse_log_crc32(0, back, n) == t1.crc32);
    }
    char *text = (char *)back;
    text[s_expect_len] = '\0';
    CHECK(!strncmp(text, "# muse obd trip log v1\n", 23));
    CHECK(strstr(text, "\n0,800,,,,,,,,,,\n") != NULL);
    CHECK(strstr(text, "\n# end samples=500 dropped=0\n") != NULL);

    /* Reads at the start, the middle, the exact end and past it. */
    int n = muse_log_store_read(s_dir, 1, 0, buf, 6144, &total);
    CHECK(total == t1.bytes && n == (int)(t1.bytes < 6144 ? t1.bytes : 6144));
    CHECK(!memcmp(buf, s_expect, (size_t)n));
    uint32_t mid = t1.bytes / 2;
    n = muse_log_store_read(s_dir, 1, mid, buf, 100, &total);
    CHECK(n == 100 && !memcmp(buf, s_expect + mid, 100));
    n = muse_log_store_read(s_dir, 1, t1.bytes - 10, buf, 6144, &total);
    CHECK(n == 10 && !memcmp(buf, s_expect + t1.bytes - 10, 10));
    CHECK(muse_log_store_read(s_dir, 1, t1.bytes, buf, 6144, &total) == 0 && total == t1.bytes);
    CHECK(muse_log_store_read(s_dir, 1, t1.bytes + 1, buf, 6144, &total) == MUSE_LOG_READ_PAST_END);
    CHECK(muse_log_store_read(s_dir, 1, UINT32_MAX, buf, 6144, &total) == MUSE_LOG_READ_PAST_END);
    CHECK(muse_log_store_read(s_dir, 99, 0, buf, 6144, &total) == MUSE_LOG_READ_UNKNOWN);

    /* Three trips; deleting the middle one removes exactly it. */
    muse_log_trip_t t2, t3;
    CHECK(write_trip(2, 40, 1u << 20, &t2));
    size_t t2_len = s_expect_len;
    CHECK(write_trip(3, 1000, 1u << 20, &t3));
    static char t3_bytes[1 << 16];
    size_t t3_len = s_expect_len;
    memcpy(t3_bytes, s_expect, t3_len);
    CHECK(t2.bytes == t2_len);
    muse_log_trip_t list[8];
    CHECK(muse_log_store_list(s_dir, list, 8) == 3);
    CHECK(list[0].id == 1 && list[1].id == 2 && list[2].id == 3);
    CHECK(muse_log_store_list(s_dir, list, 1) == 3 && list[0].id == 1);   /* count past max */
    CHECK(muse_log_store_max_id(s_dir) == 3);

    CHECK(muse_log_store_delete(s_dir, 2));
    CHECK(!exists("T0000002.CSV"));
    CHECK(exists("T0000001.CSV") && exists("T0000003.CSV"));
    CHECK(!exists(MUSE_LOG_MANIFEST_TMP));
    CHECK(muse_log_store_list(s_dir, list, 8) == 2);
    CHECK(list[0].id == 1 && list[1].id == 3);
    CHECK(list[0].crc32 == t1.crc32 && list[1].crc32 == t3.crc32 && list[1].bytes == t3.bytes);
    CHECK(read_all(3, 6144, back, sizeof(back)) == t3_len && !memcmp(back, t3_bytes, t3_len));
    CHECK(muse_log_store_read(s_dir, 2, 0, buf, 6144, &total) == MUSE_LOG_READ_UNKNOWN);
    CHECK(!muse_log_store_delete(s_dir, 2));    /* only once */
    CHECK(!muse_log_store_delete(s_dir, 77));   /* never there */
    CHECK(muse_log_store_list(s_dir, list, 8) == 2);

    /* A simulated reboot: nothing in memory, only the directory and the
     * persisted counter (next = 4). The manifest reads the same, recovery
     * finds nothing to do, and the next id continues: 2 is never reused. */
    uint32_t stored_next = 4;
    CHECK(muse_log_store_recover(s_dir) == 0);
    CHECK(muse_log_store_list(s_dir, list, 8) == 2 && list[0].id == 1 && list[1].id == 3);
    uint32_t next = muse_log_next_id(stored_next, muse_log_store_max_id(s_dir));
    CHECK(next == 4);
    /* A counter behind the store (restored, or lost) still never reuses. */
    CHECK(muse_log_next_id(2, muse_log_store_max_id(s_dir)) == 4);
    CHECK(muse_log_next_id(0, 0) == 1);
    CHECK(muse_log_next_id(0, 3) == 4);
    CHECK(muse_log_next_id(10, 3) == 10);
    muse_log_trip_t t4;
    CHECK(write_trip(next, 10, 1u << 20, &t4));
    CHECK(t4.id == 4 && muse_log_store_max_id(s_dir) == 4);

    /* Reset mid-trip: the file is there, its manifest line isn't. Recovery
     * adds it, with its rows counted and its times unknown. */
    {
        static char block[BLOCK];
        muse_log_meta_t m = meta(5);
        muse_log_writer_t w;
        CHECK(muse_log_trip_open(&w, s_dir, &m, block, sizeof(block), 1u << 20));
        for (int i = 0; i < 30; i++) {
            char row[MUSE_LOG_ROW_MAX];
            muse_log_sample_t s = { .ms = (uint32_t)i };
            muse_log_trip_row(&w, row, muse_log_format_row(row, sizeof(row), &s));
        }
        muse_log_trip_flush(&w);
        fclose(w.f);   /* power lost: no trailer, no manifest line */
    }
    CHECK(!muse_log_store_find(s_dir, 5, NULL));
    CHECK(muse_log_store_recover(s_dir) == 1);
    muse_log_trip_t t5;
    CHECK(muse_log_store_find(s_dir, 5, &t5));
    CHECK(t5.samples == 30 && t5.dropped == 0 && t5.started < 0 && t5.ended < 0);
    CHECK((long)t5.bytes == file_size("T0000005.CSV"));
    CHECK(muse_log_store_recover(s_dir) == 0);   /* once */

    /* Reset mid-delete, after the old manifest went and before the rename:
     * the new one is put in place. */
    char a[128], b[128];
    path_of(a, sizeof(a), MUSE_LOG_MANIFEST);
    path_of(b, sizeof(b), MUSE_LOG_MANIFEST_TMP);
    CHECK(rename(a, b) == 0);
    CHECK(muse_log_store_list(s_dir, NULL, 0) == 0);
    CHECK(muse_log_store_recover(s_dir) == 0);
    CHECK(muse_log_store_list(s_dir, list, 8) == 4);
    CHECK(!exists(MUSE_LOG_MANIFEST_TMP));
    /* And before the old one went: the old one stands, the new one goes. */
    FILE *f = fopen(b, "w");
    fputs("half written", f);
    fclose(f);
    CHECK(muse_log_store_recover(s_dir) == 0);
    CHECK(!exists(MUSE_LOG_MANIFEST_TMP));
    CHECK(muse_log_store_list(s_dir, list, 8) == 4);

    /* A line cut off by a reset during the append is skipped, not fatal. */
    f = fopen(a, "a");
    fputs("{\"id\":9,\"bytes\":1", f);
    fclose(f);
    CHECK(muse_log_store_list(s_dir, list, 8) == 4);
}

static void test_full(void)
{
    /* The store fills: the file stops at its limit, the rest of the rows
     * are dropped and counted, and the trip still closes with its trailer
     * and its manifest line. Nothing older is touched. */
    muse_log_trip_t before[8];
    int had = muse_log_store_list(s_dir, before, 8);
    muse_log_trip_t t;
    CHECK(write_trip(6, 400, 2048, &t));
    CHECK(t.dropped > 0 && t.samples + t.dropped == 400);
    CHECK(t.bytes <= 2048 + 64);
    CHECK((long)t.bytes == file_size("T0000006.CSV"));
    uint8_t back[4096];
    size_t n = read_all(6, 6144, back, sizeof(back));
    CHECK(n == t.bytes && muse_log_crc32(0, back, n) == t.crc32);
    back[n < sizeof(back) ? n : sizeof(back) - 1] = 0;
    CHECK(strstr((char *)back, "# end samples=") != NULL);
    muse_log_trip_t after[8];
    CHECK(muse_log_store_list(s_dir, after, 8) == had + 1);
    for (int i = 0; i < had; i++) {
        CHECK(after[i].id == before[i].id && after[i].crc32 == before[i].crc32);
    }

    /* Not even room for the header: the trip doesn't open, no file is left. */
    static char block[BLOCK];
    muse_log_meta_t m = meta(7);
    muse_log_writer_t w;
    CHECK(!muse_log_trip_open(&w, s_dir, &m, block, sizeof(block), 100));
    CHECK(!exists("T0000007.CSV"));
    CHECK(!muse_log_trip_open(&w, s_dir, &(muse_log_meta_t){ .id = 0 }, block, sizeof(block), 1u << 20));
}

static void cleanup(void)
{
    DIR *d = opendir(s_dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            char p[128];
            path_of(p, sizeof(p), e->d_name);
            remove(p);
        }
    }
    if (d) {
        closedir(d);
    }
    rmdir(s_dir);
}

int main(void)
{
    test_row();
    test_header();
    test_manifest_line();
    test_crc_base64_names();

    /* A short path: a store's directory has MUSE_LOG_DIR_MAX. */
    snprintf(s_dir, sizeof(s_dir), "/tmp/obdlogXXXXXX");
    if (!mkdtemp(s_dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_store();
    test_full();
    cleanup();

    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_obd_log\n");
    return 0;
}
