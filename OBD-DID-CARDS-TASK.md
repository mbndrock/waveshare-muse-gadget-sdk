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

## Results (Claude fills in)

Commits, files touched, test counts, the fixture lengths confirmed for 2610 and 2663, the per-DID floors as built, card placement as built, any deviation, and the one line Derek needs: after his flash, misfire counts live on the Engine card, A/C state on the Temps card, and a misfire under load raises the warning in the car.
