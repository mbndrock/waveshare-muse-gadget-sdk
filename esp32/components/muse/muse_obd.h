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
 * The VTEC reaction (CONFIG_MUSE_OBD_VTEC): a BLE central, beside Link's own
 * peripheral, to a LELink^2 ELM327 OBD-II adapter. It finds the adapter,
 * subscribes to its FFE1 characteristic, polls engine RPM (01 0C) and, when
 * the trigger in muse_obd_elm.h fires, plays the avatar's VTEC reaction.
 * Every other request reads a secondary PID instead, in turn: coolant (01 05)
 * and then the car data cards' PIDs, less any the support scan (01 00 / 20 /
 * 40, after the ELM init) proved unsupported. When the coolant trigger fires
 * it plays the coolant warning, and when the coolant first reaches operating
 * temperature after a cold start, a thumbs-up. The reactions are visual only:
 * a spoken line would swap the reaction face for the speaking pose.
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
 * ">vtec", ">coolant" and ">warmup". Nothing plays while OBD is turned off.
 */
void muse_obd_fire_vtec(void);
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
    MUSE_OBD_EXTRAS,
} muse_obd_extra_t;

typedef struct {
    uint8_t pid;           /* Mode 01 PID */
    const char *key;       /* obd.read's name for it: "load_pct", ... */
    const char *age_key;   /* and for its age: "load_age_ms", ... */
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
} muse_obd_status_t;

/*
 * Copies the snapshot into *out. Never blocks and never touches BLE: safe
 * from any task, before muse_obd_start() too. The obd.read Link command and
 * the bench console's ">obd" report it.
 */
void muse_obd_status(muse_obd_status_t *out);

/* *st has a link to the adapter up: notifications subscribed, or polling
 * (the status-row "OBD" label shows, and the card tiles and the avatar's
 * helmet are there). Never while turned off. */
bool muse_obd_linked(const muse_obd_status_t *st);

/* The support masks in *st prove that Mode 01 `pid` is not supported (the
 * car data cards show "--" for it, and the poll loop never asks). */
bool muse_obd_pid_unsupported(const muse_obd_status_t *st, int pid);
