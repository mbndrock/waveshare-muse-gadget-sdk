# OBD-PIPELINE-TASK: saturate the LELink link — branch obd-vtec-reaction

Written by Muse (spec), for Claude Code to implement, Muse QAs after. Read the repo AGENTS.md files first. Builds on the shipped state: OBD-LOG-TASK and OBD-VTEC-DID-TASK are in (HEAD includes them).

## Derek's requirement, verbatim in substance

"I just want the pipeline between the LELink and my ESP32 maxed out. If the LELink can send it, the ESP32 should receive it. Match the rate the LELink is capable of."

Translation into engineering terms: **the adapter's own response time is the only permitted bottleneck.** The firmware must never be the idle party on the link: no fixed cadence, no rotation timing, no sleeps between exchanges. Whenever the car is connected, the next request goes out the moment the previous response is parsed. Everything below serves that one rule.

Context for why this round exists: the current poller issues single-PID requests ("01%02X") in a fixed RPM/secondary alternation, 10 secondaries plus one DID slot per rotation, about 3.3 s per lap at the LELink's observed 150-350 ms round trips. That schedule was a display-era design. It leaves the link idle by construction and is now the bottleneck for the cards, the VTEC solenoid freshness, and the trip logs alike.

## Part 1: response-driven link loop

- The poll worker is driven by completions, not timers: on each finished exchange (prompt received, reply parsed), it immediately selects and issues the next request. There is no inter-request delay anywhere in the OBD poll path.
- The queue is never empty while connected: RPM is the standing filler request, so the link idles only when the adapter itself is still answering.
- Existing connection management (scan, connect, init, support scan, DID probe, retry on first RPM reply, backoff, kill switch) is unchanged. This round changes what happens inside the polling state only.

## Part 2: priority queue replaces the fixed rotation

