# Task: steering angle from the VSA modulator (DA28 discovery, then decode)

Status: specced by Muse 2026-10-24. Not yet built.

## Why

Steering angle is the one chassis signal nothing else in the system can
reconstruct: GPS and the IMU give speed, heading and G, but the driver's
input only exists on the car. The 2008 S2000's steering angle sensor feeds
the VSA modulator-control unit, which is a different ECU from the engine
ECU (DA10) behind every current DID (2610/2610/2663). Even a diagnostic
rate of 2-5 Hz is useful: steady-state balance, steering-vs-yaw, and
throttle-unwind timing on track.

## The lead (from OBDb, verified 2026-10-24)

No public signalset decodes steering angle for any Honda. But the OBDb
signalsets for Accord / Pilot / Fit / Civic all carry a second brake-system
ECU at header **DA28** (25 commands, service 0x22), and two of its DIDs are
decoded:

- DID **4004**: brake switch at data bit 127 (byte 15, bit 7) - the reply
  block is at least 16 data bytes.
- DID **4005**: brake pressure at data bit 56 (byte 7), 16-bit,
  range -25..120 bar.

The rest of the DA28 space OBDb scanned is recorded raw and undecoded:
4000-4003, 4006-4009, 4030-4039, 4070-4073, E600, E602. Steering angle,
yaw and lateral G are presumably fields inside one or more of those
blocks. Caveats: those maps come from newer Hondas; the S2000's VSA unit
is older, so the address is a strong hypothesis, not a certainty, and the
4004/4005 layouts are validation targets, not facts. Response addressing
mirrors DA10: ATSH DA28F1, ATCRA 18DAF128.

## Phase 1 - discovery firmware (this build)

Read-only, always: service 0x22 reads only. No session control (0x10), no
routine control, nothing written to the car, same invariant as the DA10
DIDs.

1. Generalise the DID header session: a session is parameterised by
   (tx header, rx filter). DA10 sessions behave exactly as today
   (grouped due DIDs, one restore). A VSA session uses ATSH DA28F1 /
   ATCRA 18DAF128 and the same restore afterwards (ATAR, ATSH DB33F1).
   A VSA session never interleaves a Mode 01 or DA10 exchange; the poll
   loop still has exactly one request outstanding. The OBD kill switch
   covers VSA traffic too: OBD off means no VSA requests, ever.
2. Presence probe, once per connection, after the DA10 DID probes:
   one DA28 session reading DID 4004. Any parseable reply (positive
   62 4004 or a well-formed negative) records `vsa` answered state for
   obd.read: `vsa` true/false/null like `did_2612`. A dead address costs
   one timed-out exchange per connection and nothing else; failure is
   silent in the UI.
3. Raw scan, on demand only: a new Link command `obd.vsa_scan {}` runs
   the DID list (4000-4009, 4030-4039, 4070-4073, E600, E602) at DA28,
   one read per poll-loop gap so driving polls are never starved, and
   stores each outcome (positive reply bytes / negative response code /
   timeout). `obd.vsa_raw {did}` returns one stored reply as hex.
   obd.read gains `vsa_scan` state (idle/running/done, n answered).
   The scan exists so a parked lock-to-lock session can be captured:
   run scan at wheel centre, full left, full right; Muse diffs the
   three raw sets and finds the field that sweeps.
4. obd.read also gains the decoded-if-confirmed fields for the two
   validation targets, clearly provisional: `vsa_brk_sw` (4004 byte 15
   bit 7) and `vsa_brk_p` (4005 byte 7, 16-bit, -25..120 bar scaling).
   They are how we confirm the address on the car: Derek presses the
   brake pedal, the bit flips and the pressure rises. If they do not
   behave, the address or layout is wrong and Phase 2 does not start.

Host tests: fake-adapter harness grows a DA28 ECU (answers 4004/4005
with scripted bytes, NRC or silence for the rest). Choreography checks
mirror the DA10 session invariant: VSA requests only inside a DA28
session, restore after, no interleave. Probe-failure path: polling
continues, `vsa` false, no retry storm (probe once per connection).

No trip-log or card changes in Phase 1. No new sdkconfig.

## Phase 2 - decode and productionise (separate build, after the car capture)

- Muse's diff of the three-position raw capture identifies the
  steering field (expect a signed multi-byte value sweeping roughly
  +/-540 deg of wheel angle, or raw counts with a fixed centre).
  Brake switch / pressure behaviour from Phase 1 anchors the block
  layouts.
- Production read: the steering DID joins the schedule on its own tier
  (target 250-500 ms; it is one short session per read) while the link
  is polling. `steer_deg` (1 decimal, signed) in obd.read with its age;
  CSV gains a trailing `steer_deg` column (appended last, empty until
  first read, same rules as the other appended columns). Cards are out
  of scope until the value is trusted; a Drive-card steering cell is a
  later round if Derek wants it.
- If yaw rate / lateral G ride in the same block, decode them in the
  same round as `yaw_dps` / `lat_g` with the same treatment, but only
  from captured evidence, never by analogy.

## Gates

- Host suite green (baseline at spec time: 729 tests, 0 failures
  expected, 6 known cJSON environment errors, 10 skips; one unrelated
  failure under investigation from the 7a00147 QA - recheck before
  calling this round's suite).
- Invariants: muse_hatch_say in muse_obd.c stays 0; coolant defines and
  reaction paths untouched; partitions.csv and secrets untouched;
  read-only toward the car.
- Mac build is the compile gate (app.c / settings are not host-compiled).
- Car validation is Derek, parked: presence probe verdict, brake pedal
  flip, then the three-position scan for the Phase 2 diff.

## Results

(to be filled by the builder, with Muse's QA appended)
