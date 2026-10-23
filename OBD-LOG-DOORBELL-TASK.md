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

## Results

Implemented on `obd-vtec-reaction` (baseline `caaa4bc`).

- **Decision unit (new, host-tested):** [muse_obd_doorbell.h](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/components/muse/muse_obd_doorbell.h) / [muse_obd_doorbell.c](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/components/muse/muse_obd_doorbell.c) — pending trips + per-connection announced set + session-ready flag in; send/don't-send plus the exact line out. Ids ascending, bytes summed with no separators (`Trip logs ready: trips 5, 6 (198203 bytes total).`). A session drop clears the announced set; announced ids leave the set once Muse has deleted the trip, so the set stays bounded (cap 128, one line announces at most one batch). The line is assembled with `memcpy` after a whole-line fit check — the first `snprintf` version failed the host suite's `-Werror=format-truncation=2` gate, which is exactly the Mac gate the spec warns about. `muse_doorbell_pending_text` gives Settings its `2 waiting, 193 KB` / `No logs waiting` text (KB = bytes / 1024).
- **IDF side (new):** [muse_obd_doorbell_task.c](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/components/muse/muse_obd_doorbell_task.c) — a 1 s watcher started from `muse_obd_log_start()` (so it runs with Car mode off): lists the stores only through the existing `muse_obd_log_list`, feeds `muse_hatch_ready()` (which includes the `muse_hatch_configured()` check; that function itself is private to the chat component, in `muse_chat_priv.h`, so the public ready flag is the pre-check), and sends via `muse_hatch_text_turn` with a heap string it takes ownership of. Trips are marked announced on the attempted send. Nothing in the file touches BLE, the adapter or the car; no `muse_hatch_say` anywhere. `muse_obd_doorbell_ring()` and `muse_obd_doorbell_pending()` (declared in [muse_obd_log.h](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/components/muse/muse_obd_log.h)) back the Settings row and its cached pending count/bytes.
- **Settings:** under the OBD switch on the home page, a tappable `Send logs to Muse` row showing the pending text; a tap answers on the row for 2 s — `Sent`, `Not connected` (nothing marked), `Already sent`, or `Nothing to send` — in [muse_settings_ui.c](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/components/muse/muse_settings_ui.c). The two extra outcomes cover taps the spec leaves open (everything pending already announced; nothing pending).
- **Conflict found and resolved without weakening anything:** the OBD-LOG task's tests pin `muse_obd_log.c` as chat-free (`test_doorbell_is_the_pending_flag` bans `muse_hatch_text_turn` there; three `test_new_text_says_muse` tests ban the word `hatch` in the log files) — they encode the older decision that `log_pending` was the only doorbell. This task supersedes that behavior, but the file-scoped assertions did not have to move: the chat calls live only in the new `muse_obd_doorbell_task.c`, `muse_obd_log.c` gained exactly one line (`muse_obd_doorbell_start();`), and no existing assertion was edited. Noted here per the spec's stop-and-say rule; flagged in CLAUDE.md too.
- **Tests:** new [tests/test_muse_obd_doorbell.py](/home/hatch/workspace/projects/waveshare-muse-gadget-sdk/esp32/tests/test_muse_obd_doorbell.py) (6 tests: the decision harness under ASan/UBSan with `-Wformat-truncation=2`, the exact format line, dedupe/reconnect, buffer-too-small marking nothing, announced-set recycling after deletes, and the wiring contracts). They were run failing first (missing unit), then passing. Full host suite from `esp32/`: `python3 -m unittest discover -s tests -p "test_*.py"` → **727 tests, 0 failures, 6 errors, 10 skipped** — the 6 errors are the known baseline cJSON ones (`managed_components/espressif__cjson` absent in this environment), matching the baseline's 721 + 6 new.
- **Invariants:** `muse_hatch_say` count in `muse_obd.c` still 0 (contract-tested); no changes to `muse_chat_session.cpp`, partitions, sdkconfig files or `secrets/`; coolant/VTEC reaction paths untouched; every new `snprintf` writes into a fixed-size local whose worst case fits (the pending text is 64 bytes for a 10-digit count; the announcement path does no caller-buffer `snprintf` at all).
- **Board build: not run here.** `tools/muse/board.sh build s18` in this environment stops at `idf.py not found; activate ESP-IDF v6.0.1 first` — there is no ESP-IDF install to run it with, so the s18 build is unverified and the Mac build remains the gate, as the spec allows. One thing for that build to confirm: the row uses `LV_SYMBOL_UPLOAD` (present in the pinned LVGL 9.5.0 symbol set, but the LVGL headers were not available here to grep).
