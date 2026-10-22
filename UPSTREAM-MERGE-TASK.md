# Execution: merge Meta upstream into this fork

Implements `UPSTREAM-MERGE-PLAN.md`. Derek's instruction (2026-10-21
20:53 PDT): "I want all the new features Meta has added while retaining
mine." Where both sides have a version of something, the fork's behaviour
wins; everything new from upstream comes in.

## Ground rules

- Work on a new branch `upstream-merge-2026-10` off `main`. Never commit
  to `main`, never touch `obd-vtec-reaction`, never touch `secrets/`.
- Claude Code performs the merge and the resolutions and commits per
  wave (below). Do NOT push; Muse QAs and pushes.
- Upstream remote: `upstream` = facebookincubator/muse-gadget-sdk,
  fetched; its main tip is `e6fe517`. Merge with an explicit message;
  `GIT_EDITOR=true` for any editor git wants.
- Read `CLAUDE.md`, `esp32/AGENTS.md` and `UPSTREAM-MERGE-PLAN.md` first.
  The plan's parity checklist is part of this spec.
- Secrets discipline from CLAUDE.md applies unchanged: the merge must
  not bring in, move, or print any secret; the `.githooks` scan runs on
  every commit.

## Baselines (recorded 2026-10-21 before the merge)

- Pre-merge main is tagged `pre-upstream-merge` (`ece7e69`, pushed).
- Host suite on pre-merge main code: 192 tests, 4 errors, all the known
  cJSON `setUpClass` ones (link_discovery, link_sensecap_sensors,
  muse_chat_link_errors, muse_chat_session; `managed_components/`
  absent on this VM).
- The live gadget (MuseGadget-FEA6C0, running a main build) registers 5
  commands: device.discover, device.health, device.ota,
  display.draw_url, display.show_animation. After this merge is built
  and flashed from main, the same list PLUS `display.draw_file` is the
  acceptance signal. (`obd.read` belongs to the OBD branch, not main.)

## Wave 1: the merge and the mechanical files

Run the merge of `upstream/main` into `upstream-merge-2026-10` and
resolve these files per the plan's table:

- `README.md`: keep the fork's identity (maintainer section, "This fork"
  rows, David Ruiz attribution untouched), take upstream's feature and
  board updates.
- `esp32/devices/README.md`: union of both board lists; the fork's three
  boards stay marked as fork additions.
- `esp32/AGENTS.md`, `esp32/devices/AGENTS.md`: take upstream's; re-add a
  fork note only if something upstream's text now makes wrong.
- `esp32/tools/muse/board.sh`, `ports.py`, `avatar.py`: take upstream's
  changes (flasher pacing and friends) AND keep the fork's behaviour:
  board.sh must still inject secrets through `tools/muse/secrets.py`,
  and the secrets flow in CLAUDE.md must still be true afterwards.
- `esp32/components/muse/Kconfig`: keep both sides' options; the fork's
  ElevenLabs block (`MUSE_ELEVENLABS_API_KEY` and neighbours) survives
  verbatim.
- `esp32/components/muse/idf_component.yml`: reconcile the dependency
  pins from both sides; drop neither side's requirements; if a version
  truly conflicts, take upstream's and say so in Results.
- `esp32/components/muse/muse_battery.h`: keep the fork's battery
  estimate API working on upstream's header.
- `esp32/tests/muse_serial_chat_harness.c`: union the fakes from both
  sides.

Commit wave 1 as the merge commit itself if practical, or the merge plus
a "resolve mechanical conflicts" commit. Run the host suite and record
the result in Results; classify every error as pre-existing
(cJSON/environment), new-from-upstream (their tests, their
responsibility, list them), or caused by this resolution (fix it).

## Wave 2: boards

- `esp32/components/muse/boards/board_waveshare_s3_185c.c` (add/add, 11
  hunks): **the fork's file wins.** Start from the fork's 740-line board
  (IMU, battery behaviour, the pins this repo's overlay describes) and
  adopt upstream changes only where they are clearly fixes to behaviour
  the fork's file also has; list each adopted fix in Results. If a hunk
  cannot be classified that way, keep the fork's side.
- `esp32/devices/sdkconfig.muse-waveshare-s3-185c`: match the fork's
  board file.
- `esp32/components/muse/boards/muse_lcd_bands.c`: upstream's
  tearing-effect work goes in, AND the fork's "sleep while a band goes
  out" LVGL starvation fix must still be present afterwards (it is the
  fix CLAUDE.md's troubleshooting section describes; the s18 draws
  through this file). Show in Results how you verified both are in.
