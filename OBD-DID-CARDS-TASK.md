# OBD-DID-CARDS-TASK: misfire counts and A/C clutch from Honda DIDs — branch obd-vtec-reaction

Written by Muse (spec), for Claude Code to implement, Muse QAs after. Read the repo AGENTS.md files first.

Precondition: the pipeline build has landed and the branch builds on the Mac (OBD-PIPELINE-HANDOFF.md is item zero of the same session). This task integrates with the pipeline scheduler and the shipped DID 0x2612 machinery. Do not start on a branch that does not build.

Scope, set by Derek 2026-10-23: DID 2663 (misfire counts, display + a warning reaction) and DID 2610 (A/C clutch state, display). Derek's standing assumption applies: 2006-2009 ECUs are the same target, so the OBDb 2006/2007 decodes are authoritative for his 2008; no per-DID on-car verification gate. Out of scope: 2611, 2660, 2661, 2662, 2666, and a card cell for generator duty.

## The decodes (OBDb, verified against the shipped 2612 decode)

Source: OBDb Honda-S2000 signalset v3, https://github.com/OBDb/Honda-S2000/blob/main/signalsets/v3/default.json

OBDb `bix` counts bits within the data payload (after the `62 26 xx` prefix), MSB-first within each byte. Cross-check against the shipped, car-verified 2612 decode: bix 174 = data byte 21, mask 0x02 (pressure switch) and bix 175 = data byte 21, mask 0x01 (solenoid), which is exactly MUSE_VTEC_DID_FLAGS / _PSW / _SOL in muse_obd_elm.h. Use the same convention:

- **DID 2610**: generator duty = data byte 23, value x 100 / 255, percent. **A/C clutch = data byte 32, mask 0x02** (1 = clutch engaged). A read is parseable only with at least 33 data bytes.
- **DID 2663**: misfire counts are big-endian uint16, **cyl 1 at data byte 34, cyl 2 at 36, cyl 3 at 38, cyl 4 at 40**, plain counts, max 65535. A read is parseable only with at least 42 data bytes.

Before fixing the length checks, confirm the exact response lengths from OBDb's fixtures (tests/test_cases/2007/commands/DA10.10.222610.yaml and DA10.10.222663.yaml in the same repo) and cite them in Results.

## Part 1: more than one DID

The firmware currently hardcodes one DID (MUSE_VTEC_DID_REQ, a single probe, a single scheduler item). Generalize it:

- A per-DID table: DID, request string, minimum data bytes, minimum interval, answered state for this connection. Entries: 2612 (existing behavior, 1000 ms), 2610 (3000 ms), 2663 (3000 ms).
- Each new DID is probed once per connection, with the exact choreography and state safety of the 2612 probe (ATSH DA10F1, ATCRA 18DAF110, send, restore ATAR + ATSH DB33F1). DID exchanges are serialized: only one in flight, headers restored after each, and a DID exchange never interleaves with another. A refused, silent, or garbled probe marks that DID not-answered for the connection and it is never retried that connection (the 2612 DID_NO pattern).
- Scheduling: the pipeline scheduler's DID handling extends to per-DID items with their own floors; when a DID exchange happens, the most overdue answered DID goes. 2612 keeps its 1000 ms floor so the VTEC held pose keeps its freshness; 2610 and 2663 at 3000 ms cost the link one long exchange each per 3 s, which the pipeline has room for.
- A failed or short read mid-connection leaves the previous readings to age out under the existing staleness rules. Never fabricate a value, never clear to zero.

## Part 2: surfaces

- **obd.read**: `did_2610` and `did_2663` booleans (answered this connection, the did_2612 pattern), plus readings in the muse_obd_reading_t pattern (valid, value, age): `misfire_1` .. `misfire_4`, `ac_clutch` (1/0), `generator_pct`.
- **Cards**: Engine card gains a misfire row: four counts labeled 1 to 4, "--" until the first 2663 read, the 3 s stale dimming rule as elsewhere. A cylinder whose count increased on the latest read shows in the warning amber (the coolant >= 100 C amber) until a later read shows no increase. Temps card gains an A/C cell: "A/C" ON in the accent color, OFF dim, "--" until the first 2610 read. Established layout rules apply (OBD-LOG-TASK Part 5); every existing value stays visible.
- **Trip log**: append columns at the end of the row: `misfire_1`, `misfire_2`, `misfire_3`, `misfire_4`, `ac_clutch`, `generator_pct`. Append-only, header lists them, empty until first read, as existing columns behave.

## Part 3: the misfire warning reaction (Derek approved display + reaction)

