# OBD-MORE-PIDS-TASK: five more Mode 01 values — branch obd-vtec-reaction

Written by Muse (spec), for Claude Code to implement, Muse QAs after. Read the repo AGENTS.md files first.

Precondition: the pipeline build has landed and the branch builds on the Mac (see OBD-PIPELINE-HANDOFF.md, which is item zero of the same session). This task integrates with the pipeline scheduler (the SCHED table and muse_sched_* in muse_obd_elm.c), not the old fixed rotation. Do not start on a branch that does not build.

## Why

Derek's 2008 S2000 supports five Mode 01 PIDs the firmware has never asked for (verified live 2026-10-22 from the car's own support masks, BE3EA813 / 8007F011 / FAD00000). All five were named by Derek for this run:

| PID | Value | J1979 decode |
|---|---|---|
| 0x34 | Wideband lambda (actual AFR) | lambda = (256A + B) / 32768; AFR = lambda x 14.7 (gasoline stoich; C/D bytes are the sensor voltage, unused) |
| 0x49 | Accelerator pedal position D | A x 100 / 255, percent |
| 0x4A | Accelerator pedal position E | A x 100 / 255, percent |
| 0x42 | Control module voltage | (256A + B) / 1000, volts |
| 0x33 | Barometric pressure | A, kPa absolute |
| 0x2F | Fuel tank level | A x 100 / 255, percent |

## Part 1: poll them

- Add all six PIDs (pedal is two) as first-class extras alongside the existing ones (muse_obd_extra_t / MUSE_OBD_EXTRA in muse_obd.h), with parsers in muse_obd_elm.c next to the existing ones. Support-scan gating keeps its exact current semantics: a PID the scan proves unsupported is never scheduled and shows "--" where it has a display surface. All six are supported on Derek's car; the gating is for other cars and for correctness.
- Scheduler tiers (freshness floors, same rules as the pipeline): 0x34 fast (1000 ms), 0x49 fast (1000 ms), 0x4A fast (1000 ms), 0x2F mid (2000 ms), 0x42 slow (3000 ms), 0x33 slow (3000 ms). Fuel level sits in mid, not slow, on purpose: its card cell should not live on the 3 s dimming boundary.
- Batching applies as it does to every Mode 01 PID: these ride batches, count misses, and evict under the pipeline's existing rules. Nothing special.

## Part 2: surfaces

- **obd.read**: every new value, following the existing extras pattern (value plus age). AFR reports both `lambda` (3 decimals) and `afr` (1 decimal, 14.7-based). Names: `afr`, `lambda`, `pedal_d_pct`, `pedal_e_pct`, `module_v`, `baro_kpa`, `fuel_pct`.
- **Cards**: two cells only this round. AFR joins the Engine card; fuel level joins the Drive card. Follow the established card layout rules (OBD-LOG-TASK Part 5: full-height open cells, hairlines, no pill backgrounds, Montserrat 48 hero / 40 normal, the 3 s stale dimming rule). Every existing value stays visible; reflow, do not delete. Pedals, voltage and baro get no card cells this round (log and obd.read only); Derek can promote them later.
- **Trip log**: append columns at the end of the row, in this order: `afr`, `fuel_pct`, `pedal_d_pct`, `pedal_e_pct`, `module_v`, `baro_kpa`. Append-only, so older parsers keep working; the header line lists them like the existing columns. This is a deliberate format extension of this round (the pipeline round's format freeze does not bind this one). Empty when a value has no reading yet, exactly as existing columns behave.

## Invariants

Read-only toward the car. Kill switch semantics. Support scan gating. The solenoid-held VTEC contract and the coolant/warmup triggers untouched. `muse_hatch_say` count in muse_obd.c stays 0. No muse_chat_session.cpp edits, no secrets changes. User-visible text says Muse, never Hatch. Provably-sized snprintf everywhere (the Mac IDF build is the real toolchain gate). Any new external API surface (none is expected; these are plain Mode 01 PIDs) gets its signature verified against real headers before it is committed, per the 2026-10-23 AGENTS.md lesson.

## Tests

- Parsers: each new PID against worked J1979 example frames, including lambda at stoich (AFR 14.7) and at the byte extremes; a missing/garbled reply is a miss, never a value.
- Scheduler: the six PIDs appear at their tiers, dedupe and eviction behave as for existing PIDs.
- Surfaces: obd.read carries the new fields; the log header and rows carry the new columns in order.
- Baseline: the suite state at branch tip after the pipeline handoff lands (OBD-PIPELINE-TASK Results say 706 tests, 0 failures, 6 known cJSON-environment errors, 10 skipped; re-baseline first if the handoff fix moved it, and say so in Results).

## Results (Claude fills in)

Commits, files touched, test counts, the tier table as built, card placement as built, any deviation, and the one line Derek needs: after his next flash, the new values are on the Engine and Drive cards, in obd.read, and in every new trip CSV.
