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

- Preview: `~/workspace/gadget-preview/warmup-thumbsup-v1.gif` shows the whole
  3 s reaction with 0.5 s of idle on either side, at 40 ms frames and 320 px.
  `~/workspace/gadget-preview/warmup-thumbsup-v1-sheet.png` shows the forced
  points 0, 0.02, 0.06, 0.1, 0.16, 0.2, 0.3, 0.5, 0.7, 0.8, 0.9 and 0.97. Both
  are new file names, made with the warmup harness's PPM output (the same way
  as the VTEC and coolant previews).
- 1. Status-row indicator (`muse_ui.c`):
  - Everything is under `#if CONFIG_MUSE_OBD_VTEC`: the include, the statics,
    the build code and the tick code.
  - The label reads `OBD`, in `lv_font_montserrat_14`, made with
    `make_label()` like its neighbours. It sits between the BLE icon and the
    battery readout.
  - LVGL's flex layout skips hidden children. To keep the row stable, the
    label lives in a fixed slot: a style-less object sized to the label's
    text width and the font's line height, which always stays in the row.
    Only the label inside it gets `LV_OBJ_FLAG_HIDDEN`. Wi-Fi, BLE and the
    battery never move or resize when it comes and goes. The row is a slot
    wider than before on OBD builds only.
  - Display states:
    - Hidden unless subscribed or polling.
    - Dim (`COLOR_DIM`) when subscribed.
    - Accent (`COLOR_ACCENT`) while polling, matching how the BLE icon is
      coloured.
  - It updates in `update_chrome()`, right after the Wi-Fi and BLE icons. It
    reads `muse_obd_status()`, which never blocks, and touches the label only
    when the state changes (`s_shown_obd`).
- 2. Silent reactions:
  - `muse_obd_fire_vtec()` / `_coolant()` now only call
    `muse_state_start_*()`.
  - Removed: the `VTEC_LINE` / `COOLANT_LINE` constants and the now-unused
    `muse_chat.h` include. `muse_obd.c` has no `muse_hatch_say` left.
  - Reworded to drop "spoken": the `muse_obd.h` comments, the Kconfig help,
    the bench comment in `muse_input.c`, the CLAUDE.md bullets and the README
    feature row and car section.
  - Triggers, thresholds, cooldowns, poll cadence, pushes and normal replies
    are unchanged.
  - No existing test asserted the spoken lines, so none needed updating. A
    new contract test (`test_link_obd.py`
    `test_car_reactions_are_visual_only`) now pins the visual-only behaviour.
- 3. Warm-up thumbs-up:
  - Trigger (`muse_obd_elm.[ch]`):
    - `MUSE_WARMUP_FIRE_C 82`, `MUSE_WARMUP_COLD_C 70`,
      `muse_warmup_trigger_t { bool armed; }`, `muse_warmup_init()` and
      `muse_warmup_update(t, coolant_c)`.
    - It starts disarmed. A reading <= 70 arms it. The first reading >= 82
      while armed fires it and disarms it.
    - Readings outside the PID's -40..215 range change nothing.
    - It has no clock, so nothing is time-based: the spec asked for none.
  - `muse_obd.c`:
    - `got_coolant()` feeds every valid coolant reading to the trigger,
      after the coolant warning.
    - `drop()` and task start call `muse_warmup_init()`.
    - `muse_obd_fire_warmup()` calls only `muse_state_start_warmup()`, and is
      declared in `muse_obd.h`.
    - Poll cadence is unchanged (`COOLANT_EVERY 4`).
  - Console: `>warmup` sits beside `>vtec` / `>coolant` in the same
    `#if CONFIG_MUSE_OBD_VTEC` block, and the bench comment mentions it.
  - State (`muse_state.[ch]`, gated):
    - `muse_state_start_warmup()` / `muse_state_warmup()` mirror the VTEC
      pair: esp_timer progress, 0 unless idle, start pokes and quietly ends
      drowsing, clears dizzy, waking and tickle.
    - Sleep clears it.
    - Like coolant, it leaves VTEC and coolant running; the renderer decides
      priority.
  - `muse_pixel.h`: `float warmup;` and `MUSE_WARMUP_S 3.0f`.
    `muse_ui.c`: `.warmup = muse_state_warmup(),` under the gate.
  - Jollybot (`avatar/muse_pixel.c`, its own commit 9ed4f07):
    - `RE_WARMUP` is checked after coolant and VTEC in `react_setup`.
    - New `react_t.arm_one` (false for every other reaction) lets only the
      right arm take the pose.
    - The paw rises beside the head, tipped away from it. On top sits an
      8x10 fist-and-thumb sprite in the fur's outline, highlight, light and
      crease tones.
    - Face: happy closed eyes, a grin and blush.
    - Motion: a small hop as it lands, two pumps of the thumb, and a glint
      off the thumb tip. The arm lowers from 0.82.
    - The licence header lists "warm-up". There is no Apache header and the
      Meta copyright line is kept.
