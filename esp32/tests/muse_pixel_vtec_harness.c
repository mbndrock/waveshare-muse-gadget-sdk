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
 * Drives the avatar's VTEC reaction (pose.vtec), which tools/muse/anim.c
 * doesn't, through the real renderer, and checks the grid it draws: the
 * banner is whole and readable, it clears the head, the avatar buzzes a
 * pixel side to side, the face is excited, and nothing new reaches the
 * grid's edges. The renderer is included rather than linked so the checks
 * can read its 64x64 grid; it is not changed.
 *
 *   muse_pixel_vtec [DIR]   with DIR, also writes the frames as PPMs:
 *                           DIR/points/<progress>.ppm and DIR/anim/NNN.ppm
 */
#include "../avatar/muse_pixel.c"

#include <stdio.h>
#include <sys/stat.h>

#define S 5
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */

static uint16_t s_out[N * N];
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

static void render(float t, float vtec)
{
    muse_pose_t p = { .mode = MUSE_MODE_IDLE, .t = t, .mode_t = t, .vtec = vtec };
    muse_pixel_render(&p);
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

/* The body's centre column: the mean x of its cells below the arms. A bounce
 * up and down doesn't move it; a shift sideways moves it one for one. */
static float body_centre(void)
{
    long sum = 0, n = 0;
    for (int y = 36; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (body_colour(s_fb[y * W + x])) {
                sum += x;
                n++;
            }
        }
    }
    return n ? (float)sum / (float)n : -1;
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

/* The banner as it should be at full strength, top row at y (white while it
 * lands, then red). */
static void check_banner_whole(int y, const char *when)
{
    static const char *const ROWS[] = {
        "#...#.#####.####..###.#",
        "#...#...#...#....#....#",
        "#...#...#...###..#....#",
        ".#.#....#...#....#.....",
        "..#.....#...####..###.#",
    };
    int x0 = W / 2 - 11;
    int wrong = 0;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; ROWS[r][c]; c++) {
            uint8_t px_c = s_fb[(y + r) * W + x0 + c];
            bool lit = px_c == C_VTEC || px_c == C_WHITE;
            wrong += lit != (ROWS[r][c] == '#');
        }
    }
    CHECK(wrong == 0, "%s: %d banner pixels wrong", when, wrong);
    CHECK(x0 >= 0 && x0 + 23 < W && y >= 0 && y + 5 < H, "%s: banner box off the grid", when);
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

    /* Idle's own edges, and how far its centre wanders as it breathes. */
    float t = warm_up(10.0f);
    render(t, 0);
    check_edges("idle");
    float idle_centre[2] = { 999, -1 };
    for (int i = 0; i < 75; i++, t += DT) {
        render(t, 0);
        float c = body_centre();
        idle_centre[0] = fminf(c, idle_centre[0]);
        idle_centre[1] = fmaxf(c, idle_centre[1]);
    }
    float idle_spread = idle_centre[1] - idle_centre[0];

    /* Forced points through the reaction. */
    static const float POINTS[] = { 0.0f, 0.02f, 0.05f, 0.08f, 0.12f, 0.3f, 0.5f, 0.7f, 0.85f, 0.9f, 0.97f };
    for (size_t i = 0; i < sizeof(POINTS) / sizeof(POINTS[0]); i++) {
        float v = POINTS[i];
        t = warm_up(100.0f + 10.0f * (float)i);
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "vtec=%.2f", (double)v);
        check_edges(when);
        if (v > 0) {
            /* The banner's rows hold no part of the avatar: it clears the head. */
            int body = 0;
            for (int y = 0; y <= 7; y++) {
                for (int x = 0; x < W; x++) {
                    body += body_colour(s_fb[y * W + x]);
                }
            }
            CHECK(body == 0, "%s: avatar reaches the banner rows (%d cells)", when, body);
        }
        if (v >= 0.12f && v <= 0.7f) {
            check_banner_whole(2, when);
            /* Excited: the big grin shows its tongue, the wide eyes their shine. */
            CHECK(count(C_TONGUE, 0, 20, W - 1, 40) > 0, "%s: no open grin", when);
            CHECK(count(C_SHINE, 0, 15, W - 1, 35) >= 2, "%s: eyes not wide", when);
        }
        if (v == 0.0f || v >= 0.97f) {
            CHECK(count(C_VTEC, 0, 0, W - 1, H - 1) == 0, "%s: banner showing", when);
        }
        if (dir) {
            snprintf(path, sizeof(path), "%s/points/%03d.ppm", dir, (int)(v * 100 + 0.5f));
            save(path);
        }
    }

    /* The whole reaction at the device frame rate, with a beat of idle either side. */
    t = warm_up(500.0f);
    float centre[2] = { 999, -1 }, prev = -1;
    int flips = 0, steady = 0;
    int frames = (int)((MUSE_VTEC_S + 1.0f) / DT + 0.5f);
    for (int i = 0; i < frames; i++, t += DT) {
        float rt = i * DT - 0.5f;
        float v = rt > 0 && rt < MUSE_VTEC_S ? rt / MUSE_VTEC_S : 0;
        render(t, v);
        char when[32];
        snprintf(when, sizeof(when), "frame %d", i);
        check_edges(when);
        if (v > 0.2f && v < 0.8f) {
            float c = body_centre();
            centre[0] = fminf(c, centre[0]);
            centre[1] = fmaxf(c, centre[1]);
            if (prev >= 0) {
                flips += fabsf(c - prev) > 0.5f;
                steady += fabsf(c - prev) <= 0.5f;
            }
            prev = c;
        }
        if (dir) {
            snprintf(path, sizeof(path), "%s/anim/%03d.ppm", dir, i);
            save(path);
        }
    }
    /* Buzzing: the body's centre jumps a pixel nearly every frame, and strays
     * no further than that beyond idle's own breathing. */
    CHECK(centre[1] - centre[0] >= 0.8f, "the avatar doesn't buzz (centre %.2f..%.2f)",
          (double)centre[0], (double)centre[1]);
    CHECK(centre[1] - centre[0] <= idle_spread + 1.3f, "the avatar moves too far (centre %.2f..%.2f, idle %.2f)",
          (double)centre[0], (double)centre[1], (double)idle_spread);
    CHECK(flips > 3 * steady, "the buzz doesn't alternate (%d moves, %d holds)", flips, steady);

    /* Held (the solenoid stays on): progress waits at MUSE_VTEC_HOLD while
     * the clock runs, for longer than the whole reaction. He must stay
     * alive, buzzing and changing frame to frame, with the banner up: a
     * frozen last frame would fail. */
    t = warm_up(800.0f);
    int held_frames = (int)(2.0f * MUSE_VTEC_S / DT + 0.5f), changed = 0;
    float held_centre[2] = { 999, -1 };
    flips = steady = 0;
    prev = -1;
    static uint8_t last[W * H];
    for (int i = 0; i < held_frames; i++, t += DT) {
        render(t, MUSE_VTEC_HOLD);
        char when[32];
        snprintf(when, sizeof(when), "held frame %d", i);
        check_edges(when);
        if (i % 25 == 0) {
            check_banner_whole(2, when);
        }
        if (i > 0) {
            changed += memcmp(last, s_fb, sizeof(last)) != 0;
        }
        memcpy(last, s_fb, sizeof(last));
        float c = body_centre();
        held_centre[0] = fminf(c, held_centre[0]);
        held_centre[1] = fmaxf(c, held_centre[1]);
        if (prev >= 0) {
            flips += fabsf(c - prev) > 0.5f;
            steady += fabsf(c - prev) <= 0.5f;
        }
        prev = c;
    }
    CHECK(changed > held_frames * 3 / 4, "held: only %d of %d frames change", changed, held_frames);
    CHECK(held_centre[1] - held_centre[0] >= 0.8f, "held: not buzzing");
    CHECK(flips > 3 * steady, "held: the buzz stops (%d moves, %d holds)", flips, steady);
    CHECK(MUSE_VTEC_HOLD > 0.16f && MUSE_VTEC_HOLD < 0.8f, "the hold isn't on the plateau");

    if (s_fail) {
        fprintf(stderr, "%d checks failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_pixel_vtec\n");
    return 0;
}
