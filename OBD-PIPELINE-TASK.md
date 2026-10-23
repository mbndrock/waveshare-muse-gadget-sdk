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

Commits, files touched, test counts, the scheduler targets as built, the DID floor as built, ATST value(s) and step-back rule as built, BLE parameter outcome, any logger/buffer changes, deviations, and the one line Derek needs: after his next flash, the `link` stats in obd.read are where the achieved rate shows up.