- Tests and QA (this VM, no ESP-IDF, no cJSON download):
  - ELM: `test_warmup_trigger` in `tests/muse_obd_elm_harness.c` (built with
    `-Werror` and ASan/UBSan) covers:
    - A cold start fires once at exactly 82 (71 and 81 don't), with no refire
      across 82..110.
    - 71 and 90 don't re-arm. 70 re-arms, and the next warm-up fires once.
    - A first reading already hot (90, then 82/75/95) never fires.
    - Invalid readings (-41, -1000, 216, 9999) neither arm nor fire. -40 and
      215 count as real readings.
    - Re-init after a drop requires cold again.
    - Parsed `41 05` replies with NO DATA and `?` mixed in fire exactly once.
    - Mutation check: starting armed, not disarming after a fire, re-arming
      at 71, dropping the range check, or firing only at 83 are each caught.
  - Avatar: new `tests/muse_pixel_warmup_harness.c` +
    `tests/test_muse_pixel_warmup.py`:
    - The renderer builds with `-Wall -Wextra` and no warnings. The harness
      uses `-Werror` and ASan/UBSan.
    - From 0.16 to 0.8, the exact thumbs-up cells appear once, at x >= 44,
      raised beside the head.
    - The thumb tip is clear above, and no more than a 2-cell fur tuft is
      left of it (so it's clear of the head).
    - The left arm is no higher than idle's.
    - Face: happy closed eyes (no shine), a grin with tongue, no brows.
    - No thumbs-up at 0 or 0.97. Under VTEC=0.5 or coolant=0.5 the frame is
      byte-identical to that reaction alone. Other modes are unchanged.
    - Nothing appears on the grid edges in any frame.
    - Over the whole 3 s: the thumbs-up moves and glints.
    - Pose plumbing tests: the `muse_pixel.h` field and duration, the
      `muse_state` pair (gated, sleep clears it), the gated `.warmup =` line
      in `muse_ui.c`, and the coolant > vtec > warmup order in `react_setup`.
    - Mutation check: warmup outranking VTEC, both arms up, no fist, open
      eyes, an inward-tipped arm, or the arm closer to the body are each
      caught. A pump of 0 is not caught on its own, because the breathing
      also moves the fist, so the check is described as "moves", not "pumps".
  - Regression: a 1,884-frame sweep with warmup at 0 hashes identically
    (`d0492f268c160f48`, grid and RGB565 output) through the pre-change
    renderer and the new one. It covers all 7 modes, pet, the dizzy, sleepy,
    waking, tickle, full VTEC and full coolant sweeps, VTEC and coolant
    together, and reactions in non-idle modes. VTEC and coolant frames are
    therefore byte-identical, and the existing VTEC and coolant harnesses
    still pass.
  - `test_link_obd.py` gains 3 tests and one extended test:
    - the gated `>warmup` entry;
    - the fires are visual only;
    - the warmup trigger is fed in `got_coolant()` and re-initialised in
      `drop()`, with cadence 4;
    - the status-row indicator is all inside the gate, with nothing about it
      outside.
  - `muse_state.c` type-checks with `-Wall -Wextra -Wmissing-prototypes
    -Wstrict-prototypes` against FreeRTOS/esp_timer stubs, with
    `CONFIG_MUSE_OBD_VTEC` at 0 and at 1: no diagnostics. `muse_obd.h`
    compiles on its own.
  - Not compiled (they need the IDF, NimBLE or LVGL), reviewed by diff
    instead:
    - `muse_obd.c` (not re-checked against esp-nimble this time; nothing was
      cloned). Its change only removes calls, adds calls to declared
      functions and adds one static.
    - `muse_ui.c`, `muse_input.c`.
  - OBD subset (`test_muse_obd_elm`, `test_link_obd`, `test_muse_pixel_vtec`,
    `test_muse_pixel_coolant`, `test_muse_pixel_warmup`,
    `test_link_transport_contract`): 25 tests, OK.
  - Full host suite `python3 -m unittest discover -s tests -p 'test_*.py'`:
    658 ran (649 + 9 new), 0 failures, 10 skipped. The 6 errors are the same
    missing-cJSON setUpClass set as the baseline (link_discovery,
    link_reterminal_sht4x, link_sensecap_sensors, muse_chat_link_errors,
    muse_chat_link_note, muse_chat_session). The baseline run on this branch
    before any change matched the task's numbers: 649, 6, 0, 10.
- Commits on obd-vtec-reaction (not pushed):
  - 3ba1d1c OBD: VTEC and coolant reactions are visual only (no spoken line)
  - 87573e9 UI: OBD label in the face's status row while linked to the car adapter
  - 4529c5f OBD warmup: coolant operating-temperature trigger and its host tests
  - 8db8140 OBD warmup: state, pose plumbing, firing and the >warmup bench command
  - 9ed4f07 Jollybot: warm-up thumbs-up (pose.warmup) (Jollybot-only commit)
  - f4d7a8e Tests: warm-up thumbs-up avatar driver, OBD indicator and silent-fire contracts
  - the commit adding these Results and the README / CLAUDE.md docs
- Unverified until the Mac build and the car:
  - The s18 build (`check_sizes`) and the other boards building unchanged.
    `muse_ui.c` has not been compiled.
  - Whether the slot's size from `lv_obj_update_layout()` matches on the
    device. The label's look in the real row on the 368x448 panel.
  - The warm-up on the S2000: a cold start should cross 70 -> 82 C within
    minutes. A short stop with the engine still warm won't fire again, by
    design.
  - The thumbs-up at the panel's real scale.
