# CLAUDE.md

Guidance for Claude Code (and other coding agents) in this repository: Meta's
Muse Gadget SDK, forked to run on three Waveshare ESP32-S3 boards with spoken
replies, pushes, touch volume and battery reading. Humans: start with
`README.md`.

The SDK's own agent guide still applies — read it for the build system, board
contract and conventions: @esp32/AGENTS.md. Adding a board: @esp32/devices/AGENTS.md.

## Rules that matter most

1. **Secrets never leave `secrets/`.** The Muse SDK token (`mgst_…`) and the
   ElevenLabs key (`sk_…`) are one-line files in `secrets/` (gitignored).
   - Never print them, paste them into chat, write them into tracked files or
     put them on a command line that gets echoed. Read them from the file inside
     a script and report only "set" / "missing" and the length or prefix.
   - `esp32/tools/muse/board.sh build` injects them through
     `tools/muse/secrets.py`. Don't edit `build-*/sdkconfig` by hand unless you
     must, and then without echoing the value.
   - Every `build-*/` directory and every flashed image contains them: never
     commit, upload or attach either.
   - The firmware itself logs a truncated token (`SDK token: mgst_XXXX…`).
     Mask `(mgst|sk)_[A-Za-z0-9_-]+` when you quote logs.
   - **Before any push**, scan the files to be committed **and** `git log -p
     upstream/main..HEAD` for the real values (whole and in pieces) and for
     `mgst_[A-Za-z0-9]{12,}` / `sk_[a-f0-9]{24,}`. The only expected hit is the
     fake `mgst_AAAA…` in `esp32/tests/link_pairing_handshake_harness.c`.
   - `.githooks/` runs that scan on every commit and push
     (`esp32/tools/muse/check_secrets.py`). Turn it on once per clone with
     `git config core.hooksPath .githooks`; never bypass it with `--no-verify`
     to get a secret through.
2. **Back up before the first flash.** `python -m esptool -p PORT read-flash 0
   0x1000000 backup/<board>-<mac>-factory.bin` (0x800000 for 8 MB parts).
   `backup/` is not in this repo; keep dumps out of git (vendor firmware).
3. **Identify the board; don't assume.** A wrong profile writes the wrong pin
   map. See "Identify the board" below.
4. **Don't delete or overwrite** the user's files, backups or branches without
   asking. `erase-flash` wipes pairing and Wi-Fi: only on first install or when
   asked.

## Boards

| Alias | Board | Chip / flash / PSRAM | Panel · touch | Audio | Power | Buttons |
|---|---|---|---|---|---|---|
| `s185c` | Waveshare ESP32-S3-Touch-LCD-1.85C | S3 QFN56 · 16 MB · 8 MB octal | ST77916 360×360 round (QSPI) · CST816 0x15 | ES8311 + amp GPIO15, ES7210 | no PMU; battery ADC GPIO8 ×3 | BOOT = talk; RST; slide switch cuts battery |
| `s143c` | Waveshare ESP32-S3-Touch-AMOLED-1.43C | S3-PICO-1-N8R8 · 8 MB · 8 MB octal | SH8601 466×466 round, mounted 180° · FT-style 0x15 | ES8311 + NS4150B GPIO46, ES7210, LDO EN GPIO18 | no PMU; battery ADC GPIO4 ×2, charge GPIO7 (low = charging) | BOOT = talk; PWR is a power latch |
| `s18` | Waveshare ESP32-S3-Touch-AMOLED-1.8 | S3 QFN56 · 16 MB · 8 MB octal | SH8601 or CO5300 368×448 · FT3168 0x38 or CST816 0x15 | ES8311 (speaker + mic), amp GPIO46 | AXP2101 0x34 | BOOT = talk; PWR = aux via PMU |

Board files: `esp32/components/muse/boards/board_waveshare_s3_{185c,143c,18}.c`.
Overlays: `esp32/devices/sdkconfig.muse-waveshare-s3-{185c,143c,18}`. All three
drive `esp_lcd` directly (no vendor BSP) with `esp_lv_adapter` +
`muse_lcd_bands`, and re-enable the watchdogs that `sdkconfig.defaults` turns off.

