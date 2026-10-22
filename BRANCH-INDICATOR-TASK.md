# Branch indicator on the gadget screen

Branch: `main` (work ONLY on this branch; never commit to or push
`obd-vtec-reaction` in this run; never touch `secrets/`). Claude Code
implements and commits on `main` but does NOT push; Muse QAs the diff,
cherry-picks the finished change onto `obd-vtec-reaction`, and pushes both.

## Why

Derek switches one Mac checkout between `main` and `obd-vtec-reaction`,
builds the s18 profile, and flashes the same gadget. Once a build is on the
board there is no visible way to tell which source branch produced it. The
firmware itself should answer that at a glance, on both branches.

## Design

Stamp the Git branch that was checked out at build time into the firmware.
Do not store it in NVS, do not derive it from a feature flag, and do not use
the app version for it. The same code lands on both branches, so the label
must follow the checkout used for the build:

- Face badge: `MAIN BUILD` on `main`, `OBD BUILD` on
  `obd-vtec-reaction`, `DEV BUILD` on any other branch, a detached HEAD, or
  an unknown source tree.
- Exact branch: `main` or `obd-vtec-reaction` in the Settings footer and in
  the button menu's Status page.

## What to build

### 1. Build info API

Add `esp32/components/muse/muse_build_info.h` and
`esp32/components/muse/muse_build_info.c`, and add the `.c` file to the
`CONFIG_MUSE_ENABLED` source list in
`esp32/components/muse/CMakeLists.txt`.

API:

```c
const char *muse_build_branch(void);
const char *muse_build_badge(void);
```

- `muse_build_branch()` returns the compile-time `MUSE_GIT_BRANCH` macro
  when it is defined, otherwise `"unknown"`.
- `muse_build_badge()` returns a static string, with no allocation and no
  mutable buffer:
  - `"main"` -> `"MAIN BUILD"`
  - `"obd-vtec-reaction"` -> `"OBD BUILD"`
  - anything else -> `"DEV BUILD"`
- Header comments should say the branch is a build-time stamp from the
  checkout, not a runtime or persisted setting.

### 2. Capture the branch in CMake

In `esp32/components/muse/CMakeLists.txt`, after
`idf_component_register(...)` and under `CONFIG_MUSE_ENABLED`:

- Resolve the repository root from `COMPONENT_DIR` (`../../..`).
- Look for `<repo>/.git/HEAD`. When it exists, register it with
  `CMAKE_CONFIGURE_DEPENDS` (`set_property(DIRECTORY APPEND PROPERTY
  CMAKE_CONFIGURE_DEPENDS ...)`). This matters: `tools/muse/board.sh`
  reuses `build-muse-<board>/` across branch switches, so a branch switch
  must re-run CMake and restamp the firmware instead of reusing the last
  branch's definition.
- Read the HEAD line without depending on a `git` executable:
  - `ref: refs/heads/<branch>` -> `<branch>`
  - a raw commit hash -> `"detached"`
  - missing, unreadable, a `.git` file (worktree), or any other shape ->
    `"unknown"`
- Sanitize the branch for a C string macro: keep only
  `[A-Za-z0-9._/-]`, replace every other character with `-`, and cap it at
  64 characters. Use a normal, non-cache CMake variable so a reconfigure can
  replace it.
- Apply it as a PRIVATE compile definition on the Muse component library:
  `MUSE_GIT_BRANCH="<sanitized branch>"`, with the quotes escaped for
  CMake. Every consumer in this change is inside the same component.

### 3. Face badge in muse_ui.c

- Include `muse_build_info.h`.
- Add `static lv_obj_t *s_branch_lbl;` beside `s_name_lbl`.
- In `build_screen()`, after the name label, create the branch label with
  `make_label`, the same font choice as the name label, and `COLOR_DIM`.
  Set its text once to `muse_build_badge()`. Give it 1 px letter spacing.
- Align it `LV_ALIGN_TOP_MID` at `s_small ? 42 : 80 + s_dy`: one row below
  the gadget name row. Apply the same initial hidden rule as the state and
  name labels on tiny square screens
  (`s_small && !s_tall && s_h < 200`).
- The read-answer layout covers the top of the screen, so add the branch
  label as the third hidden object in `s_answers[ANSWER_READ]`
  (`hides[2]`, and the `add_hides` count goes from 2 to 3). Do not hide it
  in the heard-answer layout.
- Do not change the state label, name label, pairing behaviour, captions,
  or any avatar code.

### 4. Exact branch in Settings and the button menu

- `esp32/components/muse/muse_settings_ui.c`: include
  `muse_build_info.h`. In `tick_home()`, the footer becomes
  `"Muse %s - %s - %s"` with the app version, `muse_build_branch()`, then
  the IP address or `"offline"`. Grow the local buffer from 96 to 128
  bytes; the note already wraps.
- `esp32/components/muse/muse_menu.c`: include `muse_build_info.h`. In
  `status_text()`, add a `Build %s` line with `muse_build_branch()`
  between the Power and Ver lines. Grow the caller's buffer in `refresh()`
  from 160 to 192 bytes so the version line is not pushed out.

