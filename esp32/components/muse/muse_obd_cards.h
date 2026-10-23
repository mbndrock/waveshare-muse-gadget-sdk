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
 * muse_obd_status(). "Temps": coolant, intake air, ambient. "Engine": RPM,
 * timing advance, load beside MAP, air-fuel ratio. "Drive": speed (km/h and
 * mph), throttle beside fuel level, short- and long-term fuel trim.
 *
 * A reading older than MUSE_OBD_CARD_STALE_MS is dimmed, one not received yet
 * reads "...", and one the support scan proved unsupported reads "--". While
 * the link isn't polling, each card shows the link's state instead.
 */
#define MUSE_OBD_CARDS 3
#define MUSE_OBD_CARD_STALE_MS 3000u

/* Builds one card on each tile, in the order above. LVGL task (or the
 * display lock held). */
void muse_obd_cards_build(lv_obj_t *const tiles[MUSE_OBD_CARDS]);

/* Forgets the cards, before their tiles are deleted; a later build starts
 * afresh. LVGL task. */
void muse_obd_cards_drop(void);

/* Refreshes them from the snapshot; touches only what changed, and nothing
 * without cards. LVGL task. */
void muse_obd_cards_tick(void);
