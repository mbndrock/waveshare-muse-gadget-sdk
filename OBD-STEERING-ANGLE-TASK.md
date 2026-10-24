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

No new sdkconfig.

## Phase 1 addendum (Derek, 2026-10-24): brake pressure is a product value

Brake pressure is not just a probe. Once it is being read, Derek wants it
visible while driving:

5. Scheduled VSA polling: once the presence probe confirms DA28, VSA
   reads join the schedule as their own tier (500 ms target): one DA28
   session reading 4004 and 4005 grouped, exactly like the grouped DA10
   DID sessions. No VSA polling before presence is confirmed, and none
   while OBD is off.
6. Drive card gains a **BRAKE** cell: brake pressure in bar (from 4005),
   with a thin 0-120 bar gauge bar in the same style as throttle/fuel.
   Pair it with THROTTLE (the two pedals together); reflow the remaining
   cells without shrinking any below the existing minimum sizes, and
   document the final layout in CLAUDE.md as usual. The cell appears
   only once VSA presence is confirmed (before that the card is exactly
   as today), and reads "--" until the first successful read.
7. Trip CSV gains a trailing `brake_p` column (bar, 2 decimals), empty
   until the first read, appended after `generator_pct` - the same
   treatment as every other card value, since card values are logged
   values.

Phase 2 note: when the steering DID is decoded, it joins this same VSA
session group rather than paying for its own header setup.

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

Built by Muse Code; the first run was killed by a VM restart ~47 minutes
in with nothing committed, and a second run audited its working tree item by
item against this spec, finished it, and committed.

**State of the inherited work.** Essentially complete. All seven Phase 1
items were implemented in the tree and the full host suite already passed
on it as inherited (737 tests, 0 failures, the 6 known cJSON-environment
errors, 10 skips). The audit found one defect worth fixing and made that
one change (plus its test); everything else was verified as built and
committed as the first run left it.

**What was built** (by spec item; `CLAUDE.md`'s VSA bullets are the
architecture record):