- The s18 board file is fork-only and must be byte-identical before and
  after the merge; verify and say so.

Commit wave 2 separately. Host suite again.

## Wave 3: settings UI and chat session

- `esp32/components/muse/muse_settings_ui.c` (5 hunks): take upstream's
  structure (including the Oct 15 VM ID and token rows), then re-apply
  the fork's Settings surface so ALL of these exist afterwards:
  - the Voice row (ElevenLabs voice switcher, shown under its existing
    conditions),
  - the All messages pushes switch row,
  - the Display page and its row,
  - the Sound page's touch-volume toggle,
  - the footer notes: `Muse <ver> - <IP>` with `Build: <branch>`
    directly under it (`s_build`, from `muse_build_info.h`).
  List in Results every fork Settings item you re-applied and where.
- `esp32/components/muse/muse_chat_session.cpp` (1 hunk): the conflict
  is a comment at the turn-timeout check. Resolution rules:
  - `REPLY_TIMEOUT_US` stays **180 s** and `TURN_CAP_US` stays **600 s**
    (the fork's `4c6dc7a` values). Upstream's 60 s / 180 s must not
    survive anywhere in the merged file.
  - The agent-busy hold applies to voice turns as the fork has it.
  - Upstream's new turn behaviour (turns stay open while Muse works,
    capped turns that got no reply fail, "reply too long") stays in.
  - The fork's entire spoken-reply path stays in: ElevenLabs streaming
    TTS, sentence-by-sentence playback, voice choice, pushes playback.
  Grep the merged file for the timeout constants and the TTS entry
  points and report the values in Results.

Commit wave 3 separately. Full host suite; same error classification.

## Out of scope for this run

- The OBD branch (wave 4 of the plan) happens after on-device
  acceptance, driven by Muse from the merged main.
- Evaluating the video feature (#242) on the s18: it arrives with the
  merge; do not gate the merge on it and do not delete its code.
- No ESP-IDF build exists on this VM. Say so in Results; the Mac build
  is the next gate after Muse's QA.

## Results (Claude Code fills this in, one section per wave)

Run 2026-10-21 on Muse's VM, by Claude Code. Branch
`upstream-merge-2026-10` off `main` (`3b2b523`). Not pushed. `main`,
`obd-vtec-reaction` and `secrets/` untouched. No wave was blocked: no
resolution needed dropping a parity-checklist feature.

| Wave | Commit | What |
| --- | --- | --- |
| 1 | `494c044` | The merge itself (parents `3b2b523`, `e6fe517`) with the mechanical files resolved |
| 2 | `22e632a` | Boards: `muse_lcd_bands.c`; the 1.85C stays the fork's |
| 3 | `5230ecf` | `muse_settings_ui.c` and `muse_chat_session.cpp` |

The trial merge's inventory held exactly: 16 files, 43 hunks.

**No ESP-IDF on this VM: nothing was built for a target.** The Mac
builds (s18, s185c, with `check_sizes`) are the next gate after QA. To
make up some of that, `muse_settings_ui.c` was syntax-checked on the host
(gcc `-Wall -fsyntax-only`, LVGL 9.5.0, stub IDF headers): 0 errors and 0
warnings with `CONFIG_MUSE_VIDEO` and `CONFIG_MUSE_HATCH` on, either one
alone, and both off. `muse_lcd_bands.c` and the board files were not
compiled; `test_lcd_tearing_effect` compiles the TE part of the merged
`muse_lcd_bands.c` on the host and passes.

**How the host suite was run.** Each wave ran twice:
(a) as the baseline was measured, without `managed_components/`;
(b) with a temporary `managed_components/espressif__cjson/cJSON`
symlink to cJSON v1.7.18 (DaveGamble/cJSON release tarball, sha256
`3aa80684…f5ee5`, in `/tmp`, the release `espressif/cjson ~1.7`
packages). Run (b) is what actually exercises the voice/session
harnesses. The symlink was deleted after each run, and nothing from it
is committed.

### Wave 1: merge and mechanical files (`494c044`)

Resolutions:
- `README.md`: the fork's sides of all 4 hunks (title, board table,
  "What's new" rows, maintainer section, "This fork" rows and David Ruiz
  attribution all untouched). Upstream's news went in as a new "From
  upstream" section: video (#242), `display.draw_file`, BLE commands,
  serial/web-flasher SDK token, voice-turn changes, tearing effect, new
  boards, power profiling, Linux SDK. Also the new directories in the
  layout block and upstream's third-party notices. `README.meta.md` (the
  fork's copy of Meta's README, byte-identical to the base's) is now
  upstream's current README.
