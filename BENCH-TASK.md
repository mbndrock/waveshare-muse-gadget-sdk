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
- Host QA:
- Commits on obd-vtec-reaction:
- Unverified until Mac build:
