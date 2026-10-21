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
  - `esp32/components/muse/muse_obd_elm.[ch]`: plain-C ELM327 byte-stream
    parser (bytes in one at a time, complete at '>', CR to '\n', NULs and
    control bytes dropped, overflow cut) + `muse_elm_parse_rpm()` (first
    `41 0C A B` line, spaces/case tolerated; `NO DATA`, `?`, `UNABLE TO
    CONNECT`, `CAN ERROR`, `BUS INIT: ...ERROR`, `STOPPED` and friends
    classified) + the trigger (starts DISARMED; arms at rpm <= 5500; fires
    once at >= 6000; >= 30 s between fires; a re-armed crossing inside the
    cooldown is swallowed and needs a fresh drop, so a fire never lands late
    mid-pull; wrap-safe ms clock; a dropped link disarms but keeps the
    cooldown). Boundary choice: "drops below 5500" is implemented as <= 5500,
    same as the initial arm.
  - `esp32/components/muse/muse_obd.[ch]`: NimBLE central task (PSRAM stack,
    prio 3), started from `muse_app.c` after `muse_ble_apply()` under
    `#if CONFIG_MUSE_OBD_VTEC`. Waits for `muse_link_ble_started()` +
    `ble_hs_synced()`; active scan 10 s at 100 ms interval / 30 ms window
    (leaves the radio to Wi-Fi) matching name prefix "LELink" or 16-bit
    service FFE0 in adv or scan response; connect; discover FFE0 -> FFE1
    (must have notify + write/write-no-rsp) -> CCCD 0x2902 (falls back to
    val+1 with a log line); write 01 00; ATZ (3 s) ATE0 ATL0 ATH0 ATS0 ATSP0
    (1.5 s each), each waiting for '>'; then 010C back to back. Backoff
    2 s doubling to 60 s, reset after a session that got answers. All GAP/
    GATT procedures are async; callbacks on the host task only post to a
    queue / stream buffer and notify; the task never calls into NimBLE from a
    callback context and the host task never blocks. On FIRE:
    `muse_state_start_vtec()` + `muse_hatch_say("VTEC just kicked in yo!")`.
    No change to `muse_chat_session.cpp`.
  - Hook dispatch: no change was needed to `ble_server.c` / `muse_glue.c` /
    `muse_ble.c`. NimBLE delivers every event of a central-initiated
    connection (CONNECT, DISCONNECT, NOTIFY_RX, MTU, ...) to the callback
    passed to `ble_gap_connect()`, and discovery events to the one passed to
    `ble_gap_disc()`; Link's `gap_event_cb` (and so `muse_ble_gap_event`)
    only ever sees the connections it advertised for. The OBD client
    therefore has full visibility of its own connection and the pairing
    peripheral path is untouched. (Documented at the top of `muse_obd.c`.)
  - Deviation, deliberate: the 1500 ms "drop and reconnect" timeout applies
    while RPM frames are flowing. The first 010C after ATSP0, and any request
    after `NO DATA`/an error, gets 12 s because the ELM may be running its
    protocol search (`SEARCHING...`, several seconds); with a flat 1500 ms the
    link would reconnect forever with the engine off. After a non-RPM reply it
    also waits 500 ms before asking again so it doesn't hammer the bus.
  - Config: `CONFIG_MUSE_OBD_VTEC` in `components/muse/Kconfig` (default n,
    depends on `MUSE_HATCH && BT_NIMBLE_ROLE_CENTRAL && BT_NIMBLE_ROLE_OBSERVER`).
    `devices/sdkconfig.muse-waveshare-s3-18` only: CENTRAL=y, OBSERVER=y,
    MAX_CONNECTIONS=2, MUSE_OBD_VTEC=y. `muse_obd.c`/`muse_obd_elm.c` are
    added to CMake only under the option; `muse_state_start_vtec()` /
    `muse_state_vtec()`, the `.vtec` line in `muse_ui.c` and the start call
    are all under `#if CONFIG_MUSE_OBD_VTEC`. Not gated (cannot be: host-
    compiled and Kconfig-free): the `float vtec` field + `MUSE_VTEC_S` in
    `muse_pixel.h` and the draw path in `avatar/muse_pixel.c`, which is inert
    at 0 (verified byte-identical, see Host QA).
  - Avatar: `muse_pixel.h` `float vtec;` + `#define MUSE_VTEC_S 3.0f`;
    `muse_state.[ch]` start/progress mirroring dizzy (esp_timer, 0 unless
    IDLE, start pokes and ends drowsing quietly, clears the other reactions,
    sleep clears it); `muse_ui.c` `.vtec = muse_state_vtec()`;
    `avatar/muse_pixel.c` `RE_VTEC` in `react_setup` (takes priority over
    the other reactions), `react_face` and the foreground: jolt (squash down,
    "pushed back in the seat"), arms up, wide eyes (`EYES_WIDE`), raised
    brows, a 7 px D-shaped grin with tongue that opens a row on each rev,
    a 1 px whole-avatar buzz (new `react_t.shake`, toggles every 40 ms =
    every frame at the UI's 25 fps) and a "VTEC!" banner from new V/T/E/C/!
    glyphs with a dark-red drop shadow, white flash as it lands then red
    (`C_VTEC` 0xff3b30 / `C_VTECD` 0x8c1414), dither-faded in and out.
    Jollybot licence header updated to list VTEC; kept in its own commit.
  - Docs: README feature row + licence notice, CLAUDE.md feature bullet.
  - Tests: `esp32/tests/test_muse_obd_elm.py` (+ harness),
    `esp32/tests/test_muse_pixel_vtec.py` (+ driver).
- Host QA:
  1. ELM parser + trigger: `tests/muse_obd_elm_harness.c` built with
     `cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined`:
     zero warnings, PASS. Covers ATZ ("ATZ\r\r\rOBDII v1.5\r\r>") cut at
     every chunk size 1..len; "SEARCHING..." then five `41 0C` frames incl.
     ATS0 form, lower case, an embedded NUL and two replies in one
     notification, at chunk sizes 1..20 (750/5500/6000/6784/16383 rpm);
     NO DATA, ?, UNABLE TO CONNECT, CAN ERROR, BUS INIT ERROR, STOPPED, echo,
     truncated / wrong-PID / headers-on frames; overflow; trigger: no fire
     while unarmed, 5501..5999 doesn't arm, arm at 5500 + fire at 6000 once,
     hovering doesn't re-arm, re-arm inside cooldown swallowed and not fired
     late, fires after cooldown, exact cooldown edge, disarm on link drop,
     negative rpm ignored, millisecond wrap.
  2. Avatar: `avatar/muse_pixel.c` compiled alone with `-std=c11 -O2 -Wall
     -Wextra`: zero warnings. `tools/muse/anim.c` still builds against it.
     anim.c can't drive reactions, so `tests/muse_pixel_vtec_harness.c`
     (includes the renderer unchanged to read its grid) renders vtec forced
     at 0, 0.02, 0.05, 0.08, 0.12, 0.3, 0.5, 0.7, 0.85, 0.9, 0.97 plus the
     whole 3 s at 40 ms, ASan/UBSan, -Werror: PASS. Asserts: banner pixels
     exactly the V T E C ! pattern at 0.12-0.7; no avatar pixel in rows 0-7
     (banner clears the head) at every vtec > 0; open grin (tongue) + eye
     shine present; nothing but background/aura/shadow on the grid's four
     edges in every frame (so nothing clipped); body x-centroid buzzes >= 0.8
     px, alternates on > 3/4 of frames, and wanders no more than idle's
     breathing + 1.3 px; no banner at 0 and 0.97. Frames inspected by eye:
     banner legible, face excited, nothing clipped. Regression: 2,500 frames
     (all 7 modes, pet, dizzy/sleepy/waking/tickle sweeps) rendered with the
     previous `muse_pixel.c` and the new one hash identically
     (58bf688e67a6543e) - with vtec at 0 Jollybot renders exactly as before.
  3. BLE client: no ESP-IDF here, so `muse_obd.c` was type-checked with
     `cc -fsyntax-only -Wall -Wextra` against the real esp-nimble headers
     (espressif/esp-nimble, branch nimble-1.5.0-idf; FreeRTOS/esp_log
     stubbed): no diagnostics in muse_obd.c. API signatures (ble_gap_disc,
     ble_gap_connect, ble_gattc_disc_*, ble_gattc_write_flat / _no_rsp_flat,
     callback typedefs) checked against those headers.
  4. Full host suite `python3 -m unittest discover -s tests -p 'test_*.py'`:
     188 run, 4 skipped, 4 errors - all four are setUpClass failing on the
     missing `managed_components/espressif__cjson` (needs `idf.py
     reconfigure`, not possible here); none of them build files this branch
     changes. Every other test passes, including the Link BLE/pairing
     contract tests.
- Preview file: `~/workspace/gadget-preview/vtec-reaction-v1.gif` (the full
  reaction with 0.5 s idle either side, 40 ms frames, 320 px) and
  `~/workspace/gadget-preview/vtec-reaction-v1-sheet.png` (the forced
  progress points). Temporary frame dumps in /tmp were deleted.
- Commits on obd-vtec-reaction:
  - 88a11b0 OBD VTEC: ELM327 parser, RPM trigger, Kconfig and s18 overlay
  - a4ed6d5 VTEC reaction: state and pose plumbing
  - 8e58945 OBD VTEC: BLE central client for the LELink^2 adapter
  - 0671187 Jollybot: VTEC reaction (pose.vtec) (Jollybot-only commit)
  - 76c3d38 Tests: host driver for the avatar's VTEC reaction
  - the commit adding these Results and the README/CLAUDE.md docs
- Unverified until Mac build + on-car test:
  - The firmware build itself (Kconfig resolution with the overlay, link,
    `check_sizes`), and that the other boards build unchanged. If IDF 6.0.1's
    NimBLE also needs `CONFIG_BT_NIMBLE_GATT_CLIENT` (y by default when
    CENTRAL is on), confirm it's y in `build-muse-waveshare-s3-18/sdkconfig`.
  - Anything on the radio: scanning / connecting while Link advertises or a
    phone is connected (2 connections), Wi-Fi + BLE coexistence while TTS
    streams, the adapter's real advertisement (name in adv vs scan response),
    FFE1's real properties (write vs write-no-rsp) and CCCD handle, the real
    reply fragmentation, how long the car's protocol search takes, and
    reconnect after the ignition cycles.
  - That `muse_hatch_say()` actually speaks at the moment of a fire (it drops
    the line when the session is busy or offline - in a car that's whenever
    the phone hotspot is down).
  - The reaction is idle-only by spec, so once the spoken line starts
    (SPEAKING mode) the animation is hidden; on device it will likely show
    for roughly the TTS latency (~0.5-1 s) and then give way to the speaking
    pose. Worth judging on the car; a follow-up could let vtec run on top of
    SPEAKING.
  - A fire while the screen is asleep plays the line but the reaction isn't
    seen (start_vtec pokes, like dizzy, but doesn't wake the screen).
  - Device frame timing for the 1 px buzz (40 ms toggle assumes ~25 fps).
  - Follow-ups (v1 scope): no Settings toggle - the reaction is live whenever
    the OBD link is up; no bench/console key to trigger the reaction without
    a car; adapter name / thresholds are compile-time constants.
