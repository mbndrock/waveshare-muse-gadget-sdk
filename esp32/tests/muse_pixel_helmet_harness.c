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
 * Drives the avatar's racing helmet (pose.helmet) through the real renderer,
 * in every mode and reaction, and checks each frame against the same frame
 * without it (the renderer's state is saved and put back in between, so the
 * two differ only by the helmet): the face panel, eyes and mouth are all
 * still there, the arms stay in front, the reactions' overlays (banners,
 * stars, hearts, sweat, the thumbs-up) are untouched, the helmet adds only
 * its own colours and only over the head, the visor is above the eyes, the
 * stripe is the mode's accent, and thinking drops the headphones for it.
 * The renderer is included rather than linked so the checks can read its
 * 64x64 grid; it is not changed.
 *
 *   muse_pixel_helmet [DIR]   with DIR, also writes DIR/<pose>.ppm, helmet on
 */
#include "../avatar/muse_pixel.c"

#include <stdio.h>

#define S 5
#define N (MUSE_PX_W * S)
#define DT 0.04f        /* 40 ms, same as muse_ui */

static uint16_t s_out[N * N];
static uint8_t s_plain[W * H], s_plain_mask[W * H];
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
    muse_pixel_set_size(N);
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

/* Everything the renderer carries from frame to frame. */
typedef struct {
    eyes_t eyes;
    uint32_t rng;
    rgb_t scheme[5];
    bool scheme_init;
} carry_t;

static carry_t carried(void)
{
    carry_t c = { .eyes = s_eyes, .rng = s_rng, .scheme_init = s_scheme_init };
    memcpy(c.scheme, s_scheme, sizeof(s_scheme));
    return c;
}

static void restore(const carry_t *c)
{
    s_eyes = c->eyes;
    s_rng = c->rng;
    s_scheme_init = c->scheme_init;
    memcpy(s_scheme, c->scheme, sizeof(s_scheme));
}

static bool face_colour(uint8_t c)
{
    return c == C_SKIND || c == C_SKIN || c == C_SKINL || c == C_IRIS || c == C_SHINE || c == C_BROW
        || c == C_BLUSH || c == C_BLUSHD || c == C_MOUTH || c == C_TONGUE;
}

/* Drawn over the avatar by the reactions and modes: the helmet must leave them be. */
static bool overlay_colour(uint8_t c)
{
    return c == C_VTEC || c == C_VTECD || c == C_HOT || c == C_HOTD || c == C_STAR || c == C_STARD
        || c == C_HEART || c == C_TEAR || c == C_STEAM || c == C_STEAMD;
}

static bool helmet_colour(uint8_t c)
{
    return c == C_HELM || c == C_HELM_SH || c == C_VISOR || c == C_VISOR_HI || c == C_WHITE || c == C_OUT
        || c == C_ACC;
}

static int count(const uint8_t *fb, uint8_t c)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += fb[i] == c;
    }
    return n;
}

/* The lowest face-panel row of the plain frame, and the top row of its eyes. */
static int face_bottom(void)
{
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++) {
            if (s_plain_mask[y * W + x] == M_FACE) {
                return y;
            }
        }
    }
    return -1;
}

static int eye_top(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (s_fb[y * W + x] == C_IRIS) {
                return y;
            }
        }
    }
    return H;
}

typedef struct {
    const char *name;
    muse_pose_t pose;
    const char *ramp;   /* the reaction to run through 0..1, or NULL */
    float dur;
} scene_t;

static void set_ramp(muse_pose_t *p, const char *ramp, float v)
{
    if (!ramp) return;
    if (!strcmp(ramp, "dizzy")) p->dizzy = v;
    if (!strcmp(ramp, "sleepy")) p->sleepy = v;
    if (!strcmp(ramp, "waking")) p->waking = v;
    if (!strcmp(ramp, "tickle")) p->tickle = v;
    if (!strcmp(ramp, "vtec")) p->vtec = v;
    if (!strcmp(ramp, "coolant")) p->coolant = v;
    if (!strcmp(ramp, "warmup")) p->warmup = v;
}

