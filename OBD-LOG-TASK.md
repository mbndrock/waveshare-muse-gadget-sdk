# OBD-LOG-TASK: trip logging foundation (flash + microSD log stores, pull commands, doorbell flag) — branch obd-vtec-reaction

Owner: Derek. Spec by Muse, implementation by Claude Code, QA by Muse, flash by Derek on the Mac.
Base: tip 9501b3a (cards + presence rounds). Derek approved this on 2026-10-22: "start building the foundation" for drive-data logging on HIS car only. This round is the foundation only: the gadget records trips to its microSD card and Muse can pull them off and delete them after verified receipt. No analytics, no automatic Muse-side pull habit yet, no productization. The VTEC indicator stays just for fun; nothing here changes any existing behavior (cards, helmet, kill switch, triggers, poll schedule all untouched).

## Design in one paragraph

While the OBD link is polling, the firmware appends samples to a per-trip file in one of two stores: the internal flash log partition (Part 1b, always present on this board) or the microSD (TF) card. A trip starts when polling starts and ends when the link drops; the gadget is battery-backed, so on drop it can flush and close the file cleanly instead of dying mid-write. Muse later pulls each file over the Link in chunks (the proven command channel), verifies size and checksum against the gadget's own report, and only then sends a delete. The gadget never deletes on its own in either store, and never blocks polling on storage: if a store is absent, full, or stalled, logging degrades silently and the OBD work carries on exactly as today.

## Part 1: the logger (firmware, esp32/components/muse/)

- New module `muse_obd_log.[ch]` next to muse_obd, gated on CONFIG_MUSE_OBD_VTEC like everything OBD. The OBD task feeds it the same values it already puts in the snapshot (RPM, coolant, and each secondary as it is polled); the logger must not add bus requests or change the poll loop's timing.
- One file per trip on the card's FAT filesystem (the board's existing SD/SPI or SDMMC setup for the s18's TF slot; use whatever the tree already uses for the slot, and if the tree has no card driver wired for this board, say so in Results and stop at a compile-clean stub rather than inventing a driver).
- Format: CSV, one header block then one row per sample. Header lines start with `#`: firmware commit (short hash), trip id, protocol name, supported masks (hex, from the scan), start wall time if the RTC/NTP has one else `t0=boot`. Columns: `ms` (trip-relative), `rpm`, `coolant_c`, `iat_c`, `timing_deg`, `map_kpa`, `load_pct`, `throttle_pct`, `speed_kmh`, `stft_pct`, `ltft_pct`, `ambient_c`. Empty field when a channel has no value yet. Integers as in the snapshot.
- Writes go through a RAM buffer flushed in blocks (e.g. 4 KB) from the OBD task or a low-priority writer; a stalled card drops samples (count them in a `dropped` counter in the header trailer or the manifest) rather than stalling polls. On link drop (drop() path) flush, close, and mark the trip complete in the manifest.
- Manifest: a small `trips.json` (or line-delimited equivalent) per store listing each trip in that store: id, file name, byte size, sample count, dropped count, start/end wall time if known, and a simple checksum of the file content (CRC32 is fine) computed at close. Trip ids are monotonically increasing integers across BOTH stores, persisted across reboots (the counter lives in NVS next to the other OBD settings; ids are never reused).
- Store choice per trip: if a microSD card is mounted and healthy, the trip goes to the card; otherwise it goes to the flash store (Part 1b). A trip never spans stores.
- Card absent or mount failure is NOT a logging failure anymore: the flash store is the fallback (and the state is visible, Part 3 fields). Only when both stores are unavailable is logging off. Never retry-mount in a hot loop; check at boot and when a trip would start.

## Part 1b: the flash store (internal, 16 MB boards)

Derek's direction (2026-10-22): the board has 16 MB of flash, use it as well. The Muse partition layout (`esp32/partitions_muse.csv`) ends at 0x822000 (prod_bak), leaving about 7.8 MB of the 16 MB completely unallocated.

