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
 * The held VTEC pose (muse_state.c, muse_state_set_vtec) on a clock the
 * harness moves, fed the way muse_ui.c feeds it: once per 0.25 s chrome tick
 * with the solenoid's level. It runs in, waits at MUSE_VTEC_HOLD for as long
 * as the level stays on (repeated "on" ticks never restart it), leaves the
 * hold on the tick the level goes off and settles out, starts afresh at once
 * when the level comes back (no cooldown), shows only while idle, lets the
 * coolant warning run over it, and survives the screen sleeping.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "muse_pixel.h"
#include "muse_state.h"

static int64_t s_now_us = 1000000;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

void muse_text_to_ascii(char *s, size_t cap)
{
    (void)s, (void)cap;
}

#include "../components/muse/muse_state.c"

static int s_fail;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s: ", __FILE__, __LINE__, #cond); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        s_fail++; \
    } \
} while (0)

#define TICK_US 250000   /* muse_ui.c's SETTINGS_TICK_S */

/* `ticks` chrome ticks with the level at `on`; returns the last progress. */
static float run(bool on, int ticks)
{
    float v = 0;
    for (int i = 0; i < ticks; i++) {
        s_now_us += TICK_US;
        muse_state_set_vtec(on);
        v = muse_state_vtec();
    }
    return v;
}

/* Progress one frame (40 ms) on, without a tick. */
static float frame_later(void)
{
    s_now_us += 40000;
    return muse_state_vtec();
}

static bool near(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

int main(void)
{
    muse_state_init();
    muse_state_set_mode(MUSE_MODE_IDLE);
    CHECK(muse_state_vtec() == 0, "up with no level");
    CHECK(run(false, 20) == 0, "up with the level off");

    /* On: it runs in, then waits at the hold however long the level stays
     * on: a held pose, not a one-shot. */
    s_now_us += TICK_US;
    muse_state_set_vtec(true);
    float first = frame_later();
    CHECK(first > 0 && first < 0.05f, "doesn't start at the top: %f", (double)first);
    float v = run(true, 2);
    CHECK(v > first && v < MUSE_VTEC_HOLD, "not running in: %f", (double)v);
    v = run(true, 8);   /* 2.5 s in: past the run-in */
    CHECK(near(v, MUSE_VTEC_HOLD), "not held: %f", (double)v);
    for (int i = 0; i < 240; i++) {   /* a minute of solenoid on */
        v = run(true, 1);
        CHECK(near(v, MUSE_VTEC_HOLD), "lost the hold at tick %d: %f", i, (double)v);
    }

    /* Off: the hold ends on that tick, into the settle, then gone. */
    v = run(false, 1);
    CHECK(v >= MUSE_VTEC_SETTLE && v < 1, "off didn't leave the hold: %f", (double)v);
    v = run(false, 3);   /* 1 s later: the settle (0.54 s) is over */
    CHECK(v == 0, "still up after the settle: %f", (double)v);
    CHECK(run(false, 20) == 0, "came back by itself");

    /* Off and straight back on: from the top at once, no cooldown. */
    run(true, 12);
    v = run(false, 1);
    CHECK(v >= MUSE_VTEC_SETTLE, "off");
    v = run(true, 1);
    CHECK(v < 0.01f && frame_later() > 0, "on again didn't start afresh: %f", (double)v);
    v = run(true, 12);
    CHECK(near(v, MUSE_VTEC_HOLD), "on again isn't held: %f", (double)v);

    /* A blip shorter than the run-in still lets go on its off tick. */
    run(false, 10);
    run(true, 1);
    v = run(false, 1);
    CHECK(v >= MUSE_VTEC_SETTLE && v < 1, "a blip: %f", (double)v);
    CHECK(run(false, 4) == 0, "a blip stays");

    /* Idle only: another mode hides it, and it's back, still held, after. */
    run(true, 12);
    muse_state_set_mode(MUSE_MODE_SPEAKING);
    CHECK(run(true, 4) == 0, "shown while speaking");
    muse_state_set_mode(MUSE_MODE_IDLE);
    CHECK(near(run(true, 1), MUSE_VTEC_HOLD), "not back after speaking");

    /* The coolant warning runs over it (the renderer shows coolant first)
     * and leaves it held: VTEC is there again once the warning is over. */
    muse_state_start_coolant();
    float c = 0;
    for (int i = 0; i < 8; i++) {
        v = run(true, 1);
        c = muse_state_coolant();
        CHECK(near(v, MUSE_VTEC_HOLD), "the coolant warning stopped the hold");
    }
    CHECK(c > 0, "no coolant warning");
    v = run(true, (int)(MUSE_COOLANT_S * 4) + 1);
    CHECK(muse_state_coolant() == 0 && near(v, MUSE_VTEC_HOLD), "VTEC didn't resume after the warning");
    /* The warm-up too. */
    muse_state_start_warmup();
    CHECK(near(run(true, 2), MUSE_VTEC_HOLD) && muse_state_warmup() > 0, "the warm-up stopped the hold");

    /* The screen sleeps: gone. It wakes with the solenoid still on: up
     * again from the top on the next tick. */
    muse_state_set_asleep(true);
    CHECK(muse_state_vtec() == 0, "up asleep");
    muse_state_set_asleep(false);
    v = run(true, 1);
    CHECK(v < 0.01f && frame_later() > 0, "not back after waking: %f", (double)v);
    CHECK(near(run(true, 12), MUSE_VTEC_HOLD), "not held after waking");
    run(false, 8);

    /* Starting pokes and ends drowsing, as the old fire did; holding doesn't
     * keep poking. */
    float idle = muse_state_idle_secs();
    run(true, 1);
    CHECK(muse_state_idle_secs() < 0.01f, "start didn't poke (%f)", (double)idle);
    run(true, 20);
    CHECK(muse_state_idle_secs() > 4.9f, "the hold keeps poking");

    if (s_fail) {
        fprintf(stderr, "%d failed\n", s_fail);
        return 1;
    }
    printf("PASS muse_state_vtec\n");
    return 0;
}
