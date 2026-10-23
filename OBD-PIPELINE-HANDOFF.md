# OBD-PIPELINE-HANDOFF: state of the pipeline build, for Claude Code

Written by Muse, 2026-10-23. Derek reassigned OBD-PIPELINE-TASK.md to
Muse as a coding trial on 2026-10-23 00:43 PDT, then reassigned it back
to Claude Code at about 02:25 PDT after two Mac build failures in Muse's
code. This file is the complete handoff: what exists, what is verified,
what is broken, and what remains. The task spec itself is
OBD-PIPELINE-TASK.md (its Results section is written and accurate except
where this file corrects it).

## Branch state

- Branch `obd-vtec-reaction`, tip `5903c78`, working tree clean, pushed
  to origin. Verify before starting: HEAD is 5903c78 and the tree is
  clean. If not, stop and report.
- Commit map (4d1c089 is the spec commit, the base):
  - `b7a920c` the pipeline implementation (Muse)
  - `3f32081` tests (Muse)
  - `69a39c2` Results section of OBD-PIPELINE-TASK.md (Muse)
  - `71de529` QA correction to the Results test count (Muse QA)
  - `5903c78` the ble_gap_update_params type fix (Muse) — CONTAINS THE
    KNOWN DEFECT described below.

## What is implemented (all five parts of the spec)

- Response-driven poll loop in poll_rpm (muse_obd.c): the next request
  issues in the same iteration the previous reply parses; no delay,
  cadence, or timer in the poll path (verified by reading, and a contract
  test asserts the wiring).