### 5. Docs

Add one bullet to the "Features added in this fork" section of `CLAUDE.md`:
the build branch indicator, where the face badge and exact branch appear,
and the `.git/HEAD` configure dependency that keeps a reused build
directory honest across branch switches.

Fill in this file's Results section when done.

## Host QA (NO ESP-IDF on this VM, no idf.py builds)

- New harness `esp32/tests/muse_build_info_harness.c` and test
  `esp32/tests/test_muse_build_info.py`. Compile
  `muse_build_info.c` on the host with
  `cc -std=c11 -Wall -Wextra -Werror`, once per case, and run it:
  - `-DMUSE_GIT_BRANCH='"main"'` -> branch `main`, badge `MAIN BUILD`
  - `-DMUSE_GIT_BRANCH='"obd-vtec-reaction"'` -> branch
    `obd-vtec-reaction`, badge `OBD BUILD`
  - `-DMUSE_GIT_BRANCH='"feature/new-thing"'` -> branch
    `feature/new-thing`, badge `DEV BUILD`
  - no macro -> branch `unknown`, badge `DEV BUILD`
- The Python test also checks the contracts by source inspection:
  - the CMake block reads `.git/HEAD`, registers it as a configure
    dependency, falls back to `detached` / `unknown`, sanitizes, and
    defines `MUSE_GIT_BRANCH` on the component library;
  - `muse_ui.c` creates `s_branch_lbl` from `muse_build_badge()` and
    hides it in the read-answer layout;
  - `muse_settings_ui.c` and `muse_menu.c` show `muse_build_branch()`.
- Run the new test, then the full host suite. Report the known pre-existing
  failures plainly (the cJSON `setUpClass` errors in link_discovery,
  link_sensecap_sensors, muse_chat_link_errors and muse_chat_session when
  `managed_components/` is absent) and do not claim an ESP-IDF build ran.
- `muse_ui.c`, `muse_settings_ui.c` and `muse_menu.c` cannot be
  host-compiled here (LVGL and IDF headers); review those diffs line by line
  and say so in Results.

## Commits

Separate logical commits on `main` (the spec is already its own commit;
then build info plus CMake plus tests; then the UI surfaces plus docs and
Results). Do NOT push.

## Acceptance on the real board (after Derek's Mac build and flash)

- Build and flash `main` for s18 from the existing build directory: the
  face shows `MAIN BUILD` under the state and name rows, and Settings ends
  with `Muse 999.0.0 - main - <IP>`.
- Switch the same checkout to `obd-vtec-reaction`, pull, build the same s18
  profile WITHOUT deleting the build directory, and flash: the face shows
  `OBD BUILD`, Settings shows the exact `obd-vtec-reaction`, and
  `obd.read` is still registered. This is the case the configure dependency
  exists for.
- A read reply (speaker off) hides the badge with the state and name, and
  it comes back when the reply layout leaves.

## Results (Claude Code fills this in)

- Commits on `main` (not pushed): build info API + CMake stamping + host
  test/harness; then the face badge, Settings footer, menu Status line,
  `CLAUDE.md` bullet and these Results.
- `muse_build_info.{h,c}` as specified; `muse_build_info.c` added to the
  `CONFIG_MUSE_ENABLED` source list. The CMake block sits after
  `idf_component_register(...)` inside the existing `CONFIG_MUSE_ENABLED`
  block, registers `<repo>/.git/HEAD` as a configure dependency, reads it
  with `file(STRINGS)` (no `git`), and defines `MUSE_GIT_BRANCH` PRIVATE on
  `${COMPONENT_LIB}` from a normal (non-cache) variable.
- Deviations / additions, both small:
  - `answer_layout_t.hides` grew from 4 to 5. With three fixed labels,
    `add_hides()` can append both hint icons (mic and aux), which would
    have written `hides[4]` out of bounds. `set_answer()` loops over
    `sizeof(hides)`, so nothing else changes. The test checks it.
  - On CMake 3.29+ (what IDF v6 ships) `IS_READABLE` guards the read, so
    an unreadable HEAD gives `unknown` instead of a configure error; older
    CMake reads it unguarded.
  - The Settings footer uses the spec's single-space `" - "` separator; the
    old footer had `"  -  "`.
- CMake block exercised by hand with `cmake -P` (host CMake 3.28) on fake
  repos: `main`, `obd-vtec-reaction`, `feat/x y$z` -> `feat/x-y-z`, a raw
  hash -> `detached`, garbage -> `unknown`, a worktree `.git` file ->
  `unknown`, an 80-char branch -> capped at 64.
- Host QA: `test_muse_build_info` 7/7 OK (four `cc -std=c11 -Wall -Wextra
  -Werror` cases: main/MAIN BUILD, obd-vtec-reaction/OBD BUILD,
  feature/new-thing/DEV BUILD, no macro/unknown/DEV BUILD; plus the CMake,
  face, Settings and menu source contracts). Full suite: 191 tests, 4
  errors, 4 skipped. The 4 errors are the known pre-existing `setUpClass`
  failures in link_discovery, link_sensecap_sensors, muse_chat_link_errors
  and muse_chat_session (`managed_components/espressif__cjson/.../cJSON.c`
  absent); the same 4 occur with this change stashed.
