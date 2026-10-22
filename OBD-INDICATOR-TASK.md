# OBD round 2: status-bar car indicator, silent reactions, warmup thumbs-up

Three changes Derek asked for on 2026-10-22, all committed directly to
`obd-vtec-reaction` (no new branch). Read `CLAUDE.md`, `esp32/AGENTS.md`,
`VTEC-TASK.md`, `COOLANT-TASK.md` and `OBD-STATUS-TASK.md` for how the
existing OBD work is built. Do not touch `main` or `secrets/`.

## 1. Car-connected indicator in the top status bar

The face has a status row (`esp32/components/muse/muse_ui.c`, built around
line 1001, updated in the tick around line 1665) holding the Wi-Fi icon
label, the BLE icon label and the battery readout, all in
`lv_font_montserrat_14`.

- Add one more small label to that same row, in the same font and style:
  the text `OBD`. It is visible only while the gadget is connected to the
  car adapter (the OBD status from `muse_obd_status()`: subscribed, or
  connected and polling), and hidden otherwise. Accent colour while
  polling, dim otherwise, matching how the BLE icon is coloured.
- Gate everything on `CONFIG_MUSE_OBD_VTEC` so non-OBD builds are
  byte-for-byte unaffected in behaviour.
- Keep the row's existing layout stable: the Wi-Fi, BLE and battery items
  must not move or resize when the OBD label appears or disappears (give
  the label a fixed slot in the row's layout, hidden with
  `LV_OBJ_FLAG_HIDDEN` when off).
- Update it in the same tick that refreshes the Wi-Fi and BLE labels, and
  only touch the label when its state actually changes, like its
  neighbours.

## 2. No more spoken lines for the VTEC and coolant reactions

Derek: when the spoken line plays, the speaking pose replaces the reaction
face, which defeats the point of the reaction. The reactions stay visual
only.

- In `esp32/components/muse/muse_obd.c`, `muse_obd_fire_vtec()` and
  `muse_obd_fire_coolant()` keep the `muse_state_start_*()` call and lose
  the `muse_hatch_say(...)` call. Remove the now-unused line constants.
- Update the stale words that say otherwise: the `muse_obd.h` comment
  ("Play a reaction and say its line"), any CLAUDE.md bullet and the
  branch README section that describe the reactions as spoken.
- Nothing else may change about the triggers: thresholds, arming,
  sustain, cooldowns and the state plumbing stay exactly as they are.
  Pushes and normal spoken replies are untouched (this is only about the
  two car reactions).

## 3. New reaction: coolant reached operating temperature (thumbs-up)

When the engine warms up, Jolly gives a thumbs-up. Silent, like the other
car reactions now are.

- Trigger, in the pure-C layer (`muse_obd_elm.c/.h`) next to the VTEC and
  coolant triggers, host-testable the same way:
  - `MUSE_WARMUP_FIRE_C 82` and `MUSE_WARMUP_COLD_C 70`.
  - It may only fire after this drive has been seen cold: a valid coolant
    reading of 70 C or below arms it. A first-ever reading already at
    operating temperature (hot restart, reconnect mid-drive) must NOT
    fire it.
  - Once armed, the first valid reading at 82 C or above fires it, once.
    It then stays quiet until a valid reading at 70 C or below re-arms it.
  - Absent or invalid coolant readings change nothing.
  - `drop()` in `muse_obd.c` re-initialises the trigger (a new connection
    starts unarmed and must see cold again), like the link state reset.
- Firing: `muse_obd_fire_warmup()` in `muse_obd.c` (state start only, no
  speech), declared in `muse_obd.h`, plus a `>warmup` bench console
  command in `muse_input.c` gated exactly like `>vtec` and `>coolant`.
- Avatar: a new reaction alongside VTEC and coolant:
  `muse_state_start_warmup()`, `MUSE_WARMUP_S 3.0f`, a `warmup` field in
  the pose struct in `muse_pixel.h`, and the drawing in
  `esp32/avatar/muse_pixel.c`. Priority is below coolant and below VTEC
  (it is a pat on the back, not an alarm): if either of those is showing,
  the warmup pose does not replace it.
- The pose: Jolly pleased, one arm raised beside the head with a clear
  thumbs-up (a fist with the thumb extended upward), happy closed or
  bright eyes and a smile; a small bounce is fine. It must read as a
  thumbs-up at the panel's size, in the same pixel style as the VTEC and
  coolant poses, and the VTEC and coolant frames must stay byte-identical
  to before (the existing refactor rule).
- The poll loop feeds every valid coolant reading to the warmup trigger
  (coolant is already read every 4th request; do not change the poll
  cadence).

## Tests and docs

- Extend the OBD ELM host tests for the warmup trigger: cold start fires
  once at 82, no refire while hot, re-arm at 70 and fire again on the
  next warmup, hot first reading never fires, invalid readings ignored.
- Extend the avatar host tests for the warmup pose the way the VTEC and
  coolant poses are covered (pose plumbing and priority below VTEC and
  coolant).
- If any existing test asserts the spoken VTEC or coolant lines, update it
  to the new visual-only behaviour and say so in Results.
- Update the branch README car section: indicator in the status bar,
  reactions are visual only, the warmup reaction and its thresholds, the
  `>warmup` bench command.
- Run the OBD test subset and the full host suite from `esp32/`
  (`python3 -m unittest discover -s tests -p 'test_*.py'`). Baseline on
  this branch: 649 tests, 6 errors (all the missing-cJSON environment
  set), 0 failures, 10 skipped. No ESP-IDF on this VM: no board build, no
  downloading cJSON.
- Render a preview of the warmup pose with the repo's host animation
  renderer, the way the VTEC and coolant previews were made, and put the
  file path in Results so Muse can inspect it.

## Results (Claude Code fills this in)

- Pending.