1. Generalised DID sessions: `MUSE_DID_INFO` rows carry their ECU's
   (tx header, rx filter); `did_begin(d)` sends them. DA10's three DIDs and
   DA28's two group only within their own ECU's session (grouping keyed on
   the row's `atsh`), one restore (`ATAR`, `ATSHDB33F1`) per session, one
   request outstanding, read-only 0x22 throughout. OBD off means no VSA
   traffic: the poll loop stops, and `obd.vsa_scan` is refused outright.
2. Presence probe: `probe_vsa()`, once per connection inside `probe_did()`
   after the DA10 probes (so the first-RPM protocol retry covers it),
   protocol 7 only, one DA28 session reading DID 4004. A parseable positive
   records `vsa = 1` and schedules both VSA DIDs (4005 is not separately
   probed). Refused (a well-formed negative still records the verdict),
   silent or garbled records `vsa = 0` and neither DID is asked again on
   that connection; `-1`/null while no connection has asked. The verdict
   survives a drop like `did_2612`; the readings clear. One interpretation
   to note: the spec said "any parseable reply" records the answered
   state; as built, a negative records `vsa = false` (the verdict is kept,
   per-connection, no retry) and only a positive starts polling — the
   `did_2612` pattern.
3. Raw scan: `obd.vsa_scan {}` (refused while OBD is off) runs
   `MUSE_VSA_SCAN_DIDS` (4000-4009, 4030-4039, 4070-4073, E600, E602; 26
   DIDs) at DA28, one DID per poll-loop pass in its own header session
   (`vsa_scan_step`, `muse_elm_parse_did_raw`, replies up to 128 bytes),
   storing positive bytes / the refusal's NRC / timeout per DID.
   `obd.vsa_raw {did}` takes a hex string or a number and returns one
   stored outcome (`result`, `nrc`, `bytes`, `hex`). `obd.read`'s
   `vsa_scan` is `{state: idle|running|done, answered, total}` (answered
   counts positives). A dropped link forgets the scan and its results; a
   non-CAN-29 link ends it done with nothing answered. The scan makes
   progress only while the link polls.
4. Provisional decodes in `obd.read` (and `>obd` on the console):
   `vsa_brk_sw` (4004 data byte 15 mask 0x80, needs >= 16 data bytes) and
   `vsa_brk_p` (4005 data bytes 7-8 big-endian, needs >= 9; bar, 2
   decimals), each with its `_age_ms`, plus `vsa` (true/false/null).
   Scaling: `muse_vsa_brake_centibar` maps the raw count linearly over
   OBDb's -25..120 bar range, so raw 0 is -25.00 bar and 0 bar sits at raw
   11299 — provisional like the layouts, to be confirmed by Derek's pedal
   press.
5. Scheduled VSA polling: once presence is confirmed, 4004+4005 join the
   scheduler on a 500 ms floor, grouped in one DA28 session with the most
   overdue first; nothing before confirmation, nothing while OBD is off.
6. Drive card BRAKE cell (final layout): halves pair BRAKE/THROTTLE then
   FUEL/STFT, with LTFT a lone half in its own row, so Drive is five
   height units where it was four. `fit_font` steps Drive's bar cells
   (BRAKE, THROTTLE, FUEL) to the ladder's 28 minimum; SPEED keeps 48 and
   the trims 40. BRAKE reads tenths of a bar (`47.5`, negatives as `-2.5`)
   with a thin 0-120 bar gauge. It is hidden until `vsa` is confirmed and
   reads `--` until the first 4005 read. One deviation from "before that
   the card is exactly as today": pairing is build-time, so before
   confirmation the same five values already ride the five-unit layout
   (THROTTLE stretches BRAKE's row); documented in `CLAUDE.md`. Documented
   there too that BRAKE/THROTTLE/FUEL sit at the 28 font minimum — the
   existing ladder minimum, not below it.
7. Trip CSV: trailing `brake_p` after `generator_pct` (`MUSE_LOG_BRAKE_P`,
   bar with 2 decimals, empty until the first VSA read; negatives format
   through the existing signed fixed-point path).

**Fix made in the finishing run.** The two VSA parsers copied the DA10
parsers' 64-byte reply buffer, but the VSA block sizes are unknown and the
scan already stores 128: a 4004 reply longer than 64 bytes would have
parsed as no-reply, the presence probe would record `vsa = false`, and
polling, the card and the log column would never start even though the
ECU answered. `muse_elm_parse_vsa_4004/4005` now accept 128-byte replies
(the 320-char ELM response buffer remains the real ceiling, as it already
was for the scan; only the parser stops being the tighter limit). The elm
harness's `test_vsa_parse` gains 83-byte 4004/4005 blocks, and its
`elm_bytes` renderer now sizes its abort bound from the frame count
instead of a flat 64-byte cap.

**`esp32/main/noise_control.cpp` verdict: keep — it is required by this
build.** That file's `build_register_json` is where every `obd.*` Link
command is advertised to the Muse app: the change registers the two new
commands (`obd.vsa_scan`, `obd.vsa_raw` with its `did` parameter) and
extends `obd.read`'s description with `vsa`, `vsa_brk_sw`, `vsa_brk_p` and
`vsa_scan`. Without it the commands in `app.c` would be unreachable from
the app. It is not compiled by the host suite, so its wording was checked
by eye against the neighbouring registrations.

**Tests.** New: `esp32/tests/test_muse_obd_vsa.py` (8 tests — decodes,
generalised sessions, presence probe, scan commands, obd.read/console,
BRAKE cell, trip column, invariants) and the elm harness's `test_vsa_parse`;
`muse_obd_enable_harness.c` grows a fake DA28 ECU (session choreography,
grouping, probe-failure once per connection, kill switch). Existing tests
adapted only where the design changed: `test_muse_obd_did.py` and
`test_muse_obd_did_cards.py` now pin the per-ECU header literals in
`muse_obd_elm.c` instead of inside `did_begin`; `test_muse_obd_more_pids.py`
expects `MUSE_SCHED_ITEMS 22` and Drive at 5 height units; the log tests
expect the trailing `brake_p` column. No test was weakened or deleted.

Full host suite (`python3 -m unittest discover -s tests -p "test_*.py"`
from `esp32/`): **737 tests, 0 failures, 6 errors, 10 skips** — the 6 are
the known cJSON-environment errors (no `managed_components` in this VM),
unchanged from baseline. That's 729 baseline + 8 new. The one failure
from the earlier QA round did not reproduce (confirmed flake before this
run; absent in both full runs here).

**Gates checked here.** `muse_hatch_say` in `muse_obd.c`: 0. Coolant
defines and reaction paths untouched (no coolant lines in the diff;
FIRE 105 / REARM 100 / SUSTAIN 5000 / COOLDOWN 120 s verified in place).
`partitions.csv` and `secrets/` untouched. Every DID request in the table
is a `22xxxx` read; no 0x10 session control anywhere in the diff. New
snprintf/hex code in `app.c`, `muse_input.c` and `muse_obd_cards.c`
checked by hand for provable sizing (they are not host-compiled).

**Not verifiable from here.** The Mac build (the `app.c` / settings /
cards / `noise_control.cpp` compile gate, `-Werror=format-truncation`) and
the real car: DA28 presence on the 2008's VSA unit, the brake-pedal flip
confirming the 4004/4005 layouts, and the three-position raw scan that
unblocks Phase 2. Phase 2 (steering decode) is not in this build.
