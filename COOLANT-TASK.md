# Coolant temperature warning reaction (S2000), same branch

Branch: `obd-vtec-reaction` (work ONLY on this branch; never commit to or
push `main`; never touch `secrets/`). This branch is Derek's S2000 car
reactions branch; the VTEC reaction from VTEC-TASK.md already lives here.
Extend it, do not restructure it.

## Goal

While the gadget is connected to the LELink^2 (see VTEC-TASK.md for the
adapter and the BLE client in `muse_obd.c`), also watch coolant
temperature. When the coolant is genuinely too hot, the avatar fires a
worried warning reaction with a "HOT!" banner and the gadget speaks, in
its configured ElevenLabs voice, exactly:

    Coolant temperature is high!

## Data

- Mode 01 PID 0x05: coolant_c = A - 40 (single byte A of the `41 05`
  reply).
- Poll schedule: interleave into the existing back-to-back RPM loop in
  `muse_obd.c`: every 4th request is "0105", the rest stay "010C". The
  K-line bus is slow; coolant moves slowly, so this cadence is plenty
  and RPM responsiveness must not degrade.
- Extend the pure-C parser in `muse_obd_elm.c` to surface `41 05`
  replies with the same defensive parsing as RPM (fragmentation
  anywhere, spaces, "SEARCHING...", "NO DATA", "?"). No BLE code in
  the parser.

## Trigger rules (constants beside the VTEC ones in muse_obd_elm.h)

- MUSE_COOLANT_FIRE_C 105, MUSE_COOLANT_REARM_C 100,
  MUSE_COOLANT_SUSTAIN_MS 5000, MUSE_COOLANT_COOLDOWN_MS 120000,
  engine-running gate: latest RPM >= 400.
- FIRE once when coolant has been >= 105 C CONTINUOUSLY for 5000 ms
  (the sustain timer resets on any reading below 105) while the engine
  is running. Readings with the engine off never build sustain and
  never fire (a hot soak with the key on is not an alarm).
- After a fire: no re-fire for 120000 ms, and not at all until coolant
  has dropped to <= 100 C (re-arm).
- On FIRE, `muse_obd.c` calls `muse_state_start_coolant()` and
  `muse_hatch_say("Coolant temperature is high!")`. No changes to
  muse_chat_session.cpp; say still self-gates when busy or offline.

## Avatar reaction (same recipe as the VTEC one)

- `muse_pixel.h`: add `float coolant;` to muse_pose_t and
  `#define MUSE_COOLANT_S 4.0f`.
- `muse_state.h` / `muse_state.c`: `muse_state_start_coolant()` and
  `muse_state_coolant()`, mirroring the vtec pair (esp_timer progress,
  reads 0 unless MUSE_MODE_IDLE, start pokes and ends drowsing).
- `muse_ui.c` pose assembly: add `.coolant = muse_state_coolant(),`.
- `esp32/avatar/muse_pixel.c`: new RE_COOLANT draw path. Worried, not
  celebratory: raised inner brows, an uneasy wobbly mouth, sweat drops
  at the sides of the head, steam wisps above the head, and a "HOT!"
  banner above the head in orange-red (glyphs H, O, T are new; "!"
  already exists from the VTEC banner). Envelope in/out with progress
  like the other reactions. Nothing draws outside the 64x64 grid.
- Priority: in react_setup, the coolant reaction OUTRANKS vtec when
  both are active. Safety beats fun.
- The Jollybot drawing change goes in its own commit, like the VTEC
  one, so it can be removed on request.

## Docs

- Extend the README feature row and the CLAUDE.md VTEC bullet to cover
  the coolant warning on this branch. Leave VTEC-TASK.md as is.

## Host QA (required; NO ESP-IDF on this VM, no idf.py builds)

1. Extend the ELM host harness: `41 05` decoding (105 C is byte 0x91;
   also verify the -40 offset on a cold reading, e.g. 0x3C -> 20 C),
   fragmented replies, and the trigger: sustain build/reset, the
   engine-off gate, cooldown, re-arm at <= 100 C. -Wall -Wextra -Werror
   clean, sanitizers on, all passing.
