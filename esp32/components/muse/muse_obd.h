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
#include <stdint.h>

/*
 * The car reactions (CONFIG_MUSE_OBD_VTEC): a BLE central, beside Link's own
 * peripheral, to a LELink^2 ELM327 OBD-II adapter. It finds the adapter,
 * subscribes to its FFE1 characteristic and polls the car data cards' PIDs,
 * engine RPM (01 0C) first among them, for the cards and the trip log. The
 * poll loop is response-driven and scheduled (muse_obd_elm.h): the next
 * request goes out the moment the last reply is parsed, the scheduler asks
 * for the most overdue signal against soft freshness targets, and due PIDs
 * share one multi-PID Mode 01 request. On a CAN 29-bit link whose engine
 * ECU answers Honda's DID 0x2612, the DID is in the schedule too (the VTEC
 * solenoid, which holds the VTEC reaction up while it is on:
 * muse_obd_vtec_on), less any PID the support scan (01 00 / 20 / 40, after
 * the ELM init) proved unsupported. When the coolant trigger fires it plays
 * the coolant warning, and when the coolant first reaches operating
 * temperature after a cold start, a thumbs-up. The reactions are visual
 * only: a spoken line would swap the reaction face for the speaking pose.
 *
 * Starts a task that waits for Link's BLE host, then keeps the adapter
 * connected, reconnecting with backoff. Call once, after the UI and the
 * voice session are up.
 */
void muse_obd_start(void);

/*
 * Settings' OBD switch (muse_settings_obd_on), from any task; on until told
 * otherwise. Off is completely off: the task drops a link that is up, stops
 * a scan or connection under way, and then neither scans nor connects, so
 * the adapter is free for a phone app; the reactions don't play and the
 * snapshot's state is "off" with no readings. Turned back on, it starts
 * looking for the adapter again, without a reboot.
 */
void muse_obd_set_enabled(bool on);
bool muse_obd_enabled(void);

/*
 * Play a reaction (visual only, no speech), as a trigger firing would,
 * without touching either trigger (no arming, sustain or cooldown) and without
 * needing the adapter. The triggers call these; so do the bench console's
 * ">coolant" and ">warmup". Nothing plays while OBD is turned off. VTEC has
 * no fire: it is held, see muse_obd_vtec_on().
 */
void muse_obd_fire_coolant(void);
void muse_obd_fire_warmup(void);

/* The car data cards' readings beyond RPM and coolant, in obd.read's order. */
typedef enum {
    MUSE_OBD_LOAD,       /* 04, % */
    MUSE_OBD_MAP,        /* 0B, kPa */
    MUSE_OBD_SPEED,      /* 0D, km/h */
    MUSE_OBD_TIMING,     /* 0E, degrees */
    MUSE_OBD_IAT,        /* 0F, degrees C */
    MUSE_OBD_THROTTLE,   /* 11, % */
    MUSE_OBD_STFT,       /* 06, % */
    MUSE_OBD_LTFT,       /* 07, % */
    MUSE_OBD_AMBIENT,    /* 46, degrees C */
    MUSE_OBD_AFR,        /* 34, air-fuel ratio x 10 (lambda x 14.7) */
    MUSE_OBD_LAMBDA,     /* 34 too, lambda x 1000 */
    MUSE_OBD_PEDAL_D,    /* 49, % */
    MUSE_OBD_PEDAL_E,    /* 4A, % */
    MUSE_OBD_MODULE_V,   /* 42, mV (volts x 1000) */
    MUSE_OBD_BARO,       /* 33, kPa */
    MUSE_OBD_FUEL,       /* 2F, % */
    MUSE_OBD_EXTRAS,
} muse_obd_extra_t;

typedef struct {
    uint8_t pid;           /* Mode 01 PID */
    const char *key;       /* obd.read's name for it: "load_pct", ... */
    const char *age_key;   /* and for its age: "load_age_ms", ... */
    uint8_t decimals;      /* the value is in 10^-decimals of its unit: AFR 1, lambda and volts 3 */
} muse_obd_extra_info_t;

/* One per muse_obd_extra_t, in that order. */
extern const muse_obd_extra_info_t MUSE_OBD_EXTRA[MUSE_OBD_EXTRAS];

typedef struct {
    bool valid;            /* value holds a current reading */
    int value;             /* in the unit above */
    uint32_t age_ms;       /* ms since that reading; UINT32_MAX if none */
} muse_obd_reading_t;

#define MUSE_OBD_SUPPORT_RANGES 3   /* PIDs 01-20, 21-40, 41-60 */

/*
 * What the OBD task is doing and its latest readings. state is one of "off"
 * (not started, or turned off in Settings: see enabled), "waiting" (for
 * Link's BLE host), "scanning", "connecting", "discovering",
 * "initializing" (the ELM's AT commands and the support scan), "polling" and
 * "backoff" (between attempts). A dropped link clears every reading: there is
 * no current one. The protocol and the supported-PID masks describe the car,
 * not a moment: they are the last ones read and stay across a dropped link,
 * so they can be read after the drive, until the next scan replaces them.
 */
