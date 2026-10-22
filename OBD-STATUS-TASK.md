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
- Host QA:
- Commits:
- Unverified until Mac build:
