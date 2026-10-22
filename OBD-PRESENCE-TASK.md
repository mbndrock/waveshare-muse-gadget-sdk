# OBD-PRESENCE-TASK: cards only in the car, helmet while connected, Settings kill switch (branch obd-vtec-reaction)

Owner: Derek. Spec by Muse, implementation by Claude Code, QA by Muse, flash by Derek on the Mac.
Builds on OBD-CARDS-TASK (tip 2b62428, which includes the ESP-IDF format-truncation fix). Three changes, all gated on `CONFIG_MUSE_OBD_VTEC` as usual. Derek's words: the cards should only show up when the gadget is connected to his OBD adapter, otherwise it should look like the normal gadget (face + Settings); the little Muse guy should wear a race car helmet while connected; and Settings needs a global option to completely disable the OBD features and connection.

## Part 1: cards exist only while the OBD link is up

Today the three card tiles (Temps, Engine, Drive) are permanent tiles between the face and Settings. Change to:

- The tileview holds exactly two tiles (face, Settings) whenever the OBD link is not connected, exactly like a non-OBD build. Page dots match the tile count, as they do today for two tiles.
- "Connected" means the link snapshot (`muse_obd_status()`) reports subscribed or polling (the same condition that lights the round-2 status-bar indicator at full accent). While scanning/connecting/backoff, no cards.
- When the link becomes connected, insert the three card tiles between the face and Settings. If the user is on the face, stay on the face. If the user is in Settings (or a Settings subpage), do not yank the view; the Settings tile they are looking at must remain the visible one (track tiles by object, not index).
- When the link drops, remove the card tiles. If the user is currently on a card, land on the face tile. If they are in Settings, stay in Settings.
- No flicker machinery beyond that: connect/disconnect are deliberate events in the OBD task, not per-frame states. Apply tile changes on the LVGL task like every other UI mutation in muse_ui.c.
- The cards' contents and states from OBD-CARDS-TASK are unchanged. `>card <n>` clamps/no-ops gracefully when the cards are absent (jumping to face or Settings still works).
- The status-bar OBD indicator behavior is unchanged.

Implementation latitude: LVGL tileview tiles are objects; add/remove them at runtime and keep the dots in sync (the dots code already derives from tile identity in muse_ui.c's refresh). If tileview fights removal, a fallback that satisfies Derek is acceptable only if the *visible* result is identical: cards unreachable by swipe and dots back to two. Note which route you took in Results.

## Part 2: helmet while connected

- The avatar wears a racing helmet whenever the same "connected" condition from Part 1 holds, in the normal face views (idle and the usual modes/reactions). Not in Settings, not when disconnected, not when OBD is disabled (Part 3).
- Implement it as an overlay in the pixel avatar renderer, driven by a flag the UI layer sets from the OBD snapshot (follow how the existing accessories/poses are plumbed; the thinking pose's headphones are the closest precedent). The flag must be a no-op in non-OBD builds.
- Design: full racing helmet drawn over the crown of the head, visor RAISED so the eyes and mouth stay visible and reactions (VTEC grin, coolant worry, warm-up thumbs-up framing) still read. Shell in a light color with an accent stripe matching the UI accent; a chin-bar hint at the lower sides of the head is welcome if it does not cover the mouth. Keep the renderer's pixel style and palette discipline.
- The helmet must not break the existing pose set or the avatar host tests. Render a host preview of at least the idle pose with the helmet (use the existing avatar host tooling the earlier pose work used) and record the preview path in Results; Muse will inspect it during QA.

## Part 3: Settings kill switch

- Settings gains an "OBD" on/off switch in OBD builds only, in the same page style as the existing toggles (the All-messages pushes switch is the precedent). Persist it in NVS next to the other Muse settings (own key, e.g. `obd_on`), default ON so behavior after flashing is unchanged until Derek turns it off.
- OFF means completely off:
  - The OBD task does not scan, advertise-hunt, or connect; if a link is up when the switch flips, drop it promptly.
  - `obd.read` state reports "off" (the state string already exists), readings null.
  - No status-bar indicator, no cards (Part 1 sees "not connected" forever), no reactions, no helmet.
  - The firmware makes no OBD BLE connection attempts at all, so the adapter is free for a phone app.
- ON (including flipping it back at runtime) resumes normal scanning without a reboot.
- Runtime toggling must be safe while the OBD task is mid-scan or mid-connection: an enable flag the task checks at its existing decision points, plus a disconnect request on disable, is the expected shape. No task restart races, no leaked BLE connection.
- Expose the enabled state in the snapshot/`obd.read` (e.g. `"enabled": false`) so Muse can see it from the Link side.

## Tests (host, from esp32/: `python3 -m unittest discover -s tests -p "test_*.py"`)

- Contract tests in the style of test_link_obd.py: card tile creation/removal is driven by the link snapshot (not unconditional at UI init); the helmet flag is set from the same condition and is inert without CONFIG_MUSE_OBD_VTEC; the Settings switch exists only in OBD builds, persists via NVS, and gates scanning in muse_obd.c; "off" state is reachable in obd.read.
- Existing suites (ELM harness, triggers, link contracts) pass unchanged. Full suite: no new failures beyond the 6 known cJSON-environment errors.
- The Mac idf.py build is the real toolchain gate (the VM has no ESP-IDF). Keep new format strings provably sized: the 2b62428 format-truncation fix is the cautionary tale, so no `%02X`-style specifiers into buffers GCC cannot prove large enough, anywhere in the new code.

## Out of scope

No changes to what the cards show, the poll schedule, trigger thresholds, or the support scan. No persisting of scan results to flash (still parked). No helmet in non-OBD contexts (calls, thinking pose is fine to include if the plumbing makes it natural, but idle + reactions are the requirement).

## Results (Claude Code fills this in)

- Commits, files changed, test counts, the tile add/remove route taken, the helmet preview path, and any deviation with its reason.