- No ESP-IDF build ran (no IDF on this VM). `muse_ui.c`,
  `muse_settings_ui.c` and `muse_menu.c` need LVGL/IDF headers and were not
  compiled; their diffs were reviewed line by line. The real-board
  acceptance steps above are still to do on Derek's Mac.

## Amendment: indicator belongs in Settings, not on the face

Derek's verdict (2026-10-21 20:37 PDT): the face badge is dumb. Remove it
and put the branch label in the Settings page instead, directly under the
version line.

### Changes

1. Remove the face badge from `esp32/components/muse/muse_ui.c`:
   - Delete `s_branch_lbl`, its creation in `build_screen()`, and the
     branch label's `hides[2]` entry; restore `answer_layout_t.hides` to
     `[4]` and the `add_hides` count to 2. The face goes back to having no
     branch chrome at all.
   - Remove the `muse_build_info.h` include from `muse_ui.c` if nothing
     else uses it there.
2. Remove `muse_build_badge()` from `muse_build_info.h` and
   `muse_build_info.c`: only the exact branch is shown now, so the badge
   API is dead code. Keep `muse_build_branch()`, the CMake stamping and
   the configure dependency exactly as they are.
3. In `esp32/components/muse/muse_settings_ui.c`:
   - Add a second footer note, `s_build`, immediately after the existing
     `s_about` note in `build_home()`.
   - In `tick_home()`, set `s_about` back to the version/IP line
     (`"Muse %s - %s"`) and set `s_build` to `"Build: %s"` with the exact
     branch from `muse_build_branch()`. This puts the branch directly
     under the version number on the Settings home page.
   - Keep the 128-byte buffer; both lines are set from it.
4. Keep the button menu's Status page `Build %s` line and its 192-byte
   buffer unchanged.
5. Update the tests:
   - The harness and `test_muse_build_info.py` now cover the branch only
     (main / obd-vtec-reaction / feature/new-thing / no macro -> unknown);
     remove the badge mapping cases and the badge contract checks.
   - Contract checks: the face badge code and `muse_build_badge` are gone
     (no `s_branch_lbl`, no badge call in `muse_ui.c`); CMake still stamps
     `MUSE_GIT_BRANCH`; Settings creates `s_build` after `s_about` and
     shows the exact branch; the menu still shows it.
6. Update docs: the `CLAUDE.md` bullet now describes the Settings home
   `Build:` label under the version/IP line (plus the button menu Status
   line), with no face badge. Append the amendment's outcome to this
   file's Results.

### Acceptance on the real board

- Settings home shows the version/IP line and directly beneath it
  `Build: main` on a main build and `Build: obd-vtec-reaction` on an OBD
  build.
- The face shows no branch badge in any mode or layout, including after a
  read reply comes and goes.

## Amendment results (Claude Code fills this in)

- One commit on `main` (not pushed).
- Face: `muse_ui.c` is back to its pre-badge text exactly (restored from
  `8e7dd15`): no `s_branch_lbl`, no `muse_build_info.h` include,
  `hides[4]`, `add_hides(&s_answers[ANSWER_READ], 2)`. Two labels plus
  the two hint icons fit in 4 again, so the earlier `hides[5]` deviation
  is gone with the third label.
- `muse_build_badge()` removed from `muse_build_info.{h,c}` (and the
  now-unused `<string.h>`). `muse_build_branch()`, the CMake stamping and
  the `.git/HEAD` configure dependency are untouched.
- Settings home: `s_build = note(list, "")` right after `s_about` in
  `build_home()`; `tick_home()` sets `s_about` to `"Muse %s - %s"`
  (version, IP or `offline`) and `s_build` to `"Build: %s"` from
  `muse_build_branch()`, both from the 128-byte buffer. The version line
  keeps the single-space `" - "` separator the spec gives (the original
  pre-task footer used `"  -  "`).
- Button menu Status page `Build %s` line and its 192-byte buffer
  unchanged.
- Host QA: `test_muse_build_info` 8/8 OK: four harness cases now check
  the branch only (main, obd-vtec-reaction, feature/new-thing, no macro ->
  unknown); contracts check the CMake stamp, that `muse_ui.c` has no
  badge/branch code and is back to `hides[4]` / count 2, that
  `muse_build_badge` is gone from the header, source and harness, that
  Settings creates `s_build` right after `s_about` with the version/IP and
  `Build: %s` lines, and that the menu still shows the branch.
- Full suite: 192 tests, 4 errors, 4 skipped. The 4 errors are the same
  pre-existing `setUpClass` failures as before (link_discovery,
  link_sensecap_sensors, muse_chat_link_errors, muse_chat_session:
  `managed_components/espressif__cjson/.../cJSON.c` and `cJSON.h` absent),
  unrelated to this change.
- No ESP-IDF build ran (no IDF on this VM); `muse_ui.c` and
  `muse_settings_ui.c` were not compiled. The real-board acceptance above
  is still to do on Derek's Mac.
