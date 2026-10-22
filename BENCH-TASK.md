# Bench triggers for the car reactions (same branch)

Branch: `obd-vtec-reaction` (work ONLY on this branch; never commit to or
push `main`; never touch `secrets/`). Derek wants to force the VTEC and
coolant reactions from the USB serial console while the gadget is plugged
into his Mac, no car or adapter involved. Both earlier Results sections
flagged this gap ("no console key to trigger it without a car").

## What to build

1. One source of truth for a fire. In `esp32/components/muse/muse_obd.c`
   the two triggers currently call `muse_state_start_*()` +
   `muse_hatch_say(LINE)` inline. Factor those pairs into two public
   functions and make the triggers call them instead:
   - `void muse_obd_fire_vtec(void);`
   - `void muse_obd_fire_coolant(void);`
   Declare them in `muse_obd.h`. The phrases stay exactly
   "VTEC just kicked in yo!" and "Coolant temperature is high!".
   A fire function must NOT touch either trigger state machine: no
   cooldown, arming or sustain side effects. It is a pure output forcer,
   and it must work with no adapter connected.

2. Console commands. The bench console lives in
   `esp32/components/muse/muse_input.c`, `console_command()`: a
   string-match chain over the line typed after ">". Add, in the same
   style as the "nap" and "status" entries:
   - line `vtec` -> `muse_obd_fire_vtec(); return true;`
   - line `coolant` -> `muse_obd_fire_coolant(); return true;`
   Gate BOTH the commands and the `muse_obd.h` include with
   `#if CONFIG_MUSE_OBD_VTEC`, so every build without the option
   compiles exactly as before. Derek will type `>vtec` and `>coolant`
   on the USB serial console (/dev/cu.usbmodem101 on his Mac).

3. Docs: mention the two commands in the "Bench testing over the USB
   cable" comment block above console_command(), and extend the
   CLAUDE.md OBD bullet. Leave VTEC-TASK.md and COOLANT-TASK.md as is.

## Host QA (NO ESP-IDF on this VM, no idf.py builds)

- `muse_obd.c` still type-checks against the real esp-nimble headers
  with no diagnostics, as in the previous runs.
- The full host test suite runs with no new failures (the 4 pre-existing
  cJSON setUpClass errors are known and unrelated).
- Self-review the muse_input.c diff line by line: match style identical
  to the neighbouring entries, both commands return true, the #if covers
  the include and the commands, and with the option off the file is
  unchanged in behaviour. muse_input.c cannot be host-compiled (LVGL);
  say so plainly in Results rather than working around it.
- Confirm by grep that both phrases in the source are unchanged.

## Results (Claude Code fills this in)

- Implemented:
  - `esp32/components/muse/muse_obd.[ch]`: public `muse_obd_fire_vtec()` and
    `muse_obd_fire_coolant()`. Each one is `muse_state_start_*()` +
    `muse_hatch_say(VTEC_LINE / COOLANT_LINE)` and nothing else: it doesn't
    touch arming, sustain or cooldown, and needs no adapter. The triggers'
    `fire()` / `fire_coolant()` keep their log lines and now call these.
    The header comment says the triggers and the bench console share them.
  - `esp32/components/muse/muse_input.c` `console_command()`: `vtec` and
    `coolant` entries right after `nap`, in its style (`!strcmp`, call,
    `return true;`). One `#if CONFIG_MUSE_OBD_VTEC` covers both, and a second
    covers the `muse_obd.h` include (beside the `CONFIG_MUSE_WATCHER_CAMERA`
    one). This matches the CMake gate, which compiles `muse_obd.c` only with
    that option, so the link is consistent either way.
  - Docs: the "Bench testing over the USB cable" comment lists `vtec` /
    `coolant`. Note: that block actually sits above `serial_task()`, just
    after `console_command()`, not above it. That's where it was edited.
    CLAUDE.md's OBD bullet gained a line about the fire functions and
    `>vtec` / `>coolant`. VTEC-TASK.md and COOLANT-TASK.md are untouched.
- Host QA (this VM, no ESP-IDF):
  1. `muse_obd.c` type-check: `cc -fsyntax-only -Wall -Wextra -Wformat=2
     -Wmissing-prototypes -Wstrict-prototypes` against the real esp-nimble
     headers (espressif/esp-nimble `nimble-1.5.0-idf`, shallow clone).
     FreeRTOS, esp_log (as printf), esp_err, esp_heap_caps, esp_timer and
     `os_mempool.h` were stubbed, plus one syscfg value
     (`MYNEWT_VAL_BLE_GATT_CSFC_SIZE`). Result: **no diagnostics in
     muse_obd.c**. The only output is from the headers (glibc `sys/queue.h`
     vs NimBLE's `CIRCLEQ_*` redefinitions and an unused parameter in
     `transport/monitor.h`). `muse_obd.h` also compiles on its own. Mutation
     check: with the two declarations removed from the header,
     `-Wmissing-prototypes` flags both definitions, so the header really
     declares what the .c defines. The clone and stubs were deleted after.
  2. Full host suite `python3 -m unittest discover -s tests -p 'test_*.py'`:
     190 ran, 4 skipped, 4 errors. That's the same as the coolant run. The
     4 errors are the known setUpClass failures (link_discovery,
     link_sensecap_sensors, muse_chat_link_errors, muse_chat_session): no
     `managed_components/espressif__cjson`. No new failures.
  3. `muse_input.c` **was not compiled**. It needs LVGL and the rest of the
     IDF, and per the spec I didn't work around that. Line-by-line review of
     its diff:
     - Both entries copy `nap`'s form exactly (`if (!strcmp(line, "..."))
       {`, one call, `return true;`, 4-space indent).
     - Both return true, so `>vtec` / `>coolant` never fall through to
       `muse_ble_command()`.
     - They sit before the `chat` prefix check, which returns false for
       non-chat lines, so they're reachable.
     - The `#if` wraps exactly the include and the two entries.
       `sdkconfig.h` is included above it.
     - With the option off, the only other change is in a comment. By
       inspection, not by running the preprocessor, the compiled code and
       behaviour are as before.
  4. Phrases, by grep: `muse_obd.c:74 #define VTEC_LINE "VTEC just kicked in
     yo!"` and `:75 #define COOLANT_LINE "Coolant temperature is high!"`,
     unchanged and used only by the fire functions. The Kconfig help and
     `muse_obd.h` text quote them identically.
- Commits on obd-vtec-reaction:
  - 41a262d OBD: muse_obd_fire_vtec/_coolant as the one place a fire happens
  - 0e82e2e Console: ">vtec" and ">coolant" force the car reactions on the bench
  - the commit adding these Results and the CLAUDE.md line
- Unverified until Mac build:
  - The s18 build with the overlay, and that the other boards build
    unchanged. `muse_input.c` hasn't been compiled at all.
  - On the device: `>vtec` / `>coolant` on /dev/cu.usbmodem101 play the
    reactions and speak the lines. Expect the same limits as a real fire:
    - The animation is idle-only. With the screen asleep it pokes but
      doesn't wake the screen.
    - `muse_hatch_say()` drops the line if the session is busy or offline.
      Typing `>coolant` right after `>vtec` may lose the second line.
  - Calling the fire functions from the serial task rather than the OBD
    task. Both calls are already made off the UI task (state is
    esp_timer-based; `muse_hatch_say` queues to the session), so this should
    be fine, but it hasn't run.
  - Very early in boot: the fire functions don't depend on
    `muse_obd_start()`. But before the voice session exists,
    `muse_hatch_say` drops the line (`s_cmds` is null), so only the
    animation would play.
