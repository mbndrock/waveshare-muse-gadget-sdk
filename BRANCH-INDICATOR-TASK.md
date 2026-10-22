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

- Pending.