- Producers express interest as soft freshness targets, not a fixed order: RPM (permanent top priority), DID 0x2612 (target about 1 s), and the secondaries with per-signal targets (fast tier: MAP, throttle, load, speed — about 1 s; mid: STFT, LTFT, timing — about 2 s; slow: coolant, IAT — about 3 s; coolant's reaction path keeps its own rules regardless).
- The scheduler picks the most overdue item; ties break by priority. **Targets are floors on freshness, never rate caps**: if the link runs faster than the targets, everything simply refreshes sooner. Nothing may be deliberately delayed to meet a target.
- Dedupe: a signal already queued or in flight is never queued twice.
- One shaping rule, flagged as tunable in Results: the DID keeps a minimum interval of about 1 s between reads. Its multi-frame exchange is the longest on the link; without the floor it could crowd out Mode 01 when everything is overdue at connect time. The pipeline stays saturated regardless; this only shapes the mix.

## Part 3: multi-PID batching

- Mode 01 requests may carry up to 6 PIDs in one request when the scheduler's due items are batchable (RPM rides along in a batch whenever it is due and the batch has room). CAN ECUs answer with one 0x41 line per PID; parse each line into its reading with the usual age tracking.
- A PID missing from a batch reply is a miss for that PID only, fed through the existing invalid/unsupported machinery (two misses and it leaves the schedule, as today).
- If a batch request as a whole errors or returns NO DATA, latch batching OFF for the rest of that connection and continue single-PID through the same queue. Never retry-loop a refused batch. Record the latch state where Part 5's stats live.
- Single-PID requests remain the fallback shape and the shape for anything not batchable. The DID exchange is unchanged and never batched.

## Part 4: link tuning

- After the protocol is known at init: enable aggressive adaptive timing (ATAT2) and trim the ELM response timeout (ATST) toward the car's measured reply latency instead of the 200 ms default. Implement a bounded step-back: if timeouts accumulate, raise ATST a notch (and log it); never let tuning wedge or reconnect the session — the existing init/retry machinery owns recovery, as with the DID exchange.
- On the BLE side, request fast connection parameters (7.5-15 ms interval range) for the adapter connection. If the stack or the adapter refuses, so be it; note the outcome in Results if it is observable.
- Init's existing formatting wins (ATE0/ATL0/ATH0/ATS0) stay.

## Part 5: prove it — link stats

- `obd.read` gains a `link` object: exchanges completed, rolling exchanges/second, last round-trip ms, timeouts, batch share (percent of Mode 01 readings that arrived via batch), and whether batching is latched off on this connection. The verdict on this round is read from these numbers on Derek's car; host tests cannot measure BLE.
- The trip logger must keep up: rows are written from the same snapshot the logger already consumes, and `log_dropped` staying at 0 under the faster rate is an acceptance check. If the writer or buffer needs resizing to hold that, it is in scope; say what changed in Results.

## Invariants (unchanged, QA will check)

- Read-only toward the car. Kill switch semantics. Support scan and protocol detection. DID choreography and state safety. Coolant/warmup thresholds and logic. The solenoid-held reaction contract. Card behavior contracts (the 3 s stale dimming rule stays; it should simply stop triggering). Trip CSV format unchanged, only denser. `muse_hatch_say` count in muse_obd.c stays 0. No muse_chat_session.cpp edits, no secrets changes. User-visible text says Muse, never Hatch. Provably-sized snprintf everywhere (the Mac IDF build is the real toolchain gate).

## Tests

- Scheduler: priority and overdue ordering, dedupe, DID floor, filler behavior when nothing is overdue.
- Batch parser: full reply, partial reply (one PID missing), whole-batch refusal latching single-PID mode, malformed lines.
- Stats math: rates and shares over scripted exchange sequences.
- Baseline to beat: 706 tests, 0 failures, 6 known cJSON-environment errors, 10 skipped.

## Results (Claude fills in)

Implemented by Muse, not Claude Code: Derek reassigned this build on
2026-10-23 as a coding trial, on this same spec and its invariants.

Commits: b7a920c (the pipeline), 3f32081 (tests), and this document's commit.
Files touched: components/muse/muse_obd.c, muse_obd.h, muse_obd_elm.c,
muse_obd_elm.h, muse_obd_log.c; main/app.c, main/noise_control.cpp (the
obd.read link object and its description); the two OBD harnesses, the
enable fakes, and the three OBD contract test files.

As built:

- Poll loop: response-driven. poll_rpm issues the scheduler's pick the
  moment the previous exchange completes; there is no vTaskDelay, cadence
  or timer anywhere in the poll path. Connection management outside the
  polling state is unchanged.
- Scheduler targets (freshness floors, never caps; most overdue first,
  ties by tier then table order): RPM 0 ms (the standing filler); DID
  1000 ms (MUSE_SCHED_DID_MIN_MS in muse_obd_elm.h, the tunable floor);
  fast tier 1000 ms (MAP 0x0B, throttle 0x11, load 0x04, speed 0x0D);
  mid tier 2000 ms (STFT 0x06, LTFT 0x07, timing 0x0E); slow tier 3000 ms
  (coolant 0x05, IAT 0x0F, ambient 0x46). Nothing is queued twice: one
  request outstanding, no PID twice inside it.
- Batching: up to 6 PIDs per Mode 01 request, RPM riding along whenever
  due and there is room. A whole-batch NO DATA or error latches batching
  OFF for the rest of the connection and the loop carries on single-PID,
  with no retry of the batch and no misses counted for its PIDs. A PID
  missing from a batch reply is a miss for that PID only. The DID is
  never batched.
- Misses: a PID leaves the schedule after two misses against a live ECU
  (another PID in the same exchange read a value, or RPM is locked).
  Misses against a silent ECU do not count, so an engine-off period at
  connect cannot evict the schedule. RPM is never evicted.
- Link tuning: ATAT2 once per connection, after the protocol is known.
  ATST starts at the ELM default 0x32 (200 ms). After 4 valued exchanges
  it is trimmed once, to twice the mean round trip (the ATST byte is the
  round trip in 4 ms units), clamped to 0x08..0x32. After 3 consecutive
  answerless exchanges it steps back up one 0x04 (16 ms) notch, capped at
  0x32. Every step is logged; tuning never wedges or reconnects the
  session.
- BLE parameters: fast connection parameters are requested right after
  connect (interval min 6 / max 12, 7.5-15 ms; latency 0; supervision
  timeout 4 s). The request's return code is logged, and a
  connection-update event logs the negotiated interval when one arrives.
  A refusal leaves the defaults in force and changes nothing else. The
  fake stack in the host harness accepts (returns 0); the real adapter's
  answer is observable only in the flash log on the car.
- Link stats in obd.read, under "link": exchanges, per_sec (rolling, from
  the completion times of the last 16 exchanges), last_rtt_ms, timeouts,
  batch_pct (the share of readings that arrived in multi-PID replies;
  null until the first reading), batch_off.
- Logger: the row queue went from 64 to 256 entries (PSRAM-backed, about
  51 KB, roughly 25 s of headroom at the pipeline's fastest sample rate)
  so a slow flash write cannot starve it into drops. log_sample is called
  whenever a request carried RPM, as before; the CSV format is unchanged.

Tests: 718 run, 0 failures, 6 errors (the pre-existing cJSON-environment
ones, unchanged from baseline), 10 skipped. Baseline to beat was 706 / 0 /
6 / 10. New coverage: scheduler ordering, dedupe, DID floor, filler,
two-miss eviction and the batch latch; batch parser (full reply, partial
reply, a malformed line, whole-batch NO DATA and "?"); link stats math;
the ATST policy. The DID and enable harnesses were reworked to the
pipeline: the per-DID choreography assertions are unchanged, tier
coverage is judged over the whole inter-DID span (one window is only
about a second of link time, shorter than the slow tier's target), and
the fake car now answers batched Mode 01 requests.

Deviations and notes:

- Ambient (0x46) sits in the slow tier with IAT. The spec tiered coolant
  and IAT as slow and did not place ambient; dropping it would have
  broken a card field on cars that support it.
- The spec's "existing invalid/unsupported machinery" for per-PID misses
  did not exist as code: support came only from the scan. The two-miss
  eviction lives in the scheduler now, gated on a live ECU as above.
- Stats "timeouts" are no-prompt failures plus NO DATA outcomes. A
  whole-batch error is not counted as one (it latches batching instead).
- The console's >obd printout is unchanged; only obd.read gained the
  link object.
- Host QA only on this VM, as ever: the Mac IDF build remains the
  toolchain gate. The poll path adds no snprintf (the batch command and
  the ATST command are built digit by digit, sized by construction).

Derek: after the next flash, obd.read's link object is the verdict.
per_sec is the achieved exchange rate and batch_pct how much of it rides
in batches; if batch_off is true, the adapter refused batching and the
loop is running single-PID for that connection.
