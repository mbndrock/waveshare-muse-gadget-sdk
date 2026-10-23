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

## Results (Claude Code, 2026-10-23)

Built by one Claude Code session that was killed mid-stage by a runtime restart, then audited and finished by the next. The audit read the whole uncommitted diff against this spec before keeping anything; nothing was thrown away, and what was added on top is listed below.

**Commit.** One commit on `obd-vtec-reaction`, on top of `5e22ddc` (Stage 0), with the implementation, the tests, the docs and this section.

**Files.** `components/muse/`: `muse_obd.c`, `muse_obd.h`, `muse_obd_elm.c`, `muse_obd_elm.h`, `muse_obd_cards.c`, `muse_obd_cards.h`, `muse_obd_log.c`, `muse_obd_log_store.c`, `muse_obd_log_store.h`, `muse_input.c`. `main/`: `app.c`, `noise_control.cpp` (the obd.read description). Tests: `muse_obd_elm_harness.c`, `muse_obd_enable_harness.c`, `muse_obd_log_harness.c`, `test_link_obd.py`, `test_muse_obd_did.py`, `test_muse_obd_log.py`, and a new `test_muse_obd_more_pids.py`. Docs: `README.md`, `CLAUDE.md`.

**Decodes as built** (`pid_nbytes` / `pid_value` in `muse_obd_elm.c`, the one table the single-PID parsers and the batch walk share):

| PID | Bytes | Held as | Reported |
|---|---|---|---|
| 34 | 4 (A B lambda, C D unused) | raw 256A+B; `muse_elm_afr_x10` and `muse_elm_lambda_x1000` round to nearest | `afr` 1 decimal (0x8000 = 14.7), `lambda` 3 decimals (1.000) |
| 49, 4A | 1 | A x 100 / 255 | `pedal_d_pct`, `pedal_e_pct` |
| 42 | 2 | 256A+B mV | `module_v` 3 decimals (0x3615 = 13.845) |
| 33 | 1 | A | `baro_kpa` |
| 2F | 1 | A x 100 / 255 | `fuel_pct` |

A reply cut short of its PID's byte count (34 needs all four, 42 both) is a miss, never a value, singly and inside a batch.

**Tier table as built** (`SCHED` in `muse_obd_elm.c`, `MUSE_SCHED_ITEMS` 12 -> 18):

| Tier | Floor | PIDs |
|---|---|---|
| filler | 0 | 0C RPM |
| DID | 1000 ms | 2612 |
| fast | 1000 ms | 0B, 11, 04, 0D, **34, 49, 4A** |
| mid | 2000 ms | 06, 07, 0E, **2F** |
| slow | 3000 ms | 05, 0F, 46, **42, 33** |

Support-scan gating, batching, miss counting and eviction are the pipeline's, unchanged: the new PIDs are ordinary rows of the same table. On Derek's masks (BE3EA813 / 8007F011 / FAD00000) all six are supported and scheduled; each one alone cleared from its mask leaves the schedule (harness).

**Card placement as built.** Engine: RPM (hero), TIMING, LOAD beside MAP, **AFR** (`14.7`, unit `:1`). Drive: SPEED (hero), THROTTLE beside **FUEL** (both with the 0-100 bar), STFT beside LTFT. Halves now pair two at a time in order (`second_half()`), so a card can hold more than one pair. Pairing LOAD/MAP and THROTTLE/FUEL keeps each card at the height units it had (Engine 5, Drive 4, Temps 3), so every normal cell keeps Montserrat 40 and the heroes 48; nothing was removed. A row the scan proves unsupported hides as before (a pair goes once both halves have), and its value reads `--`.

**obd.read and the console.** The seven keys, each with its `_age_ms`, null without a current reading. `MUSE_OBD_EXTRA[].decimals` carries the scaling: obd.read sends the value in its unit as a JSON number (`14.7`, `13.845`; lambda at stoich is the number `1`, since JSON numbers carry no trailing zeros), and `>obd` prints it fixed-point (`1.000`) through `obd_fixed()`. The Link registration text names the new keys.

**Trip log.** Columns appended after `vtec_psw`: `afr,fuel_pct,pedal_d_pct,pedal_e_pct,module_v,baro_kpa`, the header line listing them; AFR and volts are written with their point (`14.7`, `13.845`), every column empty until it has a reading. `MUSE_LOG_CHANNELS` 13 -> 19 and `MUSE_LOG_ROW_MAX` 168 -> 259 bytes, past 255, so the log queue's row length went from `uint8_t` to `uint16_t` (it would have truncated the widest rows). The 256-row PSRAM queue grows from about 51 KB to about 68 KB.

**snprintf size proofs.** `obd_fixed()` (`muse_input.c`): an int with 1..3 decimals prints at most 12 characters into a 16-byte buffer (`-214748364.8`, `-2147483.648`), checked at both extremes by `test_console_fixed_point` built with `-Wformat-truncation=2 -Werror`. The AFR cell (`muse_obd_cards.c`): `shown` is >= 0 there, so `%d.%d` is at most 9 + 1 + 1 characters into `text[16]`. The log uses no printf (`put_fixed()` writes into the bounded buffer). No new external API: everything new is plain C, LVGL and cJSON calls already used in the tree.

**Tests.**
- Baseline, re-run at `5e22ddc` on a clean worktree before any of this: 706 tests, 0 failures, 6 errors, 10 skipped. That matches the handoff, so the Stage 0 fixes did not move it.
- After: **712 tests, 0 failures, 6 errors, 10 skipped** (`python3 -m unittest discover -s tests -p "test_*.py"` from `esp32/`).
- The 6 errors are the same six `setUpClass` errors as at the baseline, identical by name: `managed_components/espressif__cjson` doesn't exist on this VM (no ESP-IDF).
- OBD subset (`test_link_obd`, `test_muse_obd_did`, `test_muse_obd_elm`, `test_muse_obd_log`, `test_muse_obd_enable`, `test_muse_obd_more_pids`): 58 tests, all pass (the 52 from before plus the 6 new ones).
- What the new tests cover:
  - Parsers: stoich (14.7 / 1.000), both byte extremes, rich and lean, no spaces, the echo, and every shape of miss (short, garbled, refused, another PID's, NO DATA, `?`).
  - All six in one three-frame J1979 batch, and AFR cut short at a message's end.
  - Tiers by sweeping the scheduler at +1.5 s, +2.1 s and +3.05 s. Eviction after two live misses and none after silent ones. Scan gating on Derek's masks.
  - The real OBD task against a fake car carrying Derek's masks: all seven values land in the snapshot, stay within 3 s over 4 s of polling, and reach the log rows.
  - Log rows and header with the appended columns, at the widest row and the longest header. The card rows and height units. obd.read and `>obd` scaling. The invariants.

**Deviations and notes.**
- The spec says "five" PIDs, and its table has six (fuel level is the sixth). All six were built, which is seven values with lambda.
- On top of the inherited work, the audit added three things. `README.md` now covers the new keys, cards and columns. `CLAUDE.md`'s Init bullet still described the pre-pipeline `MUSE_ELM_SECONDARY` rotation, so it now describes the scheduler and its tiers. The coolant bullet said "first secondary in each cycle" and now says "slow tier".
- Invariants hold: read-only toward the car, the kill switch, scan gating, the VTEC solenoid contract and the coolant and warm-up constants are untouched (asserted in `test_invariants`). `muse_hatch_say` occurs 0 times in `muse_obd.c`. There are no `muse_chat_session.cpp` or `secrets/` changes, and no user-visible "Hatch".

**Not verified here.** The ESP-IDF build: this VM has no IDF, so `tools/muse/board.sh build s18` on the Mac is still the real compiler gate. Nothing has run on the car.

**For Derek:** after your next flash (`board.sh build s18`, then `flash s18`), AFR is on the Engine card and fuel level on the Drive card. All seven values (`afr`, `lambda`, `pedal_d_pct`, `pedal_e_pct`, `module_v`, `baro_kpa`, `fuel_pct`) are in obd.read, and every new trip CSV ends with the six new columns.
