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
- Host QA:
- Preview file:
- Commits on obd-vtec-reaction:
- Unverified until Mac build + on-car test:
