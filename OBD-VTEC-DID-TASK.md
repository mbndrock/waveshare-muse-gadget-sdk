# OBD-VTEC-DID-TASK: true VTEC solenoid state via Honda DID 0x2612 — branch obd-vtec-reaction

Written by Muse (spec), for Claude Code to implement, Muse QAs after. Read the repo AGENTS.md files first. This task is implemented in the SAME Claude session as OBD-LOG-TASK.md, immediately after it (Derek's direction, 2026-10-22: one run when the limits reset at 8 PM, not a separate later build), so Part 3 below can extend the logger that was just built in the same session. If the log module does not exist when you reach this file, stop and say so in Results instead of improvising.

## Why

The VTEC reaction currently fires on an RPM proxy (arm <= 5500, fire >= 6000) because generic Mode 01 has no VTEC signal. Research on 2026-10-22 (full report: ~/workspace/projects/vtec-did-research.md) found the real thing in the OBDb open signal database, backed by CAN captures from 2006 and 2007 S2000s: Honda's PGM-FI data block, UDS service 0x22, DID 0x2612, carries the VTEC solenoid valve state and the VTEC oil pressure switch state. Derek's decision (2026-10-22, superseding his earlier either/or idea): **the reaction triggers on the solenoid only.** The 6000 RPM trigger is removed, not kept as a backup: if the DID does not answer on his car, the reaction simply never fires, and that honesty is the point. This task also doubles as the verification for Derek's 2008 car, for which no capture exists: if his ECU answers, we learn it from obd.read and from the new card readout (Part 4).

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
- **Trigger (solenoid only, HELD, Derek 2026-10-22):** the VTEC reaction is a **held, level-driven pose, not a flash**. It turns on when vtec_sol is freshly ON (fresh under the same 3 s staleness rule the cards use) and **stays up for the entire duration the solenoid is on**, clearing within one evaluation tick of a fresh OFF reading or of the value going stale. The pose mirrors the solenoid; there is no separate re-arm and **the old 30,000 ms cooldown is removed for this reaction** (it existed for a momentary flash and would fight a state mirror). A momentary reaction (coolant warning, warmup) may interrupt it; when that finishes, the VTEC pose resumes if the solenoid is still freshly ON (if the reaction machinery cannot resume, say so in Results rather than restructuring it). The RPM-based fire path is DELETED: no code path may fire the VTEC reaction from RPM, at any threshold, armed or not (RPM polling itself is untouched; cards and logging still consume it). The old RPM arm/fire constants come out with it. If the DID is unsupported or the value is stale or unknown, the reaction never shows. Warmup and coolant reactions and their thresholds do not move. Idle-only and no-screen-wake rules for reactions still apply to the held pose. **The held pose stays animated the whole time**: the current reaction is a timed animation (progress-driven in muse_state), so holding it must keep its animation cycling (loop the progress, or the equivalent in the state/renderer wiring) for as long as the solenoid is on. Pinning the progress at its end value and freezing on the final frame is a QA fail; he should look alive, blinking and moving, for the whole engagement. **In-repo precedent: the tickle reaction** (tickle_locked in muse_state.c) already does exactly this pattern: progress runs in, waits inside the giggle at TICKLE_WAIT while the stimulus continues (plus a short linger after the last touch), then winds down once it stops. Model the held VTEC pose on that machinery, with the solenoid's fresh ON state as the continuing stimulus.
- `obd.read` gains `vtec_sol`, `vtec_psw`, `did_2612`, `did_2612_b23` with the usual null semantics.
- Bench: extend the existing bench pattern with `>vtecsol <0|1>` driving the trigger input directly, if the bench structure allows it as cleanly as the existing commands. The old `>vtec` simulator exercised the deleted RPM path: remove it or repurpose it, and say which in Results.

## Part 3: trip log columns (the log parts land earlier in this same session)

- The CSV from OBD-LOG-TASK gains two trailing columns, `vtec_sol` and `vtec_psw` (1/0, empty when unknown), sampled from the same snapshot values the logger already consumes. No other format change; the header comment notes the block DID when known.

## Part 4: VTEC status on the Engine card (Derek: "for me to see if this is working")

- The Engine card (as relaid out by OBD-LOG-TASK Part 5, earlier in this same session) gains a VTEC solenoid status readout: the word VTEC with **OFF** or **ON** beside it. Placement: a status line inside or directly under the RPM hero cell, so it reads as part of the RPM cluster; final placement is yours within that constraint, note it in Results.
- ON renders in the accent color (unmissable at a glance); OFF renders dim. When the DID is unsupported or no fresh value exists, it renders **--** dim, so Derek can tell "no signal" apart from "solenoid off" at a glance.
- It follows the card rules: updates only on change, dims with staleness like the other values, and the link-state takeover still replaces the whole card when not polling.

## Tests

- DID response parser against the REAL captured frames from the research report (~/workspace/projects/vtec-did-research.md): the 2007 VTEC-ON sample (byte 21 = 0x01) and a typical OFF sample (byte 21 = 0x02), plus truncated and wrong-prefix negatives.
- Trigger: the reaction is active exactly while vtec_sol is freshly ON (held across ticks, not a one-shot); a fresh OFF clears it; a stale value clears it; a quick OFF-ON re-engagement reactivates it immediately (no cooldown suppression); RPM at any value (including 9000) with the solenoid OFF, absent, or stale never activates it.
- Exchange safety: a simulated failed DID exchange leaves the poll schedule and RPM parsing intact.
- Baseline to beat: the post-logging suite (673 pre-log tests, 0 failures, 6 known cJSON-environment errors, 10 skipped, plus whatever the log build added). Same rules as every round: provably-sized snprintf for the Mac toolchain gate; user-visible text says Muse, never Hatch.

## Out of scope

No other enhanced DIDs (2611 AFR, 2662 knock/VTC, 2663 misfires are a future round). No writes to the car of any kind; this is a read. No change to the Mode 01 poll order, the helmet, or the kill switch. The only card change is the Part 4 VTEC readout; the only trigger change is the Part 2 solenoid-only swap. No change to the reaction's visuals.

## Results (Claude fills in)

Commits, files touched, test counts, the AT choreography chosen for state safety, the exact cadence as built, deviations, and the one line Derek needs: after his next flash and drive, obd.read's `did_2612` field is the verdict on whether the 2008 answers.

### Results

**The one line for Derek:** after your next flash and drive, `obd.read`'s `did_2612` is the verdict on whether the 2008 answers DID 0x2612. `true` means the Engine card's VTEC line will read ON/OFF and the reaction works off the solenoid. `false` means it doesn't answer at DA10, and the reaction will never show, by design.

- **The log module existed when I got here.** It was built and committed earlier in this session (OBD-LOG-TASK), so Part 3 extends it.
- **Commits on obd-vtec-reaction:**
  - 5ee5b5b OBD: VTEC from the solenoid (Honda DID 0x2612), held while it's on; RPM fire path deleted
  - 5b54f48 Jollybot: VTEC animates on the clock, so the held pose keeps buzzing (its own commit, under Jollybot's notice)
  - c874282 Link and cards: DID 0x2612 fields in obd.read, VTEC status on the Engine card
  - 30f449c Tests: DID 0x2612 on real S2000 frames, exchange safety, the held pose
  - the commit adding these Results and the README / CLAUDE.md docs
- **Files touched:**
  - `esp32/components/muse/`: `muse_obd_elm.[ch]`, `muse_obd.[ch]`, `muse_state.[ch]`, `muse_pixel.h`, `muse_ui.c`, `muse_input.c`, `muse_obd_cards.c`, `muse_obd_log.[ch]`, `muse_obd_log_store.[ch]`.
  - `esp32/avatar/muse_pixel.c` (one line).
  - `esp32/main/app.c`, `esp32/main/noise_control.cpp`.
  - Tests: new `test_muse_obd_did.py`, `vtec_did_frames.h`, `muse_state_vtec_harness.c` and `state_fakes/`; changed `muse_obd_elm_harness.c`, `muse_obd_enable_harness.c` (now also built with `-DMUSE_OBD_HARNESS_DID`), `muse_obd_log_harness.c`, `muse_pixel_vtec_harness.c`, `test_link_obd.py`, `test_muse_obd_elm.py`, `test_muse_obd_log.py`.
  - Docs: `README.md`, `CLAUDE.md`, this file.
- **Tests:** 706 tests, 0 failures, the 6 known cJSON-environment errors, 10 skipped. That's the post-log baseline of 695 / 0 / 6 / 10 plus 11 new. Three `test_link_obd.py` contracts encoded the RPM trigger (`muse_vtec_update` in `poll_rpm`, `muse_obd_fire_vtec`) and now assert the new contract. Two harness checks were mutation-tested:
  - Dropping `ATAR` from the restore makes the exchange-safety scenario fail: RPM goes missing, and 21 Mode 01 requests hit the DID's state.
  - Putting VTEC's animation back on progress makes the held-renderer check fail ("0 moves, 149 holds").
  - As in the log round, there was no ESP-IDF here. I type-checked the touched firmware files against stand-in IDF/LVGL/cJSON headers with `-Wall -Wextra -Werror -Wformat-truncation=2`, and the Mac build is the gate. The only new `snprintf` is in the test helper; the firmware's DID logging uses `ESP_LOG`.
- **Real frames:** the research report quotes only part of one reply. I fetched the fixture it cites, OBDb `Honda-S2000/tests/test_cases/2007/commands/DA10.10.222612.yaml`, and took three complete 9-frame replies:
  - case 15: the report's exact "VTEC on" sample, `00 1E 00 C2 5F 44 19...`, byte 21 = 0x01;
  - case 1: another "on";
  - case 0: an "off", byte 21 = 0x02.
  - All three decode as the report says, with byte 23 = 1 in both "on" samples. The frames are in `tests/vtec_did_frames.h`. A helper renders them the way the ELM prints them with `ATH0` and CAN formatting (`039`, `0:622612FFFFC0`, `1:...`), so the parser sees exactly what the adapter would send. `MUSE_ELM_RESP_MAX` went from 160 to 320: the 57-byte reply is ~155 characters, ~190 with spaces.
- **AT choreography (state safety):** every exchange is `ATSHDA10F1` -> `ATCRA18DAF110` -> `222612`, then always `did_restore()`: `ATAR` (automatic receive, no CRA filter) and `ATSHDB33F1` (the functional 18 DB 33 F1 header, the ELM's own default on protocol 7, which Mode 01 used before because nothing ever set a header).
  - The restore runs whatever happened before it: setup failed, a refusal, NO DATA, garbage, or a request that never got its prompt (1.5 s timeout).
  - It's tried twice, because a late reply to a hung request can take the first try's prompt. Each step must see `OK`.
  - If both tries fail, `did_read` reports the link lost, `poll_rpm` returns, and `drop()` plus the normal reconnect and ELM init take over. That's the existing machinery; there's no second recovery path.
  - A garbled or hung read mid-connection does not drop the link. It leaves the last reading to age out (never a stale "on") and keeps the DID marked supported. The harness checks no drop, RPM still 1726, and no Mode 01 request sees the DID's state.
  - Nothing is written to the car. A test pins every command the task can send to the ELM setup set, the Mode 01 PIDs and `222612`.
- **Probe:** once per connection, only when this connection's ATDPN said 7. It runs right after the support scan and `read_protocol()`, before polling starts, so the trip header knows. If the protocol only turned up at the first RPM reply (engine off at init), it runs after those retries. Refused (`7F 22 xx`), NO DATA, no prompt or garbage means `DID_NO` for the connection; the harness checks there is never a second `222612`. The probe is reset in `drop()`. `did_2612` is kept across drops, like the protocol, and replaced by the next probe. The live bits clear on drop.
- **Cadence as built:** a rotation is the DID read followed by `MUSE_ELM_SECONDARY` less unsupported PIDs (`muse_elm_next_slot()`), so the poll sequence is `010C, [DID], 010C, 0105, 010C, 010F, ...`. With the S2000-style scan in the harness (8 secondaries) one rotation is 9 RPM + 8 PIDs + 1 DID slot, checked command by command. The DID slot costs 5 ELM round trips (2 setup, the request, 2 restore) against 1 for a PID.
  - Freshness caveat: the DID is read once per rotation, ~1.5-2.5 s apart at typical BLE round trips. That's under the 3 s staleness rule, but not by a wide margin. If Derek's adapter is slow enough that a rotation takes over 3 s, the held pose would drop to the settle between readings and restart on the next one. `vtec_sol_age_ms` in `obd.read` shows the real spacing.
- **Trigger, solenoid only and held:** `muse_obd_vtec_on(st)` is `enabled && muse_vtec_engaged(valid, sol, age <= 3000)`, and RPM isn't an input. `muse_ui.c` calls `muse_state_set_vtec()` with it every 0.25 s chrome tick.
  - `muse_state.c` follows the tickle pattern: progress runs in, waits at `MUSE_VTEC_HOLD` (0.5: banner up, buzzing) for as long as the level stays on, and repeated "on" ticks never restart it.
  - On the tick the level goes off (a fresh "off", or the reading going stale), the hold ends and the short settle plays from `MUSE_VTEC_SETTLE` (0.82 -> 1, ~0.5 s). I read "clearing within one tick" as the hold ending on that tick, with the existing wind-down after it as tickle does, rather than an abrupt cut.
  - There's no cooldown: off then on starts afresh on the next tick.
  - To keep the hold animated, Jollybot's `RE_VTEC` takes `ts` from `pose.t`, as `RE_TICKLE` does, instead of from progress. The renderer harness holds progress at 0.5 for 6 s and checks he keeps buzzing, frames keep changing, and the banner stays whole. No visual changed otherwise.
  - Idle only (another mode hides it, and it's back after). Starting pokes like the old fire, holding doesn't keep poking, and a poke never wakes the screen. Sleep clears it, and a solenoid still on when the screen wakes starts it again.
  - Momentary reactions: the coolant warning shows over a held VTEC (the renderer ranks coolant first), and VTEC resumes when the warning ends; the state harness checks this. The warm-up ranks below VTEC in the existing renderer, so while VTEC is held the warm-up doesn't visibly interrupt it, and its 3 s run out underneath. Making it interrupt would have meant changing the renderer's priority order, which the spec rules out ("no change to the reaction's visuals").
  - RPM fire path deleted: `muse_vtec_trigger_t`, `muse_vtec_init/disarm/update`, `MUSE_VTEC_ARM_RPM/FIRE_RPM/COOLDOWN_MS`, `fire(rpm)` and `muse_obd_fire_vtec()` are gone, and a test scans the firmware for all of them. RPM polling is unchanged.
- **obd.read / >obd:** `vtec_sol`, `vtec_sol_age_ms`, `vtec_psw`, `vtec_psw_age_ms` (null without a reading), `did_2612` (bool), `did_2612_b23` (raw byte or null).
- **Bench:** `>vtec` is **removed**; it only exercised the deleted RPM path. `>vtecsol 1` / `>vtecsol 0` set a solenoid reading that stays fresh until `>vtecsol 0`/`1`, the car's next real reading, or a dropped link. It drives the same input as the car (snapshot -> `muse_obd_vtec_on` -> the held pose, the card and `obd.read`). It answers `@obd.off` while OBD is off; other arguments get `@vtecsol.error`.
- **Engine card (Part 4):** the VTEC status line sits inside the RPM hero cell, directly under the RPM bar: `VTEC` (16, dim) then the word (20). It reads ON in the accent for a fresh "on", OFF dim for a fresh "off", and `--` dim when there's no fresh reading (DID unsupported, not yet read, or older than 3 s). The spec asks for both "dims with staleness" and "-- when no fresh value exists"; I folded staleness into `--`, which is also when the trigger lets go. It updates only on change, and the not-polling takeover still replaces the whole card. `fit_font()` reserves the line's height when sizing the RPM value.
- **Trip log (Part 3):** two trailing columns, `vtec_sol,vtec_psw` (1/0, empty when unknown), from the same snapshot. The header gains `# did_2612=yes` or `# did_2612=no` after this connection's probe, and no line when the protocol never said 7.
- **Deviations:**
  - A reply of any length other than 57 counts as garbage: unsupported for the connection, with the raw reply in the warning log. This follows "validate the total length". If the 2008 answers with a different block size, `did_2612` will read false and the console log line shows what came back.
  - The `ATCRA`/`ATSH` commands are sent without spaces (`ATCRA18DAF110`) to fit `elm_cmd`'s 16-byte line; the ELM accepts both forms.