typedef struct {
    bool started;          /* muse_obd_start() created the task */
    bool enabled;          /* Settings' OBD switch is on (muse_obd_set_enabled) */
    const char *state;     /* one of the state strings above */
    bool adapter_found;    /* the current/last scan saw the adapter */
    bool connected;        /* a BLE connection to the adapter is up */
    bool subscribed;       /* FFE1 notifications are on */
    bool polling;          /* inside poll_rpm(): ELM init done, asking PIDs */
    bool rpm_valid;        /* rpm holds a current reading */
    int rpm;               /* latest engine RPM; 0 is a real reading */
    uint32_t rpm_age_ms;   /* ms since that reading; UINT32_MAX if none */
    bool coolant_valid;    /* coolant_c holds a current reading */
    int coolant_c;         /* latest coolant temperature, degrees C */
    uint32_t coolant_age_ms; /* ms since that reading; UINT32_MAX if none */
    muse_obd_reading_t extra[MUSE_OBD_EXTRAS];   /* the cards' other PIDs */
    int protocol;          /* ATDPN's protocol number (6: CAN 11/500); -1 if not known */
    char protocol_name[20]; /* its short name ("CAN 11/500", "proto 13"); "" if not known */
    bool support_valid[MUSE_OBD_SUPPORT_RANGES];   /* support[i] was read */
    uint32_t support[MUSE_OBD_SUPPORT_RANGES];     /* 01 00 / 20 / 40's bitmask (A << 24 | ... | D) */
    /* Honda's DID 0x2612 (muse_obd_elm.h). did_2612 is whether the engine
     * ECU answered it on the last connection that asked (only CAN 29-bit
     * links ask); like the protocol it stays after the link drops. The
     * readings are live and clear with the link. */
    muse_obd_reading_t vtec_sol;      /* the VTEC solenoid: 1 on, 0 off */
    muse_obd_reading_t vtec_psw;      /* the VTEC oil pressure switch: 1 on, 0 off */
    bool did_2612;
    muse_obd_reading_t did_2612_b23;  /* the block's data byte 23, raw (meaning unknown) */
    /* The poll link's numbers (muse_obd_elm.h's muse_link_stats_t), this
     * connection only: they start over with each connection and read as
     * zeroes (and a -1 batch share) while no link is polling. */
    uint32_t link_exchanges;   /* exchanges completed (a prompt came back) */
    uint32_t link_timeouts;    /* asked exchanges the ECU didn't answer in time */
    uint32_t link_last_rtt_ms; /* the last completed exchange's round trip */
    uint32_t link_rate_x100;   /* rolling exchanges per second, x100 */
    int link_batch_pct;        /* Mode 01 readings that arrived via batch, %; -1: none yet */
    bool link_batch_off;       /* batching refused on this connection: single-PID */
    /* The trip log (muse_obd_log.h), which works with OBD turned off too. */
    bool log_card;         /* a microSD card is mounted */
    bool log_flash;        /* the obdlog flash partition is mounted */
    const char *log_store; /* where the current or next trip goes: "sd", "flash"; NULL for nowhere */
    bool log_active;       /* a trip file is open */
    bool log_trip_valid;   /* log_trip is the open trip's id */
    uint32_t log_trip;
    uint32_t log_pending;  /* completed trips not yet deleted, both stores: Muse's doorbell */
    uint32_t log_dropped;  /* samples dropped this trip */
    bool log_full;         /* a store filled up and takes no more trips */
} muse_obd_status_t;

/*
 * Copies the snapshot into *out. Never blocks and never touches BLE: safe
 * from any task, before muse_obd_start() too. The obd.read Link command and
 * the bench console's ">obd" report it. A trip that completes raises
 * log_pending, which is the flag Muse checks for trips to pull.
 */
void muse_obd_status(muse_obd_status_t *out);

/* *st has a link to the adapter up: notifications subscribed, or polling
 * (the status-row "OBD" label shows, and the card tiles and the avatar's
 * helmet are there). Never while turned off. */
bool muse_obd_linked(const muse_obd_status_t *st);

/*
 * The VTEC reaction's level: on while *st has a fresh "on" solenoid reading
 * (no older than MUSE_VTEC_FRESH_MS, the cards' staleness rule) and OBD is
 * on; off on an "off" reading, no reading, or a stale one. RPM plays no part.
 * muse_ui.c feeds it to muse_state_set_vtec() on every chrome tick.
 */
bool muse_obd_vtec_on(const muse_obd_status_t *st);

/* The bench console's ">vtecsol <0|1>": a solenoid reading that stays fresh
 * until the car's next real one (or a dropped link) replaces it. Does
 * nothing while OBD is turned off. */
void muse_obd_bench_vtec_sol(bool on);

/* The support masks in *st prove that Mode 01 `pid` is not supported (the
 * car data cards show "--" for it, and the poll loop never asks). */
bool muse_obd_pid_unsupported(const muse_obd_status_t *st, int pid);
