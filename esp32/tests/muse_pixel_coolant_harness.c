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
 * Drives the avatar's coolant warning (pose.coolant), which tools/muse/anim.c
 * doesn't, through the real renderer, and checks the grid it draws: the HOT!
 * banner is whole and readable, it and the steam clear the head, the face is
 * worried (brows raised in the middle, a wobbly mouth, no grin), sweat and
 * steam show, the warning outranks VTEC, and nothing new reaches the grid's
 * edges. The renderer is included rather than linked so the checks can read
 * its 64x64 grid; it is not changed.
 *
 *   muse_pixel_coolant [DIR]   with DIR, also writes the frames as PPMs:
 *                              DIR/points/<progress>.ppm and DIR/anim/NNN.ppm
 */
#include "../avatar/muse_pixel.c"

#include <stdio.h>
#include <sys/stat.h>

#define S 5
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */
#define BANNER_W 19
#define BANNER_X (W / 2 - BANNER_W / 2)

static uint16_t s_out[N * N];
static uint8_t s_grid[W * H];
static int s_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        s_fail++; \
    } \
} while (0)

static void save(const char *path)
{
    muse_pixel_scale(s_out, N, 0, N - 1, 0, N - 1);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fprintf(f, "P6 %d %d 255\n", N, N);
    for (int i = 0; i < N * N; i++) {
        uint16_t c = s_out[i];
        uint8_t rgb[3] = { (uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                           (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void render_as(muse_mode_t mode, float t, float coolant, float vtec)
{
    muse_pose_t p = { .mode = mode, .t = t, .mode_t = t, .coolant = coolant, .vtec = vtec };
    muse_pixel_render(&p);
}

static void render(float t, float coolant)
{
    render_as(MUSE_MODE_IDLE, t, coolant, 0);
}

/* Settle the palette blend and the eyes, as the device has long since. */
static float warm_up(float t)
{
    for (int i = 0; i < 40; i++, t += DT) {
        render(t, 0);
    }
    return t;
}

static bool body_colour(uint8_t c)
{
    return c == C_OUT || c == C_OUT2 || c == C_BD || c == C_BM || c == C_BL || c == C_BH || c == C_RIM
        || c == C_SKIND || c == C_SKIN || c == C_SKINL;
}

static int count(uint8_t c, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            n += s_fb[y * W + x] == c;
        }
    }
    return n;
}

/* The highest body cell: the top of the head. */
static int head_top(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (body_colour(s_fb[y * W + x])) {
                return y;
            }
        }
    }
    return H;
}

/* The banner as it should be at full strength, top row at y (white while it
 * lands, then orange-red). */
static void check_banner_whole(int y, const char *when)
{
    static const char *const ROWS[] = {
        "#...#..###..#####.#",
        "#...#.#...#...#...#",
        "#####.#...#...#...#",
        "#...#.#...#...#....",
        "#...#..###....#...#",
    };
    int wrong = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; ROWS[r][c]; c++) {
            uint8_t px_c = s_fb[(y + r) * W + BANNER_X + c];
            bool lit = px_c == C_HOT || px_c == C_WHITE;
            wrong += lit != (ROWS[r][c] == '#');
        }
    }
    CHECK(wrong == 0, "%s: %d banner pixels wrong", when, wrong);
    CHECK(BANNER_X >= 0 && BANNER_X + BANNER_W < W && y >= 0 && y + 5 < H, "%s: banner box off the grid", when);
    /* Readable: nothing else inside its box but its own shadow. */
    int clutter = 0;
    for (int r = 0; r < 6; r++) {
        for (int c = 0; c <= BANNER_W; c++) {
            uint8_t px_c = s_fb[(y + r) * W + BANNER_X + c];
            clutter += px_c == C_STEAM || px_c == C_STEAMD || px_c == C_TEAR || body_colour(px_c);
        }
    }
    CHECK(clutter == 0, "%s: %d cells of steam/sweat/avatar inside the banner", when, clutter);
}