## Identify the board

All three enumerate as Espressif `303a:1001` "USB JTAG/serial debug unit"
(`/dev/cu.usbmodem*` on macOS). The model isn't in the descriptor, but the
**MAC is its USB serial number**: `ioreg -p IOUSB -l -w0 | grep '"USB Serial Number"'`
tells boards apart **without touching them**.

**Never poll esptool to watch for a board.** Every `esptool` call resets the
chip; a loop calling `read-mac` every few seconds boot-loops whatever is
plugged in. Use the USB serial number (or `tools/muse/ports.py`) to wait for a
board, and run esptool once you've decided to act.

To find the model:

1. Running this firmware: the boot log says `muse: board: <name>`, and
   `python3 esp32/tools/muse/chat.py --status` returns `"board"`.
2. `python -m esptool -p PORT chip-id` and `flash-id`: `ESP32-S3-PICO-1` with
   8 MB embedded flash is the 1.43C; QFN56 with 16 MB is the 1.85C or the 1.8.
3. Vendor firmware: back it up, then `strings backup.bin | grep -iE
   "board|1\.85|1\.43|amoled|st77916|sh8601|cst816|ft5x06|axp2101"`. The 1.85C's
   demo names `ST77916`/`CST816` and `ES7210`; the 1.43C's says `Board:
   S3_AMOLED_1_43C`; the 1.8 often ships xiaozhi (`esp32-s3-touch-amoled-1.8`).

Say which board you found and what told you before flashing.

## Build, flash, monitor

```sh
# once: ESP-IDF v6.0.1 at ~/esp/esp-idf-v6 (board.sh finds it), cmake, ninja
cd esp32
tools/muse/board.sh build s185c        # → build-muse-waveshare-s3-185c/, log /tmp/muse_build_s185c.log
tools/muse/board.sh flash s185c        # finds the port by USB device
```

- `board.sh build` wipes and refetches `managed_components/` each time, so run
  builds **one at a time**. For parallel work, use separate copies of the repo.
- Flash keeps NVS (pairing, Wi-Fi). Fresh install: `python -m esptool -p PORT
  erase-flash` first.
- Errors: `grep -E "error:|warning:" /tmp/muse_build_<board>.log`. Size: the
  `check_sizes` line (4 MB slots; 3.9 MB on the 8 MB 1.43C).
- Host tests need `managed_components/`, which `board.sh` deletes. Run
  `idf.py ... reconfigure` for one board, then `python3 -m unittest discover -s
  tests -p 'test_*.py'` (156 tests), then delete `managed_components/` and
  `dependencies.lock` again.

**Logs without resetting the board** (an agent has no TTY for `idf.py monitor`;
opening the port normally can reset it):

```python
import os, termios, time, re
fd = os.open(PORT, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[2] &= ~termios.HUPCL; termios.tcsetattr(fd, termios.TCSANOW, a)
# read for N seconds, strip ANSI with re.sub(r'\x1b\[[0-9;]*m', '', text), mask secrets
```

`tools/muse/monitor.py PORT SECS` captures from boot (it resets). A healthy
boot shows `muse: board: …`, `UI up: WxH`, `audio ready`, a mic `self-test`
line and `advertising as MuseGadget-XXXXXX` (unpaired) or `setup=done wifi=up
ws=up` (paired).

The serial console accepts bench keys: `z`/`w` sleep/wake the screen, `m` plays
a test MP3, `d`/`u` press/release talk, and `>status\n` prints a JSON status
(mask the tokens in it).

## Pairing

Muse app › Settings › Devices › Developer mode on › **+** › `MuseGadget-XXXXXX`
(shown on screen) › press **BOOT** when asked › send Wi-Fi from the app. Holding
BOOT 5 s resets setup.

## Features added in this fork

- **Spoken replies:** `components/muse/muse_chat_session.cpp`.
  - `start_tts` → `tts_start` queues the text to the `muse_tts` task.
  - `tts_fetch` POSTs to ElevenLabs `…/stream?output_format=mp3_22050_32` with `esp_http_client`.
  - `tts_pump` feeds `tts_data()` on the session task, and `decode()` resamples to 16 kHz.
  - Kconfig: `MUSE_ELEVENLABS_API_KEY`, `_VOICE_ID`, `_MODEL` (`eleven_flash_v2_5`).
  - Needs `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y`: the ElevenLabs chain ends at a GlobalSign root that isn't in IDF's bundle.
  - Upstream removed server TTS (PR #12), so without a key replies are text.
- **All messages (pushes):** off by default; Settings › All messages (`muse_settings_pushes_on`, NVS key `pushes`). When on, `on_event` → `push_begin`.
  - Assistant messages that arrive with no turn pending open a reply-only turn.
  - `muse_voice.c` `play_push` plays it.
  - IDs already shown are remembered (`s_shown_ids`).
  - The idle disconnect is skipped and a closed session reconnects. Off behaves like upstream.
- **Touch volume:** `muse_ui.c` `volume_drag`.
  - A vertical drag on the face tile; horizontal stays the settings swipe.
  - The level is saved on release.
- **Avatar reactions:** `muse_pose_t`'s `dizzy`, `sleepy`, `waking` and `tickle` (`muse_pixel.h`), timed in `muse_state.c`.
  - Dizzy: shaking. `muse_imu.c` (QMI8658, probed at 0x6B/0x6A from each board's `init()`) feeds `muse_input.c` `check_shake`.
  - Sleepy: `check_sleep` drowses for `MUSE_SLEEPY_S` before auto-sleep, and plays two soft snores (`muse_voice_request_snore`).
  - Waking: plays when the screen wakes, or when drowsing is interrupted.
  - Tickle: rubbing (4 reversals in 1 s within 15% of the screen) or 4 quick taps in 1.2 s on the face tile, while idle. `muse_ui.c` `touch_read` wraps the board's touch read to sample every 15 ms; `on_touch` counts taps; `tickle_poll` takes the press from LVGL and undoes a volume drag or a slide to settings. Progress waits below `MUSE_TICKLE_HOLD` while it goes on (`muse_state_tickle_hold`).
  - Jollybot (`avatar/muse_pixel.c`) draws them in this fork (`react_setup` and what it drives, and `draw_helmet`). Those additions, like Jollybot, are **not** under the Apache License: keep Meta's copyright line and the notice under it, never add an Apache header, and keep them in their own commit so they can be removed on Meta's request.
- **Build branch indicator:** `muse_build_info.c`. CMake stamps the checked-out branch from `.git/HEAD` as `MUSE_GIT_BRANCH` (no `git` call; `detached` or `unknown` otherwise).
  - Settings home shows `Build: <branch>` (the exact branch, `muse_settings_ui.c` `s_build`) directly under the `Muse <ver> - <IP>` line; the button menu's Status page has it too (`Build`). The face has no branch badge.
  - `.git/HEAD` is a CMake configure dependency, so a branch switch in a reused `build-muse-<board>/` re-runs CMake and restamps instead of keeping the last branch.
- **VTEC reaction (held, from the solenoid), coolant warning and warm-up thumbs-up (s18 only, `CONFIG_MUSE_OBD_VTEC`):** `muse_obd.c` is a NimBLE central to a LELink^2 ELM327 adapter (FFE0/FFE1), beside Link's peripheral; its GAP events go to its own callback, not Link's.
  - The s18 overlay turns on `BT_NIMBLE_ROLE_CENTRAL`/`OBSERVER` and `MAX_CONNECTIONS=2`; every other board keeps them off.
  - `muse_obd_elm.c` (plain C, `tests/test_muse_obd_elm.py`) parses the ELM byte stream and runs the triggers. Nothing fires on RPM (OBD-VTEC-DID-TASK deleted that path and its constants); RPM feeds the cards and the trip log.
  - VTEC = Honda DID 0x2612 (OBDb Honda-S2000; `muse_elm_parse_vtec_did`, tested on real 2007 frames in `tests/vtec_did_frames.h`): data byte 21 bit 0 = solenoid, bit 1 = pressure switch, byte 23 raw. Only on ATDPN protocol 7. `probe_did()` asks once per connection after the support scan (and again after the first-RPM retries if the protocol only turned up then); refused, silent or garbled means not again on that connection. Once answered, the scheduler reads it on its 1 s floor. `did_read()` is state-safe: `ATSHDA10F1`, `ATCRA18DAF110`, the request, then always `did_restore()` (`ATAR`, `ATSHDB33F1`, retried once; failing drops the link into the normal re-init). A failed read leaves the last reading to age out. `tests/test_muse_obd_did.py` runs it against the real task with a stateful fake adapter.
  - More DIDs (OBD-DID-CARDS-TASK): `MUSE_DID_INFO` in `muse_obd_elm.c` is the table (DID, request, data bytes needed, floor): 2612 (exactly 54, 1 s), 2610 (>= 33, 3 s), 2663 (>= 42, 3 s). `probe_did()` probes each in turn, once per connection (`s_did[]`, the DID_NO pattern), each with `did_read()`'s choreography; the scheduler has one item per DID, one exchange at a time, most overdue first. 2610: data byte 32 mask 0x02 = A/C clutch, byte 23 x 100 / 255 = generator duty (`muse_elm_parse_did_2610`). 2663: big-endian counts at data bytes 34/36/38/40, cylinders 1-4 (`muse_elm_parse_did_2663`). Fixtures: `tests/did_cards_frames.h`, byte for byte from OBDb's 2007 YAML (all 57-byte replies).
  - Misfire warning: `muse_misfire_update()` (`muse_obd_elm.c`, beside the coolant trigger) on each good 2663 read with the latest RPM. The first read of a connection is the baseline; any count going down is a new baseline; a strict increase fires with RPM >= 400 and 120 s (`MUSE_MISFIRE_COOLDOWN_MS`) since the last fire; each read becomes the next baseline, so an increase is counted once. `rose` (bit per cylinder, the card's amber) is the latest read's increases. A fire calls `muse_obd_fire_misfire()`, which plays the coolant warning's pose (`muse_state_start_coolant()`; no new Jollybot drawing) and nothing while OBD is off. `drop()` resets the baseline, the cooldown holds.
  - The pose is held, not fired: `update_chrome()` calls `muse_state_set_vtec(muse_obd_vtec_on(&o))` every 0.25 s tick. `muse_obd_vtec_on` is enabled + `muse_vtec_engaged()` (fresh "on", <= `MUSE_VTEC_FRESH_MS` 3 s). `muse_state.c` runs in, waits at `MUSE_VTEC_HOLD` (0.5) while on (tickle's pattern), and on "off"/stale plays the settle from `MUSE_VTEC_SETTLE` (0.82). No cooldown. Jollybot animates `RE_VTEC` on `p->t` so the hold keeps buzzing (`tests/muse_state_vtec_harness.c`, the held check in `tests/muse_pixel_vtec_harness.c`).
  - Init: after `ATZ ATE0 ATL0 ATH0 ATS0 ATSP0`, `scan_support()` reads `0100` (then `0120` / `0140` while the chain bit is set) and `read_protocol()` sends `ATDPN` (after the scan: `ATSP0` only settles on a protocol at the first request). Both are retried once on the first RPM reply if they failed. Polling is response-driven through the scheduler (`SCHED` and `muse_sched_*` in `muse_obd_elm.c`): most-overdue-first over freshness floors, RPM the standing filler, DID 0x2612 at 1 s, fast 1 s (0B, 11, 04, 0D, 34, 49, 4A), mid 2 s (06, 07, 0E, 2F), slow 3 s (05, 0F, 46, 42, 33), up to `MUSE_ELM_BATCH_MAX` (6) PIDs per request, skipping PIDs the scan proved unsupported and evicting one after two misses against a live ECU.
  - The car reactions are visual, with no spoken line (speech would swap the reaction for the speaking pose). Jollybot draws VTEC (`RE_VTEC`, `tests/test_muse_pixel_vtec.py`).
  - Coolant: `0105` (A - 40 °C), in the slow (3 s) tier. Fires at >= 105 °C held 5 s with the latest RPM >= 400 (engine off or a dip below 105 restarts the 5 s); then waits for <= 100 °C and 120 s. Constants beside the VTEC ones in `muse_obd_elm.h`.
  - A coolant fire calls `muse_state_start_coolant()` (no speech). Jollybot draws it (`RE_COOLANT`, outranks `RE_VTEC`; `tests/test_muse_pixel_coolant.py`).
  - Warm-up: the same coolant readings feed `muse_warmup_update()`. A reading <= 70 °C (`MUSE_WARMUP_COLD_C`) arms it, the first >= 82 °C (`MUSE_WARMUP_FIRE_C`) fires it once, and only <= 70 °C re-arms it. It starts disarmed, and `drop()` re-initialises it, so a drive first seen warm never fires. A fire calls `muse_state_start_warmup()` (`MUSE_WARMUP_S` 3 s). Jollybot draws a thumbs-up on one raised paw (`RE_WARMUP`, below `RE_COOLANT` and `RE_VTEC`; `tests/test_muse_pixel_warmup.py`).
  - Status row: `muse_ui.c` shows a montserrat_14 `OBD` label between the BLE icon and the battery while `muse_obd_status()` is subscribed (dim) or polling (accent). It sits in a fixed slot, so hiding it never moves its neighbours.
  - "Linked" is that same condition, `muse_obd_linked()` (subscribed or polling, and enabled). `update_chrome()` reads it once per tick into `s_obd_linked`, which drives the cards and the helmet.
  - Helmet: `muse_pose_t.helmet` (set only in the gated pose block, from `s_obd_linked`). Jollybot's `draw_helmet()` draws a light shell over the hood with an accent stripe and a raised smoked visor. The eye port is the face panel plus a pixel, and the arms stay in front. It replaces the thinking headphones and sits under the foreground overlays. With it off, the renderer is byte-identical (`tests/test_muse_pixel_helmet.py`; previews in OBD-PRESENCE-TASK.md Results).
  - Settings › OBD (`muse_settings_obd_on`, NVS key `obd_on`, default on, OBD builds only) calls `muse_obd_set_enabled()` through `muse_app.c`'s listener. Off, every `wait_for()` in `muse_obd.c` gives up except the ones whose mask holds `UNWIND` (the disconnect in `drop()`, and the connect event after a cancel, so a connection that beats the cancel is still taken down). The task then sits in "off": no scan, no connect, no reactions, no readings, `"enabled": false`. On again, it scans without a restart. `tests/test_muse_obd_enable.py` runs the real task against fake NimBLE/FreeRTOS on a virtual clock.
  - The two fires go through `muse_obd_fire_coolant()` / `_warmup()` (`muse_obd.h`), which leave the triggers alone and need no adapter, and do nothing while OBD is off. On the bench, `>coolant` and `>warmup` force them, `>misfire` feeds the misfire trigger a cylinder 1 increase at a running RPM (`muse_obd_bench_misfire`: `@misfire fired`, or `@misfire cooldown`; it puts the car's baseline back afterwards), and `>vtecsol 1` / `>vtecsol 0` set a solenoid reading that stays fresh until the car's next one (`muse_obd_bench_vtec_sol`; `>vtec` is gone). All are in `muse_input.c` `console_command`, only with `CONFIG_MUSE_OBD_VTEC`; `@obd.off` while off.
  - Readout: the `obd.read` Link command (`app.c` `obd_read_command`) and `>obd` on the console (`@obd {...}`) report `enabled` and the adapter state (`off`/`waiting`/`scanning`/`connecting`/`discovering`/`initializing`/`polling`/`backoff`), the latest RPM/coolant and the cards' readings (`MUSE_OBD_EXTRA` in `muse_obd.c`: `load_pct`, `map_kpa`, `speed_kmh`, `timing_deg`, `iat_c`, `throttle_pct`, `stft_pct`, `ltft_pct`, `ambient_c`, and OBD-MORE-PIDS-TASK's `afr` (1 decimal) and `lambda` (3) from PID 34, `pedal_d_pct`, `pedal_e_pct`, `module_v` (3), `baro_kpa`, `fuel_pct`; `MUSE_OBD_EXTRA[].decimals` scales them) with their ages, OBD-DID-CARDS-TASK's `misfire_1`..`_4`, `ac_clutch`, `generator_pct` (with ages) and `did_2610`/`did_2663`, null when there is no current reading, plus `protocol` and `supported_00`/`_20`/`_40` (hex), and `vtec_sol`/`vtec_psw` (with ages), `did_2612` (kept after a drop) and `did_2612_b23`. Both read `muse_obd_status()`, a spinlocked snapshot that never blocks; a dropped link clears the readings but keeps the last protocol and masks. `tests/test_link_obd.py` checks the gating.
  - Cards: `muse_obd_cards.c` builds three passive tiles (Temps, Engine, Drive) between the face and settings, only while linked. `muse_ui.c` `sync_cards()` adds them (and moves settings to column 4) when the link comes up, and deletes them (`muse_obd_cards_drop()`, settings back to column 1) when it drops. It keeps the visible tile by object, a vanished card lands on the face, and it waits out a press or a slide. Dots follow `tile_count()`.
  - The cards refresh in `update_chrome()`. Older than 3 s is dimmed, `...` is no reading yet, a row whose PID the scan proved unsupported is hidden (the card reflows), and the link state replaces the rows until polling starts.
  - Card layout (OBD-LOG-TASK Part 5): a full-height flex column between the title and the dots, open cells split by 0x2a2440 hairlines (`rule_lines`), a 16 px label above each value. Values use Montserrat 40, the RPM and mph heroes 48 at twice the height (`fit_font` drops a size if a cell can't fit). RPM, throttle and fuel have 10 px bars, and RPM's cell has a `VTEC ON/OFF/--` line under its bar (`tick_vtec`: ON accent, OFF dim, `--` without a fresh reading). Halves pair up two at a time: TIMING/AFR, LOAD/MAP, THROTTLE/FUEL and STFT/LTFT share rows, so AFR (Engine, `14.7`), fuel (Drive) and the misfire row (Engine) joined without shrinking any cell below 40. The misfire row is four counts after their cylinder numbers (`tick_misfire`: `--` until a read, amber while `misfire_rose` marks it, three digits step down to 28 and four or five to 20 to fit a quarter); Temps' A/C cell reads ON (accent) / OFF (dim) / `--`. DID rows are never hidden by the PID scan. Coolant is amber at 100 °C and red at 105, trims amber at ±10 %. The s18 overlay enables `LV_FONT_MONTSERRAT_40`/`_48`.
  - Trip log (`muse_obd_log.c`, formats and store in plain C in `muse_obd_log_store.c`, `tests/test_muse_obd_log.py`): a CSV per polling link, a row per RPM reply (columns `...,vtec_sol,vtec_psw`, then OBD-MORE-PIDS-TASK's appended `afr,fuel_pct,pedal_d_pct,pedal_e_pct,module_v,baro_kpa`, AFR and volts with decimals, then OBD-DID-CARDS-TASK's `misfire_1,misfire_2,misfire_3,misfire_4,ac_clutch,generator_pct`; the header has `# did_2612=yes|no` once probed). The OBD task only queues rows (`muse_obd_log_sample`); the `muse_obd_log` task (internal stack) writes 4 KB blocks and drops and counts rows it can't keep up with. Stores: microSD (a stub: the s18's TF slot has no driver) then the `obdlog` FAT partition (`partitions_muse.csv`, 0x830000, 7.8 MB, wear-levelled, formatted only when it holds no filesystem). Each store has a `TRIPS.JSL` manifest (JSON lines, CRC-32 per trip); trip ids come from NVS `obd_trip` and are never reused. `drop()` ends the trip.
  - Pull commands, which work with OBD off: `obd.trips`, `obd.trip_read {id, offset}` (49152 bytes, base64) and `obd.trip_delete {id}`, the only deletion path. `obd.read` adds `log_*` fields; `log_pending` is the doorbell (there is no device-initiated push for a custom line).
  - Log doorbell (OBD-LOG-DOORBELL-TASK): the decision is plain C in `muse_obd_doorbell.c` (pending trips + per-connection announced set + session-ready in; the exact line `Trip logs ready: trips 5, 6 (198203 bytes total).` out, ids ascending, bytes summed; a session drop forgets the announced set). Its IDF side, `muse_obd_doorbell_task.c`, watches from `muse_obd_log_start` (so it runs with Car mode off): every 1 s it lists the stores through `muse_obd_log_list`, feeds `muse_hatch_ready()`, and sends through `muse_hatch_text_turn` — never any car-side traffic and no speech. Settings home, under the OBD switch, has a `Send logs to Muse` row (`muse_obd_doorbell_ring`) showing `N waiting, K KB` or `No logs waiting`, answering a tap on itself (`Sent` / `Not connected` / `Already sent` / `Nothing to send`, 2 s). The chat calls live in the doorbell task file alone: the older tests keep `muse_obd_log.c` free of them. Tests: `tests/test_muse_obd_doorbell.py`.
  - `>card <n>` on the console slides to tile n (0 face, 1..3 cards, 4 settings; `muse_ui_bench_tile`). While the cards are out, 1..3 answer `@card.error no cards: the OBD link is down`.
- **Battery:** each board's `read_power`.
  - The 1.85C infers USB from voltage trends, because it has no VBUS or charge pin.
  - Errs toward `usb = true`: on battery the firmware dozes Wi-Fi and closes idle sessions.

## Troubleshooting

- **Port missing or "Device not configured"**: check the cable before the firmware. Charge-only USB-C cables and docks/hubs dropping out look exactly like a hang. `system_profiler SPUSBHostDataType` shows what macOS really sees.
- **Screen frozen, firmware still running**: LVGL starving the band sender. This is fixed in `muse_lcd_bands.c` (upstream PR #34, cherry-picked).
- **Board dead until RST**: shouldn't happen any more, because the watchdogs panic with a backtrace. Capture the panic text from the console.
- **"Failed to verify certificate" for ElevenLabs**: the cross-signed option above is off.
- **ElevenLabs 401 `missing_permissions`**: the key lacks Text to Speech.
- **ElevenLabs 404 `voice_not_found` / 400 `voice_not_fine_tuned`**: pick another voice ID. A cloned voice must finish training first.
- **Wrong colours on a new panel**: byte order. **Image shifted**: column gap (`set_gap`). **Touch mirrored**: flip the axes in `tp_read`.

## Hardware gotchas (1.85C)

- Side header `E5`–`E8` are TCA9554 pins, not GPIOs. `19`/`20` are USB D−/D+.
- The header's SDA/SCL are the internal I2C bus (GPIO 11/10), shared with touch 0x15, ES8311 0x18, TCA9554 0x20, ES7210 0x40 and PCF85063 0x51.
- The label lists a QMI8658 IMU, but the V2 schematic doesn't show one. Scan I2C before relying on it.

## Upstream

`origin` is this fork; `upstream` is `facebookincubator/muse-gadget-sdk`. Check
what's new with `git fetch upstream && git log --oneline HEAD..upstream/main`,
and `gh pr list -R facebookincubator/muse-gadget-sdk` for open PRs (PR #34 adds
the AMOLED-1.8 via the vendor BSP; ours probes both revisions directly). Keep
upstream conventions: Apache headers, "Say Muse, never Hatch", one overlay per
board, docs updated in `esp32/README.md`, `esp32/AGENTS.md` and `esp32/devices/`.