- Append ONE new partition to `partitions_muse.csv` only: name `obdlog`, type data, subtype fat, starting on a clean boundary after prod_bak (0x830000), sized to the end of a 16 MB flash (0x7D0000, about 7.8 MB). Do NOT move any existing partition offset (repo rule, and moving NVS would eat Derek's settings/pairing). Leave every other partition table (8 MB boards included) untouched; on a build whose table has no `obdlog`, the flash store is simply unavailable and the SD behavior is unchanged.
- Mount it with ESP-IDF's wear-levelling FATFS (esp_vfs_fat_spiflash_mount_rw_wl or the tree's equivalent). Format ONLY if the mount reports an unformatted/corrupt volume on first use; a valid existing filesystem must survive reflashing (Derek's logs must not vanish because he reflashed the app). At roughly 1 MB written per driving hour, sequential and wear-levelled, flash endurance is a non-issue; say so in Results with the arithmetic, do not add rotation machinery.
- Same file format, same manifest format, same rules as the card: no auto-delete, no eviction of unpulled trips; when the flash store is full, logging to it stops and says so (Part 3), it does not overwrite.
- The partition table change takes effect on Derek's next flash (board.sh writes the table with the app); NVS keeps its offset, so pairing, Wi-Fi, and Settings survive.

## Part 2: Link commands (the pull path)

Advertise and handle in the usual two places (build_register_json in main/noise_control.cpp, on_ws_command in main/app.c), same CONFIG_MUSE_OBD_VTEC gate, following esp32/AGENTS.md "Adding a command" (validate params, never block the session task: chunk reads of a few KB from an already-mounted card are fine inline; anything larger goes async like device.discover):

- `obd.trips` → `{trips: [{id, store, bytes, samples, dropped, started, ended, crc32}], card: bool, flash: bool, logging: bool}` merging both stores' manifests; `store` is `"sd"` or `"flash"`. `obd.trip_read` and `obd.trip_delete` resolve the store from the id; the caller never has to know which store holds a trip.
- `obd.trip_read` with `{id, offset}` → `{id, offset, bytes, total, eof, data}` where `data` is the file slice starting at offset, at most 6144 bytes, base64-encoded (the payload is JSON; raw CSV text with escaping is acceptable instead if the codebase has a precedent, but base64 is the safe default). Unknown id or offset past end is a clean command_error.
- `obd.trip_delete` with `{id}` → removes the file and its manifest entry, returns `{id, deleted: true}`. This is the only deletion path. No auto-delete, no delete-on-read.

## Part 3: status surface + doorbell flag

- `muse_obd_status()` / `obd.read` gain: `log_card` (bool, card mounted), `log_flash` (bool, obdlog partition present), `log_store` (where the current or next trip goes: `"sd"`, `"flash"`, or null), `log_active` (bool, a trip file is open), `log_trip` (current trip id or null), `log_pending` (count of completed trips not yet deleted, across both stores), `log_dropped` (dropped samples this trip). JSON nulls/false when no store exists.
- Doorbell: when a trip completes (link drop, file closed) the snapshot's `log_pending` is the machine-readable flag Muse will check. Additionally, IF the firmware already has a device-initiated push path that can carry a short custom line without new platform surface (the chat session's push machinery), fire one short push at trip end when the session is up: `Trip N logged (M samples, B bytes), ready to pull.` If that hook does not exist cleanly, do NOT invent one; record in Results that the doorbell is flag-only for this round. That is an accepted outcome, not a failure.

## Part 4: interaction with the kill switch and presence

- OBD switch off (obd_on = 0): no logging, an open trip is flushed and closed when the link drops from the disable, and the commands above still answer (trips can be pulled while OBD is off; the data already exists). `obd.trips` must work with the switch off.
- Cards/helmet/reactions: untouched. Logging adds no UI.

## Part 5: card layout pass for the small screen (Derek's in-car photo, 2026-10-22)

Derek's photo of the Temps card in the car shows the problem plainly: the rows are 54 px pills (LIST layout copied from Settings) clustered in the middle band of the 368x448 screen, leaving the top and bottom thirds black; values render at Montserrat 28, small at arm's length; and the filled pills read as a settings list rather than a gauge cluster. Rework `muse_obd_cards.c` only (plus font enablement below). Data, sources, stale rules, and the state takeover do not change.