- Scheduler (muse_obd_elm.c/.h): most-overdue-first over freshness floors,
  never caps. RPM target 0 (standing filler); DID 0x2612 floor 1000 ms
  (MUSE_SCHED_DID_MIN_MS); fast tier 1000 ms (MAP 0x0B, throttle 0x11,
  load 0x04, speed 0x0D); mid 2000 ms (STFT 0x06, LTFT 0x07, timing 0x0E);
  slow 3000 ms (coolant 0x05, IAT 0x0F, ambient 0x46). Dedupe is
  structural (one request outstanding, no PID twice per request). Items
  enter the schedule only if the support scan did not prove them
  unsupported. Per-PID misses evict after two, gated on a live ECU (a
  silent ECU's misses never count; RPM never evicts). NOTE: the spec's
  "existing invalid/unsupported machinery" for misses did not exist as
  code; this eviction was built new into the scheduler.
- Batching: up to 6 PIDs per Mode 01 request, RPM rides along when due
  and there is room. Whole-batch NO DATA or error latches batching OFF
  for the rest of the connection (muse_sched_latch_batch_off), no
  retries, no misses counted for its PIDs. DID is never batched.
- Link tuning: ATAT2 once per connection after the protocol is known.
  ATST starts at the ELM default 0x32 (200 ms); after 4 valued exchanges
  it trims once to twice the mean round trip, clamped 0x08..0x32; after 3
  consecutive answerless exchanges it steps back +0x04 (16 ms), capped at
  0x32. All steps logged; tuning never reconnects the session.
- Link stats in obd.read under "link" (main/app.c; registration text in
  main/noise_control.cpp): exchanges, per_sec (rolling over the last 16
  exchanges), last_rtt_ms, timeouts, batch_pct (null until the first
  reading), batch_off.
- Logger: row queue 64 -> 256 entries (muse_obd_log.c, PSRAM, about
  51 KB); trip CSV format unchanged.

## Verified by Muse QA (host side, all firsthand)

- Full host suite from esp32/ (`python3 -m unittest discover -s tests
  -p "test_*.py"`): **706 tests, 0 failures, 6 errors, 10 skipped** —
  identical profile to the pre-pipeline baseline; the 6 errors are the
  known cJSON-environment ones. (The implementer's report claimed 718;
  that was wrong — coverage deepened existing test methods and harness
  scenarios instead of adding cases. Corrected in 71de529.)
- OBD subset (test_link_obd, test_muse_obd_did, test_muse_obd_elm,
  test_muse_obd_log, test_muse_obd_enable): 52 tests, all pass.
- Invariants: muse_hatch_say count in muse_obd.c is 0; zero new snprintf
  in the diff; coolant/warmup trigger defines (MUSE_COOLANT_FIRE_C 105,
  REARM 100, SUSTAIN 5000, COOLDOWN 120000) untouched by the diff; no
  muse_chat_session.cpp, secrets, or partitions changes.
- Host QA cannot validate ESP-IDF/NimBLE API truth: this VM has no
  ESP-IDF. That gap is exactly where both defects below lived.

## The two Mac build failures (both Muse's, both the same root cause)

Root cause, stated once: Muse wrote first-use NimBLE API surface from
memory instead of from the real headers, and the host harness fake
repeated the same assumption, so host tests could not catch either one.

1. First build (tip 69a39c2): `muse_obd.c:672: error: passing argument 2
   of 'ble_gap_update_params' from incompatible pointer type`. The code
   passed `struct ble_gap_conn_params *`; the API takes
   `struct ble_gap_upd_params *`. Fixed in 5903c78 (type corrected in
   muse_obd.c; the fake header and harness were corrected to the real
   signature as well).
2. Second build (tip 5903c78, current): Muse's fix added two
   designated initializers from memory, and they are wrong for
   ESP-IDF's NimBLE:
   ```
   muse_obd.c:671:10: error: 'const struct ble_gap_upd_params' has no member named 'min_ce_events'; did you mean 'min_ce_len'?
   muse_obd.c:671:30: error: 'const struct ble_gap_upd_params' has no member named 'max_ce_events'; did you mean 'max_ce_len'?
   ```

## The known defect and its remedy (start here)

File: esp32/components/muse/muse_obd.c, in connect(), currently:

```c
    static const struct ble_gap_upd_params FAST = {
        .itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400,   /* 7.5 ms, 15 ms, 4 s */
        .min_ce_events = 0, .max_ce_events = 0,
    };
```

Remedy: delete the `.min_ce_events / .max_ce_events` line entirely
(designated initializers already zero the remaining members), or rename
the members to `.min_ce_len = 0, .max_ce_len = 0` per the compiler's own
hint. Check the real header on the Mac
($IDF_PATH/components/bt/host/nimble/nimble/nimble/include/nimble/ble.h
or the vendored copy in the build tree) before choosing; do not trust
this file's memory either.

Also audit while you are in there: the only other first-use NimBLE token
in the diff is `BLE_GAP_EVENT_CONN_UPDATE` in the gap event handler,
which reads only `event->conn_update.status` (believed correct, never
verified against real headers). Ninja stops at the first failing file,
so further from-memory surface may surface one error at a time; the
fastest path is to grep the pipeline diff (4d1c089..HEAD) for every
external API call with no pre-existing usage in the tree and check each
signature against the IDF v6.0.1 headers on the Mac in one pass.

## What remains

1. Fix the build (above) until `tools/muse/board.sh build s18` passes
   clean on Derek's Mac. The IDF toolchain exists only on the Mac; the
   VM loop is candidate fix -> Derek builds -> next error or pass.
2. Derek flashes (`tools/muse/board.sh flash s18`).
3. On-car verification (Derek drives; Muse reads obd.read):
   - link.per_sec is the achieved exchange rate (compare with the old
     rotation's roughly 3 exchanges/s effective per-signal rate and the
     about 3.3 s full lap).
   - link.batch_pct high and link.batch_off false (if the LELink refuses
     batches, batch_off true with single-PID running is a valid outcome,
     not a failure).
   - link.timeouts not climbing; no log_dropped on the next trip pull.
   - Regressions to watch: VTEC held pose on the solenoid, coolant and
     warmup reactions, card dimming behavior, trip logging end to end.
4. If the on-car numbers disappoint, the tuning knobs in order: DID
   floor (MUSE_SCHED_DID_MIN_MS), tier targets, ATST policy constants
   (MUSE_TUNE_*), BLE interval request values in connect().

## Standing rules for whoever touches this branch

- Invariants in OBD-PIPELINE-TASK.md still bind (read-only toward the
  car, kill switch, DID choreography, trigger constants, CSV format,
  muse_hatch_say = 0, no new snprintf without a size proof).
- Process lesson, also in ~/AGENTS.md (2026-10-23): any external API
  with zero existing usage in the tree gets its signature verified
  against the vendor's headers/docs before it is committed, and a test
  fake is never evidence for the signature it fakes.
- There is no scheduled Claude launch for this task anymore: the Friday
  cron was deleted when Derek reassigned the build. Whoever runs this
  handoff was launched by Derek directly.

## Resolution (Claude Code, 2026-10-23)

- No ESP-IDF or NimBLE headers exist on this VM. The real header was read
  instead from the exact source IDF v6.0.1 pins: the
  `components/bt/host/nimble/nimble` submodule of espressif/esp-idf at tag
  v6.0.1 is espressif/esp-nimble commit `7075927f97c6`, and its
  `nimble/host/include/host/ble_gap.h` was fetched raw from GitHub.
- `struct ble_gap_upd_params` there is `itvl_min, itvl_max, latency,
  supervision_timeout, min_ce_len, max_ce_len` (all `uint16_t`). The
  `.min_ce_events / .max_ce_events` line in connect() was deleted; the
  designated initializer zeroes `min_ce_len` and `max_ce_len`, which is
  what the line meant. The host fake (`tests/obd_enable_fakes/obd_fake.h`)
  now uses the real member names too.
- Audit of the pipeline diff (4d1c089..5903c78, components and main) for
  external API with no prior use in the tree: only three tokens, all now
  checked against that same header:
  - `ble_gap_update_params(uint16_t conn_handle, const struct
    ble_gap_upd_params *params)`: matches the call.
  - `BLE_GAP_EVENT_CONN_UPDATE` (3) and `event->conn_update.status`
    (`int`): match the handler, which logs the status only (it does not
    read the negotiated interval; the pipeline Results said it did and is
    corrected).
  - Everything else new in the diff already had uses in the tree
    (`cJSON_AddNullToObject`, `cJSON_AddBoolToObject`,
    `taskENTER_CRITICAL`, `ESP_LOGD`, `xQueueCreateWithCaps` unchanged).
    `muse_obd_elm.c` is plain C with no external API.
- Host suite after the fix: 706 tests, 0 failures, 6 errors (the known
  cJSON-environment ones), 10 skipped. OBD subset: 52 tests, all pass.
- Still unverified: the Mac build (`tools/muse/board.sh build s18`) is the
  real compiler gate, and remains Derek's step.

### A second defect, found by the same audit: batch replies (follow-up commit)

- The pipeline's batch parser expected a CAN ECU to answer a multi-PID
  Mode 01 request with one `41 <pid> <data>` line per PID, and the host
  fake car answered that way. That is not what J1979 sends. The ELM327
  datasheet (ELM327DSJ, "Multiple PID Requests", p.45, read from
  elmelectronics.com) shows the real answer to `01 04 05 0B 0C`: one
  message, `41` once, then PID/data pairs in any order, ten bytes printed as
  `00A` / `0: 41 04 3F 05 44 0B` / `1: 21 0C 17 B8 00 00 00`.
- On the car (protocol 7) the old parser would have read a multi-frame
  batch as nothing at all (the `0:` prefix fails every line, no NO DATA,
  so batching never latched off and RPM read as unknown), and a
  single-frame batch (`410C1AF80D3C`) as its first PID only, evicting the
  rest after two misses against a "live" ECU.
- Fixed: `muse_elm_parse_batch` now reads the reply as J1979 messages
  (single-frame lines, or a length line and its in-order `N:` frames cut at
  that length), walks `41` + PID/data pairs using each PID's byte count,
  stops at an unasked PID or a cut-short one, and lets each answering ECU's
  message contribute (first answer wins). The old line-per-PID shape still
  parses, since each such line is a one-PID message. Single-PID requests
  are unchanged.
- Tests: the ELM harness has the datasheet reply verbatim (two orders,
  with and without spaces, after SEARCHING...), a single-frame batch, two
  ECUs, a missing and an out-of-order frame, an unasked PID and a cut-short
  one. The enable harness's fake car now answers batches as one J1979
  message (multi-frame past seven bytes); against the old parser it fails
  (`batch 0%`), against the new one it passes.
- Still unverified on the car: whether the LELink passes multi-PID
  requests through. If it doesn't, the existing latch (whole-batch NO DATA
  or error: single-PID for the connection) covers it.
- Host suite after the batch fix: 706 tests, 0 failures, 6 errors (known cJSON), 10 skipped; OBD subset 52, all pass.
