# OBD-CARDS-TASK: swipeable car data cards + PID support scan (branch obd-vtec-reaction)

Owner: Derek. Spec by Muse, implementation by Claude Code, QA by Muse, flash by Derek on the Mac.
Context: Derek's car is a **2008 S2000 (AP2)**: drive-by-wire, and its DLC talks to the ECM over **CAN (ISO 15765-4)**. Earlier branch docs say "the S2000 talks K-line"; that is true only for 2000-2005 cars. This task is a **testing build**: Derek wants swipeable card faces showing car data (temps, IAT, timing, more) so he can see in the car what his ECU actually answers. The cards double as the PID discovery harness we discussed: supported PIDs show values, unsupported PIDs show "--" and are never polled again.

## What exists today (do not regress)

- OBD link over BLE to the LELink^2: ELM init (`ATZ ATE0 ATL0 ATH0 ATS0 ATSP0`), poll loop in `esp32/components/muse/muse_obd.c`, parsers/triggers in `muse_obd_elm.[ch]`.
- Polling: `010C` RPM back to back with `0105` coolant every 4th request. Snapshot + `obd.read` Link command + `>obd` bench line report `{state, adapter_found, connected, subscribed, polling, rpm, rpm_age_ms, coolant_c, coolant_age_ms}`.
- Reactions (VTEC, coolant warning, warm-up) are visual only and fire from `muse_obd_fire_*()` in muse_obd.c. The status-bar OBD indicator shipped in round 2 (OBD-INDICATOR-TASK.md) and Derek verified it in the car.
- Main UI is an LVGL tileview in `muse_ui.c`: tile 0 = the face, tile 1 = Settings (`muse_settings_ui`), page dots, `muse_ui_set_swipe_enabled()`, and a bench hook `muse_ui_bench_page(bool settings)`. All OBD code is gated on `CONFIG_MUSE_OBD_VTEC` (=y only in `esp32/devices/sdkconfig.muse-waveshare-s3-18`).

## Part 1: support scan and protocol report

In the ELM init sequence, after the existing setup commands and before the poll loop starts:

1. Send `ATDPN` once and store the protocol number; map it to a short string for display (at minimum: 6 = ISO 15765-4 CAN 11/500, 7 = CAN 29/500, 3 = ISO 9141-2, 4 = KWP2000 5-baud, 5 = KWP2000 fast; unknown -> "proto N"). Add `protocol` to the snapshot and to `obd.read` / `>obd` output.
2. Request `0100` and parse the 4-byte supported-PID bitmask for PIDs 01-20. If PID 0x20 is supported, request `0120` (21-40); if 0x40 is supported, request `0140` (41-60). Store the three masks in the snapshot. Bit parsing follows the existing shared PID line parser conventions in muse_obd_elm (echo tolerant, headers off, spaces optional). A failed or NO DATA scan must not break the link: fall back to "unknown support" and poll the Part 2 display set with the existing NO DATA tolerance pattern (the way coolant is handled today).
3. Expose the masks in `obd.read` as hex strings (e.g. `"supported_00": "BE1FA813"`, likewise `_20`, `_40`, null when unknown). This is the discovery payload Derek and Muse will read after his test drive.

## Part 2: extended PID polling

Parsers in muse_obd_elm.[ch], one per PID, following the existing rpm/coolant parser shape exactly (same frame validation, same result enum style). Formulas (Mode 01):

| PID | Name | Formula | Unit |
|---|---|---|---|
| 04 | Calculated load | A * 100 / 255 | % |
| 0B | Manifold pressure (MAP) | A | kPa |
| 0D | Vehicle speed | A | km/h |
| 0E | Timing advance | A / 2 - 64 | deg |
| 0F | Intake air temp | A - 40 | C |
| 11 | Throttle position | A * 100 / 255 | % |
| 06 | Short-term fuel trim B1 | (A - 128) * 100 / 128 | % |
| 07 | Long-term fuel trim B1 | (A - 128) * 100 / 128 | % |
| 46 | Ambient air temp | A - 40 | C |