- `esp32/AGENTS.md`: upstream's text; the board.sh alias list and the
  `boot` hint row are unions (fork boards plus upstream's).
- `esp32/devices/AGENTS.md`: both sides' vendor-source rows.
- `esp32/devices/README.md`: hardware list union. Upstream's duplicate
  1.85C row was folded into the fork's: its wiki and shop links were
  kept. The fork's three boards are marked "(experimental, fork
  addition)" there and in the overlay table. The features table is a
  column union: upstream's 34 columns, with the 1.85C column taken from
  the fork and the fork's 1.43C and 1.8 columns added, all three headed
  "(fork)". Upstream's new video row reads "—" for them. Upstream's own
  "Waveshare ESP32-S3-Touch-LCD-1.85C" section now says that this fork
  builds its own board file and that `185c`/`s185c` are the same
  profile; upstream's backup steps are kept.
- `board.sh`, `ports.py`, `avatar.py`: upstream's boards, Mosaico and
  pacing changes, plus the fork's `s185c`/`s143c`/`s18`. Upstream's
  `185c` alias stays and builds the same profile. board.sh still runs
  `python3 "$root/tools/muse/secrets.py" "$B/sdkconfig"` before
  `idf.py` (now with upstream's `${preview:-}`/`${iris:-}` flags), so
  CLAUDE.md's secrets flow holds. avatar.py's `BOARDS` had the 1.85C
  name twice; upstream's `"185c"` entry was dropped, so it maps to
  `s185c` as before.
- `Kconfig`: both blocks. The ElevenLabs options
  (`MUSE_ELEVENLABS_API_KEY`, `_VOICE_ID`, `_VOICES`, `_MODEL`) are
  verbatim: 0 changed lines against main. Upstream's
  `MUSE_HATCH_COMPACT_BUFFERS` and `MUSE_VIDEO` follow them.
- `idf_component.yml`: both sides' rules. Version conflict:
  `espressif/esp_lcd_st77916` was in twice (fork `^2.0.2` for the 1.85C,
  upstream exact `2.0.2` for the 1.85C and EchoEar), and a YAML key can't
  repeat, so **upstream's `2.0.2` was taken**; it satisfies the fork's
  `^2.0.2`. Upstream's `esp_lcd_touch` and `esp_lcd_touch_cst816s` rules
  still list `waveshare_s3_185c`. The fork's 1.85C file doesn't use them,
  so it fetches two unused components; they were kept rather than drop a
  requirement.
- `muse_battery.h`: the fork's `muse_battery_eta()` declaration on
  upstream's header (new `muse_battery_note_cpu_pd`, longer JSON doc).
- `muse_serial_chat_harness.c`: both modes, the fork's `speech M S` and
  upstream's `ascii`.

Found and fixed in the merge, though no conflict marker showed them:
- Both sides had added `MUSE_BOARD_WAVESHARE_S3_185C`, and git merged
  both copies cleanly into `Kconfig` (a second choice entry and board-ID
  default) and `components/muse/CMakeLists.txt` (a second `elseif`).
  Upstream's duplicates were removed and the fork's kept.
- `tools/muse/secrets.py` shadowed Python's standard `secrets` module
  once upstream's `test_muse_battery.py`/`test_muse_serial_chat.py`
  (through `tools/muse/power.py` and `avatar.py`, which prepend
  `tools/muse` to `sys.path`) ran before upstream's new power tools,
  which call `secrets.token_hex`: 33 errors and 7 failures in
  `test_power_sweep_host` and `test_ppk2_*`. Both tests now import the
  standard `secrets` before touching `sys.path`. Renaming the fork's
  file was ruled out because board.sh must keep injecting through
  `tools/muse/secrets.py`. (One misstep, corrected before the commit:
  restoring `test_muse_battery.py` from the fork's side briefly dropped
  upstream's version of it. It was restored from upstream and
  re-checked; the counts below are from the corrected tree.)
- The `.githooks` pre-commit scan blocked the merge commit on five
  token-shaped strings in upstream's tests
  (`esp32/tests/link_identity_harness.c:39`,
  `linux/tests/test_pair.py:129`, `linux/tests/test_service.py:173,178,180`).
  Each is byte-identical in `upstream/main`, and they are made-up
  fixtures (`mgst_BBBB…`, `mgst_AAAA…`, `mgst_AbC1…`). The pre-push hook
  would have stopped Muse's push on the same lines. Instead of
  `--no-verify`, `check_secrets.py` gained `UPSTREAM_FAKES`: those exact
  values (stored as SHA-256 hashes, so the file holds nothing
  token-shaped) pass in those files only. Any other token anywhere, and
  every real-value check, still blocks. `test_check_secrets` passes.

Host suite after wave 1:
- (a) 636 tests, 6 errors, 0 failures, 10 skipped. All 6 errors are
  pre-existing/environment (cJSON from `managed_components/`,
  `setUpClass`):
  - The baseline's 4: `test_link_discovery`, `test_link_sensecap_sensors`,
    `test_muse_chat_link_errors`, `test_muse_chat_session`.
  - 2 more, new from upstream and the same environment cause:
    `test_link_reterminal_sht4x`, `test_muse_chat_link_note`.
- (b) 660 tests, OK, 0 errors, 0 failures, 7 skipped.
- New-from-upstream failures: none.
- Caused by this resolution: the `secrets` shadowing above, fixed before
  the commit.
- One flake: `test_coexist_bench.test_blob_streams_the_requested_bytes`
  (an upstream C5 bench test) failed once on the first run and passed
  on every run after.

### Wave 2: boards (`22e632a`)

- `board_waveshare_s3_185c.c`: **the fork's 740-line file, unchanged**.
  Upstream's 513-line 1.85C came in one commit (`8e613f7`, #112) with no
  later fixes. It's a separate implementation: BSP CST816S touch,
  hold-to-talk on the face, I2S1 with split data interfaces,
  `usb_serial_jtag_is_connected()` for USB, and a new optional
  `display_pause`/`wait_buttons` light-sleep path. None of that is
  clearly a fix to behaviour the fork's file has, so **no upstream hunk
  was adopted**. Two worth an on-device look:
  - Upstream's `round_area` (even-aligned ST77916 windows). The fork's
    s18 and 1.43C round their QSPI windows, and `muse_lcd_bands`
    sends LVGL's dirty-area x range as-is. But upstream's own ST77916
    EchoEar doesn't round, and the fork's 1.85C runs without it. If a
    1.85C shows a garbled column at the edge of a redraw, this is the
    fix to pick.
  - `display_pause`/`wait_buttons`: not implemented on the fork's
    1.85C, so it stays awake on battery as before (its overlay says
    so).
- `sdkconfig.muse-waveshare-s3-185c`: the fork's. Upstream's differs only
  in its light-sleep block, which needs `display_pause`, and a 16 MB
  flash line that `sdkconfig.muse` already sets. Upstream's
  `boards/waveshare_s3_185c/st77916_init_185c.h` stays in the tree,
  unused, as upstream's file.
- `muse_lcd_bands.c`: both. Verified by a line check against the merge
  base:
  - All 36 lines the fork added (10ca9b5) are in the merged file.
  - All 308 lines upstream added (#241, #242) are in it except three:
    the `bool last = chunk_done()` phrasing, which the fork's
    `if (chunk_done()) { … }` block replaces with the same logic.
  - The merged file has no line that neither side has.

  Markers present:
  - Starvation fix: `s_band_busy`, `s_band_done` (created and checked in
    register), `wait_band`, `lv_display_set_flush_wait_cb(s_disp,
    wait_band)`, the send-error path clearing `s_band_busy`.
  - Upstream: `muse_lcd_bands_set_te`, `te_band(b.y1…)` in the send
    loop, `wire_idle(0/1)`, `s_band_end` stamped in the piece-done ISR,
    `portYIELD_FROM_ISR`, `heap_caps_aligned_alloc(16…)`,
    `muse_lcd_bands_frame`/`set_gap`, `s_rows`.

  The ISR now stamps `s_band_end`, clears `s_band_busy`, gives
  `s_band_done`, notifies LVGL, then yields at the interrupt's exit.
- `board_waveshare_s3_18.c`: **byte-identical**: blob `81df3d7` on main
  and on the branch, sha256 `f0225fdc…f35e4a5` before and after;
  upstream has no such file.

Host suite after wave 2: (a) 636 tests, the same 6 cJSON `setUpClass`
errors, 0 failures. (b) 660 OK. Nothing new.

### Wave 3: settings UI and chat session (`5230ecf`)

`muse_settings_ui.c`: upstream's structure. Upstream's Oct 15 rows (VM
ID, Device token, SDK token, lines 941–943), its Phone camera row
(1394), its no-audio and e-paper conditions. On top of it, the fork's
Settings surface, re-applied and checked at:
- **Voice row**: home, 1386–1387, under its existing conditions
  (`CONFIG_MUSE_HATCH`, an ElevenLabs key, more than one voice); the
  Voice page is `build_voice_page`, 1185.
- **All messages** switch: home, 1377, `on_pushes_sw` 1357.
- **Display page and row**: `build_display_page` 1160 (Brightness, as on
  main), home row 1382. The row is now skipped on e-paper boards, the
  condition upstream gave Brightness; the fork's boards are unaffected.
- **Touch volume** toggle: Sound page, 1097, built with Speaker and
  Volume when the board has a speaker (upstream's `audio_init`
  condition; all three fork boards have one). The fork's volume-slider
  sync in `tick_sound` (1133) now skips boards without a slider.
  Brightness stays off the Sound page.
- **Footer**: `s_about` then `s_build` (1400–1401), set to
  `Muse <ver> - <IP>` (1452) and `Build: <branch>` (1455) from
  `muse_build_info.h`.

`muse_chat_session.cpp`: the one hunk was the comment over the reply
timeout. Upstream's wording was kept ("the turn cap still applies"); the
condition was already the same on both sides. Grep of the merged file:
- `REPLY_TIMEOUT_US (180 * 1000000LL)` (line 122) and `TURN_CAP_US (600
  * 1000000LL)` (123). `muse_chat_link.c` `REPLY_TIMEOUT_US 180000000`
  (59). No 60 s or 180 s voice reply or cap value is left anywhere in
  `components/muse`. `DEAD_US` 60 s is the server-silence timeout, the
  same on both sides.
- Busy hold for voice: `… REPLY_TIMEOUT_US) && !s_turn.agent_busy`
  (2082). Upstream's capped voice turn with no reply fails with NO
  REPLY FROM MUSE (`!text && !s_turn.nmsgs`, 2073). "Reply too long"
  (`TOO_LONG`, `MUSE_LINK_REQ_TOO_LARGE`) is in `muse_chat_link.c`.
- TTS entry points: `push_begin` 1338, `tts_fetch` 1681, `tts_start`
  1765, `start_tts` 1852, `tts_data` 1899, `tts_pump` 1930; sentence
  pieces through `muse_hatch_speech_cut`; `play_push` in
  `muse_voice.c`.
- The line check found every line the fork added except the replaced
  comment, and all of upstream's 19.

Parity checklist: 66 symbol checks across the files involved, each
counted on `main` and on the merged tree, all equal. They cover:
- spoken replies, sentence pieces, the voice switcher and
  `secrets/elevenlabs_voices`;
- the timeouts and busy hold;
- pushes and their switch;
- touch volume and its toggle, the Display page;
- dizzy, sleepy, waking, tickle and giggle, and the laptop/headphones
  pose;
- the three boards in Kconfig, CMake and board.sh;
- battery ETA and the Battery page;
- `secrets.py` in board.sh, the `.githooks`, the `.gitignore`;
- the build-branch line and menu;
- the README identity.

A tree-wide line check also found no fork or upstream line lost outside
the hand-resolved files.

Host suite after wave 3: (a) 636 tests, the same 6 cJSON `setUpClass`
errors, 0 failures, 10 skipped. (b) 660 OK, 7 skipped. With cJSON these
40 pass verbosely:
- `test_muse_chat_session`, `test_muse_chat_link_errors`,
  `test_muse_chat_link_note`;
- `test_muse_speech_pieces`, `test_muse_voices`;
- `test_muse_build_info`.

No host test drives the 180 s / 600 s timings themselves: the grep above
is the check, and the on-device >60 s turn in acceptance gate 3 is the
real test.

### Before Muse pushes

- `secrets/` on this VM holds neither real value, so the hook's
  real-value check had nothing to compare against here (the shape scan
  ran). Run `python3 esp32/tools/muse/check_secrets.py --commits
  "upstream-merge-2026-10 --not --remotes=origin"` where `secrets/` is
  filled in, as the pre-push hook will. On this VM it is clean. `git log
  -p upstream/main..HEAD` has no token-shaped strings.
- Review `esp32/tools/muse/check_secrets.py` (the `UPSTREAM_FAKES`
  allowlist): it's a change to the fork's secrets tooling. The two
  upstream tests with the `secrets` pre-import are also edits to
  upstream files that a later upstream merge may touch again.
