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
 * The ELM327 side of the OBD reactions (muse_obd.c: VTEC, the coolant
 * temperature warning and the warm-up thumbs-up) and of the car data cards
 * (the PID support scan and the cards' PIDs), as plain C with no BLE or
 * FreeRTOS in it, so the host tests can drive it byte by byte.
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
    MUSE_ELM_NONE,      /* nothing that answers the request (an echo, "OK", empty) */
    MUSE_ELM_COOLANT,   /* a 41 05 reply: *coolant_c is set */
    MUSE_ELM_LOAD,      /* a 41 04 reply: *pct is set */
    MUSE_ELM_MAP,       /* a 41 0B reply: *kpa is set */
    MUSE_ELM_SPEED,     /* a 41 0D reply: *kmh is set */
    MUSE_ELM_TIMING,    /* a 41 0E reply: *deg is set */
    MUSE_ELM_IAT,       /* a 41 0F reply: *c is set */
    MUSE_ELM_THROTTLE,  /* a 41 11 reply: *pct is set */
    MUSE_ELM_STFT,      /* a 41 06 reply: *pct is set */
    MUSE_ELM_LTFT,      /* a 41 07 reply: *pct is set */
    MUSE_ELM_AMBIENT,   /* a 41 46 reply: *c is set */
    MUSE_ELM_SUPPORT,   /* a 41 00 / 20 / 40 reply: *mask is set */
} muse_elm_result_t;

/* Reads engine RPM from the response to "010C": ((A * 256) + B) / 4. Spaces
 * and header-free lines like "SEARCHING..." are skipped; the first line that
 * reads 41 0C A B wins. */
muse_elm_result_t muse_elm_parse_rpm(const char *resp, int *rpm);

/* Reads the coolant temperature from the response to "0105": A - 40 (deg C,
 * -40..215). Parsed like RPM: the first line that reads 41 05 A wins. */
muse_elm_result_t muse_elm_parse_coolant(const char *resp, int *coolant_c);

/* The car data cards' PIDs, each parsed like coolant (the first line that
 * reads 41 <pid> A wins), in whole units with C's integer division:
 *   04 load      A * 100 / 255        %      0..100
 *   0B MAP       A                    kPa    0..255
 *   0D speed     A                    km/h   0..255
 *   0E timing    A / 2 - 64           deg    -64..63 (the half degree is cut)
 *   0F intake    A - 40               C      -40..215
 *   11 throttle  A * 100 / 255        %      0..100
 *   06 STFT B1   (A - 128) * 100 / 128 %     -100..99
 *   07 LTFT B1   (A - 128) * 100 / 128 %     -100..99
 *   46 ambient   A - 40               C      -40..215 */