/* Worried brows: on each side, the brow rises toward the middle of the face. */
static void check_worried(const char *when)
{
    int lo[2] = { 99, 99 }, lo_x[2] = { 0, 0 }, hi[2] = { -1, -1 }, hi_x[2] = { 0, 0 }, n[2] = { 0, 0 };
    for (int y = 10; y < 40; y++) {
        for (int x = 0; x < W; x++) {
            if (s_fb[y * W + x] != C_BROW) {
                continue;
            }
            int s = x >= 32;
            n[s]++;
            if (y < lo[s]) {
                lo[s] = y;
                lo_x[s] = x;
            }
            if (y >= hi[s]) {
                hi[s] = y;
                hi_x[s] = x;
            }
        }
    }
    CHECK(n[0] >= 3 && n[1] >= 3, "%s: brows missing (%d, %d)", when, n[0], n[1]);
    CHECK(hi[0] > lo[0] && hi[1] > lo[1], "%s: brows not slanted", when);
    CHECK(lo_x[0] > hi_x[0] && lo_x[1] < hi_x[1], "%s: brows not raised in the middle", when);
    /* No grin, no tongue: an uneasy mouth, at least 6 cells of it. */
    CHECK(count(C_TONGUE, 0, 15, W - 1, 45) == 0, "%s: mouth open like a grin", when);
    CHECK(count(C_MOUTH, 0, 15, W - 1, 45) >= 6, "%s: no wobbly mouth", when);
    /* Flushed, not grinning: no happy ^^ eyes (they have no shine). */
    CHECK(count(C_SHINE, 0, 15, W - 1, 40) >= 2, "%s: eyes not open", when);
}

/* Edge cells may hold only what idle already puts there: glow and background. */
static bool edge_ok(uint8_t c)
{
    return c == C_BG || c == C_AURA1 || c == C_AURA2 || c == C_SHADOW;
}

static void check_edges(const char *when)
{
    int bad = 0;
    for (int i = 0; i < W; i++) {
        bad += !edge_ok(s_fb[i]) + !edge_ok(s_fb[(H - 1) * W + i]);
        bad += !edge_ok(s_fb[i * W]) + !edge_ok(s_fb[i * W + W - 1]);
    }
    CHECK(bad == 0, "%s: %d drawn cells on the grid's edge (possibly clipped)", when, bad);
}

