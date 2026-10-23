# OBD-VTEC-DID-TASK: true VTEC solenoid state via Honda DID 0x2612 — branch obd-vtec-reaction

Written by Muse (spec), for Claude Code to implement, Muse QAs after. Read the repo AGENTS.md files first. This task is implemented in the SAME Claude session as OBD-LOG-TASK.md, immediately after it (Derek's direction, 2026-10-22: one run when the limits reset at 8 PM, not a separate later build), so Part 3 below can extend the logger that was just built in the same session. If the log module does not exist when you reach this file, stop and say so in Results instead of improvising.

## Why

The VTEC reaction currently fires on an RPM proxy (arm <= 5500, fire >= 6000) because generic Mode 01 has no VTEC signal. Research on 2026-10-22 (full report: ~/workspace/projects/vtec-did-research.md) found the real thing in the OBDb open signal database, backed by CAN captures from 2006 and 2007 S2000s: Honda's PGM-FI data block, UDS service 0x22, DID 0x2612, carries the VTEC solenoid valve state and the VTEC oil pressure switch state. Derek's original spec for the reaction was "either solenoid OR RPM over 6000"; this task finally wires the solenoid half. It also doubles as the verification for Derek's 2008 car, for which no capture exists: if his ECU answers, we learn it from obd.read; if it does not, the firmware must behave exactly as it does today.

## The read (from OBDb Honda-S2000, captures verified by Muse)

- Physical addressing on the confirmed CAN 29/500 bus: request to **0x18DA10F1**, response on **0x18DAF110**. ELM form: `ATSH DA10F1`, `ATCRA 18DAF110`, protocol 7 (ATSP7 semantics; the firmware already knows the negotiated protocol from ATDPN).
- Request bytes: `22 26 12`. Default session, no security access, no session setup.
- Response: ISO-TP multi-frame, `62 26 12` + 54 data bytes (57 total). The ELM issues flow control itself; the firmware sees the assembled lines and must validate the `62 26 12` prefix and the total length before trusting any byte.
- **VTEC solenoid: data byte 21 (0-based after `62 26 12`), bit 0 (mask 0x01). 1 = ON.**
- **VTEC pressure switch: same byte, bit 1 (mask 0x02).** Observed byte values: 0x02 at rest, 0x01 engaged; the bits are complementary in every captured sample.
- Data byte 23 also flipped 0 to 1 in all three captured ON samples; unknown meaning. Expose it raw (below) so we can learn from Derek's car.

## Part 1: the DID exchange in the ELM layer

- Only on a CAN 29-bit link (ATDPN protocol 7). On anything else, this feature is absent, silently.
- Probe once per connection, after the PID support scan: set the header and receive filter, send `222612`, parse. Three outcomes: answered (mark DID supported), negative response / no response after the normal ELM timeout (mark unsupported for this connection, never probe again until the next connection), or garbage (treat as unsupported, log it).
- The exchange must be **state-safe**: whatever AT state the DID read changes (header, receive filter, anything else), the Mode 01 poller must get its expected state back after every DID exchange, success or failure, proven by the next RPM reply parsing normally. A failed or slow DID exchange must never wedge the session, trigger a reconnect loop, or delay the RPM alternation by more than the exchange itself. If the exchange errors mid-flight, use the existing init/retry machinery, do not invent a second recovery path.
- Polling cadence once supported: one DID read per secondary rotation, inserted as an extra slot at the START of the rotation (RPM alternation pattern unchanged; the secondary cycle gains one slot). Each read refreshes the solenoid/switch values with the same age tracking the extras use.
- Parse defensively: wrong prefix, short frame, or an ISO-TP error means invalid reading (ages out like any other), never a stale ON.

## Part 2: snapshot, trigger, and obd.read

- `muse_obd_status()` gains: `vtec_sol` and `vtec_psw` readings (valid/value/age, same shape as the extras), `did_2612` (bool: supported on this connection), and `did_2612_b23` (raw byte 23 value or null). DID *availability* is retained in RAM after the link drops, exactly like the protocol and support masks; the live bits clear on drop, like every other reading.
- **Trigger (Derek's original either/or spec):** the VTEC reaction fires when (RPM valid and >= 6000, armed as today) **OR** (vtec_sol valid, fresh under the same 3 s staleness rule the cards use, and == ON). Arming extends symmetrically: seeing the solenoid OFF also arms. Cooldown stays 30,000 ms; warmup and coolant reactions untouched; thresholds in muse_obd_elm.h for RPM/coolant/warmup do not move.
- `obd.read` gains `vtec_sol`, `vtec_psw`, `did_2612`, `did_2612_b23` with the usual null semantics.
- Bench: extend the existing bench pattern with `>vtecsol <0|1>` driving the trigger input directly (keeping `>vtec` working as today), if the bench structure allows it as cleanly as the existing commands; if not, say so in Results rather than forcing it.

## Part 3: trip log columns (the log parts land earlier in this same session)

- The CSV from OBD-LOG-TASK gains two trailing columns, `vtec_sol` and `vtec_psw` (1/0, empty when unknown), sampled from the same snapshot values the logger already consumes. No other format change; the header comment notes the block DID when known.

## Tests

- DID response parser against the REAL captured frames from the research report (~/workspace/projects/vtec-did-research.md): the 2007 VTEC-ON sample (byte 21 = 0x01) and a typical OFF sample (byte 21 = 0x02), plus truncated and wrong-prefix negatives.
- Trigger: solenoid-ON fires without RPM reaching 6000; solenoid stale does not fire; DID-unsupported connections behave byte-identically to today (RPM path only); arming via solenoid-OFF.
- Exchange safety: a simulated failed DID exchange leaves the poll schedule and RPM parsing intact.
- Baseline to beat: the post-logging suite (673 pre-log tests, 0 failures, 6 known cJSON-environment errors, 10 skipped, plus whatever the log build added). Same rules as every round: provably-sized snprintf for the Mac toolchain gate; user-visible text says Muse, never Hatch.

## Out of scope

No other enhanced DIDs (2611 AFR, 2662 knock/VTC, 2663 misfires are a future round). No writes to the car of any kind; this is a read. No change to the RPM thresholds, the poll order for Mode 01, the cards, the helmet, or the kill switch. No change to the reaction's visuals.

## Results (Claude fills in)

Commits, files touched, test counts, the AT choreography chosen for state safety, the exact cadence as built, deviations, and the one line Derek needs: after his next flash and drive, obd.read's `did_2612` field is the verdict on whether the 2008 answers.
