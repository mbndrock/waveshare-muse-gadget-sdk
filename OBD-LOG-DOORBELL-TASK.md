# OBD Log Doorbell: Settings "Send logs to Muse" button + auto-ring on connect

Repo: waveshare-muse-gadget-sdk, branch obd-vtec-reaction, baseline tip caaa4bc.
Target board: s18 (the OBD feature is gated to that device overlay).

## Why

Finished trips sit on the gadget's flash until Muse happens to ask for them
(obd.trips / obd.trip_read are Muse-initiated). The gadget can originate a
message into its Muse chat session via `muse_hatch_text_turn(char *text)`
(components/muse/muse_chat.h; live today, used by the serial console in
muse_input.c). Use it as a doorbell: the gadget announces pending logs, Muse
sees the message and pulls them (49,152-byte chunks since caaa4bc), verifies
each CRC, then deletes the gadget copies. This task builds the doorbell only;
no file bytes move in this feature, and no Muse-side changes are needed.

## Announcement format (exact)

One line, sent as one text turn:

    Trip logs ready: trips 5, 6 (198203 bytes total).

- Prefix is exactly `Trip logs ready:` (Muse pattern-matches on it).
- Trip ids ascending, comma+space separated; bytes are the sum of the trips'
  manifest byte counts, no thousands separators.
- Only send when at least one trip is pending.

## Part 1: Settings button

- In the OBD section of Settings, near the Car mode (OBD) switch: a tappable
  row labeled `Send logs to Muse`.
- The row shows the pending state: `2 waiting, 193 KB` style subtext when
  trips are pending, `No logs waiting` when none. (KB = total bytes / 1024,
  integer.)
- Tap with nothing pending: brief on-row feedback only; send nothing.
- Tap with trips pending and the chat session ready: send the announcement,
  show brief `Sent` feedback on the row, and record those trip ids as
  announced for this connection (see Part 2 dedupe).
- Tap with trips pending but the session not ready: show `Not connected`
  feedback; do NOT mark anything announced.

## Part 2: Auto-ring

- When the chat session becomes ready (transition from not-ready to ready)
  and unannounced pending trips exist, send the announcement once, within a
  few seconds of the transition.
- If a trip closes while the session is already up and that trip has not been
  announced, announce it (batched with any other unannounced pending trips)
  shortly after it closes.
- Dedupe is per connection: the announced set resets when the session drops,
  so a later reconnect re-announces anything still pending (the earlier ring
  may never have been acted on).
- Never announce when nothing is pending. Never announce the same trip twice
  on one connection.

## Implementation notes

- The pending-trip source of truth is the existing log store listing behind
  obd.trips (bytes + ids). Do not duplicate store logic or scan the
  filesystem separately.
- Factor the announce decision (pending set + per-connection announced set +
  session-ready flag in; send/don't-send + formatted line out) into a small
  host-testable unit, following the existing OBD host-test patterns. Settings
  UI wiring and session watching are IDF-side.
- `muse_hatch_text_turn` takes ownership of the heap string (it frees it on
  all paths) and returns void. Pre-check `muse_hatch_configured()` before
  sending and mark trips announced on an attempted send; a silently dropped
  send self-heals on the next reconnect or button press.
- The doorbell concerns STORED trips, so it works regardless of the Car mode
  switch state (stored trips still need delivery when logging is off). It
  must never create ELM/BLE traffic: no car-side activity of any kind.
- Do not modify the chat session's turn semantics; call the existing API
  only. Do not use `muse_hatch_say` (no speech in this feature). User-visible
  strings say Muse, never Hatch.

## Invariants (all previous OBD work)

- Firmware stays read-only toward the car; coolant-warning defines and all
  reaction paths untouched; `muse_hatch_say` count in muse_obd.c stays 0.
- No changes to muse_chat_session.cpp, partitions, sdkconfig files, or
  anything under secrets/.
- Every snprintf provably sized for its buffer (the Mac build gates on
  -Werror=format-truncation; app and settings code are NOT compiled by the
  host suite, so be conservative there).

## Tests and reporting

- Full host suite from esp32/: `python3 -m unittest discover -s tests -p
  "test_*.py"`. Baseline at caaa4bc: 721 tests, 0 failures, 6 known cJSON
  errors, 10 skips. Add contract tests for the announce decision and the
  exact format line; do not weaken existing assertions.
- Append a `## Results` section to this file with exact test counts, then
  commit everything and push to origin/obd-vtec-reaction.
- If you cannot run the real board build (tools/muse/board.sh build s18)
  in your environment, say so plainly in Results; the Mac build is the gate
  and will be run by the owner.
- Fix causes, not workarounds. If something in this spec conflicts with the
  code as built, stop and say so in Results instead of improvising.