/* Over the reaction, the banner rows and the head stay apart. */
static void check_clear(const char *when)
{
    int body = 0;
    for (int y = 0; y <= 7; y++) {
        for (int x = 0; x < W; x++) {
            body += body_colour(s_fb[y * W + x]);
        }
    }
    CHECK(body == 0, "%s: avatar reaches the banner rows (%d cells)", when, body);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    char path[512];
    if (dir) {
        snprintf(path, sizeof(path), "%s/points", dir);
        mkdir(path, 0755);
        snprintf(path, sizeof(path), "%s/anim", dir);
        mkdir(path, 0755);
    }
    muse_pixel_set_size(N);

    float t = warm_up(10.0f);
    render(t, 0);
    check_edges("idle");

    /* Forced points through the reaction. */
    static const float POINTS[] = { 0.0f, 0.02f, 0.05f, 0.08f, 0.12f, 0.3f, 0.5f, 0.7f, 0.85f, 0.9f, 0.97f };
    for (size_t i = 0; i < sizeof(POINTS) / sizeof(POINTS[0]); i++) {
        float v = POINTS[i];
        t = warm_up(100.0f + 10.0f * (float)i);
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "coolant=%.2f", (double)v);
        check_edges(when);
        if (v > 0) {
            check_clear(when);
        }
        if (v >= 0.12f && v <= 0.7f) {
            check_banner_whole(2, when);
            check_worried(when);
            CHECK(count(C_VTEC, 0, 0, W - 1, H - 1) == 0, "%s: VTEC's red in the HOT! banner", when);
        }
        if (v == 0.0f || v >= 0.97f) {
            CHECK(count(C_HOT, 0, 0, W - 1, H - 1) == 0, "%s: banner showing", when);
            CHECK(count(C_STEAM, 0, 0, W - 1, H - 1) + count(C_STEAMD, 0, 0, W - 1, H - 1) == 0,
                  "%s: steam showing", when);
            CHECK(count(C_TEAR, 0, 0, W - 1, H - 1) == 0, "%s: sweat showing", when);
        }
        if (dir) {
            snprintf(path, sizeof(path), "%s/points/%03d.ppm", dir, (int)(v * 100 + 0.5f));
            save(path);
        }
    }

    /* Priority: with VTEC on too, the frame is the coolant warning's alone. */
    static const float BOTH[] = { 0.02f, 0.3f, 0.6f, 0.9f };
    for (size_t i = 0; i < sizeof(BOTH) / sizeof(BOTH[0]); i++) {
        t = warm_up(300.0f + 10.0f * (float)i);
        render_as(MUSE_MODE_IDLE, t, BOTH[i], 0);
        memcpy(s_grid, s_fb, sizeof(s_grid));
        t = warm_up(300.0f + 10.0f * (float)i);
        render_as(MUSE_MODE_IDLE, t, BOTH[i], 0.5f);
        CHECK(memcmp(s_grid, s_fb, sizeof(s_grid)) == 0, "coolant=%.2f vtec=0.5: VTEC shows through",
              (double)BOTH[i]);
        CHECK(count(C_VTEC, 0, 0, W - 1, H - 1) == 0, "coolant=%.2f vtec=0.5: VTEC banner", (double)BOTH[i]);
    }
    /* Idle's only: other modes keep their own look. */
    for (unsigned m = 0; m < MUSE_MODE_COUNT; m++) {
        if (m == MUSE_MODE_IDLE) {
            continue;
        }
        render_as((muse_mode_t)m, 400.0f, 0, 0);
        memcpy(s_grid, s_fb, sizeof(s_grid));
        render_as((muse_mode_t)m, 400.0f, 0.5f, 0);
        CHECK(memcmp(s_grid, s_fb, sizeof(s_grid)) == 0, "mode %d: coolant shows outside idle", m);
    }

    /* The whole reaction at the device frame rate, with a beat of idle either side. */
    t = warm_up(500.0f);
    int frames = (int)((MUSE_COOLANT_S + 1.0f) / DT + 0.5f);
    int sweat_l = 0, sweat_r = 0, steam_side = 0, steam_mid = 0, mouth_flips = 0, darts = 0;
    int lowest_top = H, prev_dart = -1;
    unsigned prev_mouth = 0;
    bool have_mouth = false;
    for (int i = 0; i < frames; i++, t += DT) {
        float rt = i * DT - 0.5f;
        float v = rt > 0 && rt < MUSE_COOLANT_S ? rt / MUSE_COOLANT_S : 0;
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "frame %d (%.3f)", i, (double)v);
        check_edges(when);
        if (v > 0) {
            check_clear(when);
            int top = head_top();
            lowest_top = top < lowest_top ? top : lowest_top;
            /* Steam: none of it reaches into the head itself. */
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    uint8_t c = s_fb[y * W + x];
                    if (c == C_STEAM || c == C_STEAMD) {
                        CHECK(y < top + 2, "%s: steam inside the head at %d,%d", when, x, y);
                        if (x < BANNER_X - 1 || x > BANNER_X + BANNER_W) {
                            steam_side++;
                        } else {
                            steam_mid++;
                        }
                    }
                }
            }
            sweat_l += count(C_TEAR, 0, 0, 31, H - 1) > 0;
            sweat_r += count(C_TEAR, 32, 0, W - 1, H - 1) > 0;
        }
        if (v >= 0.12f && v <= 0.7f) {
            check_banner_whole(2, when);
            check_worried(when);
            /* The wobble and the darting eyes move. */
            unsigned m = 0;
            for (int x = 0; x < W; x++) {
                for (int y = 25; y < 45; y++) {
                    m = m * 31u + (s_fb[y * W + x] == C_MOUTH);
                }
            }
            mouth_flips += have_mouth && m != prev_mouth;
            prev_mouth = m;
            have_mouth = true;
            int d = 0;
            for (int x = 0; x < W; x++) {
                d += s_fb[30 * W + x] == C_SHINE ? x : 0;
                d += s_fb[29 * W + x] == C_SHINE ? x : 0;
                d += s_fb[28 * W + x] == C_SHINE ? x : 0;
            }
            darts += prev_dart >= 0 && d != prev_dart;
            prev_dart = d;
        }
        if (dir) {
            snprintf(path, sizeof(path), "%s/anim/%03d.ppm", dir, i);
            save(path);
        }
    }
    CHECK(sweat_l >= 20 && sweat_r >= 20, "too little sweat (%d left, %d right frames)", sweat_l, sweat_r);
    CHECK(steam_side >= 100 && steam_mid >= 20, "too little steam (%d beside, %d below the banner)", steam_side,
          steam_mid);
    CHECK(mouth_flips >= 5, "the mouth doesn't wobble (%d changes)", mouth_flips);
    CHECK(darts >= 2, "the eyes don't dart (%d changes)", darts);
    CHECK(lowest_top >= 9, "the head rises to row %d, under the steam", lowest_top);

    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_pixel_coolant\n");
    return 0;
}
