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
