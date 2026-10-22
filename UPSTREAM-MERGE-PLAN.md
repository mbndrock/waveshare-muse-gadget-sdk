# Plan: merge Meta's upstream into this fork (bypassing wupsbr)

Status: PLAN ONLY (written 2026-10-21). No merge has been started.
Owner split when executed: Muse specs and QAs, Claude Code resolves and
commits, Derek builds and flashes on the Mac and accepts on the device.

## Remote layout: yes, we can skip the middle fork

Git remotes are just named URLs; nothing in git routes through the repo a
fork was made from. This repo's history already contains every commit from
wupsbr's fork, including its tip:

- `wupsbr/main` tip is `2c64881` (2026-10-07, "Merge upstream main (38
  commits) into the fork"), and it is an ancestor of this repo's `main`.
- Measured 2026-10-21: **0 commits exist on wupsbr/main that this repo
  lacks.** Bypassing it today loses nothing.
- wupsbr has not committed since Oct 7; Meta ships daily. Treat Meta as
  the upstream and keep wupsbr as an occasional reference remote.

Setup (already done in Muse's VM clone; do it in the Mac clone too):

```sh
git remote add upstream https://github.com/facebookincubator/muse-gadget-sdk.git
git fetch upstream
```

The one thing that does NOT change: GitHub still labels this repo "forked
from wupsbr/waveshare-muse-gadget-sdk", and GitHub's Sync fork button
would sync from wupsbr, not Meta. That is a GitHub-side label only; local
git neither knows nor cares. Pull requests can still target
facebookincubator because all three repos are one fork network.

## Where things stand (measured 2026-10-21)

- Merge base of this fork's `main` and Meta's `main`: `b139b45`
  (2026-10-05, a Meta commit, reached via wupsbr's Oct 7 merge).
- Meta `main` tip: `e6fe517` (2026-10-21). **76 upstream commits** not in
  this fork; **44 fork commits** not in upstream.
- A trial merge on a scratch branch (done, measured, aborted) changes
  **598 files** and conflicts in **16 files / 43 hunks** (table below).
- Host test baseline on this fork's main: 192 tests, 4 known cJSON
  `setUpClass` errors (link_discovery, link_sensecap_sensors,
  muse_chat_link_errors, muse_chat_session) when `managed_components/` is
  absent. Post-merge parity means: no NEW failures beyond those four.

## What the fork must not lose (parity checklist)

From wupsbr (Oct 4 set) and Derek's own commits (Oct 21):

- Spoken replies: ElevenLabs streaming TTS, sentence-by-sentence playback
  (`5ff4f26`), on-device voice switcher, `secrets/elevenlabs_voices`.
- **Voice turn timeouts: 180 s reply / 600 s cap with the agent-busy hold
  (`4c6dc7a`). Upstream still ships 60 s / 180 s. Do not let the merge
  silently regress these numbers.**
- All messages pushes (opt-in, off by default) and the Settings switch.
- Touch volume drag and its Sound setting toggle, the Display page
  (Brightness), avatar reactions (dizzy, sleepy, waking, tickle, giggle)
  and the thinking pose (laptop and headphones).
- The three Waveshare boards, above all the s18
  (`board_waveshare_s3_18.c`), which upstream does not have at all.
- Battery estimate and the Battery page.
- Secrets hygiene: `secrets/` flow, `secrets.py` injection in board.sh,
  the `.githooks` commit/push scan.
- The build branch indicator (`muse_build_info.c`, Settings `Build:`
  line).
- README fork identity: maintainer section, "This fork" rows, David Ruiz
  attribution untouched.

## What upstream brings (highlights of the 76)

- Video from a phone over Wi-Fi, H.264 and VP8 decode on the gadget, with
  Android/iPhone test apps (#242, today; 327 files, almost all additive).
- `display.draw_file`: an image from the VM over Noise (#250).
- BLE commands letting Muse drive the Link's Bluetooth LE (#188).
- Linux LinkSession: chat subscription, speech streaming, voice notes,
  spoken-reply requests, MuseTurn (the musepi side).
- SDK token set over serial and from the web flasher; Settings rework
  (VM ID row, real token state) on Oct 15.
- Tearing-effect synced drawing for the 1.75/1.75C panels (#241).
- Web flasher on GitHub Pages; new boards (Waveshare 1.28 LCD, upstream's
  own 1.85C, 2.16 AMOLED full UI, C6 watch, e-paper boards, ESP32-S31
  Mosaico); Noise tunnel and crypto performance work; voice-turn fixes
  (turns stay open while Muse works, capped turns fail cleanly).

Most of it is additive and merges cleanly. The pain is concentrated in
the table below.

## Conflict inventory (from the trial merge, 2026-10-21)

| File | Hunks | Nature and resolution rule |
| --- | --- | --- |
| `esp32/components/muse/boards/board_waveshare_s3_185c.c` | 11 | Add/add: two independent 1.85C boards (fork 740 lines with IMU and battery work; upstream 513). **Derek decision, see below.** |
| `esp32/components/muse/muse_settings_ui.c` | 5 | Both sides reworked Settings (upstream: VM ID and token rows; fork: Voice row, pushes switch, Display, Build line). Take upstream's structure, re-apply the fork's rows by hand. |
| `README.md` | 4 | Keep fork identity and attribution; take upstream's feature and board updates. |
| `esp32/devices/README.md` | 4 | Board list: union of both, fork's three boards marked as fork additions. |
| `esp32/tools/muse/board.sh` | 3 | Upstream added flasher pacing; the fork injects secrets via `secrets.py`. Keep both behaviours. |
| `esp32/AGENTS.md` | 2 | Take upstream's, keep any fork-specific notes that still apply. |
| `esp32/components/muse/boards/muse_lcd_bands.c` | 2 | The fork's LVGL starvation fix vs upstream's tearing-effect work. Merge carefully; the s18 draws through this file. |
| `esp32/components/muse/idf_component.yml` | 2 | Dependency pins from both sides; reconcile versions, do not drop the fork's. |
| `esp32/tests/muse_serial_chat_harness.c` | 2 | Harness grew on both sides; union the fakes. |
| `esp32/devices/sdkconfig.muse-waveshare-s3-185c` | 2 | Follows whichever 1.85C board file wins. |
| `esp32/components/muse/Kconfig` | 1 | The fork's ElevenLabs block vs upstream's neighbours. Keep both. |
| `esp32/components/muse/muse_battery.h` | 1 | Small; keep the fork's estimate API on upstream's header. |
| `esp32/components/muse/muse_chat_session.cpp` | 1 | Only a comment collision at the turn-timeout check. The real work is semantic: keep the fork's 180 s / 600 s constants and busy hold with upstream's capped-turn behaviour, then run the voice harnesses. |
| `esp32/devices/AGENTS.md` | 1 | Docs; take upstream's. |
| `esp32/tools/muse/avatar.py` | 1 | Small; check the fork's flow still works. |
| `esp32/tools/muse/ports.py` | 1 | Small; union. |

## Strategy: one merge, in waves, on its own branch

Not a rebase: the fork's 44 commits include merges and long-lived feature
work; rebasing would replay every conflict repeatedly. One merge commit of
`upstream/main` into a dedicated branch, resolved in waves, each wave its
own commits so a bad wave can be dropped without losing the rest.

- **Wave 0, prep.** Tag the current main tip (`pre-upstream-merge`,
  currently `9c52d18`). Freeze new feature work on main until the merge
  lands. Record the host test baseline (above) and the live gadget's
  command list (`device.describe` on MuseGadget-FEA6C0) for comparison
  after flashing.
- **Wave 1, mechanical conflicts.** Branch `upstream-merge-2026-10` off
  main, merge `upstream/main`, resolve the docs, tools, Kconfig,
  idf_component.yml, battery header and harness files. Host suite parity
  check.
- **Wave 2, boards.** Resolve the 1.85C per Derek's decision, plus
  `muse_lcd_bands.c` and the 1.85C overlay. The s18 board file is
  fork-only and untouched by the merge; say so in the wave's verification.
- **Wave 3, the semantic pair.** `muse_settings_ui.c` and
  `muse_chat_session.cpp`, with the parity checklist above re-verified
  item by item in the diff, and the voice/session host harnesses green.
- **Wave 4, the OBD branch.** After merged main is accepted and pushed,
  rebuild `obd-vtec-reaction` fresh off the new main by cherry-picking
  its 23 OBD commits (`d8db55e` through `4f1e4d4`; the branch-indicator
  commits are already in main and are not part of the range). Run the OBD
  test set (build info, obd link, ELM, VTEC, coolant). Do not merge the
  old branch forward; its base is pre-merge main and would drag the whole
  conflict back in.
- **Wave 5, new feature evaluation (not merge blockers).**
  `display.draw_file` and the BLE commands arrive with the merge; test
  them on the device (note: BLE commands plus the OBD branch's own NimBLE
  central share the radio and the 2-connection budget on the s18).
  Video (#242) gets its own evaluation on the s18 afterwards; the merge
  does not wait for it.

## Acceptance gates

1. Host suite: no failures beyond the 4 known cJSON errors.
2. Mac builds pass for s18 and s185c, with `check_sizes` inside the slot.
3. Flash the s18 (NVS kept, no erase) and verify on the device: pairing
   and Wi-Fi survive; a voice note gets a spoken reply in the chosen
   ElevenLabs voice; a long agent turn survives past 60 s (the timeout
   regression check); pushes play when enabled; Settings shows the fork's
   rows and `Build: main`; touch volume and the Display page work; the
   thinking pose renders.
4. Only then does `main` move: push the merge branch, fast-forward main
   to it, push. Until acceptance, origin's main stays at the tagged
   pre-merge tip and rollback is "flash the old build, do nothing".

## Decisions for Derek

1. **The 1.85C board file:** keep the fork's 740-line version (IMU,
   battery work, the one this repo's overlay and README describe) and
   port upstream's fixes onto it, or take upstream's 513-line version
   and re-add the fork's extras? Recommendation: keep the fork's; the
   1.85C is not the daily board, and upstream's board work keeps arriving
   as separate commits that can be picked later.
2. **Timing:** upstream is shipping about 5 commits a day and the
   chat-session conflict is currently a single comment hunk. Every week
   of delay grows the settings and session divergence. Recommendation:
   do it within the next week or two, then merge upstream monthly (or
   every 30 to 50 upstream commits) so it never gets this big again.
3. **Video:** evaluate #242 on the s18 as its own follow-up, or skip it
   entirely? It is additive, so skipping costs nothing at merge time.

## Effort estimate (an estimate, not a promise)

Waves 1 to 3: one focused evening with Claude Code resolving under
Muse's QA; the settings UI and 1.85C are the slow files, the rest is
mechanical. Mac build, flash and on-device acceptance: a second, shorter
session. Wave 4 (OBD branch rebuild): about an hour including tests.
