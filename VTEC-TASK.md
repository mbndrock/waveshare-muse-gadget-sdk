# VTEC reaction: OBD BLE trigger for the s18 gadget

Branch: `obd-vtec-reaction` (work ONLY on this branch; never commit to or
push `main`; never touch `secrets/`).

## Goal

When the gadget is connected over BLE to Derek's LELink^2 ELM327 adapter
and engine RPM crosses 6000, the avatar fires an excited reaction with a
"VTEC!" banner and the gadget speaks, in its configured ElevenLabs voice,
exactly:

    VTEC just kicked in yo!

## Adapter facts (verified from working ESPHome configs, HA community)

- Advertised name: "LELink BLE OBDII" (match name prefix "LELink", or the
  service UUID in the advertisement).
- Service: FFE0 (0000ffe0-0000-1000-8000-00805f9b34fb).
- ONE characteristic, FFE1, used for BOTH write and notify.
- AT commands go as ASCII terminated by \r. Responses arrive FRAGMENTED
  across multiple notifications; accumulate until the '>' prompt byte.
- ATZ on this adapter answers "OBDII v1.5". No PIN / pairing needed.

## Trigger rules

- There is NO standard Mode 01 PID for the VTEC solenoid, and Honda
  Mode 22 PIDs for it are unverified. Do NOT ship a solenoid read.
  RPM is the trigger, per Derek's "either/or" spec.
- Poll Mode 01 PID 0x0C; rpm = ((A * 256) + B) / 4.
- State machine: ARMED once rpm <= 5500; FIRE once when rpm >= 6000;
  minimum 30 s between fires; re-arm only after rpm drops below 5500.

## Verified seams in this codebase (do not rediscover)

- NimBLE host lives in `esp32/main/ble_server.c` (nimble_port_init,
  on_sync/on_reset). Link's hooks are wired in `esp32/main/muse_glue.c`
  (~line 623: .svcs / .configure_host / .on_gap_event = muse_ble_*).
  The OBD central client goes in `esp32/components/muse/` as new
  `muse_obd.c` / `muse_obd.h`. It needs visibility of GAP events for its
  own connection; extend the existing hook dispatch minimally and do not
  disturb the pairing peripheral path.
- Config: `esp32/devices/sdkconfig.muse` forces
  CONFIG_BT_NIMBLE_ROLE_CENTRAL=n and ROLE_OBSERVER=n for every board;
  `esp32/sdkconfig.defaults` sets CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1.
  Override in `esp32/devices/sdkconfig.muse-waveshare-s3-18` ONLY:
  CENTRAL=y, OBSERVER=y, MAX_CONNECTIONS=2. Gate ALL new code behind a
  new Kconfig option `CONFIG_MUSE_OBD_VTEC` in
  `esp32/components/muse/Kconfig` (default n), set =y in the s18
  overlay. Every other board must build unchanged.
- ELM layer: factor the byte-stream parser as pure C with no BLE
  dependencies. Feed bytes in, detect a complete response at '>',
  extract the `41 0C` payload. Parse defensively: tolerate spaces,
  "SEARCHING...", "NO DATA", "?", and fragmentation at any byte.
- Poll task: FreeRTOS task started from `esp32/components/muse/
  muse_app.c` next to the other `_start()` calls (~lines 85-92), only
  under the Kconfig gate. Flow: scan for the adapter, connect as
  central, discover FFE0/FFE1, subscribe to FFE1 notifications (write
  the CCCD), then ELM init: ATZ, ATE0, ATL0, ATH0, ATS0, ATSP0 (wait
  for '>' after each). Then poll "010C" back to back (next request only
  after '>'; 1500 ms timeout means drop the connection and reconnect
  with backoff). Use async GAP/GATT procedures with callbacks (the
  ESP-IDF blecent example pattern); never block the NimBLE host task.
- Spoken phrase: `muse_hatch_say("VTEC just kicked in yo!")` already
  exists and is public in `muse_chat.h`. It speaks through the normal
  ElevenLabs path when idle and silently drops when busy or offline.
  Call it on FIRE. NO changes to muse_chat_session.cpp.
- Avatar reaction, following the existing wupsbr recipe exactly:
  * `muse_pixel.h`: add `float vtec;` to muse_pose_t and
    `#define MUSE_VTEC_S 3.0f`.
  * `muse_state.h` / `muse_state.c`: `muse_state_start_vtec()` and
    `muse_state_vtec()`, mirroring the tickle/dizzy style
    (esp_timer-based progress, progress reads 0 unless MUSE_MODE_IDLE,
    starting it pokes and ends drowsing). No hold semantics.
  * `muse_ui.c` (~line 1860, pose assembly): add
    `.vtec = muse_state_vtec(),`.
  * `esp32/avatar/muse_pixel.c`: extend react_setup and add a draw
    path. While vtec is active: excited face (wide eyes, big grin;
    reuse existing eye/mouth styles where they fit), a 1 px alternating
    vibration of the whole avatar, and a "VTEC!" banner stamped above
    the head with stamp()-style glyph patterns (define V, T, E, C, !
    glyphs) in an accent color. Envelope in/out with progress like the
    other reactions. Nothing may draw outside the 64x64 grid.
- No settings toggle in v1: the reaction is live whenever the OBD link
  is up. Note it as a follow-up in Results.

## Host QA (required; there is NO ESP-IDF on this VM, do not attempt
## idf.py builds - firmware build/flash happens later on Derek's Mac)

1. ELM parser + trigger: standalone host test compiled with cc,
   -Wall -Wextra clean. Feed: a fragmented ATZ reply, "SEARCHING..."
   followed by `41 0C` frames, "NO DATA". Assert decoded RPM values
   and the fire / re-arm / cooldown behavior of the state machine.
2. Avatar: host-compile muse_pixel.c with the existing tools/muse
   anim harness approach, -Wall -Wextra with zero warnings in
   muse_pixel.c. Render frames with vtec progress forced at several
   points (0, early, mid, late). If the harness cannot drive the vtec
   channel, add a minimal test driver; do not change production
   behavior for QA. Inspect the frames: banner legible, face excited,
   nothing clipped.
3. Save a preview (frames sheet or GIF) under
   ~/workspace/gadget-preview/ with a FRESH filename (never reuse a
   path; the client caches by name), e.g. vtec-reaction-v1.gif.
   Clean up large temp frame dumps; /tmp is a small tmpfs.

## Results (Claude Code fills this in)

- Implemented:
- Host QA:
- Preview file:
- Commits on obd-vtec-reaction:
- Unverified until Mac build + on-car test:
