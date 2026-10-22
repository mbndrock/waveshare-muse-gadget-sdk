# Remote OBD state readout: obd.read Link command + >obd console line

Branch: `obd-vtec-reaction` (work ONLY on this branch; never commit to or
push `main`; never touch `secrets/`). Claude Code implements and commits on
this branch but does NOT push; Muse QAs the diff and pushes.

## Why

Derek is at the car with only his phone and asks Muse: "is the gadget
seeing RPM, and what is it?" Today that is unanswerable remotely:

- `esp32/components/muse/muse_obd.c` keeps the latest RPM in a private
  `s_rpm` and logs each valid reading only at `ESP_LOGD`.
- The latest coolant value is not stored at all (it is parsed in
  `got_coolant()` and fed straight to the trigger).
- The gadget's Link registration exposes only device.health,
  device.discover, device.ota, display.draw_url and display.show_animation.
- The serial `>status` JSON (board/chat/device) has no OBD section.

Goal: a synchronous, non-blocking snapshot of the OBD task's state that
Muse can fetch with a Link command, plus the same snapshot on the bench
console. After this lands and Derek flashes it, Muse answers RPM questions
by invoking `obd.read` on MuseGadget-FEA6C0.

## What to build

### 1. Snapshot API in muse_obd.h / muse_obd.c

Declare in `esp32/components/muse/muse_obd.h` (add `<stdbool.h>` and
`<stdint.h>` includes):

```c
typedef struct {
    bool started;          /* muse_obd_start() created the task */
    const char *state;     /* one of the state strings below */
    bool adapter_found;    /* the current/last scan saw the adapter */
    bool connected;        /* a BLE connection to the adapter is up */
    bool subscribed;       /* FFE1 notifications are on */
    bool polling;          /* inside poll_rpm(): ELM init done, asking PIDs */
    bool rpm_valid;        /* rpm holds a current reading */
    int rpm;               /* latest engine RPM; 0 is a real reading */
    uint32_t rpm_age_ms;   /* ms since that reading; UINT32_MAX if none */
    bool coolant_valid;    /* coolant_c holds a current reading */
    int coolant_c;         /* latest coolant temperature, degrees C */
    uint32_t coolant_age_ms; /* ms since that reading; UINT32_MAX if none */
} muse_obd_status_t;

void muse_obd_status(muse_obd_status_t *out);
```

State strings (exact): "off", "waiting", "scanning", "connecting",
"discovering", "initializing", "polling", "backoff".

Implementation requirements:

- Store the snapshot fields in muse_obd.c behind a `portMUX_TYPE` spinlock
  (the SenseCAP sensors in `esp32/main/sensecap_sensors.c` are the in-tree
  pattern). `muse_obd_status()` copies under the lock, computes ages from
  the existing `now_ms()`, and never blocks, never touches BLE, and is safe
  to call from any task, including before `muse_obd_start()`.
- Track state at the points the OBD task already reaches:
  - `muse_obd_start()` success: started, state "waiting". Failure: "off".
  - Task loop waiting for the BLE host: "waiting".
  - Before `scan()`: "scanning", and adapter_found reset for this scan.
    `scan()` succeeding sets adapter_found.
  - Before `connect()`: "connecting".
  - Before `subscribe()`: "discovering"; success sets subscribed.
  - Before `elm_init()`: "initializing".
  - Before `poll_rpm()`: "polling", polling=true; after it returns,
    polling=false.
  - After `drop()` / any failed attempt, while sleeping between attempts:
    "backoff".
- Mirror the readings at the exact points the triggers consume them:
  - In `poll_rpm()`, a valid RPM parse sets rpm_valid, the value and its
    timestamp; a non-RPM reply clears rpm_valid (keep the existing `s_rpm`
    behaviour identical; the status mirrors it).
  - In `got_coolant()`, a valid coolant parse sets coolant_valid, the value
    and its timestamp. An invalid or NO DATA coolant reply does NOT clear
    the last valid value; its age simply grows.
  - `drop()` clears connected/subscribed/polling and BOTH readings'
    validity: once the link is down there is no current reading, and Derek
    must never be told a stale RPM as if it were live.
- Do not change the triggers, thresholds, phrases, poll cadence or the ELM
  parser. Do not modify muse_chat_session.cpp or the avatar.

### 2. Link command `obd.read`

Follow `esp32/AGENTS.md` "Adding a command" exactly; `sensors.read` is the
model. The name must match in both places, both gated on the existing
`CONFIG_MUSE_OBD_VTEC` (no new Kconfig, no CMake changes expected: main
already REQUIRES muse, and muse_obd.c already compiles under that option).

- Advertise in `build_register_json()` in `esp32/main/noise_control.cpp`:

```c
#if CONFIG_MUSE_OBD_VTEC
    add_command(commands, "obd.read",
                "Report the OBD-II adapter state and the latest engine "
                "readings from the LELink^2 adapter: the link state, "
                "whether it is polling, engine RPM and coolant temperature "
                "in degrees Celsius, each null when there is no current "
                "reading, with each reading's age in milliseconds.",
                nullptr, nullptr);
#endif
```

  Keep the description about this size; link.register must stay small.

- Handle in `on_ws_command()` in `esp32/main/app.c`: include `muse_obd.h`
  under `#if CONFIG_MUSE_OBD_VTEC`, add a small static helper that builds
  the standard result from one `muse_obd_status()` call, and dispatch
  `"obd.read"` to it under the same `#if`. Synchronous return only, no
  async task: the snapshot never blocks.

Result shape (match the sensors.read convention):

```json
{"ok": true, "payload": {
  "state": "polling",
  "adapter_found": true,
  "connected": true,
  "subscribed": true,
  "polling": true,
  "rpm": 850,
  "rpm_age_ms": 120,
  "coolant_c": 91,
  "coolant_age_ms": 450
}}
```

`rpm` / `coolant_c` are JSON null when not valid, and the matching
`*_age_ms` is null then too (never leak UINT32_MAX into the JSON).

### 3. Bench console `>obd`

In `console_command()` in `esp32/components/muse/muse_input.c`, under the
same `#if CONFIG_MUSE_OBD_VTEC` (the include is already gated there), add
an `obd` entry beside `vtec` / `coolant` that prints the same snapshot as
one line and returns true:

```
@obd {"state":"polling","adapter_found":true,"connected":true,"subscribed":true,"polling":true,"rpm":850,"rpm_age_ms":120,"coolant_c":91,"coolant_age_ms":450}
```

with JSON `null` for invalid readings and ages, exactly as the Link
payload. Mention `obd` in the bench-testing comment block that already
lists `vtec` / `coolant`.

### 4. Docs

- Extend the CLAUDE.md OBD bullet: `obd.read` Link command and `>obd`
  console line report the adapter state and latest RPM/coolant.
- Fill in this file's Results section (below) when done.

## Host QA (NO ESP-IDF on this VM, no idf.py builds)

- `muse_obd.c` still type-checks against the real esp-nimble headers with
  no diagnostics in the file, as in the previous runs
  (`-Wall -Wextra -Wformat=2 -Wmissing-prototypes -Wstrict-prototypes`,
  stubs as before). `muse_obd.h` compiles on its own, and a mutation check
  (deleting the `muse_obd_status` declaration) makes
  `-Wmissing-prototypes` flag the definition.
- New contract test `esp32/tests/test_link_obd.py`, modelled on
  `test_link_sensecap_sensors.py`'s command test: the registration block
  and the dispatch block each sit under `#if CONFIG_MUSE_OBD_VTEC` and
  name `obd.read`; `muse_obd.h` declares `muse_obd_status_t` and
  `muse_obd_status`; the `obd` console entry in muse_input.c is under the
  same gate.
- Run the new test, `test_muse_obd_elm.py`, `test_link_transport_contract.py`
  and `test_muse_pixel_vtec.py` / `test_muse_pixel_coolant.py` (the avatar
  must be untouched, so these are regression cover). Then the full host
  suite: no new failures beyond the known 4 cJSON setUpClass errors
  (link_discovery, link_sensecap_sensors, muse_chat_link_errors,
  muse_chat_session; no managed_components/espressif__cjson on this VM).
- `main/app.c`, `main/noise_control.cpp` and `muse_input.c` cannot be
  host-compiled here; review their diffs line by line instead and say so
  plainly in Results: gate placement, exact name match in both places,
  null handling, and that with the option off all three files behave
  exactly as before.
- Grep to confirm: the VTEC and coolant phrases and both trigger
  thresholds are unchanged, and `obd.read` appears in exactly the
  registration, the dispatch and the docs.

## Commits

Separate logical commits on obd-vtec-reaction (snapshot API; Link + console
plumbing; tests/docs/results). Do NOT push.

## Acceptance on the real board (after Derek's Mac build + flash)

- The gadget's command list gains `obd.read`.
- No adapter nearby: `obd.read` answers state "scanning" or "backoff",
  connected false, rpm and coolant_c null.
- Adapter in the car, ignition on, engine idling: state "polling",
  connected/subscribed/polling true, rpm roughly 700-900 and fresh
  (rpm_age_ms small), coolant_c plausible and fresh.
- `>obd` on the USB console prints the same snapshot.

## Results (Claude Code fills this in)

- Implemented:
  - `esp32/components/muse/muse_obd.[ch]`:
    - `muse_obd_status_t` and `muse_obd_status()` are declared exactly as
      specified. The header now includes `<stdbool.h>` / `<stdint.h>` and
      lists the state strings in a comment.
    - The snapshot is a static `muse_obd_status_t s_status` (starts as
      "off"), plus the two readings' `now_ms()` timestamps. All of it sits
      behind `portMUX_TYPE s_status_lock`, the `sensecap_sensors.c` pattern.
    - Small setters each take the lock once: `set_state`, `set_flag`,
      `set_rpm`, `set_coolant` and `set_link_down`.
    - `muse_obd_status()` copies under the lock and computes ages
      (`now - at`, wrap-safe) outside it. Ages are `UINT32_MAX` whenever the
      reading isn't valid. It ignores a NULL `out`, and it never blocks or
      touches BLE, so it works before `muse_obd_start()` too ("off", all
      false).
  - State tracking:
    - `muse_obd_start()` sets started / "waiting" *before* it creates the
      task, so the task can't run ahead and get overwritten. If the queue,
      stream buffer or task creation fails, it goes back to started=false /
      "off".
    - Each loop: "waiting" while it waits for the BLE host, then
      "scanning" with adapter_found reset. A successful `scan()` sets
      adapter_found.
    - Then "connecting"; a successful `connect()` sets connected. Then
      "discovering"; a successful `subscribe()` sets subscribed. Then
      "initializing" around `elm_init()`.
    - Then "polling" with polling=true around `poll_rpm()`, and
      polling=false after it returns.
    - After `drop()` or any failed step: "backoff" for the sleep.
    - The loop's `&&` chain became nested `if`s so each state is set just
      before its step. The control flow is the same: `drop()` still runs
      exactly when `connect()` succeeded.
  - Readings:
    - `poll_rpm()` calls `set_rpm(locked, rpm)` right after the existing
      `s_rpm` line. A valid parse stores the value and time. A non-RPM
      reply clears rpm_valid, which mirrors `s_rpm = -1`. `s_rpm` itself is
      untouched.
    - `got_coolant()` calls `set_coolant()` only after a valid parse.
      NO DATA or an invalid reply returns early as before, so the last value
      stays and its age keeps growing.
    - `drop()` ends with `set_link_down()`, which clears connected,
      subscribed, polling, rpm_valid and coolant_valid.
    - Choice: a disconnect noticed inside `wait_for()` doesn't clear
      `connected` there. `drop()` follows right away, because every step
      fails fast with `s_conn` gone, so the window is short.
  - Link (`esp32/main/noise_control.cpp`, `esp32/main/app.c`):
    - `obd.read` is advertised in `build_register_json()` after
      `sensors.read`, with the spec's description verbatim, under
      `#if CONFIG_MUSE_OBD_VTEC`.
    - `app.c` includes `muse_obd.h` under the same `#if`, beside the
      sensecap include.
    - A gated `obd_read_command()` (plus an `add_obd_reading()` helper)
      builds `{"ok":true,"payload":{...}}` from one `muse_obd_status()`
      call. `rpm` / `coolant_c` and their `*_age_ms` are all JSON null when
      the reading isn't valid, so UINT32_MAX never reaches the JSON.
    - If an allocation fails it returns `command_error("out_of_memory", ...)`,
      never NULL. It answers synchronously, with no task.
    - Dispatch is in `on_ws_command()` right after `sensors.read`, under the
      same `#if`.
  - Console (`muse_input.c`):
    - A gated `print_obd()` prints the spec's `@obd {...}` line, using
      `obd_number()` for number-or-null, then flushes.
    - An `obd` entry follows `vtec` / `coolant` inside their existing
      `#if`, in the same form (`!strcmp`, call, `return true;`).
    - The bench-testing comment above `serial_task()` mentions `"obd"`.
  - CLAUDE.md: the OBD bullet has a new "Readout" line.
- Host QA (this VM, no ESP-IDF):
  1. `muse_obd.c` type-check:
     - Command: `cc -std=gnu11 -fsyntax-only -Wall -Wextra -Wformat=2
       -Wmissing-prototypes -Wstrict-prototypes`, against the real NimBLE
       headers (espressif/esp-nimble `nimble-1.5.0-idf`, shallow clone).
     - Stubs, as in the previous runs: FreeRTOS (incl. `portMUX_TYPE` and
       `taskENTER/EXIT_CRITICAL`), esp_log (as printf), esp_err,
       esp_heap_caps, esp_timer, `os/os_mempool.h`, and
       `MYNEWT_VAL_BLE_GATT_CSFC_SIZE`.
     - Result: **no diagnostics in muse_obd.c**. The only output comes from
       the headers: glibc vs NimBLE `CIRCLEQ_*` redefinitions, and an unused
       parameter in `transport/monitor.h`.
     - `muse_obd.h` compiles on its own with the same flags: no output.
     - Mutation check: with the `muse_obd_status` declaration deleted from
       a copy of the header, `-Wmissing-prototypes` reports `no previous
       prototype for 'muse_obd_status'`. With the real header, 0 warnings.
       (The .c had to be copied beside the mutated header, because a quoted
       include searches the source's own directory first.)
  2. Not required by the spec, extra checks of the two output builders.
     Each was extracted verbatim from its source file into a scratch harness
     with a fake `muse_obd_status()`, built with `-Werror` and ASan/UBSan:
     - `print_obd()` prints exactly the spec's example line for the
       polling case. With no valid readings it prints `null` for all four
       fields, even with stale values in the struct. `rpm` 0 and coolant
       -40 print as real numbers. Every line parses as JSON.
     - `obd_read_command()`, run against the in-tree
       `tests/link_fakes/cJSON.c` (the harness adds `cJSON_AddNullToObject`,
       which that fake lacks), gives exactly the spec's result shape. The
       no-reading case gives nulls for all four fields.
     - These are scratch checks only, not committed tests.
  3. New `esp32/tests/test_link_obd.py` (3 tests):
     - `obd.read` appears once in `noise_control.cpp`, and its
       `add_command` sits inside a `#if CONFIG_MUSE_OBD_VTEC` block.
     - It appears once in `on_ws_command()`'s dispatch, inside a gated block
       that calls `obd_read_command()`. The `muse_obd.h` include and the
       helper are gated too.
     - `muse_obd.h` declares `muse_obd_status_t` and `muse_obd_status`.
     - The `obd` entry in `console_command()`, and `print_obd()`, are
       gated.
     - Mutation check (on a scratch copy of the tree): moving the dispatch
       out of its `#if` and the console entry out of its `#if` fails the two
       matching tests.
  4. Targeted runs: `test_link_obd`, `test_muse_obd_elm`,
     `test_link_transport_contract`, `test_muse_pixel_vtec` and
     `test_muse_pixel_coolant`: 16 tests, OK.
  5. Full host suite `python3 -m unittest discover -s tests -p 'test_*.py'`:
     193 ran (190 + the 3 new), 4 skipped, 4 errors. The errors are the
     known setUpClass failures (link_discovery, link_sensecap_sensors,
     muse_chat_link_errors, muse_chat_session), all from the missing
     `managed_components/espressif__cjson`. No new failures.
  6. `main/app.c`, `main/noise_control.cpp` and `muse_input.c` were **not
     compiled** (they need the IDF, LVGL and the rest). Line-by-line review
     of their diffs:
     - Gates: every added code line is inside `#if CONFIG_MUSE_OBD_VTEC ...
       #endif`. That's the include and the helpers in app.c, the dispatch,
       the registration, `obd_number`/`print_obd` and the console entry.
       The only change outside a gate is the comment above `serial_task()`.
       With the option off, all three files compile to the same code as
       before. This is by inspection, not by running the preprocessor.
     - Name: `"obd.read"` is identical in the registration and the
       dispatch, and the test pins both.
     - Nulls: both outputs key on `rpm_valid` / `coolant_valid` alone, for
       the value and its age together.
     - Include path: `muse` is in main's unconditional `GADGET_REQUIRES`,
       and `muse_obd.h` is in muse's public `INCLUDE_DIRS "."`.
       `muse_obd.c` is compiled exactly when `CONFIG_MUSE_OBD_VTEC` is on,
       so every reference resolves at link time. No CMake or Kconfig change.
  7. Greps:
     - The phrases are unchanged: `muse_obd.c:74 VTEC_LINE "VTEC just
       kicked in yo!"`, `:75 COOLANT_LINE "Coolant temperature is high!"`.
     - The thresholds are unchanged. `muse_obd_elm.[ch]`, `avatar/` and
       `muse_chat_session.cpp` have an empty diff against b677001.
     - As code, `obd.read` appears only in `noise_control.cpp:1382`
       (registration) and `app.c:1944` (dispatch). It also appears in
       comments in `muse_obd.h` and `muse_input.c`, in CLAUDE.md, in this
       file and in the new test.
  - Scratch: the esp-nimble clone, stubs and harnesses were in
    `~/scratch-obd`, which was deleted afterwards.
- Commits on obd-vtec-reaction (not pushed):
  - 82f27e6 OBD: muse_obd_status() snapshot of the link state and latest readings
  - d204eec Link: obd.read command and ">obd" console line for the OBD snapshot
  - the commit adding `tests/test_link_obd.py`, the CLAUDE.md line and these
    Results
- Unverified until Mac build:
  - The s18 build with the overlay (`check_sizes`), and that the other
    boards build unchanged. `app.c`, `noise_control.cpp` and `muse_input.c`
    haven't been compiled at all.
  - That `link.register` still fits. The description adds about 300 bytes
    to the 8 KB registration.
  - On the device: `obd.read` in the command list, and the acceptance cases
    above (no adapter: "scanning"/"backoff" with nulls; idling: "polling"
    with RPM about 700-900 and fresh ages), plus `>obd` printing the same
    snapshot.
  - Expectations for the car:
    - Coolant is read on every 4th request, so `coolant_age_ms` will
      normally be larger than `rpm_age_ms`.
    - After `ATZ`, while the ELM is still searching for the protocol, state
      is "polling" with rpm null until the first frame arrives.
    - With the engine off but the adapter powered, NO DATA replies make rpm
      null while state stays "polling". Coolant keeps its last value (and
      growing age) until the link drops.