2. Avatar: host-compile muse_pixel.c clean; a coolant harness like the
   vtec one checks the banner is complete and legible, clears the head,
   face reads worried, nothing clipped. Regression: with vtec = 0 AND
   coolant = 0, frames across all modes and the other reactions stay
   byte-identical to the current renderer.
3. Preview with a FRESH filename (client caches by name):
   ~/workspace/gadget-preview/coolant-reaction-v1.gif plus a frame
   sheet PNG. Clean up large temp dumps; /tmp is a small tmpfs.

## Results (Claude Code fills this in)

- Implemented:
  - `esp32/components/muse/muse_obd_elm.[ch]`: `muse_elm_parse_coolant()`
    (`41 05 A` -> A - 40 deg C, new result `MUSE_ELM_COOLANT`). RPM and coolant
    now share one PID line parser, so the coolant path has exactly RPM's
    defences: fragmentation anywhere (the byte feeder is unchanged), spaces,
    case, `SEARCHING...`, `NO DATA`, `?`, the bus errors, echo, cut-short and
    headers-on frames. No BLE in it. The trigger sits beside VTEC's:
    `MUSE_COOLANT_FIRE_C 105`, `MUSE_COOLANT_REARM_C 100`,
    `MUSE_COOLANT_SUSTAIN_MS 5000u`, `MUSE_COOLANT_COOLDOWN_MS 120000u`,
    `MUSE_COOLANT_RUNNING_RPM 400`, and `muse_coolant_init/reset/update()`.
    Choices the spec left open:
    - Any reading with the engine off or its RPM unknown restarts the 5 s
      sustain as well as not building it. A stall halfway through therefore
      starts over: the 5 s must all be "hot while running".
    - It starts **armed** (unlike VTEC, which starts disarmed so a mid-pull
      connect doesn't fire). Connecting to a car that's already overheating
      should warn.
    - Once the coolant has re-armed and stayed hot, a fire held back only by
      the cooldown lands when the 120 s run out. VTEC swallows that case,
      but an overheat that's still going on is worth repeating.
    - A dropped link restarts the sustain, and arming and the cooldown carry
      over. Readings outside -40..215 are ignored.
  - `esp32/components/muse/muse_obd.c`: in the back-to-back loop every 4th
    request is `0105`, the rest `010C`. Only RPM replies set the
    POLL_MS/SEARCH_MS choice and the 500 ms pause after a non-answer. A
    `NO DATA` to `0105` (an ECU without PID 05) is skipped, so RPM polling
    doesn't slow down. The latest RPM gates the trigger. It becomes -1 after a
    non-RPM reply to `010C` and on a link drop. On FIRE:
    `muse_state_start_coolant()` + `muse_hatch_say("Coolant temperature is
    high!")`. No change to `muse_chat_session.cpp`. Kconfig help and
    `muse_obd.h` describe the warning. It stays under `CONFIG_MUSE_OBD_VTEC`,
    with no new option, so the s18 overlay and every other board are as before.
  - `muse_pixel.h`: `float coolant;` + `#define MUSE_COOLANT_S 4.0f`.
    `muse_state.[ch]`: `muse_state_start_coolant()` / `muse_state_coolant()`
    mirror the vtec pair (esp_timer, 0 unless IDLE, start pokes and ends
    drowsing quietly, clears dizzy/waking/tickle, sleep clears it). It does
    not clear a running VTEC; the renderer's priority shows the warning over
    it. `muse_ui.c`: `.coolant = muse_state_coolant(),` beside `.vtec`, gated
    the same way.
  - `esp32/avatar/muse_pixel.c` (Jollybot-only commit): `RE_COOLANT`, checked
    first in `react_setup`, so it outranks VTEC and everything else. The
    sequence:
    - A flinch ("uh-oh": wide eyes, O mouth).
    - He hunches (squash 0.9, which keeps the head low) and frets: hands to
      his cheeks, a side-to-side fidget, flushed cheeks.
    - Worried face: brows raised at the inner ends, eyes darting left and
      right, an uneasy 6 px wobbly mouth that flips at 4 Hz.
    - Sweat drops (`C_TEAR` with a white glint) slide down each side of the
      head, following its outline. The sides take turns.
    - Steam wisps (new `C_STEAM`/`C_STEAMD`): two rise and drift out past
      the banner's sides, and two short ones die out above the crown, below
      the banner. A guard box keeps all steam out of the banner.
    - The "HOT!" banner: new H and O glyphs. T and ! come from the VTEC
      banner (its glyphs moved to file scope and are shared, not copied).
      Orange-red `C_HOT` 0xff5a1f with a `C_HOTD` 0x7a2008 drop shadow, a
      white flash as it lands, then a dither fade out, with the same timing
      as VTEC's.
    VTEC's banner now goes through the shared `draw_banner()`, and is
    verified byte-identical (below). The licence header lists "coolant".
  - Docs: README feature row + Jollybot notice; CLAUDE.md's VTEC bullet now
    also covers the coolant warning. VTEC-TASK.md is unchanged.
- Host QA (all on this VM, no ESP-IDF):
  1. ELM: `tests/muse_obd_elm_harness.c` +
     `components/muse/muse_obd_elm.c` with `cc -std=c11 -Wall -Wextra
     -Werror -fsanitize=address,undefined`: zero warnings, PASS (all the old
     VTEC checks still pass).
     - Decoding: 0x91 -> 105 C, 0x3C -> 20 C (the -40 offset), 0x00 -> -40,
       0xFF -> 215, 0x8C -> 100, 0x7B -> 83, 0x92 -> 106.
     - Fragmentation: `SEARCHING...`, a NUL, ATS0 and lower-case forms
       interleaved with an RPM reply, at every chunk size from 1 to 24. RPM
       and coolant replies never read as each other.
     - Rejected: NO DATA / ? / UNABLE TO CONNECT / BUS INIT ERROR / STOPPED,
       echo, cut short, half a byte, wrong PID, headers on, non-hex.
     - Accepted: odd spacing and trailing bytes. A frame wins over NO DATA,
       and with two answers the first ECU wins.
     - Trigger:
       - Sustain fires at exactly 5000 ms, not at 4999.
       - It fires once while it stays hot.
       - After the cooldown it stays quiet until <= 100 C (101 doesn't
         re-arm).
       - Re-armed inside the cooldown, it fires right at the cooldown edge.
       - A dip to 104 restarts the sustain.
       - Engine off (0 rpm, unknown, 399 rpm) for 3 minutes of 112 C
         readings never fires. 400 rpm counts as running. A stall mid-sustain
         restarts it.
       - A link reset restarts the sustain and keeps it disarmed after a
         fire.
       - Out-of-range readings are ignored.
       - Millisecond wrap is handled for both the sustain and the cooldown.
     - Mutation check: removing the engine gate, the re-arm gate, the sustain
       or the cooldown, re-arming at 104, dropping the -40 offset, or a no-op
       reset each make the harness fail.
  2. Avatar:
     - Build: `avatar/muse_pixel.c` alone with `-std=c11 -O2 -Wall -Wextra`
       gives zero warnings.
     - The new `tests/muse_pixel_coolant_harness.c` (ASan/UBSan, -Werror)
       PASSes. It renders forced points 0, 0.02, 0.05, 0.08, 0.12, 0.3, 0.5,
       0.7, 0.85, 0.9 and 0.97, plus the whole 4 s at 40 ms.
     - Banner: exactly the H O T ! pattern at 0.12-0.7, with no steam, sweat
       or avatar inside its box.
     - Clear of the head: no avatar cell in rows 0-7 at any coolant > 0, and
       the head top never above row 9.
     - Worried face: both brows present, slanted and raised at the inner end;
       no tongue or grin; at least 6 mouth cells; eyes open. The mouth
       wobbles and the eyes dart across frames.
     - Sweat on both sides in at least 20 frames. Steam beside and below the
       banner, never inside the head.
     - No banner, steam or sweat at 0 and 0.97.
     - Nothing but background, aura or shadow on the four edges in any
       frame, so nothing is clipped.
     - Priority: coolant + vtec=0.5 is byte-identical to coolant alone.
     - Idle only: every other mode is unchanged by coolant=0.5.
     - Mutation check: putting VTEC first, breaking the H glyph, removing a
       brow, or a startle that lifts the head are each caught.
     - Frames inspected by eye: banner legible, the face reads worried and
       nothing is clipped.
     - Regression: a 2,465-frame sweep hashes identically (eb32f11848b9f98b)
       through the pre-change renderer (HEAD f2c2d31) and the new one. It
       covers all 7 modes, pet, the dizzy, sleepy, waking, tickle and full
       VTEC sweeps, and reactions in non-idle modes, all with coolant at 0.
       VTEC frames are unchanged too, so the shared-banner refactor is exact.
     - The VTEC harness still passes.
  3. Firmware sources, type-checked only (no IDF):
     - `muse_obd.c` with `cc -fsyntax-only -Wall -Wextra -Wformat=2` against
       the real esp-nimble headers (espressif/esp-nimble, nimble-1.5.0-idf,
       shallow clone, since deleted; FreeRTOS, esp_log as printf, and
       esp_err stubbed): no diagnostics in muse_obd.c.
     - `muse_state.c` with `CONFIG_MUSE_OBD_VTEC` at 0 and at 1 (FreeRTOS
       and esp_timer stubbed): no diagnostics.
     - `muse_ui.c` was not compiled (LVGL). Its change is the one gated
       `.coolant =` line.
  4. Full host suite `python3 -m unittest discover -s tests -p 'test_*.py'`:
     190 ran (188 + the 2 new coolant-avatar tests), 4 skipped, 4 errors. The
     4 errors are the same setUpClass failures as on the VTEC run, from the
     missing `managed_components/espressif__cjson` (needs `idf.py
     reconfigure`). Everything else passes.
- Preview file: `~/workspace/gadget-preview/coolant-reaction-v1.gif` (the full
  4 s warning with 0.5 s of idle either side, 40 ms frames, 320 px) and
  `~/workspace/gadget-preview/coolant-reaction-v1-sheet.png` (the forced
  progress points). Both are new names. The temporary frame dumps, scratch
  binaries and the esp-nimble clone were deleted from the home scratch
  directory (nothing was put in /tmp beyond the test suite's own
  self-cleaning temp dirs).
- Commits on obd-vtec-reaction:
  - 8cd173c OBD coolant: PID 05 parser and the coolant warning trigger
  - 3eb74ee Coolant warning: state and pose plumbing
  - 859f806 OBD coolant: read coolant every 4th poll and fire the warning
  - 3c69a85 Jollybot: coolant warning (pose.coolant) (Jollybot-only commit)
  - 9fcd9c9 Tests: host driver for the avatar's coolant warning
  - the commit adding these Results and the README/CLAUDE.md docs
- Unverified until Mac build + on-car test:
  - The firmware build (s18 with the overlay, `check_sizes`), and that the
    other boards build unchanged. `muse_ui.c` hasn't been compiled at all.
  - The real `41 05` replies from Derek's S2000 through the LELink^2: that
    the ECU answers PID 05, the reply's framing, and the actual poll cadence
    on the car's bus. On a slow K-line, every 4th request may mean a reading
    every second or two. That's fine for the 5 s sustain, but nobody has
    measured it, nor confirmed that RPM latency for VTEC stays as before.
  - Real thresholds: S2000 coolant normally runs around 80-95 C, and the fan
    cuts in near 95-100 C. 105 C sustained should only happen in a genuine
    overheat, but it's untested.
  - That `muse_hatch_say()` actually speaks at the moment of a fire. It drops
    the line when the session is busy (say, mid-VTEC line) or offline, and a
    dropped warning isn't retried until the coolant re-arms or the 120 s pass.
    Worth deciding on the car whether the warning should queue or retry.
  - As with VTEC, the reaction is idle-only by spec. Once the spoken line
    starts (SPEAKING mode) the animation gives way to the speaking pose, and
    a fire while the screen is asleep plays the line without the visuals (it
    pokes but doesn't wake the screen). For a safety warning, waking the
    screen and/or showing it over SPEAKING is probably worth a follow-up.
  - Device look: steam and sweat at the panel's real scale and colours, and
    the 4 Hz mouth wobble at the device frame rate.
  - Follow-ups (v1 scope): no Settings toggle (the warning is live whenever
    the OBD link is up), no bench/console key to trigger it without a car,
    and the thresholds are compile-time constants.