1. **Fill the tile.** The card content spans the tile's safe area (under the title, above the page dots; use the existing tile content-rect helpers for the vertical safe insets). Rows divide the available height equally via a full-height flex column with grow, replacing the content-sized centered list. Near-full tile width with the existing side padding.
2. **Open cells, no pills.** No filled row backgrounds. Cells separated by a 1 px hairline (about 0x2a2440), none under the last visible row. In each cell the label sits ABOVE the value (small, dim, uppercase), so the eye lands on the number.
3. **Bigger type.** Values: Montserrat 48 for hero cells, 40 for normal cells (fall back to the largest enabled size if a height cannot fit, and say so in Results). Labels and units stay 16. If Montserrat 40/48 are not enabled in the LVGL font config, enable them in the Muse defaults chain in the narrowest place that covers the display boards, and state in Results exactly what was enabled where.
4. **Heroes and bars.** Engine: the RPM cell grows 2x, value in the accent at 48, with a thin bar under the value (LVGL bar, 0 to 8000 RPM, accent fill, dark track, about 10 px). Drive: MPH becomes the hero (grow 2x, 48) with km/h demoted to a small dim unit line; Throttle gets the same thin bar treatment (0 to 100). STFT and LTFT share one row as two half-width cells.
5. **Hide proven-unsupported rows.** Once the support scan is known, a row whose PID is proven unsupported is hidden entirely and the card reflows around it (on Derek's car Ambient disappears and Temps becomes two tall cells). Before the scan is known, today's "..." behavior stands; "--" now only appears transiently before the first scan completes. Put the visible/hidden decision where the existing card contract tests can reach it if the tree's structure allows.
6. **Glance colors.** Coolant value: amber at 100 C or above, red at 105 C or above (the reaction thresholds), fresh values only; stale dimming still wins over every color except it never applies to a value that is not valid anyway. Trims (STFT/LTFT) amber when the absolute value reaches 10%. Everything else keeps today's colors.
7. Unchanged: titles, the not-polling state text, page dots, swipe-anywhere behavior (cells stay non-clickable), update-only-on-change, bench `>card <n>`. Results must summarize the final layout per card and the fonts enabled, and note that visual sign-off is Derek's post-flash look (there is no host renderer for these tiles).

## Tests (host, from esp32/: python3 -m unittest discover -s tests -p "test_*.py")

Baseline is 673 tests, 0 failures, 6 known cJSON-environment errors, 10 skipped.
- Put the file-format decisions in a pure layer (row formatting, manifest encode/decode, CRC32) with a host harness in the style of muse_obd_enable_harness.c, including: a full trip's bytes round-trip, chunk reads at offsets (start, middle, exact end, past end), delete removes exactly one trip, manifest survives a simulated reboot (counter continues, no id reuse).
- Contract tests in the style of test_link_obd.py: the three commands are advertised and dispatched under the same gate; obd.read carries the log fields; logging compiles out without CONFIG_MUSE_OBD_VTEC.
- Full suite: no new failures/errors beyond baseline.
- Toolchain caution (this bit us twice): no new snprintf format that GCC cannot prove fits its buffer (-Werror=format-truncation on the Mac IDF build), and no new user-visible string may say "Hatch" (repo rule: Muse).

## Out of scope

No Muse-side pull automation or analytics (next round, only if Derek asks). No pushing file contents from the device. No GPS. No changes to triggers, thresholds, poll schedule, cards, helmet, or the support scan. No deleting or overwriting old trips to manage space (at ~1 MB per driving hour, the ~7.8 MB flash store holds about a work week of commutes and the card is effectively infinite; if a store fills, logging to it stops and says so, it does not evict).

## Results (Claude Code fills this in)

- Commits, files, test counts, the SD driver situation on this board (real or stubbed), the doorbell outcome (push wired or flag-only, and why), the exact chunk size and encoding used, and any deviation with its reason.