Poll scheduling (RPM stays dominant, everything else is a guest):

- Request pattern alternates: RPM, secondary, RPM, secondary, ...
- The secondary slot cycles through a fixed list: coolant (05) first, then 0F, 0E, 0B, 04, 11, 0D, 06, 07, 46.
- If the support scan succeeded, drop unsupported PIDs from the cycle entirely. If support is unknown, keep them and let NO DATA answers skip cheaply, as coolant does today.
- VTEC/coolant/warm-up trigger behavior must be bit-for-bit unchanged: same thresholds, same sustain/cooldown logic, fed by the same RPM and coolant values. RPM effective rate halves versus today; the triggers work off values and timestamps, not request counts, so verify by reading the trigger code, and note anything you find that secretly depends on request cadence in the Results.
- Snapshot gains one value + age per polled secondary (follow the rpm/coolant spinlock snapshot pattern in muse_obd.h). `drop()` clears them all. `obd.read` gains the same fields (null when invalid): `load_pct, map_kpa, speed_kmh, timing_deg, iat_c, throttle_pct, stft_pct, ltft_pct, ambient_c`, each with `<name>_age_ms` where the existing pattern makes that natural; if you simplify, document it in Results.

## Part 3: the cards (UI)

Add **three card tiles** to the main tileview, between the face tile and the Settings tile, only in OBD builds (`CONFIG_MUSE_OBD_VTEC`). Non-OBD builds must be byte-for-byte unaffected in behavior: no new tiles, same dots, same swipes.

- **Card 1 "Temps"**: Coolant (existing value), Intake air (0F), Ambient (46). Big value + small label + unit per row. Coolant keeps C like the rest of the firmware.
- **Card 2 "Engine"**: RPM (large, top), Timing advance (0E), Load (04), MAP (0B).
- **Card 3 "Drive"**: Speed (0D, show km/h and mph), Throttle (11), STFT (06), LTFT (07).

Behavior rules:

- Values refresh from the snapshot on the UI's normal refresh cadence. A value older than ~3 s renders dimmed; a value never received or a PID the scan proved unsupported renders "--". Distinguish "unsupported" from "no data yet" only if it costs nothing (e.g. "--" vs "···"); do not invent a third state.
- When the OBD link is not polling, each card shows the link state line instead of stale numbers (reuse the snapshot state strings: scanning, connecting, backoff, ...). The status-bar OBD indicator from round 2 keeps working exactly as before.
- Page dots: extend to cover the new tiles and keep the existing Settings subpage behavior (dots hidden in subpages, swipe-right-to-go-back in Settings unchanged, `muse_ui_set_swipe_enabled` semantics preserved). The face tile keeps all its tap/talk/rub interactions; cards are passive displays, no buttons, no gestures of their own beyond tileview swiping.
- Sleep/wake and the reply/answering layouts must not be disturbed: cards are just tiles; when the device is answering or showing a reply, existing behavior wins.
- Bench hook: add a serial command in muse_input.c next to the existing OBD bench commands: `>card <n>` jumps the tileview to tile n (0 = face, 1..3 = cards, last = settings), reusing or generalizing `muse_ui_bench_page`. Document it in Results.

## Part 4: docs fix

README.md, "In the car" section: replace the flat claim that the S2000 talks K-line with the accurate version: 2000-2005 cars are K-line, 2006+ cars (DBW) are CAN, the firmware auto-detects via ATSP0 and reports the negotiated protocol in `obd.read`. Keep it to a sentence or two. Also add the cards + the new `obd.read` fields to that section briefly.

## Tests (host, from esp32/: `python3 -m unittest discover -s tests -p "test_*.py"`)

- Extend the OBD ELM harness (esp32/tests/muse_obd_elm_harness.c pattern) with cases for every new parser: nominal frames, echo present, wrong PID, truncated frames, headers-on junk, and the bitmask parser (0100/0120/0140 replies, including the PID 20/40 chain bits).
- Trigger regression: existing VTEC/coolant/warm-up harness cases must pass unchanged.
- Contract tests in the style of test_link_obd.py: card tiles exist only under CONFIG_MUSE_OBD_VTEC; `obd.read` exposes the new fields; `>card` bench command registered under the same gate.
- Full suite must pass with no new failures beyond the known cJSON-environment errors (6, pre-existing on this VM; if your environment has cJSON, zero errors is the bar).

## Out of scope

No new reactions, no speech, no changes to trigger thresholds, no Settings pages for the cards, no logging to SD, no changes to non-s18 boards' behavior, no lap timer work (that is a separate parked project).

## Results (Claude Code fills this in)

- Commits, files changed, test counts (before/after), anything that depended on request cadence, deviations from this spec and why, and the exact `obd.read` field list as built.

### Results

- **Commits on obd-vtec-reaction:**
  - ed6a34d OBD: card PIDs' parsers, the supported-PID bitmask and ATDPN, with host tests
  - 231ced8 OBD: support scan, protocol report and RPM alternating with the card PIDs
  - df40416 UI: three car data card tiles between the face and settings, and >card
  - 2db3951 Tests: OBD card tiles, obd.read card fields, support scan and >card contracts
  - the commit adding these Results and the README / CLAUDE.md docs
- **Files changed:**
  - `esp32/components/muse/`: `muse_obd_elm.[ch]`, `muse_obd.[ch]`, `muse_obd_cards.[ch]` (new), `muse_ui.[ch]`, `muse_input.c`, `CMakeLists.txt`, `Kconfig` (help text only).
  - `esp32/main/`: `app.c` (obd.read), `noise_control.cpp` (obd.read's description).
  - `esp32/tests/`: `muse_obd_elm_harness.c`, `test_muse_obd_elm.py` (docstring), `test_link_obd.py`.
  - Docs: `README.md`, `CLAUDE.md`, this file.
- **Part 1: support scan and protocol**
  - `muse_obd.c` `scan_support()` runs right after `elm_init()`, still in the `initializing` state.
    - It sends `0100` (with `SEARCH_MS`, because this is now the request that makes `ATSP0` search), then `0120` if PID 20's bit is set, then `0140` if PID 40's is.
    - `muse_elm_parse_support()` parses each reply with the shared `parse_pid()` (echo tolerant, headers off, spaces optional). The four bytes become `A << 24 | B << 16 | C << 8 | D`.
  - Failure handling:
    - A reply that isn't a mask (NO DATA, an error, a timeout) ends the scan. The ranges read so far stand and the rest stay unknown.
    - If `0100` itself fails, support is unknown, and the poll loop asks every PID and lets NO DATA skip it, as coolant does.
    - Only a disconnect makes `scan_support()` return false, and then the link is dropped as before.
  - `read_protocol()` sends `ATDPN`. `muse_elm_parse_protocol()` reads `A6` / `6` as 6, and `A0` / `0` / junk as unknown. `muse_elm_protocol_name()` gives: 1 `SAE J1850 PWM`, 2 `SAE J1850 VPW`, 3 `ISO 9141-2`, 4 `KWP2000 5-baud`, 5 `KWP2000 fast`, 6 `CAN 11/500`, 7 `CAN 29/500`, 8 `CAN 11/250`, 9 `CAN 29/250`, A `SAE J1939`, B/C `USER1/2 CAN`, and `proto N` otherwise.
  - Retry: if the scan or ATDPN failed at init (for example engine off at connect), each is tried again once per connection, on the first RPM reply.
  - Support logic (`muse_elm_pid_support`): a range that wasn't read counts as unsupported when the nearest range below it that was read has its chain bit clear. So `0100` without PID 20 rules out 21-60, and 0x46 too. Otherwise it is unknown.
- **Part 2: extended PIDs**
  - One parser per PID in `muse_obd_elm.c` (`muse_elm_parse_load/map/speed/timing/iat/throttle/stft/ltft/ambient`), the same shape as `muse_elm_parse_coolant`, each with its own result value (`MUSE_ELM_LOAD`, ...), appended after `MUSE_ELM_COOLANT` so the existing values don't move.
  - Values are ints, using the spec's formulas with C integer division. Timing `A / 2 - 64` drops the half degree; fuel trims truncate toward zero.
  - Cycle (`MUSE_ELM_SECONDARY`, `muse_elm_next_secondary`): 05, 0F, 0E, 0B, 04, 11, 0D, 06, 07, 46. `poll_rpm()` asks `010C` on even requests and the next secondary on odd ones, skipping PIDs the scan proved unsupported. If every secondary is ruled out, it polls RPM alone.
  - Timeouts and the NO DATA pause are unchanged: `SEARCH_MS` until RPM answers and `POLL_MS` after, `NO_RPM_PAUSE_MS` only after an RPM non-answer. A secondary NO DATA is skipped without a pause, like coolant's.
  - Snapshot (`muse_obd_status_t`):
    - `extra[MUSE_OBD_EXTRAS]`, a `{valid, value, age_ms}` per card PID, timestamped in `set_extra()` under the same spinlock as rpm/coolant, with ages computed in `muse_obd_status()`.
    - `protocol` / `protocol_name[20]`, and `support_valid[3]` / `support[3]`.
    - `drop()` → `set_link_down()` clears every reading. `drop()` also resets the connection's `s_support`, `s_cursor` and `s_have_protocol`.
  - `MUSE_OBD_EXTRA[]` (`muse_obd.c`) holds each PID's obd.read key and age key. `app.c`, `muse_input.c` and the tests all iterate it.
- **Cadence (read from the trigger code):**
  - `muse_vtec_update`, `muse_coolant_update` and `muse_warmup_update` use only values and the `now_ms()` passed in. None of them counts requests. Thresholds, sustain, cooldowns and arming are bit-for-bit unchanged, and the existing VTEC, coolant and warm-up harness cases pass untouched.
  - The only old code tied to request counts was `COOLANT_EVERY 4` itself, and the `test_link_obd.py` assertion that pinned it ("poll cadence unchanged"). The spec changes the cadence on purpose, so that pin now checks the alternating cycle instead.
  - What the new cadence changes, through sampling only:
    - RPM is sampled half as often, so a blip above 6000 rpm shorter than two requests can be missed, and likewise a dip to 5500 that would arm the trigger.
    - Coolant goes from every 4th request to every 20th when all ten secondaries are polled, fewer if the scan drops some. The 5 s sustain is measured between readings, so a hot-coolant fire can land up to one coolant interval later. A dip below 105 °C shorter than that interval is not seen, so it doesn't restart the sustain (it already could be missed before, over a shorter window). The warm-up fires on the first reading at or above 82 °C, so it can arrive up to one interval later.
    - The coolant warning's `s_rpm` is at most one request old, as before.
- **Part 3: the cards**
  - `muse_obd_cards.c` (built only with the option, from `CMakeLists.txt`) makes three tiles:
    - **Temps**: Coolant, Intake air, Ambient.
    - **Engine**: RPM in a taller row in the accent colour at the top, then Timing, Load, MAP.
    - **Drive**: Speed (km/h, with mph beside it), Throttle, STFT and LTFT (signed).
  - Each row has a small name, a big value (montserrat_28) and a unit (C, deg, %, kPa, km/h, rpm). The rows are styled like the settings cards.
  - The cards are refreshed from `update_chrome()` every 0.25 s, the settings tick. They set a label only when its text or colour changes. Display rules:
    - Older than 3 s (`MUSE_OBD_CARD_STALE_MS`): dimmed.
    - Never received: `...`.
    - Proved unsupported: `--`.
    - Not polling: a centred `OBD` + state line (scanning, connecting, backoff, ...) replaces the rows.
  - The status-row OBD label is untouched.
  - `muse_ui.c` adds the tiles in a gated block: `s_cards[]` at columns 1-3 (`LV_DIR_HOR`), settings at `SETTINGS_COL` (4 with the option, 1 without).
    - Dots: `s_dots[TILES]`, placed at `16 * i - 8 * (TILES - 1)`, so two tiles keep -8 / +8. The active dot comes from `tile_index()`. Dots hide only on settings while in a subpage, and the swipe disables only there, as before.
    - `muse_settings_ui_tick()` now counts settings as visible once the scroll passes the last card (`> 0` without the option).
    - Face gestures (volume drag, rub, taps) are all gated on the face tile in `drag_can_start()`, so the cards get none. Their layout boxes are neither clickable nor scrollable, and they have no event callbacks.
    - Listening still slides back to the face, sleep is untouched, and the avatar holds still while any non-face tile is on screen, as it did for settings.
  - Bench: `>card <n>` in `muse_input.c` (in the OBD block, next to `>obd`) calls `muse_ui_bench_tile(n)`: 0 the face, 1-3 the cards, 4 settings. Anything else prints `@card.error tiles are 0..4`.
    - It reuses `muse_ui_bench_page()`'s pending-tile variable, which now holds a tile index (`tile_at()`), applied on the next frame. `muse_ui_bench_page()` itself is unchanged in behaviour.
    - Unlike `>ui=settings`, it doesn't need `LV_USE_SNAPSHOT`.
- **Part 4: docs**
  - README "In the car":
    - 2000-2005 cars are K-line (ISO 9141-2) and 2006+ DBW cars like the AP2 are CAN (ISO 15765-4). `ATSP0` auto-detects which, and `obd.read` reports it as `protocol`.
    - Plus the support scan, the new cadence, the new `obd.read` fields, the cards, and `>card`.
  - The README feature row and intro, `CLAUDE.md` and the Kconfig help drop "every 4th request".
- **`obd.read` fields as built**, in order (`>obd` prints the same list, as one `@obd {...}` line): `state, adapter_found, connected, subscribed, polling, rpm, rpm_age_ms, coolant_c, coolant_age_ms, load_pct, load_age_ms, map_kpa, map_age_ms, speed_kmh, speed_age_ms, timing_deg, timing_age_ms, iat_c, iat_age_ms, throttle_pct, throttle_age_ms, stft_pct, stft_age_ms, ltft_pct, ltft_age_ms, ambient_c, ambient_age_ms, protocol, supported_00, supported_20, supported_40`.
  - Readings and ages are null when there is no current reading.
  - `protocol` is a short name (`"CAN 11/500"`), null until ATDPN has answered.
  - `supported_*` are 8 uppercase hex digits (`"BE1FA813"`), null when that range wasn't read.
- **Deviations from the spec, and why:**
  - ATDPN runs after `0100`, not before. Under `ATSP0` the ELM picks the protocol at the first OBD request, so an ATDPN before it would answer `A0` (nothing yet).
  - Age keys drop the unit (`load_age_ms`, not `load_pct_age_ms`), the existing pattern (`coolant_c` → `coolant_age_ms`).
  - The protocol and masks are not cleared on drop (the readings are). The spec calls them the payload to read after the test drive, and the link drops when the ignition goes off. A new scan replaces them. A new connection's failed scan leaves the last good one in the snapshot, while that connection polls every PID.
  - Multi-ECU replies: the first answering line wins, the existing `parse_pid` rule. Masks from a second ECU aren't ORed in. A single-ECM car like the S2000 isn't affected.
  - No bigger font for RPM: montserrat_28 is the largest one enabled, and adding a 48 px font to the s18 overlay would cost flash I couldn't measure here. RPM gets the hero row (taller, accent colour, top of the card) instead.
  - The spec's `···` for "no data yet" is ASCII `...`, because the built-in Montserrat fonts may not have U+00B7.
- **Tests and QA (this VM: no ESP-IDF, no cJSON):**
  - ELM harness (`-Werror`, ASan/UBSan) adds `test_extra_parse_cases`, `test_extra_frames`, `test_support_parse`, `test_pid_support`, `test_secondary_cycle` and `test_protocol`:
    - Every parser, at five raw values including the range ends: ATS0 form, lower case, trailing bytes, echo then answer, SEARCHING..., two ECUs, and a frame beating NO DATA.
    - Rejected: echo alone, another PID, cut short, half a byte, headers on (spaced and not), non-hex, mode 42, RPM and coolant frames, OK and empty. NO DATA and the errors are classified.
    - No parser reads another's frame.
    - All nine replies with NULs, fed at chunk sizes 1-24.
    - Masks: `0100`/`0120`/`0140`, the wrong range, cut short, headers on, NO DATA / UNABLE TO CONNECT, and the fragmented 00 → 20 → 40 chain.
    - Per-PID support for `BE1FA813`, the PID 20/40 chain bits (clear, set but unread, fully read).
    - The cycle's order and wraparound, the RPM/secondary alternation over 40 requests, skipping unsupported PIDs, and -1 when none is left.
    - ATDPN parsing and names.
  - Mutation check: 12 seeded bugs, each caught: the load divisor, the timing formula, the IAT offset, the ambient PID, mask byte order, mask bit order, the chain looking one range back only, the cycle ignoring support, `A` alone, a wrong protocol name, the cycle order, the LTFT divisor.
  - The existing VTEC / coolant / warm-up cases are unchanged and pass.
  - `test_link_obd.py`: 6 to 12 tests. New: the poll cycle, the scan and ATDPN before polling with reset on drop, the obd.read / `>obd` card fields, the cards gated (with the two-tile layout left without the option), the cards passive, and `>card` gated.
  - Type-checks:
    - `muse_obd_cards.c`, `muse_ui.c` and `muse_input.c` were type-checked with `-Wall -Wextra` against the real LVGL 9.5.0 headers (the version pinned in `idf_component.yml`) plus stub ESP-IDF/FreeRTOS headers. `muse_ui.c` and `muse_input.c` were checked with `CONFIG_MUSE_OBD_VTEC` 0 and 1, and with `LV_USE_SNAPSHOT` 0 and 1. No diagnostics in project code beyond the stubs' own.
    - `muse_obd.c` was checked with empty NimBLE stubs: no diagnostics on any changed line. The rest are the stubs' missing NimBLE types.
  - One full-suite run hit an unrelated `test_coexist_bench` timing race (its log line is written from a server thread). It passed on rerun and in the final full run.
  - Totals for `python3 -m unittest discover -s tests -p "test_*.py"`:
    - Before (this branch at 621666e): 658 ran, 0 failures, 6 errors, 10 skipped.
    - After: 664 ran, 0 failures, 6 errors, 10 skipped.
    - The 6 errors are the same missing-cJSON setUpClass set both times: link_discovery, link_reterminal_sht4x, link_sensecap_sensors, muse_chat_link_errors, muse_chat_link_note, muse_chat_session.
- **Unverified until the Mac build and the car:**
  - The s18 build and `check_sizes`, and every other board building unchanged. No file here was compiled by the real toolchain.
  - The cards' look on the 368x448 panel: row fit (the Drive card's speed row carries the mph text), the dimming contrast, and the page-dot spacing with five dots.
  - What the AP2's ECM answers: the `supported_*` masks and `protocol` (expected `CAN 11/500`) are the discovery payload. Ambient (46) is likely absent on a 2008 car; the scan will show it.
  - The real poll rate over BLE on CAN, and whether the slower coolant sampling ever matters for the warning in practice.