/* One frame, plain then helmeted from the same state, and the checks. */
static void frame(const scene_t *sc, muse_pose_t p, float when)
{
    carry_t before = carried();
    p.helmet = false;
    muse_pixel_render(&p);
    memcpy(s_plain, s_fb, sizeof(s_fb));
    memcpy(s_plain_mask, s_mask, sizeof(s_mask));
    carry_t after_plain = carried();
    restore(&before);
    p.helmet = true;
    muse_pixel_render(&p);
    /* The helmet changes nothing the next frame starts from. */
    carry_t after_helmet = carried();
    CHECK(!memcmp(&after_plain, &after_helmet, sizeof(carry_t)), "%s t=%.2f: state differs", sc->name, when);

    int bottom = face_bottom(), changed = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int i = y * W + x;
            uint8_t a = s_plain[i], b = s_fb[i];
            if (face_colour(a) || overlay_colour(a) || s_plain_mask[i] == M_ARM || s_plain_mask[i] == M_FACE) {
                CHECK(a == b, "%s t=%.2f: (%d,%d) covered: %d -> %d", sc->name, when, x, y, a, b);
            }
            if (a != b) {
                changed++;
                CHECK(helmet_colour(b) || (p.mode == MUSE_MODE_THINKING && (a == C_HP || a == C_HP_IN || a == C_HP_EDGE)),
                      "%s t=%.2f: (%d,%d) drew colour %d", sc->name, when, x, y, b);
                CHECK(y <= bottom + 4, "%s t=%.2f: (%d,%d) below the jaw (face ends at %d)", sc->name, when, x, y,
                      bottom);
                CHECK(x >= 1 && x <= W - 2 && y >= 1, "%s t=%.2f: (%d,%d) at the edge", sc->name, when, x, y);
            }
        }
    }
    CHECK(changed > 150, "%s t=%.2f: only %d cells of helmet", sc->name, when, changed);
    CHECK(count(s_fb, C_HELM) > 60, "%s t=%.2f: %d shell cells", sc->name, when, count(s_fb, C_HELM));
    CHECK(count(s_fb, C_HELM_SH) > 0, "%s t=%.2f: no shade", sc->name, when);
    int visor = count(s_fb, C_VISOR), stripe = 0;
    for (int i = 0; i < W * H; i++) {
        stripe += s_fb[i] == C_ACC && s_plain[i] != C_ACC;   /* not the rings and waves it covers */
    }
    CHECK(visor > 20, "%s t=%.2f: %d visor cells", sc->name, when, visor);
    CHECK(count(s_fb, C_VISOR_HI) > 0, "%s t=%.2f: no glint on the visor", sc->name, when);
    /* Stars, hearts and banners in front may hide some of it, never all. */
    int overlays = 0;
    for (int i = 0; i < W * H; i++) {
        overlays += overlay_colour(s_fb[i]);
    }
    CHECK(stripe >= (overlays ? 1 : 4), "%s t=%.2f: %d stripe cells", sc->name, when, stripe);
    /* The visor is raised: all of it above the eyes, which are all there. */
    int eyes = eye_top();
    CHECK(count(s_fb, C_IRIS) == count(s_plain, C_IRIS), "%s t=%.2f: eyes covered", sc->name, when);
    for (int y = eyes; y < H && eyes < H; y++) {
        for (int x = 0; x < W; x++) {
            CHECK(s_fb[y * W + x] != C_VISOR, "%s t=%.2f: visor at (%d,%d), eyes start at %d", sc->name, when, x, y,
                  eyes);
        }
    }
    CHECK(count(s_fb, C_HP) == 0 && count(s_fb, C_HP_EDGE) == 0, "%s t=%.2f: headphones over the helmet", sc->name,
          when);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    const scene_t SCENES[] = {
        { "idle", { .mode = MUSE_MODE_IDLE }, NULL, 3.0f },
        { "boot", { .mode = MUSE_MODE_BOOT }, NULL, 1.6f },
        { "listening", { .mode = MUSE_MODE_LISTENING, .level = 0.7f }, NULL, 2.0f },
        { "thinking", { .mode = MUSE_MODE_THINKING }, NULL, 2.0f },
        { "speaking", { .mode = MUSE_MODE_SPEAKING, .level = 0.6f }, NULL, 2.0f },
        { "error", { .mode = MUSE_MODE_ERROR }, NULL, 2.0f },
        { "happy", { .mode = MUSE_MODE_IDLE, .happy = 1 }, NULL, 2.0f },
        { "dizzy", { .mode = MUSE_MODE_IDLE }, "dizzy", MUSE_DIZZY_S },
        { "sleepy", { .mode = MUSE_MODE_IDLE }, "sleepy", MUSE_SLEEPY_S },
        { "waking", { .mode = MUSE_MODE_IDLE }, "waking", MUSE_WAKING_S },
        { "tickle", { .mode = MUSE_MODE_IDLE }, "tickle", MUSE_TICKLE_S },
        { "vtec", { .mode = MUSE_MODE_IDLE }, "vtec", MUSE_VTEC_S },
        { "coolant", { .mode = MUSE_MODE_IDLE }, "coolant", MUSE_COOLANT_S },
        { "warmup", { .mode = MUSE_MODE_IDLE }, "warmup", MUSE_WARMUP_S },
    };
    float t = 10.0f;
    for (size_t s = 0; s < sizeof(SCENES) / sizeof(SCENES[0]); s++) {
        const scene_t *sc = &SCENES[s];
        float mode_t = 0;
        /* Settle into the mode first (the palette blend), then the frames. */
        for (int i = 0; i < 30; i++, t += DT, mode_t += DT) {
            muse_pose_t p = sc->pose;
            p.t = t;
            p.mode_t = sc->pose.mode == MUSE_MODE_BOOT ? 0 : mode_t;
            muse_pixel_render(&p);
        }
        if (sc->pose.mode == MUSE_MODE_BOOT) {
            mode_t = 0;
        }
        bool reaction = false, saved = false;
        for (float u = 0; u < sc->dur; u += DT, t += DT, mode_t += DT) {
            muse_pose_t p = sc->pose;
            p.t = t;
            p.mode_t = mode_t;
            set_ramp(&p, sc->ramp, u / sc->dur);
            frame(sc, p, u);
            if (sc->ramp) {
                react_t re;
                react_setup(&p, &re);
                reaction = reaction || re.kind != RE_NONE;
            }
            if (dir && !saved && u >= sc->dur * 0.4f) {
                char path[512];
                snprintf(path, sizeof(path), "%s/%s.ppm", dir, sc->name);
                save(path);
                saved = true;
            }
        }
        CHECK(!sc->ramp || reaction, "%s: the reaction never showed", sc->name);
    }
    /* A light shell. */
    for (int k = 0; k < 3; k++) {
        CHECK(((FIXED[C_HELM] >> (8 * k)) & 0xff) >= 0xd0, "shell channel %d dark", k);
    }

    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_pixel_helmet\n");
    return 0;
}