- **Trigger**: any cylinder's counter strictly increases between consecutive good 2663 reads, while the engine is running (latest RPM >= MUSE_COOLANT_RUNNING_RPM, 400) and OBD is on and linked.
- **Baseline**: the first good read after connect sets the baseline and can never fire. A counter that decreases (ECU reset, wrap) re-baselines silently and can never fire. Only an increase against the standing baseline fires.
- **Behavior**: visual only, in the coolant-warning family: a momentary warning pose through the same plumbing as muse_obd_fire_coolant, idle-only (never wakes a sleeping screen), and `muse_hatch_say` in muse_obd.c stays 0. The trigger evaluation lives with the other triggers and reads DID state the way muse_obd_vtec_on reads the solenoid; no RPM inference, no duplicate trigger logic in the UI.
- **Cooldown**: 120 seconds between fires (mirrors MUSE_COOLANT_COOLDOWN_MS), so a genuinely misfiring engine re-warns every two minutes instead of spamming or going silent after the first.
- **Bench**: a `>misfire` console command that injects a counter increase through the same trigger path, mirroring `>coolant` and `>warmup`, so the pose is testable without a misfiring car. Host tests drive the same injection.

## Invariants

Read-only toward the car, always. Kill switch off means no DID reads at all, and the presence rules hide the cards as they do today. DID choreography and state safety (Part 1). Coolant/warmup thresholds and logic untouched; the VTEC held-pose contract untouched. Trip CSV changes are append-only. No muse_chat_session.cpp edits, no secrets changes. User-visible text says Muse, never Hatch. Provably-sized snprintf everywhere. Any new external API surface (none is expected) gets verified against real headers before commit, per the 2026-10-23 AGENTS.md lesson.

## Tests

- Parsers: 2610 and 2663 against the real OBDb fixture frames (byte-exact from the YAML fixtures), including a short reply (rejected, prior readings kept), a garbled header (rejected), and the clutch bit / counter offsets at their exact bytes.
- Probe and schedule: a refused DID is asked once per connection; per-DID floors hold (2612 at 1000 ms is never starved by the 3000 ms pair); DID exchanges never overlap a Mode 01 request.
- Reaction matrix: baseline read no fire; increase while running fires; increase while engine off does not; decrease re-baselines; a second increase inside cooldown does not fire, after cooldown does; kill switch off never fires; bench `>misfire` fires through the same path.
- Baseline: the suite state at branch tip when this task starts (after OBD-MORE-PIDS-TASK if it ran first); state it in Results.

## Results (2026-10-23)

Stage 2 audited and completed on `obd-vtec-reaction`, based on `1cb4a77`.
Implementation/test commit: `1df0ff1` (Add Honda misfire and A/C DID cards with visual warning); this Results section is the following documentation commit, titled `Record Stage 2 DID audit and verification results`.

**Baseline and verification.** Stage 1's recorded baseline at `1cb4a77` is 712 tests, 0 failures, 6 known cJSON environment errors, 10 skips (OBD-MORE-PIDS-TASK Results). The inherited uncommitted Stage 2 tree was reported by Muse as 721 tests, 2 harness compilation failures, the same 6 errors and 10 skips. Final full-suite result: **721 tests, 0 failures, 6 known cJSON errors, 10 skips**, in 236.544 seconds. Unittest exits nonzero solely for those six baseline errors. The 51-test OBD discovery subset passed; the 11 DID task/contract tests passed again after adding the disconnect regression. Run from `esp32`: `python3 -m unittest discover -s tests -p "test_*.py"`. The VM has no `idf.py`; Mac ESP-IDF v6.0.1 build, flash and hardware checks remain Derek's responsibility.

The six baseline errors are `setUpClass` in `test_link_discovery`, `test_link_reterminal_sht4x`, `test_link_sensecap_sensors`, `test_muse_chat_link_errors`, `test_muse_chat_link_note`, and `test_muse_chat_session`: missing `managed_components/espressif__cjson/cJSON/cJSON.c`. They were left untouched.

**Fixture verification.** Independently fetched OBDb Honda-S2000 revision `3f1cf3385de2fa11a210e057197475f883ec7b11` and checked the signalset and both complete fixture files:

- [2610 fixtures](https://github.com/OBDb/Honda-S2000/blob/3f1cf3385de2fa11a210e057197475f883ec7b11/tests/test_cases/2007/commands/DA10.10.222610.yaml): all **55** replies declare ISO-TP length **0x039 = 57 bytes**, comprising `62 26 10` + **54 data bytes**.
- [2663 fixtures](https://github.com/OBDb/Honda-S2000/blob/3f1cf3385de2fa11a210e057197475f883ec7b11/tests/test_cases/2007/commands/DA10.10.222663.yaml): all **26** replies declare **57 bytes**, comprising `62 26 63` + **54 data bytes**.
- Each reply has one first frame plus eight correctly sequenced continuation frames; trailing CAN padding is not payload. All seven arrays in `tests/did_cards_frames.h` match their cited YAML cases byte for byte: 2610 cases 0/33; 2663 cases 0/9/7/8/5 (zero-based).
- [Signalset](https://github.com/OBDb/Honda-S2000/blob/3f1cf3385de2fa11a210e057197475f883ec7b11/signalsets/v3/default.json): generator bix 184, clutch bix 262, counters bix 272/288/304/320; checked against 2612 bix 174/175. Parsers require complete declared transport lengths and at least 33/42 data bytes respectively; 2612 retains its exact 57-byte requirement. Short, malformed, wrong-DID, negative and silent replies do not fabricate readings.

**Spec audit and implementation.** `MUSE_DID_INFO` contains DID/request/minimum data length/interval, with connection state in the parallel `s_did[]` array. Each CAN-29 DID is probed once per connection and a failed probe stays disabled until reconnect. The serial request loop uses ATSH DA10F1, ATCRA 18DAF110, the read, then ATAR and ATSH DB33F1 with existing restoration/reconnect safety. Scheduler floors are **2612: 1000 ms; 2610: 3000 ms; 2663: 3000 ms**, from completed attempts; most overdue wins. Host tests check floors, 2612 service, serialization and restored Mode 01 state. Failed subsequent reads retain values and let their ages grow.

`obd.read` and `>obd` expose answered flags, all four counts, clutch and generator with ages/nulls. CSV appends exactly `misfire_1,misfire_2,misfire_3,misfire_4,ac_clutch,generator_pct` after the existing columns, empty until read. The warning uses consecutive good reads, RPM >= 400, baseline/reset/wrap handling and 120-second cooldown, through the existing visual coolant-warning plumbing. No speech, UI trigger duplication or wake request. Bench `>misfire` uses the same trigger and cooldown, then restores the real baseline. Disconnect resets the baseline while retaining cooldown.

**Corrections during completion.** Scoped `polled_for` to the enable harness and `seen_during` to the DID harness using their existing compile-time variant split; kept `-Werror` and both tests. Added an enabled/live-connection guard before consuming a misfire reply, covering a disconnect observed during header restoration; deterministic harness assertions check disconnected and disabled replies cannot fire or move the baseline. Added explicit size preconditions to the synthetic test frame encoders (at most 64 bytes, encoded output under 180 bytes). Corrected README's obsolete fixed-rotation description.

**Cards as built.** Temps has coolant, intake air, ambient and A/C (ON accent, OFF dim, `--` before a read). Engine has RPM/VTEC hero, TIMING beside AFR, LOAD beside MAP, then MISFIRE with four counts labelled 1–4. Drive is unchanged. Every existing value remains; unsupported Mode 01 rows still hide and DID rows remain. Misfire increases use coolant amber until the next non-increasing read; readings older than 3 seconds dim. Existing full-height open cells, hairlines, label/unit 16, hero 48 and normal 40 fonts remain, with existing height fallback. No font configuration changed. **Layout accommodation:** three-digit misfire counts use 28 and four/five-digit counts use 20 to fit four cylinders across the screen. Physical clipping and visual sign-off remain unverified until flash.

**Files.** Firmware: `esp32/components/muse/muse_obd.[ch]`, `muse_obd_elm.[ch]`, `muse_obd_cards.[ch]`, `muse_obd_log.c`, `muse_obd_log_store.[ch]`, `muse_input.c`; `esp32/main/app.c`, `noise_control.cpp`. Tests: `esp32/tests/muse_obd_elm_harness.c`, `muse_obd_enable_harness.c`, `muse_obd_log_harness.c`, `did_cards_frames.h`, `test_muse_obd_did_cards.py`, `test_link_obd.py`, `test_muse_obd_did.py`, `test_muse_obd_log.py`, `test_muse_obd_more_pids.py`. Docs: `README.md`, `CLAUDE.md`, this task.

**Invariants/deviations.** No writes to the car, out-of-scope DIDs or generator card cell. No `muse_chat_session.cpp` or secrets changes; zero `muse_hatch_say` occurrences in `muse_obd.c`. Coolant/warmup logic and VTEC held-pose contract unchanged. New firmware calls reuse existing APIs; no new external API surface. Connection state is stored alongside the immutable DID table rather than inside it; long-count font fallback is described above. No other intentional scope deviations.

**Derek after the Mac build and flash:** misfire counts live on the Engine card, A/C state on the Temps card, and an increasing misfire count under load raises the warning in the car. Check A/C toggles, all card values fit, VTEC remains fresh, CSV columns agree, `>misfire` shows the idle warning without waking a sleeping screen, and OBD off remains quiet. Actual on-car behavior has not been verified here.
