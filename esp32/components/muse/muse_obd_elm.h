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
 * The ELM327 side of the OBD reactions (muse_obd.c: VTEC from Honda's DID
 * 0x2612, the coolant temperature warning and the warm-up thumbs-up) and of
 * the car data cards (the PID support scan and the cards' PIDs), as plain C
 * with no BLE or FreeRTOS in it, so the host tests can drive it byte by byte.
 *
 * An ELM327 answers each command with text and then its '>' prompt. Over BLE
 * that text arrives in notifications cut anywhere, so bytes go in one at a
 * time and a response is complete at '>'.
 */

/* Room for DID 0x2612's 57-byte reply, the longest: its length line and
 * nine frame lines ("0:622612FFFFC0", ...), spaces too if they were on. */
#define MUSE_ELM_RESP_MAX 320

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
    MUSE_ELM_VTEC_DID,  /* a whole 62 26 12 reply: the VTEC flags are set */
    MUSE_ELM_NEGATIVE,  /* 7F 22 <NRC>: the ECU refused the DID */
    MUSE_ELM_BATCH,     /* a multi-PID reply: at least one asked PID read a value */
    MUSE_ELM_AFR,       /* a 41 34 reply: *raw is set */
    MUSE_ELM_PEDAL_D,   /* a 41 49 reply: *pct is set */
    MUSE_ELM_PEDAL_E,   /* a 41 4A reply: *pct is set */
    MUSE_ELM_MODULE_V,  /* a 41 42 reply: *mv is set */
    MUSE_ELM_BARO,      /* a 41 33 reply: *kpa is set */
    MUSE_ELM_FUEL,      /* a 41 2F reply: *pct is set */
    MUSE_ELM_DID_2610,  /* a 62 26 10 reply with its bytes: the A/C clutch and generator are set */
    MUSE_ELM_DID_2663,  /* a 62 26 63 reply with its bytes: the misfire counts are set */
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

/* OBD-MORE-PIDS-TASK's PIDs, parsed the same way (the J1979 decodes):
 *   34 wideband lambda  256A + B, raw: lambda = raw / 32768 (C and D unused)
 *   49 pedal D          A * 100 / 255        %      0..100
 *   4A pedal E          A * 100 / 255        %      0..100
 *   42 module voltage   256A + B             mV     0..65535
 *   33 barometric       A                    kPa    0..255
 *   2F fuel level       A * 100 / 255        %      0..100
 * A reply cut short of its PID's bytes (34 has four, 42 two) is no reading. */
muse_elm_result_t muse_elm_parse_afr(const char *resp, int *raw);
muse_elm_result_t muse_elm_parse_pedal_d(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_pedal_e(const char *resp, int *pct);
muse_elm_result_t muse_elm_parse_module_v(const char *resp, int *mv);
muse_elm_result_t muse_elm_parse_baro(const char *resp, int *kpa);
muse_elm_result_t muse_elm_parse_fuel(const char *resp, int *pct);

/* PID 34's raw ratio (0..65535) as lambda in thousandths and as a gasoline
 * air-fuel ratio (lambda x 14.7) in tenths, each rounded to nearest:
 * 0x8000 is stoich, 1000 and 147 (lambda 1.000, AFR 14.7). */
int muse_elm_lambda_x1000(int raw);
int muse_elm_afr_x10(int raw);

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
 * A multi-PID Mode 01 reply, parsed for the batch the poll pipeline asked
 * for: pids[0..n) (n <= MUSE_ELM_BATCH_MAX, no support PIDs, no DID), with
 * each PID's value in values[i] and got[i] set, converted exactly as the
 * single-PID parsers convert. A CAN ECU answers a batch with one message
 * (J1979; the ELM327 datasheet's "Multiple PID Requests"): 41 once, then
 * each PID it answers and that PID's data, in any order. Over seven bytes
 * the ELM prints it as a length line and ISO-TP frames ("00A", "0:41...",
 * "1:..."), the last one padded. Each message read (one per answering ECU)
 * contributes; a PID it leaves out is a miss for that PID only.
 * MUSE_ELM_BATCH when at least one PID read a value; MUSE_ELM_NO_DATA or
 * MUSE_ELM_ERROR when the request as a whole was refused that way (nothing
 * read a value); MUSE_ELM_NONE when nothing answered any asked PID and
 * nothing refused either (garbled: each asked PID is simply missing).
 */
#define MUSE_ELM_BATCH_MAX 6
muse_elm_result_t muse_elm_parse_batch(const char *resp, const uint8_t *pids, int n, int *values, bool *got);

/*
 * Honda's PGM-FI data block (UDS 0x22, DID 0x2612; OBDb's Honda-S2000
 * signal set, from 2006/2007 S2000 captures), asked of the engine ECU by
 * physical address on a CAN 29-bit/500 link: header DA10F1, replies from
 * 18DAF110. The reply is ISO-TP, 57 bytes: 62 26 12 and 54 data bytes. With
 * headers off the ELM prints it as its length ("039") and then the frames,
 * "0:622612FFFFC0", "1:001E00...", and so on, the last one padded.
 */
#define MUSE_VTEC_DID_REQ "222612"
#define MUSE_VTEC_DID_LEN 57          /* 62 26 12 + 54 data bytes */
#define MUSE_VTEC_DID_FLAGS 21        /* data byte (after 62 26 12) with the flags */
#define MUSE_VTEC_DID_SOL 0x01        /* the VTEC solenoid valve: 1 = on */
#define MUSE_VTEC_DID_PSW 0x02        /* the VTEC oil pressure switch: 1 = on */
#define MUSE_VTEC_DID_B23 23          /* unknown: 0 -> 1 in every captured "on" sample */

typedef struct {
    bool sol;               /* the solenoid is on: VTEC engaged */
    bool psw;               /* the pressure switch is on */
    uint8_t b23;            /* data byte 23, raw */
} muse_vtec_did_t;

/*
 * The VTEC flags from the response to "222612". Only a reply whose bytes
 * add up (the frames in order, as many as its length line says) and that is
 * exactly 57 bytes starting 62 26 12 is MUSE_ELM_VTEC_DID; a refusal is
 * MUSE_ELM_NEGATIVE; NO DATA and the ELM's errors as for the PIDs; anything
 * else (a wrong prefix, a short or out-of-order reply, another length) is
 * MUSE_ELM_NONE, and *out is left alone.
 */
muse_elm_result_t muse_elm_parse_vtec_did(const char *resp, muse_vtec_did_t *out);

/*
 * OBD-DID-CARDS-TASK: two more blocks from the same ECU, asked the same way
 * (OBDb's Honda-S2000 signal set; its bix counts bits of the data after
 * 62 26 xx, MSB first, the convention 0x2612's flags were verified on). Every
 * 2007 fixture OBDb has for them is a 57-byte reply, 62 26 xx and 54 data
 * bytes, like 0x2612's.
 *   2610  data byte 23: generator duty, x 100 / 255 %; data byte 32 mask
 *         0x02: the A/C clutch, 1 engaged. Needs at least 33 data bytes.
 *   2663  misfire counts, big-endian 16-bit: cylinder 1 at data byte 34,
 *         2 at 36, 3 at 38, 4 at 40. Needs at least 42 data bytes.
 * Each parser takes a reply whose bytes add up (as 0x2612's: the frames in
 * order, as many as its length line says), that starts 62 26 xx and has at
 * least the bytes its signals need; a refusal is MUSE_ELM_NEGATIVE; NO DATA
 * and the ELM's errors as for the PIDs; anything else (a wrong prefix, short,
 * out of order) is MUSE_ELM_NONE, and *out is left alone.
 */
#define MUSE_DID_2610_REQ "222610"
#define MUSE_DID_2610_MIN 33          /* data bytes a 0x2610 read needs */
#define MUSE_DID_2610_GEN 23          /* data byte: generator duty, x 100 / 255 % */
#define MUSE_DID_2610_AC 32           /* data byte with the A/C clutch */
#define MUSE_DID_2610_AC_ON 0x02      /* its bit: 1 = the clutch is engaged */
#define MUSE_DID_2663_REQ "222663"
#define MUSE_DID_2663_MIN 42          /* data bytes a 0x2663 read needs */
#define MUSE_DID_2663_MIS 34          /* data byte: cylinder 1's count, high byte first; n at 34 + 2 (n - 1) */
#define MUSE_MISFIRE_CYLS 4

typedef struct {
    bool ac;                /* the A/C compressor clutch is engaged */
    int generator_pct;      /* the generator's duty, 0..100 */
} muse_did_2610_t;

typedef struct {
    uint16_t count[MUSE_MISFIRE_CYLS];   /* misfires per cylinder, 1..4 */
} muse_did_2663_t;

muse_elm_result_t muse_elm_parse_did_2610(const char *resp, muse_did_2610_t *out);
muse_elm_result_t muse_elm_parse_did_2663(const char *resp, muse_did_2663_t *out);

/*
 * The DIDs the poll loop reads, one table: each is asked by physical address
 * with the same state-safe choreography (muse_obd.c's did_read), probed once
 * per connection and never again on it if refused, silent or garbled, and
 * read on its own floor once answered. 0x2612 keeps its 1 s so the VTEC hold
 * stays fresh; 0x2610 and 0x2663 change slowly and take 3 s.
 */
typedef enum {
    MUSE_DID_2612,   /* the VTEC flags */
    MUSE_DID_2610,   /* the A/C clutch and the generator */
    MUSE_DID_2663,   /* the misfire counts */
    MUSE_DIDS,
} muse_did_t;

#define MUSE_DID_2610_MIN_MS 3000u    /* tunable: 0x2610's minimum interval */
#define MUSE_DID_2663_MIN_MS 3000u    /* tunable: 0x2663's minimum interval */

typedef struct {
    uint16_t did;
    const char *req;        /* the UDS 0x22 request, "222612" */
    uint8_t min_data;       /* data bytes a read needs (0x2612: exactly its 54) */
    uint32_t min_ms;        /* the scheduler's floor between reads */
} muse_did_info_t;

/* One per muse_did_t, in that order. */
extern const muse_did_info_t MUSE_DID_INFO[MUSE_DIDS];

/*
 * The misfire warning: a cylinder's counter strictly increased between two
 * consecutive good 0x2663 reads with the engine running (latest RPM >=
 * MUSE_COOLANT_RUNNING_RPM). The first good read of a connection is the
 * baseline and never fires. A read where any counter went down (the ECU
 * cleared them, or one wrapped) is a new baseline and never fires. Each read
 * becomes the baseline for the next, so an increase is counted once; one
 * with the engine off, or inside MUSE_MISFIRE_COOLDOWN_MS of the last fire,
 * is let go without firing. `rose` has bit n set when cylinder n + 1 went up
 * on the latest read (the card's amber), whatever came of it.
 */
#define MUSE_MISFIRE_COOLDOWN_MS 120000u   /* mirrors MUSE_COOLANT_COOLDOWN_MS */

typedef struct {
    bool have;                            /* base holds the last good read */
    uint16_t base[MUSE_MISFIRE_CYLS];
    uint8_t rose;                         /* bit n: cylinder n + 1 went up on the latest read */
    bool fired;                           /* has fired since init: last_fire_ms is valid */
    uint32_t last_fire_ms;
} muse_misfire_trigger_t;

void muse_misfire_init(muse_misfire_trigger_t *t);
/* The link dropped: the next read is a baseline again; the cooldown holds. */
void muse_misfire_reset(muse_misfire_trigger_t *t);
/* One good 0x2663 read at now_ms (any wrapping millisecond clock), with the
 * latest RPM (negative: unknown); true to fire. */
bool muse_misfire_update(muse_misfire_trigger_t *t, const uint16_t count[MUSE_MISFIRE_CYLS], int rpm,
                         uint32_t now_ms);

/*
 * The poll pipeline's scheduler (OBD-PIPELINE-TASK): what the response-driven
 * loop asks next, replacing the fixed RPM/secondary rotation. Producers hold
 * soft freshness targets, not an order: RPM (target 0, the standing filler,
 * top priority), DID 0x2612 (about 1 s), the fast secondaries MAP, throttle,
 * load and speed (about 1 s), the mid STFT, LTFT and timing (about 2 s) and
 * the slow coolant, IAT and ambient (about 3 s). muse_sched_pick() chooses
 * the most overdue item; ties break by priority. OBD-MORE-PIDS-TASK adds
 * AFR (34) and pedals D and E (49, 4A) to the fast tier, fuel level (2F) to
 * the mid one and module voltage (42) and barometric pressure (33) to the
 * slow one; OBD-DID-CARDS-TASK adds DIDs 0x2610 and 0x2663 (3 s each) beside
 * 0x2612, each in the schedule only once its probe answered. Targets are
 * floors on
 * freshness, never rate caps: a faster link simply refreshes everything
 * sooner, and nothing is ever delayed to meet a target.
 *
 * A Mode 01 pick carries up to MUSE_ELM_BATCH_MAX due PIDs in one request,
 * RPM first when it is among them; a DID is always a pick of its own, one at
 * a time, so DID exchanges never overlap each other or Mode 01. When DIDs
 * are due together the most overdue goes, which keeps 0x2612 on its 1 s
 * however the 3 s pair falls. A
 * signal already queued or in flight is never picked twice: a pick is the
 * only request outstanding, and an item leaves the due set the moment its
 * exchange completes (muse_sched_done_pid / muse_sched_done_did).
 *
 * One shaping rule, tunable: each DID keeps its MUSE_DID_INFO[].min_ms
 * between reads (MUSE_SCHED_DID_MIN_MS for 0x2612; the probe before polling
 * counts as one). Their multi-frame exchanges are the longest on the link;
 * without the floors they could crowd out Mode 01 when everything is overdue
 * at connect time.
 *
 * A PID that misses twice against a live ECU (muse_sched_done_pid's
 * ecu_live: another PID in its exchange read a value, or RPM is locked on)
 * leaves the schedule for the connection, like a scan-proven unsupported
 * one. Misses against a silent ECU prove nothing (the engine is off, most
 * likely) and never evict. RPM is never evicted: it is the filler and the
 * engine-start detector. Times are any wrapping millisecond clock.
 */
#define MUSE_SCHED_DID_MIN_MS 1000u   /* tunable: DID 0x2612's minimum interval */

typedef struct {
    bool did;               /* the pick is a DID read (which), never batched */
    uint8_t which;          /* the muse_did_t, when did */
    uint8_t pids[MUSE_ELM_BATCH_MAX];
    int n;                  /* Mode 01: how many of pids[] the request carries */
} muse_sched_req_t;

#define MUSE_SCHED_ITEMS 20   /* RPM, the three DIDs and the sixteen secondaries */

typedef struct {
    uint32_t last_ms[MUSE_SCHED_ITEMS];   /* the last completed attempt */
    bool have[MUSE_SCHED_ITEMS];          /* last_ms holds an attempt */
    bool present[MUSE_SCHED_ITEMS];       /* the support scan didn't rule it out */
    bool evicted[MUSE_SCHED_ITEMS];       /* missed out of the schedule */
    uint8_t misses[MUSE_SCHED_ITEMS];
    bool did_on[MUSE_DIDS];               /* this connection reads the DID (its probe answered) */
    bool batch_off;                       /* latched: single-PID for the connection */
} muse_sched_t;

/* Starts a connection's schedule: the scan's proven-unsupported PIDs are
 * absent, the rest present and all due; a DID only when its bit (1 << its
 * muse_did_t) is in `dids`, with its floor running from now_ms (the probe
 * just read it). */
void muse_sched_init(muse_sched_t *s, const muse_elm_support_t *sup, unsigned dids, uint32_t now_ms);
/* A support scan that landed after polling started (the first-RPM retry):
 * proven-unsupported PIDs leave; evictions and attempts stand. */
void muse_sched_apply_support(muse_sched_t *s, const muse_elm_support_t *sup);
/* A DID's probe answered after polling started: it joins the schedule. */
void muse_sched_set_did(muse_sched_t *s, muse_did_t did, bool on, uint32_t now_ms);
/* The next request: the most overdue eligible item, a batch of due Mode 01
 * PIDs, one DID on its own, or RPM alone when nothing is due (the filler). */
void muse_sched_pick(const muse_sched_t *s, uint32_t now_ms, muse_sched_req_t *req);
/* A Mode 01 exchange for `pid` completed: got says a value was read;
 * ecu_live says the ECU demonstrably answers (see above). */
void muse_sched_done_pid(muse_sched_t *s, int pid, bool got, bool ecu_live, uint32_t now_ms);
/* A DID exchange completed, answered or not: its floor runs from here. */
void muse_sched_done_did(muse_sched_t *s, muse_did_t did, uint32_t now_ms);
/* A batch request was refused as a whole: single-PID for the connection. */
void muse_sched_latch_batch_off(muse_sched_t *s);

/*
 * The link's numbers for one connection (obd.read's "link"): exchanges
 * completed (a prompt came back), the timeouts among what was asked (no
 * prompt, or a NO DATA: the ECU didn't answer in time), the last round trip,
 * and how many Mode 01 readings arrived, and of those how many via a batch.
 * The rolling rate is computed over the last MUSE_LINK_TIMES exchanges.
 */
#define MUSE_LINK_TIMES 16

typedef struct {
    uint32_t exchanges;
    uint32_t timeouts;
    uint32_t last_rtt_ms;
    uint32_t times[MUSE_LINK_TIMES];   /* completion times, a ring */
    unsigned t_head;                   /* the next slot to write */
    unsigned t_count;                  /* how many slots hold a time */
    uint32_t readings;
    uint32_t batch_readings;
} muse_link_stats_t;

void muse_link_stats_reset(muse_link_stats_t *s);
/* One completed exchange at now_ms that took rtt_ms; readings Mode 01 values
 * were read, via_batch when the request carried more than one PID. */
void muse_link_stats_exchange(muse_link_stats_t *s, uint32_t now_ms, uint32_t rtt_ms, int readings, bool via_batch);
/* One asked exchange that timed out (no prompt, or NO DATA). */
void muse_link_stats_timeout(muse_link_stats_t *s);
/* Rolling exchanges per second, x100; 0 until two exchanges complete. */
uint32_t muse_link_stats_rate_x100(const muse_link_stats_t *s);
/* Batch share of the Mode 01 readings, percent; -1 until one arrives. */
int muse_link_stats_batch_pct(const muse_link_stats_t *s);

/*
 * The ATST policy: the ELM's response timeout, trimmed from its 200 ms
 * default toward the car's measured reply latency, with a bounded step
 * back. In force is an ATST byte (the timeout is the byte x 4 ms). After
 * MUSE_TUNE_MEASURE valued exchanges, muse_tune_observe_value() returns
 * the trimmed byte once (twice the mean round trip, clamped), or -1 when
 * there is nothing to send. MUSE_TUNE_TIMEOUTS answerless Mode 01
 * exchanges in a row make muse_tune_observe_timeout() return the byte one
 * step up, or -1 at the cap. Tuning only ever sends ATST: it never wedges
 * or reconnects the session.
 */
#define MUSE_TUNE_ATST_MIN 0x08   /* 32 ms */
#define MUSE_TUNE_ATST_MAX 0x32   /* 200 ms: the ELM's default, and the cap */
#define MUSE_TUNE_ATST_STEP 0x04  /* 16 ms per step back */
#define MUSE_TUNE_MEASURE 4       /* valued exchanges before the trim */
#define MUSE_TUNE_TIMEOUTS 3      /* answerless in a row before a step back */

typedef struct {
    uint8_t atst;          /* the byte in force */
    bool trimmed;          /* the measured trim has been decided */
    int consec;            /* answerless Mode 01 exchanges in a row */
    uint32_t rtt_sum;      /* the measured round trips, for the trim */
    int rtt_n;
} muse_tune_t;

void muse_tune_init(muse_tune_t *t);
/* A Mode 01 exchange read at least one value, in rtt_ms. */
int muse_tune_observe_value(muse_tune_t *t, uint32_t rtt_ms);
/* A Mode 01 exchange read nothing (NO DATA, an error, no prompt). */
int muse_tune_observe_timeout(muse_tune_t *t);

/*
 * The VTEC reaction is a held pose that mirrors the solenoid: up for as long
 * as the latest DID reading says the solenoid is on and is no older than
 * MUSE_VTEC_FRESH_MS (the cards' staleness rule), down on an "off" reading,
 * on no reading, or once the reading goes stale. Nothing else, RPM
 * included, ever raises it.
 */
#define MUSE_VTEC_FRESH_MS 3000u

bool muse_vtec_engaged(bool valid, bool sol, uint32_t age_ms);

/*
 * The coolant warning: fires once the coolant has read >= MUSE_COOLANT_FIRE_C
 * continuously for MUSE_COOLANT_SUSTAIN_MS with the engine running (latest
 * RPM >= MUSE_COOLANT_RUNNING_RPM). Any reading below MUSE_COOLANT_FIRE_C, or
 * with the engine off or its RPM unknown, restarts the sustain: a hot soak
 * with the key on is not an alarm. After a fire it stays quiet until the
 * coolant has dropped to <= MUSE_COOLANT_REARM_C, and for at least
 * MUSE_COOLANT_COOLDOWN_MS. It starts armed (connecting to a car that is
 * already overheating should warn), and a fire held back only by the
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