muse_elm_result_t muse_elm_parse_load(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_map(const char *resp, int *kpa);
muse_elm_result_t muse_elm_parse_speed(const char *resp, int *kmh);
muse_elm_result_t muse_elm_parse_timing(const char *resp, int *deg);
muse_elm_result_t muse_elm_parse_iat(const char *resp, int *c);
muse_elm_result_t muse_elm_parse_throttle(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_stft(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_ltft(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_ambient(const char *resp, int *c);

/*
 * The supported-PID bitmask from the response to "0100", "0120" or "0140"
 * (base 0x00, 0x20 or 0x40): four bytes A B C D, read as A << 24 | ... | D.
 * The top bit is PID base + 1, the bottom bit PID base + 0x20, which says
 * whether the next range's mask can be asked for. Parsed like the others.
 */
muse_elm_result_t muse_elm_parse_support(const char *resp, int base, uint32_t *mask);

/* What the support scan learnt: masks[i] covers PIDs 0x20 * i + 1 .. 0x20 * (i + 1). */
#define MUSE_ELM_SUPPORT_RANGES 3

typedef struct {
    bool valid[MUSE_ELM_SUPPORT_RANGES];   /* masks[i] was read */
    uint32_t masks[MUSE_ELM_SUPPORT_RANGES];
} muse_elm_support_t;

typedef enum {
    MUSE_ELM_PID_UNKNOWN,       /* no mask says: ask, and let NO DATA skip it */
    MUSE_ELM_PID_SUPPORTED,
    MUSE_ELM_PID_UNSUPPORTED,   /* never ask */
} muse_elm_pid_support_t;

/* Whether `pid` (0x01..0x60) is supported. A range whose mask wasn't read is
 * unsupported when the nearest range below it that was read has its chain
 * bit (its last PID, 0x20 or 0x40) clear: the ECU said there is nothing past
 * it. Otherwise (no scan, a scan that failed partway, a PID outside
 * 0x01..0x60) it is unknown. */
muse_elm_pid_support_t muse_elm_pid_support(const muse_elm_support_t *s, int pid);

/* The protocol number from the response to "ATDPN": "A6" (auto, found 6) or
 * "6" -> 6. 0 (auto, nothing found yet) and anything unreadable -> -1. */
int muse_elm_parse_protocol(const char *resp);

/* A short name for an ATDPN protocol number, e.g. 6 -> "CAN 11/500",
 * 3 -> "ISO 9141-2"; anything else -> "proto N". */
void muse_elm_protocol_name(int protocol, char *buf, size_t cap);

/*
 * The poll loop alternates RPM with one secondary PID, taken in turn from
 * MUSE_ELM_SECONDARY: coolant first, then the cards' PIDs. *cursor (start
 * it at 0) remembers the place. PIDs the scan proved unsupported are skipped;
 * unknown ones are kept. Returns the next PID, or -1 when none is left.
 */
#define MUSE_ELM_SECONDARIES 10
extern const uint8_t MUSE_ELM_SECONDARY[MUSE_ELM_SECONDARIES];
int muse_elm_next_secondary(const muse_elm_support_t *s, unsigned *cursor);

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

/*
 * The coolant warning: fires once the coolant has read >= MUSE_COOLANT_FIRE_C
 * continuously for MUSE_COOLANT_SUSTAIN_MS with the engine running (latest
 * RPM >= MUSE_COOLANT_RUNNING_RPM). Any reading below MUSE_COOLANT_FIRE_C, or
 * with the engine off or its RPM unknown, restarts the sustain: a hot soak
 * with the key on is not an alarm. After a fire it stays quiet until the
 * coolant has dropped to <= MUSE_COOLANT_REARM_C, and for at least
 * MUSE_COOLANT_COOLDOWN_MS. Unlike VTEC it starts armed (connecting to a car
 * that is already overheating should warn), and a fire held back only by the
 * cooldown lands once it runs out if the coolant is still hot.
 */
#define MUSE_COOLANT_FIRE_C 105
#define MUSE_COOLANT_REARM_C 100
#define MUSE_COOLANT_SUSTAIN_MS 5000u
#define MUSE_COOLANT_COOLDOWN_MS 120000u
#define MUSE_COOLANT_RUNNING_RPM 400

typedef struct {
    bool armed;
    bool hot;               /* hot_since_ms is valid: sustain is building */
    uint32_t hot_since_ms;
    bool fired;             /* has fired since init: last_fire_ms is valid */
    uint32_t last_fire_ms;
} muse_coolant_trigger_t;

void muse_coolant_init(muse_coolant_trigger_t *t);
/* The link dropped: the sustain starts over; arming and the cooldown hold. */
void muse_coolant_reset(muse_coolant_trigger_t *t);
/* One coolant reading at now_ms (any wrapping millisecond clock), with the
 * latest RPM (negative: unknown); true to fire. */
bool muse_coolant_update(muse_coolant_trigger_t *t, int coolant_c, int rpm, uint32_t now_ms);

/*
 * The warm-up thumbs-up: the coolant reached operating temperature. It fires
 * only after this drive has been seen cold: a reading <= MUSE_WARMUP_COLD_C
 * arms it, and the first reading >= MUSE_WARMUP_FIRE_C after that fires it,
 * once. Then it stays quiet until a reading <= MUSE_WARMUP_COLD_C arms it
 * again. It starts disarmed, so a first reading that's already warm (a hot
 * restart, a reconnect mid-drive) never fires; a new connection starts over.
 */
#define MUSE_WARMUP_FIRE_C 82
#define MUSE_WARMUP_COLD_C 70

typedef struct {
    bool armed;
} muse_warmup_trigger_t;

void muse_warmup_init(muse_warmup_trigger_t *t);
/* One coolant reading (deg C); true to fire. Readings outside the PID's
 * -40..215 change nothing. */
bool muse_warmup_update(muse_warmup_trigger_t *t, int coolant_c);
