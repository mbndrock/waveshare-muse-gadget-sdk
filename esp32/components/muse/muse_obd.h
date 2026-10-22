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
 * the trigger in muse_obd_elm.h fires, plays the avatar's VTEC reaction and
 * says "VTEC just kicked in yo!". Every 4th request reads coolant (01 05)
 * instead; when the coolant trigger fires it plays the coolant warning and
 * says "Coolant temperature is high!".
 *
 * Starts a task that waits for Link's BLE host, then keeps the adapter
 * connected, reconnecting with backoff. Call once, after the UI and the
 * voice session are up.
 */
void muse_obd_start(void);

/*
 * Play a reaction and say its line, as a trigger firing would, without
 * touching either trigger (no arming, sustain or cooldown) and without needing
 * the adapter. The triggers call these; so do the bench console's ">vtec" and
 * ">coolant".
 */
void muse_obd_fire_vtec(void);
void muse_obd_fire_coolant(void);

/*
 * What the OBD task is doing and its latest readings. state is one of "off",
 * "waiting" (for Link's BLE host), "scanning", "connecting", "discovering",
 * "initializing" (the ELM's AT commands), "polling" and "backoff" (between
 * attempts). A dropped link clears both readings: there is no current one.
 */
typedef struct {
    bool started;          /* muse_obd_start() created the task */
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
} muse_obd_status_t;

/*
 * Copies the snapshot into *out. Never blocks and never touches BLE: safe
 * from any task, before muse_obd_start() too. The obd.read Link command and
 * the bench console's ">obd" report it.
 */
void muse_obd_status(muse_obd_status_t *out);
