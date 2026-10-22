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
 * Drives the avatar's warm-up thumbs-up (pose.warmup), which tools/muse/anim.c
 * doesn't, through the real renderer, and checks the grid it draws: the
 * thumbs-up is whole and sits beside the head on one raised arm, the face is
 * pleased (happy closed eyes, a grin), it ranks below the coolant warning and
 * VTEC, it shows only while idle, and nothing new reaches the grid's edges.
 * The renderer is included rather than linked so the checks can read its
 * 64x64 grid; it is not changed.
 *
 *   muse_pixel_warmup [DIR]   with DIR, also writes the frames as PPMs:
 *                             DIR/points/<progress>.ppm and DIR/anim/NNN.ppm
 */
#include "../avatar/muse_pixel.c"

#include <stdio.h>
#include <sys/stat.h>

#define S 5
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */

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

static void render_as(muse_mode_t mode, float t, float warmup, float vtec, float coolant)
{
    muse_pose_t p = { .mode = mode, .t = t, .mode_t = t, .warmup = warmup, .vtec = vtec, .coolant = coolant };
    muse_pixel_render(&p);
}

static void render(float t, float warmup)
{
    render_as(MUSE_MODE_IDLE, t, warmup, 0, 0);
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

/* The highest face-panel cell: where the head's face starts. */
static int face_top(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t c = s_fb[y * W + x];
            if (c == C_SKIN || c == C_SKINL || c == C_SKIND) {
                return y;
            }
        }
    }
    return H;
}

/* The thumbs-up as drawn: outline, highlight, light fur and creases. Every
 * one of its cells must match exactly; finds its top-left corner. */
static const char *const THUMB_ROWS[] = {
    "..##....",
    ".#hh#...",
    ".#hm#...",
    ".#mm#...",
    "##mm####",
    "#hmmmmm#",
    "#mddddm#",
    "#hmmmmm#",
    "#mddddm#",
    ".######.",
};

static int find_thumbs(int *fx, int *fy)
{
    int found = 0;
    for (int y = 0; y + 10 <= H; y++) {
        for (int x = 0; x + 8 <= W; x++) {
            bool ok = true;
            for (int r = 0; r < 10 && ok; r++) {
                for (int c = 0; c < 8 && ok; c++) {
                    char ch = THUMB_ROWS[r][c];
                    uint8_t want = ch == '#' ? C_OUT : ch == 'h' ? C_BH : ch == 'm' ? C_BL : ch == 'd' ? C_BM : 0xff;
                    ok = want == 0xff || s_fb[(y + r) * W + x + c] == want;
                }
            }
            if (ok) {
                *fx = x;
                *fy = y;
                found++;
            }
        }
    }
    return found;
}

/* The thumbs-up is there, whole, beside the head on the right, raised, thumb
 * clear above the fist (nothing but background or glow just over its tip). */
static void check_thumbs(const char *when)
{
    int x = 0, y = 0;
    int n = find_thumbs(&x, &y);
    CHECK(n == 1, "%s: %d thumbs-ups", when, n);
    if (n != 1) {
        return;
    }
    CHECK(x >= 44, "%s: the thumbs-up at x %d isn't beside the head on the right", when, x);
    CHECK(y + 9 < 30, "%s: the fist (rows %d..%d) isn't raised beside the head", when, y, y + 9);
    CHECK(y <= face_top() + 2, "%s: the thumb tip (row %d) is below the top of the face (%d)", when, y, face_top());
    for (int c = 2; c <= 3; c++) {
        uint8_t above = s_fb[(y - 1) * W + x + c];
        CHECK(!body_colour(above), "%s: the thumb tip runs into the body", when);
    }
    /* Clear of the head: no fur or face in the two columns left of the fist
     * and thumb, so the thumbs-up reads on its own. */
    int touching = 0;
    for (int r = 0; r < 10; r++) {
        touching += body_colour(s_fb[(y + r) * W + x - 1]) + body_colour(s_fb[(y + r) * W + x - 2]);
    }
    CHECK(touching <= 2, "%s: the thumb runs into the head (%d cells)", when, touching);   /* a fur tuft is fine */
}

/* Pleased: happy closed eyes (no shine), a grin with the tongue showing, no
 * worried brows. */
