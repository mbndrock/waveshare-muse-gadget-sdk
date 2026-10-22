# OBD-PRESENCE-TASK: cards only in the car, helmet while connected, Settings kill switch (branch obd-vtec-reaction)

Owner: Derek. Spec by Muse, implementation by Claude Code, QA by Muse, flash by Derek on the Mac.
Builds on OBD-CARDS-TASK (tip 2b62428, which includes the ESP-IDF format-truncation fix). Three changes, all gated on `CONFIG_MUSE_OBD_VTEC` as usual. Derek's words: the cards should only show up when the gadget is connected to his OBD adapter, otherwise it should look like the normal gadget (face + Settings); the little Muse guy should wear a race car helmet while connected; and Settings needs a global option to completely disable the OBD features and connection.

## Part 1: cards exist only while the OBD link is up

Today the three card tiles (Temps, Engine, Drive) are permanent tiles between the face and Settings. Change to:

- The tileview holds exactly two tiles (face, Settings) whenever the OBD link is not connected, exactly like a non-OBD build. Page dots match the tile count, as they do today for two tiles.
- "Connected" means the link snapshot (`muse_obd_status()`) reports subscribed or polling (the same condition that lights the round-2 status-bar indicator at full accent). While scanning/connecting/backoff, no cards.
- When the link becomes connected, insert the three card tiles between the face and Settings. If the user is on the face, stay on the face. If the user is in Settings (or a Settings subpage), do not yank the view; the Settings tile they are looking at must remain the visible one (track tiles by object, not index).
- When the link drops, remove the card tiles. If the user is currently on a card, land on the face tile. If they are in Settings, stay in Settings.
- No flicker machinery beyond that: connect/disconnect are deliberate events in the OBD task, not per-frame states. Apply tile changes on the LVGL task like every other UI mutation in muse_ui.c.
- The cards' contents and states from OBD-CARDS-TASK are unchanged. `>card <n>` clamps/no-ops gracefully when the cards are absent (jumping to face or Settings still works).
- The status-bar OBD indicator behavior is unchanged.

Implementation latitude: LVGL tileview tiles are objects; add/remove them at runtime and keep the dots in sync (the dots code already derives from tile identity in muse_ui.c's refresh). If tileview fights removal, a fallback that satisfies Derek is acceptable only if the *visible* result is identical: cards unreachable by swipe and dots back to two. Note which route you took in Results.

## Part 2: helmet while connected

- The avatar wears a racing helmet whenever the same "connected" condition from Part 1 holds, in the normal face views (idle and the usual modes/reactions). Not in Settings, not when disconnected, not when OBD is disabled (Part 3).
- Implement it as an overlay in the pixel avatar renderer, driven by a flag the UI layer sets from the OBD snapshot (follow how the existing accessories/poses are plumbed; the thinking pose's headphones are the closest precedent). The flag must be a no-op in non-OBD builds.
- Design: full racing helmet drawn over the crown of the head, visor RAISED so the eyes and mouth stay visible and reactions (VTEC grin, coolant worry, warm-up thumbs-up framing) still read. Shell in a light color with an accent stripe matching the UI accent; a chin-bar hint at the lower sides of the head is welcome if it does not cover the mouth. Keep the renderer's pixel style and palette discipline.
- The helmet must not break the existing pose set or the avatar host tests. Render a host preview of at least the idle pose with the helmet (use the existing avatar host tooling the earlier pose work used) and record the preview path in Results; Muse will inspect it during QA.

## Part 3: Settings kill switch

- Settings gains an "OBD" on/off switch in OBD builds only, in the same page style as the existing toggles (the All-messages pushes switch is the precedent). Persist it in NVS next to the other Muse settings (own key, e.g. `obd_on`), default ON so behavior after flashing is unchanged until Derek turns it off.
- OFF means completely off:
  - The OBD task does not scan, advertise-hunt, or connect; if a link is up when the switch flips, drop it promptly.
  - `obd.read` state reports "off" (the state string already exists), readings null.
  - No status-bar indicator, no cards (Part 1 sees "not connected" forever), no reactions, no helmet.
  - The firmware makes no OBD BLE connection attempts at all, so the adapter is free for a phone app.
- ON (including flipping it back at runtime) resumes normal scanning without a reboot.
- Runtime toggling must be safe while the OBD task is mid-scan or mid-connection: an enable flag the task checks at its existing decision points, plus a disconnect request on disable, is the expected shape. No task restart races, no leaked BLE connection.
- Expose the enabled state in the snapshot/`obd.read` (e.g. `"enabled": false`) so Muse can see it from the Link side.

## Tests (host, from esp32/: `python3 -m unittest discover -s tests -p "test_*.py"`)

- Contract tests in the style of test_link_obd.py: card tile creation/removal is driven by the link snapshot (not unconditional at UI init); the helmet flag is set from the same condition and is inert without CONFIG_MUSE_OBD_VTEC; the Settings switch exists only in OBD builds, persists via NVS, and gates scanning in muse_obd.c; "off" state is reachable in obd.read.
- Existing suites (ELM harness, triggers, link contracts) pass unchanged. Full suite: no new failures beyond the 6 known cJSON-environment errors.
- The Mac idf.py build is the real toolchain gate (the VM has no ESP-IDF). Keep new format strings provably sized: the 2b62428 format-truncation fix is the cautionary tale, so no `%02X`-style specifiers into buffers GCC cannot prove large enough, anywhere in the new code.

## Out of scope

No changes to what the cards show, the poll schedule, trigger thresholds, or the support scan. No persisting of scan results to flash (still parked). No helmet in non-OBD contexts (calls, thinking pose is fine to include if the plumbing makes it natural, but idle + reactions are the requirement).

## Results (Claude Code fills this in)

- Commits, files changed, test counts, the tile add/remove route taken, the helmet preview path, and any deviation with its reason.

### Results

- **Commits on obd-vtec-reaction:**
  - f99de91 OBD: Settings kill switch (obd_on), the task's enable flag and enabled in obd.read
  - 3ea5990 UI: car data card tiles only while the OBD link is up, and the helmet flag
  - 8f3d47f Jollybot: racing helmet while linked to the car (pose.helmet)
  - 4662be1 Tests: OBD switch against the real task, helmet renderer, presence and switch contracts
  - the commit adding these Results and the README / CLAUDE.md docs
- **Files changed:**
  - `esp32/components/muse/`: `muse_obd.[ch]`, `muse_settings.[ch]`, `muse_settings_ui.c`, `muse_app.c`, `muse_ui.[ch]`, `muse_obd_cards.[ch]`, `muse_input.c`, `muse_pixel.h`, `Kconfig` (help text only).
  - `esp32/avatar/muse_pixel.c` (in its own commit, under Jollybot's notice, which now lists the helmet).
  - `esp32/main/`: `app.c` (`enabled` in obd.read), `noise_control.cpp` (obd.read's description).
  - `esp32/tests/`: `test_link_obd.py` (changed), plus new `muse_obd_enable_harness.c`, `obd_enable_fakes/`, `test_muse_obd_enable.py`, `muse_pixel_helmet_harness.c` and `test_muse_pixel_helmet.py`.
  - Docs: `README.md`, `CLAUDE.md`, this file.
- **"Connected"** is `muse_obd_linked(&st)`: `enabled && (subscribed || polling)`, the same condition that shows the status-row label. `update_chrome()` (the LVGL task's 0.25 s chrome tick) reads the snapshot once into `s_obd_linked`, which drives both the cards and the helmet. The status-row indicator is unchanged.
- **Part 1: the card tiles. Route: real add and remove, no fallback.**
  - At boot the tileview holds the face (column 0) and Settings (column 1), the same as a non-OBD build. The cards are no longer created at UI init.
  - When the link comes up, `sync_cards(true)` adds three tiles at columns 1-3 with `lv_tileview_add_tile()`, builds the cards on them (`muse_obd_cards_build()`), fills them in once, and moves Settings to column 4 with `lv_obj_set_pos(s_settings, lv_pct(400), 0)`.
  - When the link drops, `sync_cards(false)` forgets the cards' label pointers (`muse_obd_cards_drop()`), deletes the three tiles (their contents go with them), and moves Settings back to column 1.
  - Either way, it then re-applies the tile that was active *by object* with `lv_tileview_set_tile(..., LV_ANIM_OFF)`. On the face, you stay on the face. In Settings or a Settings subpage, Settings stays on screen (programmatic scrolling doesn't need the SCROLLABLE flag that a subpage turns off). On a card that is going away, you land on the face.
  - Tileview didn't fight this. It finds tiles by position and keeps `tile_act` as an object, so moving Settings and re-setting the active tile is all it needs.
  - The change waits while a finger is down or the tileview is sliding (`lv_indev_get_state() == PRESSED`, `lv_obj_is_scrolling()`), so no tile is deleted under a gesture. There is no other debounce, since connect and disconnect are deliberate task events.
  - Dots: `s_dots[TILES]` (five) are created once. `place_dots()` centres the first `tile_count()` of them (two tiles still sit at -8/+8) and hides the rest, and `update_chrome()` keeps the rest hidden. The "settings visible" threshold and the subpage rules now use `tile_count()`.
  - `>card <n>` keeps fixed numbers: 0 the face, 1-3 the cards, 4 Settings. While the cards are out, 1-3 answer `@card.error no cards: the OBD link is down`, and 0 and 4 still work. A jump queued just before the cards went is a no-op (`tile_at()` returns NULL). `muse_ui_bench_page()` (snapshot builds) maps Settings to `TILES - 1`, the same tile as before.
  - Without `CONFIG_MUSE_OBD_VTEC`, `tile_count()` is the constant 2, Settings is added at column 1 as before, and none of the card code is compiled.
- **Part 2: the helmet**
  - `muse_pose_t` gains `bool helmet` (`muse_pixel.h`). `muse_ui.c` sets it only inside the gated pose block (`.helmet = s_obd_linked`). Without the option it stays false, and the renderer then outputs the same bytes as before (below).
  - `draw_helmet()` in `avatar/muse_pixel.c` runs after the face and before the foreground, so sparkles, banners, steam, sweat, stars, hearts and the thumbs-up all stay on top. The design:
    - A light shell (`C_HELM` 0xeef1f6, shade `C_HELM_SH`, `C_WHITE` highlights), lit and dithered from the top left like the fur. It is the body's own superellipse grown by 2.5 px wide and 3 px tall, so it follows squash, hop, shake and lean. Below the eyes it tucks in, so it never flares over a body the reactions squash wide.
    - A 2 px stripe in `C_ACC`, the mode's accent, down the crown to the visor. In idle that is 0xa77dff, the UI accent.
    - The eye port is the face panel plus one pixel, rimmed in `C_OUT`. The eyes, brows, cheeks and mouth are never covered.
    - The visor is raised: a smoked `C_VISOR` band hugging the top of the port, with a slanted two-wide `C_VISOR_HI` glint on the left and a pivot bolt at each end.
    - The chin-bar hint: the shell reaches up to 2 px lower at the sides of the jaw and leaves the chin and mouth open.
    - The arms stay in front of it (cells the arms cover are skipped).
    - Four palette entries are added after the existing ones.
  - Thinking draws the helmet instead of the headphones, since the cups would sit over the shell. The laptop and the typing paws are unchanged.
  - **Preview:** `~/workspace/gadget-preview/obd-helmet-idle-v1.png` (idle with the helmet, 320 px). `~/workspace/gadget-preview/obd-helmet-v1-sheet.png` (every mode and reaction, plain above and helmeted below: idle, listening, thinking, speaking, happy, VTEC, coolant, warm-up, dizzy, sleepy, tickle, error). `~/workspace/gadget-preview/obd-helmet-v1.gif` (helmeted idle, then a VTEC, a coolant warning and a warm-up in turn). These are new file names, rendered with the real renderer through a `tools/muse/preview.c`-style host program.
  - **Byte-identical without it:** I hashed `s_fb` and the existing 41 palette entries over 63 sweeps (every mode × none, happy and the seven reactions, 4 s each at 40 ms). The hash is identical before and after the change (`16240c3d…`).
- **Part 3: the Settings switch**
  - Settings home gains an **OBD** switch row, `switch_row_icon()` like All messages, right after Bluetooth, only with the option.
  - It is persisted as `obd_on` (u8) in the `muse` NVS namespace, default on, and logged at boot. `muse_settings_set_obd_on()` saves it and notifies `MUSE_SETTING_OBD`. `muse_app.c`'s listener calls `muse_obd_set_enabled()`, and `muse_app_run()` applies the saved value before `muse_obd_start()`.
  - `muse_obd_set_enabled()` sets a volatile flag and the snapshot's `enabled` under the status spinlock, and notifies the task. The task checks the flag at its existing decision points:
    - Every `wait_for()` returns 0 at once while off. That ends a scan (then `ble_gap_disc_cancel`), a connect (then `ble_gap_conn_cancel`), a GATT discovery, an ELM command or the poll loop, and each path ends in its usual `drop()`.
    - The waits that take a connection down still run. Their mask holds `UNWIND` (= `BIT_EV(EV_DISCONNECT)`): `drop()`'s wait for the disconnect, and a new wait after a cancelled connect. If the connection completed just as it was cancelled, that wait picks it up and the caller's `drop()` terminates it, so no link is leaked. That race existed before on the timeout path too, and is closed there as well.
    - `elm_cmd()` doesn't write while off, and `scan_support()` returns false, so a half-initialised link never shows as "polling".
    - At the top of its loop the task sets `"off"`, clears `adapter_found` and blocks on its notification until turned back on. The waits for Link's host and the backoff now use `nap()`, which ends early once off, instead of `vTaskDelay`.
  - Off means: no scan and no connection attempts, `obd.read` / `>obd` show `"state": "off"`, `"enabled": false` and null readings, and there is no label, no cards, no helmet and no reactions. `muse_obd_fire_*()` do nothing, and `>vtec`/`>coolant`/`>warmup` answer `@obd.off OBD is turned off in Settings`.
  - Turning it back on resets the backoff and goes straight to "waiting", then "scanning", with no task restart.
- **obd.read fields as built**, in order: `state, enabled, adapter_found, connected, subscribed, polling, rpm, rpm_age_ms, coolant_c, coolant_age_ms, load_pct, load_age_ms, map_kpa, map_age_ms, speed_kmh, speed_age_ms, timing_deg, timing_age_ms, iat_c, iat_age_ms, throttle_pct, throttle_age_ms, stft_pct, stft_age_ms, ltft_pct, ltft_age_ms, ambient_c, ambient_age_ms, protocol, supported_00, supported_20, supported_40`. `>obd` prints the same list.
- **Format strings:** the new code adds no `snprintf` or other bounded format at all. The only touched format lines are `printf`s to the console (`>obd`'s new `"enabled":%s`, `@obd.off`, `@card.error`).
  - I compiled every changed firmware file with GCC 13 `-O2 -Wall -Wextra -Wformat-truncation=2 -Wformat-overflow=2`, as real compiles (`-c`, not `-fsyntax-only`, which skips the truncation pass), against the real LVGL 9.5.0 headers and stub ESP-IDF/FreeRTOS/NimBLE headers.
  - Files: `muse_ui.c`, `muse_input.c`, `muse_settings_ui.c`, `muse_app.c` and `muse_settings.c` in all four `CONFIG_MUSE_OBD_VTEC` × `LV_USE_SNAPSHOT` combinations, plus `muse_obd.c` and `muse_obd_cards.c`.
  - Result: no diagnostics in changed code. The setup does flag the pre-2b62428 `char cmd[8]` + `"01%02X"` line.
  - Two level-2-only truncation notes appear in `muse_settings_ui.c`'s untouched `set_val()` callers, identically at HEAD. They are clean at level 1, which is what `-Wall` gives the IDF build.
- **Deviations, and why:**
  - The helmet replaces the thinking headphones instead of being drawn over them. The headphones' band and cups would sit on the shell.
  - The visor and the stripe use mode-tinted colours where the palette already has them: the stripe is `C_ACC`, which is the UI accent in idle and follows the mode's accent elsewhere, the same way the rim light does.
  - The off state also clears `adapter_found`, so `obd.read` says nothing at all about the car while off. The protocol and supported masks stay, as before (they describe the car).
  - `>vtec`/`>coolant`/`>warmup` do nothing while off (the spec's "no reactions") and say so with `@obd.off`, instead of firing anyway for the bench.
- **Tests and QA (this VM: no ESP-IDF, no cJSON):**
  - New `test_muse_obd_enable.py` runs `muse_obd_enable_harness.c`: the real `muse_obd.c` task on a thread over `obd_enable_fakes/`, a scripted LELink^2 (ATZ/ATDPN/0100/010C/0105 answers, replies delivered after the write's event as over the air), and a virtual clock that moves only while the task blocks. Built with `-Werror` and ASan/UBSan; it takes about 0.5 s. It checks:
    - `muse_obd_linked()`'s truth table.
    - Off at boot never scans, even over 5 minutes.
    - On, it polls (1726 rpm read).
    - Off while polling terminates the link and reports off with every reading cleared; then no scan or connect for 5 minutes.
    - No reactions while off.
    - Off while scanning cancels the scan.
    - Off while connecting cancels with nothing terminated; off when the connection beats the cancel terminates that connection.
    - Back on, it polls again and the reactions play.
  - Mutation check on that harness: 9 seeded bugs in `muse_obd.c`, each caught:
    - `wait_for` ignoring off;
    - no unwind after the connect cancel;
    - the `UNWIND` bit dropped;
    - no off state;
    - fires not gated;
    - `enabled` missing from the snapshot;
    - `muse_obd_linked` ignoring `enabled`;
    - `set_enabled` not waking the task;
    - `adapter_found` kept.
  - While building it, the harness first failed. The cause was the fake answering inside the write, which let one `EV_WRITE` per command pile up past the 16-slot event queue. It was not a firmware fault: on the air the write's event arrives before the ELM's reply. The fake now delivers replies once the task blocks.
  - New `test_muse_pixel_helmet.py` runs `muse_pixel_helmet_harness.c`, also with `-Werror` and ASan/UBSan. It covers 14 scenes: 6 modes, happy and the 7 reactions, every 40 ms frame. Each frame is rendered plain and helmeted from the same saved renderer state, then compared:
    - Untouched: face-panel, eye, mouth, blush and brow cells; arm cells; overlay cells (banners, stars, hearts, sweat, steam).
    - Only helmet colours added, nothing below the jaw + 4 rows and nothing at the edges.
    - Shell, shade, visor, glint and stripe all present (the stripe may be partly behind a star); every eye cell still there.
    - No visor at or below the eyes, and no headphones with the helmet.
    - The renderer's carried state is unchanged, and the shell colour is light.
    - Plus plumbing pins: the pose field, the gated `.helmet = s_obd_linked`, draw order, and the notice.
  - Mutation check on that harness: 7 seeded bugs, each caught:
    - a smaller port;
    - arms not kept in front;
    - drawn over the foreground;
    - visor lowered;
    - headphones kept;
    - no stripe;
    - shell to the feet.
  - `test_link_obd.py`: 12 to 16 tests.
    - `test_card_tiles_only_with_the_feature` now pins the two-tile start and `tile_count()`.
    - The new `test_card_tiles_follow_the_link` covers `sync_cards`: add/build, drop/delete, the Settings move, keeping the visible tile by object, landing on the face from a card, the press/slide guard, and being driven by `muse_obd_linked` before the dots.
    - `test_card_bench_command_is_gated` adds the no-cards refusal.
    - New `test_settings_switch_only_with_the_feature` (header, NVS `obd_on` default on, the Settings row, nothing outside the gate).
    - New `test_settings_switch_drives_the_obd_task` (listener, set before start, `wait_for`/`UNWIND`, the off loop before the scan, no `vTaskDelay` in the task, gated fires).
    - New `test_obd_read_reports_enabled_and_off`.
    - `test_status_row_indicator_is_gated` now finds the indicator block by its own text.
  - The existing ELM harness, trigger cases and VTEC/coolant/warm-up avatar tests pass unchanged.
  - Totals for `python3 -m unittest discover -s tests -p "test_*.py"`:
    - Before (78cd062): 664 ran, 0 failures, 6 errors, 10 skipped.
    - After: 673 ran, 0 failures, 6 errors, 10 skipped (9 new: 4 in test_link_obd.py, 1 in test_muse_obd_enable.py, 4 in test_muse_pixel_helmet.py).
    - The 6 errors are the same missing-cJSON setUpClass set both times: link_discovery, link_reterminal_sht4x, link_sensecap_sensors, muse_chat_link_errors, muse_chat_link_note, muse_chat_session.
- **Unverified until the Mac build and the car:**
  - The s18 `idf.py` build and `check_sizes`, and every other board building unchanged. No file here was compiled by the real toolchain.
  - The tile add/remove on the device: no flicker as the cards come and go, the dots with two and five tiles, and the slide back to the face from a card on the real panel.
  - The helmet on the 368x448 panel, at its real scale.
  - The kill switch against the real NimBLE: that cancelling a scan or connect and terminating behave as the fakes do, and that a phone app can take the adapter once it's off.
