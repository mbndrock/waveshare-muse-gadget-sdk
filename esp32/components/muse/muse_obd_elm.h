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

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The ELM327 side of the OBD VTEC reaction (muse_obd.c), as plain C with no
 * BLE or FreeRTOS in it, so the host tests can drive it byte by byte.
 *
 * An ELM327 answers each command with text and then its '>' prompt. Over BLE
 * that text arrives in notifications cut anywhere, so bytes go in one at a
 * time and a response is complete at '>'.
 */

#define MUSE_ELM_RESP_MAX 160

typedef struct {
    char resp[MUSE_ELM_RESP_MAX];   /* the complete response, without '>' */
    size_t len;
    bool done;                      /* resp holds a complete response */
} muse_elm_rx_t;

void muse_elm_rx_reset(muse_elm_rx_t *rx);

/* Feeds one byte; true when it was the '>' that completes a response (in
 * rx->resp until the next byte goes in). Line ends become '\n', NULs and other
 * control bytes are dropped, and text past MUSE_ELM_RESP_MAX is cut off. */
bool muse_elm_feed(muse_elm_rx_t *rx, uint8_t byte);

typedef enum {
    MUSE_ELM_RPM,       /* a 41 0C reply: *rpm is set */
    MUSE_ELM_NO_DATA,   /* NO DATA: the ECU didn't answer (engine off, say) */
    MUSE_ELM_ERROR,     /* "?", UNABLE TO CONNECT, CAN ERROR, STOPPED, ... */
    MUSE_ELM_NONE,      /* nothing that answers 010C (an echo, "OK", empty) */
} muse_elm_result_t;

/* Reads engine RPM from the response to "010C": ((A * 256) + B) / 4. Spaces
 * and header-free lines like "SEARCHING..." are skipped; the first line that
 * reads 41 0C A B wins. */
muse_elm_result_t muse_elm_parse_rpm(const char *resp, int *rpm);

/*
 * The trigger: armed once rpm <= MUSE_VTEC_ARM_RPM, fires once at
 * rpm >= MUSE_VTEC_FIRE_RPM, then must be armed again by dropping back down.
 * Fires are at least MUSE_VTEC_COOLDOWN_MS apart: a crossing inside the
 * cooldown is swallowed (and still needs a new drop to arm), so a fire never
 * lands late in the middle of a pull. It starts disarmed, so connecting
 * mid-pull doesn't fire.
 */
#define MUSE_VTEC_ARM_RPM 5500
#define MUSE_VTEC_FIRE_RPM 6000
#define MUSE_VTEC_COOLDOWN_MS 30000u

typedef struct {
    bool armed;
    bool fired;             /* has fired since init: last_fire_ms is valid */
    uint32_t last_fire_ms;
} muse_vtec_trigger_t;

void muse_vtec_init(muse_vtec_trigger_t *t);
/* Back to disarmed (the link dropped); the cooldown still holds. */
void muse_vtec_disarm(muse_vtec_trigger_t *t);
/* One RPM reading at now_ms (any wrapping millisecond clock); true to fire. */
bool muse_vtec_update(muse_vtec_trigger_t *t, int rpm, uint32_t now_ms);