static void check_pleased(const char *when)
{
    CHECK(count(C_SHINE, 0, 15, W - 1, 40) == 0, "%s: eyes not closed happy", when);
    CHECK(count(C_IRIS, 0, 15, W - 1, 40) >= 8, "%s: no eyes", when);
    CHECK(count(C_TONGUE, 0, 15, W - 1, 45) >= 1, "%s: no grin", when);
    CHECK(count(C_MOUTH, 0, 15, W - 1, 45) >= 6, "%s: mouth too small for a grin", when);
    CHECK(count(C_BROW, 0, 0, W - 1, H - 1) == 0, "%s: brows on a pleased face", when);
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

/* Only the right arm goes up: the left half above the shoulders holds no
 * more body than idle's at the same moment. */
static int left_high_body(void)
{
    int n = 0;
    for (int y = 0; y < 30; y++) {
        for (int x = 0; x < 14; x++) {
            n += body_colour(s_fb[y * W + x]);
        }
    }
    return n;
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
    static const float POINTS[] = { 0.0f, 0.02f, 0.06f, 0.1f, 0.16f, 0.2f, 0.3f, 0.5f, 0.7f, 0.8f, 0.9f, 0.97f };
    for (size_t i = 0; i < sizeof(POINTS) / sizeof(POINTS[0]); i++) {
        float v = POINTS[i];
        t = warm_up(100.0f + 10.0f * (float)i);
        render(t, 0);
        int idle_left = left_high_body();
        t = warm_up(100.0f + 10.0f * (float)i);
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "warmup=%.2f", (double)v);
        check_edges(when);
        if (v >= 0.16f && v <= 0.8f) {
            check_thumbs(when);
            check_pleased(when);
            CHECK(left_high_body() <= idle_left, "%s: the left arm is up too", when);
        }
        if (v == 0.0f || v >= 0.97f) {
            int x, y;
            CHECK(find_thumbs(&x, &y) == 0, "%s: thumbs-up showing", when);
        }
        CHECK(count(C_VTEC, 0, 0, W - 1, H - 1) + count(C_HOT, 0, 0, W - 1, H - 1) == 0,
              "%s: a car banner on the thumbs-up", when);
        if (dir) {
            snprintf(path, sizeof(path), "%s/points/%03d.ppm", dir, (int)(v * 100 + 0.5f));
            save(path);
        }
    }

    /* Priority: with VTEC or the coolant warning on too, the frame is theirs alone. */
    static const float BOTH[] = { 0.02f, 0.3f, 0.6f, 0.9f };
    for (size_t i = 0; i < sizeof(BOTH) / sizeof(BOTH[0]); i++) {
        for (int k = 0; k < 2; k++) {
            float vtec = k ? 0 : 0.5f, coolant = k ? 0.5f : 0;
            t = warm_up(300.0f + 10.0f * (float)i);
            render_as(MUSE_MODE_IDLE, t, 0, vtec, coolant);
            memcpy(s_grid, s_fb, sizeof(s_grid));
            t = warm_up(300.0f + 10.0f * (float)i);
            render_as(MUSE_MODE_IDLE, t, BOTH[i], vtec, coolant);
            CHECK(memcmp(s_grid, s_fb, sizeof(s_grid)) == 0, "warmup=%.2f under %s: the thumbs-up shows through",
                  (double)BOTH[i], k ? "coolant" : "vtec");
            int x, y;
            CHECK(find_thumbs(&x, &y) == 0, "warmup=%.2f under %s: thumbs-up drawn", (double)BOTH[i],
                  k ? "coolant" : "vtec");
        }
    }
    /* Idle's only: other modes keep their own look. */
    for (unsigned m = 0; m < MUSE_MODE_COUNT; m++) {
        if (m == MUSE_MODE_IDLE) {
            continue;
        }
        render_as((muse_mode_t)m, 400.0f, 0, 0, 0);
        memcpy(s_grid, s_fb, sizeof(s_grid));
        render_as((muse_mode_t)m, 400.0f, 0.5f, 0, 0);
        CHECK(memcmp(s_grid, s_fb, sizeof(s_grid)) == 0, "mode %d: warmup shows outside idle", m);
    }

    /* The whole reaction at the device frame rate, with a beat of idle either side. */
    t = warm_up(500.0f);
    int frames = (int)((MUSE_WARMUP_S + 1.0f) / DT + 0.5f);
    int thumbs = 0, moves = 0, glints = 0, prev_y = -1;
    for (int i = 0; i < frames; i++, t += DT) {
        float rt = i * DT - 0.5f;
        float v = rt > 0 && rt < MUSE_WARMUP_S ? rt / MUSE_WARMUP_S : 0;
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "frame %d (%.3f)", i, (double)v);
        check_edges(when);
        int x = 0, y = 0;
        bool shown = find_thumbs(&x, &y) == 1;
        thumbs += shown;
        if (v >= 0.16f && v <= 0.8f) {
            check_thumbs(when);
            check_pleased(when);
            moves += prev_y >= 0 && shown && y != prev_y;
            glints += shown && count(C_WHITE, x, y - 6, x + 9, y) > 0;
        }
        prev_y = shown ? y : -1;
        if (v == 0) {
            CHECK(!shown, "%s: thumbs-up outside the reaction", when);
        }
        if (dir) {
            snprintf(path, sizeof(path), "%s/anim/%03d.ppm", dir, i);
            save(path);
        }
    }
    CHECK(thumbs >= (int)(0.6f * MUSE_WARMUP_S / DT), "the thumbs-up shows for only %d frames", thumbs);
    CHECK(moves >= 2, "the thumbs-up is frozen in place (%d moves)", moves);
    CHECK(glints >= 2, "no glint off the thumb (%d frames)", glints);

    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_pixel_warmup\n");
    return 0;
}
