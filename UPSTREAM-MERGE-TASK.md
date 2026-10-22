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

- Pending.
