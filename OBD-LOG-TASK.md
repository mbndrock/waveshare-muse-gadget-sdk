# OBD-LOG-TASK: trip logging foundation (microSD log, pull commands, doorbell flag) — branch obd-vtec-reaction

Owner: Derek. Spec by Muse, implementation by Claude Code, QA by Muse, flash by Derek on the Mac.
Base: tip 9501b3a (cards + presence rounds). Derek approved this on 2026-10-22: "start building the foundation" for drive-data logging on HIS car only. This round is the foundation only: the gadget records trips to its microSD card and Muse can pull them off and delete them after verified receipt. No analytics, no automatic Muse-side pull habit yet, no productization. The VTEC indicator stays just for fun; nothing here changes any existing behavior (cards, helmet, kill switch, triggers, poll schedule all untouched).

## Design in one paragraph

While the OBD link is polling, the firmware appends samples to a per-trip file on the microSD (TF) card. A trip starts when polling starts and ends when the link drops; the gadget is battery-backed, so on drop it can flush and close the file cleanly instead of dying mid-write. Muse later pulls each file over the Link in chunks (the proven command channel), verifies size and checksum against the gadget's own report, and only then sends a delete. The gadget never deletes on its own, and never blocks polling on the card: if the card is absent, full, or stalled, logging degrades silently and the OBD work carries on exactly as today.

## Part 1: the logger (firmware, esp32/components/muse/)

- New module `muse_obd_log.[ch]` next to muse_obd, gated on CONFIG_MUSE_OBD_VTEC like everything OBD. The OBD task feeds it the same values it already puts in the snapshot (RPM, coolant, and each secondary as it is polled); the logger must not add bus requests or change the poll loop's timing.
- One file per trip on the card's FAT filesystem (the board's existing SD/SPI or SDMMC setup for the s18's TF slot; use whatever the tree already uses for the slot, and if the tree has no card driver wired for this board, say so in Results and stop at a compile-clean stub rather than inventing a driver).
- Format: CSV, one header block then one row per sample. Header lines start with `#`: firmware commit (short hash), trip id, protocol name, supported masks (hex, from the scan), start wall time if the RTC/NTP has one else `t0=boot`. Columns: `ms` (trip-relative), `rpm`, `coolant_c`, `iat_c`, `timing_deg`, `map_kpa`, `load_pct`, `throttle_pct`, `speed_kmh`, `stft_pct`, `ltft_pct`, `ambient_c`. Empty field when a channel has no value yet. Integers as in the snapshot.
- Writes go through a RAM buffer flushed in blocks (e.g. 4 KB) from the OBD task or a low-priority writer; a stalled card drops samples (count them in a `dropped` counter in the header trailer or the manifest) rather than stalling polls. On link drop (drop() path) flush, close, and mark the trip complete in the manifest.
- Manifest: a small `trips.json` (or line-delimited equivalent) on the card listing each trip: id, file name, byte size, sample count, dropped count, start/end wall time if known, and a simple checksum of the file content (CRC32 is fine) computed at close. Trip ids are monotonically increasing integers persisted across reboots (store the counter on the card itself, in the manifest).
- Card absent or mount failure: logging off, OBD unaffected, and the state is visible (Part 3 fields). Never retry-mount in a hot loop; check at boot and when a trip would start.

## Part 2: Link commands (the pull path)

Advertise and handle in the usual two places (build_register_json in main/noise_control.cpp, on_ws_command in main/app.c), same CONFIG_MUSE_OBD_VTEC gate, following esp32/AGENTS.md "Adding a command" (validate params, never block the session task: chunk reads of a few KB from an already-mounted card are fine inline; anything larger goes async like device.discover):

- `obd.trips` → `{trips: [{id, bytes, samples, dropped, started, ended, crc32}], card: bool, logging: bool}` from the manifest.
- `obd.trip_read` with `{id, offset}` → `{id, offset, bytes, total, eof, data}` where `data` is the file slice starting at offset, at most 6144 bytes, base64-encoded (the payload is JSON; raw CSV text with escaping is acceptable instead if the codebase has a precedent, but base64 is the safe default). Unknown id or offset past end is a clean command_error.
- `obd.trip_delete` with `{id}` → removes the file and its manifest entry, returns `{id, deleted: true}`. This is the only deletion path. No auto-delete, no delete-on-read.

## Part 3: status surface + doorbell flag

- `muse_obd_status()` / `obd.read` gain: `log_card` (bool), `log_active` (bool, a trip file is open), `log_trip` (current trip id or null), `log_pending` (count of completed trips not yet deleted), `log_dropped` (dropped samples this trip). JSON nulls/false when no card.
- Doorbell: when a trip completes (link drop, file closed) the snapshot's `log_pending` is the machine-readable flag Muse will check. Additionally, IF the firmware already has a device-initiated push path that can carry a short custom line without new platform surface (the chat session's push machinery), fire one short push at trip end when the session is up: `Trip N logged (M samples, B bytes), ready to pull.` If that hook does not exist cleanly, do NOT invent one; record in Results that the doorbell is flag-only for this round. That is an accepted outcome, not a failure.

## Part 4: interaction with the kill switch and presence

- OBD switch off (obd_on = 0): no logging, an open trip is flushed and closed when the link drops from the disable, and the commands above still answer (trips can be pulled while OBD is off; the data already exists). `obd.trips` must work with the switch off.
- Cards/helmet/reactions: untouched. Logging adds no UI.

## Tests (host, from esp32/: python3 -m unittest discover -s tests -p "test_*.py")

Baseline is 673 tests, 0 failures, 6 known cJSON-environment errors, 10 skipped.
- Put the file-format decisions in a pure layer (row formatting, manifest encode/decode, CRC32) with a host harness in the style of muse_obd_enable_harness.c, including: a full trip's bytes round-trip, chunk reads at offsets (start, middle, exact end, past end), delete removes exactly one trip, manifest survives a simulated reboot (counter continues, no id reuse).
- Contract tests in the style of test_link_obd.py: the three commands are advertised and dispatched under the same gate; obd.read carries the log fields; logging compiles out without CONFIG_MUSE_OBD_VTEC.
- Full suite: no new failures/errors beyond baseline.
- Toolchain caution (this bit us twice): no new snprintf format that GCC cannot prove fits its buffer (-Werror=format-truncation on the Mac IDF build), and no new user-visible string may say "Hatch" (repo rule: Muse).

## Out of scope

No Muse-side pull automation or analytics (next round, only if Derek asks). No pushing file contents from the device. No GPS. No changes to triggers, thresholds, poll schedule, cards, helmet, or the support scan. No deleting or overwriting old trips to manage space (at ~1 MB per driving hour the card is effectively infinite for this use; if the card fills, logging stops and says so via log_card state, it does not evict).

## Results (Claude Code fills this in)

- Commits, files, test counts, the SD driver situation on this board (real or stubbed), the doorbell outcome (push wired or flag-only, and why), the exact chunk size and encoding used, and any deviation with its reason.
