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

/*
 * The VTEC reaction (CONFIG_MUSE_OBD_VTEC): a BLE central, beside Link's own
 * peripheral, to a LELink^2 ELM327 OBD-II adapter. It finds the adapter,
 * subscribes to its FFE1 characteristic, polls engine RPM (01 0C) and, when
 * the trigger in muse_obd_elm.h fires, plays the avatar's VTEC reaction and
 * says "VTEC just kicked in yo!".
 *
 * Starts a task that waits for Link's BLE host, then keeps the adapter
 * connected, reconnecting with backoff. Call once, after the UI and the
 * voice session are up.
 */
void muse_obd_start(void);
