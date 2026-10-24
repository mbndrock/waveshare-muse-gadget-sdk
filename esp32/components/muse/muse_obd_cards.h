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

#include "lvgl.h"

/*
 * The car data cards (CONFIG_MUSE_OBD_VTEC): three passive tiles between the
 * face and settings, there only while the OBD link is up (muse_ui.c puts the
 * tiles in and takes them out), that show what the car's ECU answers, from
 * muse_obd_status(). "Temps": coolant, intake air, ambient, the A/C clutch
 * (DID 0x2610: ON, OFF or "--"). "Engine": RPM, timing advance beside the
 * air-fuel ratio, load beside MAP, and the four cylinders' misfire counts
 * (DID 0x2663, "--" until the first read, amber while a count went up on the
 * latest read). "Drive": speed (km/h and mph), brake pressure (VSA DID
 * 0x4005) beside throttle, fuel level beside short-term fuel trim, and
 * long-term fuel trim. The BRAKE cell appears only once the DA28 presence
 * probe has confirmed the VSA unit, and reads "--" until its first read.
 *
 * A reading keeps its full colour until it is replaced (no stale dimming);
 * one not received yet reads "...", and one the support scan proved
 * unsupported reads "--". While the link isn't polling, each card shows the
 * link's state instead. The VTEC word is the one exception: it reads "--"
 * once its reading is older than MUSE_OBD_CARD_STALE_MS rather than show a
 * stale ON.
 */
#define MUSE_OBD_CARDS 3
#define MUSE_OBD_CARD_STALE_MS 3000u   /* the VTEC word's freshness window only */

/* Builds one card on each tile, in the order above. LVGL task (or the
 * display lock held). */
void muse_obd_cards_build(lv_obj_t *const tiles[MUSE_OBD_CARDS]);

/* Forgets the cards, before their tiles are deleted; a later build starts
 * afresh. LVGL task. */
void muse_obd_cards_drop(void);

/* Refreshes them from the snapshot; touches only what changed, and nothing
 * without cards. LVGL task. */
void muse_obd_cards_tick(void);
