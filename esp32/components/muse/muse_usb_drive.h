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

/*
 * USB drive mode (CONFIG_MUSE_USB_DRIVE, s18 only): the gadget enumerates as
 * a read-only USB mass-storage device whose files are the closed OBD trip
 * logs (muse_msc_fat.h), so the trips show up in Finder with no Muse and no
 * terminal involved.
 *
 * Entry is a controlled reboot, not a live stack swap: on this board the
 * console uses the USB Serial/JTAG block and TinyUSB needs the USB OTG
 * peripheral, and both share the one USB PHY (GPIO19/20), so they cannot run
 * at once. Settings' "USB drive mode" toggle sets the NVS flag "usb_drv"
 * (namespace "muse") and reboots; at boot the flag makes muse_app_run start
 * the MSC stack instead of letting muse_input install the Serial/JTAG
 * console. Toggling off clears the flag and reboots back. ROM download-mode
 * flashing is untouched: it uses the ROM bootloader over the same pins
 * before any firmware runs, in both modes.
 *
 * The snapshot is the closed-trip list at the moment the mode starts
 * (muse_obd_log_list lists only closed trips); trips that close while the
 * mode is active appear only after the mode is re-entered.
 */

/* True when this boot is (or should be) in drive mode. Reads NVS; call
 * after muse_settings_init. The result is cached for muse_usb_drive_active. */
bool muse_usb_drive_requested(void);
/* Cached flag for hot paths (console install, Settings row). */
bool muse_usb_drive_active(void);
/* Sets/clears the NVS flag. The caller reboots to make it take effect. */
void muse_usb_drive_set(bool on);
/* Builds the snapshot from the closed trips and starts TinyUSB MSC.
 * Once, at boot, when muse_usb_drive_requested() is true. */
void muse_usb_drive_start(void);
