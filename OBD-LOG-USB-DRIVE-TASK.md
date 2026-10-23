# OBD Log USB Drive: expose closed trip CSVs as removable media

Repo: waveshare-muse-gadget-sdk, branch obd-vtec-reaction.
Target board: s18 only (the OBD feature's device overlay).
Implement against the branch tip as it stands when you start (the doorbell
task may have landed first; note the tip hash in Results).

## Goal

When Derek plugs the gadget into his Mac, he wants the trip CSVs in Finder,
no Muse and no terminal involved. The gadget gains a "USB drive mode": it
enumerates as a USB mass storage device presenting a read-only drive whose
files are the closed trip logs, generated on the fly from the existing
LittleFS log store.

## Design constraints (decided; do not re-litigate)

- **No partition changes.** partitions.csv is untouched. The FAT filesystem
  is fabricated in the MSC block layer (the UF2-bootloader pattern): boot
  sector, FAT tables, and directory are generated, and file data is served
  from LittleFS reads. There is no real FAT partition and no stored image.
- **Read-only media.** Host writes are refused (write-protected status).
  Finder must be able to mount and copy without ever needing to write.
- **Snapshot semantics.** The file list is the set of CLOSED trips at the
  moment drive mode is entered. The currently-open trip is never listed.
  Trips that close while drive mode is active appear only after the mode is
  re-entered. Closed trips are immutable, so MSC reads never race the
  logger (which only ever appends to the open trip's file).
- **Files on the drive:**
  - One CSV per closed trip, 8.3 name: `TRIP` + zero-padded id (minimum 4
    digits), e.g. `TRIP0005.CSV`. Content is the trip file byte-for-byte.
  - One generated `INDEX.TXT`: one line per trip with id, byte size, sample
    count, and CRC32 from the store manifest, plus a first line identifying
    the gadget and firmware. Plain text, LF endings.
  - Nothing else. No hidden files, no trash/recycle folders.
- **Drive label**: `MUSE LOGS`.

## Mode entry and exit (investigate, then implement the reliable path)

- A Settings row, `USB drive mode`, in the OBD section, shows the mode as a
  toggle. Entering the mode switches the USB port from its current console
  personality to mass storage; exiting (toggle off, or unplug) restores the
  normal personality. A controlled reboot into the mode via an NVS flag is
  acceptable if a live stack swap is unreliable on this board; say which you
  built and why.
- First investigate how USB is currently wired on s18 (USB-Serial-JTAG
  console vs the OTG peripheral), what ESP-IDF's TinyUSB MSC needs in
  sdkconfig for this device (sdkconfig changes for the s18 overlay ARE in
  scope for this task), and that ROM download-mode flashing still works
  exactly as before with the mode off and with it on. Flashing must not
  regress; if it would, stop and report instead of improvising.
- Entering or exiting the mode must never lose trips or settings, and OBD
  logging behavior outside the mode is unchanged. While the mode is active
  the gadget may keep logging if the car link is up; that must not corrupt
  the snapshot (see snapshot semantics above).

## Invariants

- Firmware stays read-only toward the car; no new ELM/BLE traffic.
- No changes to the log store's on-flash format, the trip writer, or the
  obd.trips / obd.trip_read / obd.trip_delete commands.
- `muse_hatch_say` count in muse_obd.c stays 0. Coolant defines and reaction
  paths untouched. Nothing under secrets/ is read or modified.
- User-visible strings say Muse, never Hatch.
- Every snprintf provably sized (the Mac build gates on
  -Werror=format-truncation; USB/settings code is not host-compiled, so be
  conservative).

## Tests and reporting

- Factor the FAT generation and directory-snapshot logic so it is
  host-testable where practical (geometry, 8.3 naming, INDEX.TXT content,
  file-size reporting vs LittleFS sizes), following the existing host-test
  patterns. Full host suite from esp32/ must pass with 0 failures; report the
  exact counts and the baseline you started from.
- Append a `## Results` section to this file: what you built, the entry/exit
  mechanism chosen and why, sdkconfig changes made, test counts, and an
  explicit statement of what still needs the owner's Mac (real build via
  tools/muse/board.sh build s18, Finder mount check, copy-a-CSV check).
  Then commit everything and push to origin/obd-vtec-reaction.
- Fix causes, not workarounds. If the design constraints above conflict with
  the platform as built, stop and say so in Results rather than improvising
  around them.
