# Experiment: bigger spoken-reply pieces

Derek asked to try bigger TTS pieces after noticing that long replies can
make the gadget voice slow down or drift. The current speech path cuts a
still-arriving reply after the last sentence end, with a 30-byte minimum for
the first piece and effectively a 1-byte minimum after that, so later pieces
are often single sentences. Each piece is a separate ElevenLabs request with
up to 400 bytes of `previous_text`.

## Branch and scope

- Work on `speech-bigger-pieces`, based on `upstream-merge-2026-10`
  (`2516df2`). Do not modify or push `main`, `upstream-merge-2026-10`, or
  `obd-vtec-reaction`. Do not touch `secrets/`.
- This is an experiment, not a permanent product decision. Keep the change
  narrow and easy to revert as one commit after this spec commit.
- Do not change the ElevenLabs URL, model, voice handling, `previous_text`
  limit, MP3 format, playback, timeouts, or the first-piece threshold.

## Required behaviour

1. Keep the first spoken piece at the existing 30-byte minimum
   (`SPEECH_FIRST_MIN`) so speech still starts quickly.
2. After the first piece, use a 240-byte minimum target before a sentence
   end can close the next piece. At the firmware's planning rate of roughly
   14 characters per second, 240 bytes is about 17 seconds of speech.
3. The no-sentence fallback must not undercut the target. When the caller
   asks for a minimum larger than `MUSE_HATCH_SPEECH_RUN`, the run length
   that permits a cut at the last space must rise to that minimum. Ordinary
   callers using the existing small minimum keep the current 160-byte run.
4. Once a reply is fully arrived (`done`), the remaining text still goes as
   one piece, as today.
5. Preserve the existing sentence-end rules exactly: a full stop needs the
   following space or newline, so decimals and abbreviations stay whole;
   newline and CJK sentence punctuation still work; every byte is spoken
   once and in order.

## Implementation notes

- The production session call is `speech_cut()` in
  `esp32/components/muse/muse_chat_session.cpp`, which currently passes
  `m.spoken ? 1 : SPEECH_FIRST_MIN` to `muse_hatch_speech_cut()`.
- The cutting function is `muse_hatch_speech_cut()` in
  `esp32/components/muse/muse_chat_text.c`, documented in
  `esp32/components/muse/muse_chat_priv.h`.
- Prefer a named constant for the 240-byte later-piece minimum and update
  comments/docs only where needed to describe the behaviour accurately.
- The host harness in `esp32/tests/muse_serial_chat_harness.c` currently
  hardcodes 1 as the later-piece minimum in its `speech` mode. Extend it in
  a backwards-compatible way so tests can exercise the production later
  minimum without changing the meaning of existing invocations.

## Tests

- Update or add cases in `esp32/tests/test_muse_speech_pieces.py` proving:
  - with the production first/later minimums, a multi-sentence reply that
    arrives incrementally is grouped into materially larger later pieces
    than the current sentence-at-a-time behaviour;
  - a later piece does not close at a sentence end before 240 bytes have
    accumulated, unless the reply is done;
  - a long run without sentence punctuation does not get cut at 160 bytes
    when the later minimum is 240; it waits and then cuts at a space;
  - all existing guarantees still hold: no lost or duplicated bytes,
    decimals and abbreviations are not split, short replies wait for the
    end, and text arriving in one completed message remains one piece.
- Run at least:
  - `python3 -m unittest tests.test_muse_speech_pieces` from `esp32/`
    (use the repo's documented discovery form if the module form does not
    fit its layout);
  - the full host suite from `esp32/`:
    `python3 -m unittest discover -s tests -p 'test_*.py'`.
  Report the exact totals and classify errors. The known baseline on this
  branch is 636 tests, 6 errors caused by missing cJSON in
  `managed_components/`, 0 failures, 10 skipped. Do not install packages or
  download cJSON for this task.
- There is no ESP-IDF on this VM. Do not claim a board build. A host C
  syntax/compile check through the existing tests is enough here; the Mac
  build is the next gate.

## Results

Implemented 2026-10-21 on `speech-bigger-pieces`, one commit after the spec.

- **Session:** `SPEECH_LATER_MIN 240` next to `SPEECH_FIRST_MIN 30` in
  `muse_chat_session.cpp`; `speech_cut()` passes
  `m.spoken ? SPEECH_LATER_MIN : SPEECH_FIRST_MIN`. Nothing else in the TTS
  path changed (URL, model, voice, `previous_text`, format, playback,
  timeouts).
- **Cut (`muse_chat_text.c`):** sentence-end rules untouched. The space
  fallback for a caller `min` > `MUSE_HATCH_SPEECH_RUN` now needs at least
  `min` bytes in all **and** a 160-byte run since the last sentence end (any
  sentence end, even one short of `min`). Callers with `min` <= 160 take the
  same branch as before, byte for byte.
  - Why the second condition: the plain "run rises to `min`" version cut
    ordinary prose mid-sentence at a space. Once 240 bytes were in and the
    next sentence end was still arriving, the fallback fired before that end
    showed up (a 235-byte piece cut at a space, seen in the new test). With
    the condition, prose waits for the next sentence end (pieces of 273/277
    bytes below). A run with no punctuation still cuts at the last space once
    240 bytes are in.
- **Harness:** `speech M S [L]`. The optional `L` is the later-piece minimum,
  default 1, so existing `speech M S` calls mean what they did.
- **Tests (`tests/test_muse_speech_pieces.py`):** 4 new cases with the
  production 30/240 minimums, next to the 7 existing ones (unchanged):
  - grouping into larger later pieces at steps 1/7/40;
  - no later sentence-end close before 240 bytes unless done, and a short
    remainder goes as one piece when done;
  - a no-punctuation run breaks at 160 for the first piece, then at a space
    after 236 to 240 bytes, never at 160;
  - sentence rules at steps 1/3/64 (decimals and `e.g` whole, newline and
    `。` ends, byte-exact cuts, short reply waits, one-go reply is one piece).
  - Example, 605-byte reply of 16 sentences arriving 1 byte at a time:
    before, 16 pieces `[35, 39, 39, … 40, 20]`; now, 4 pieces
    `[35, 273, 277, 20]`. No-punctuation `"word " * 200`: `[160, 240, 240,
    240, 119]`.
- **Runs (from `esp32/`):**
  - `python3 -m unittest tests.test_muse_speech_pieces`: 11 tests, OK.
  - `python3 -m unittest discover -s tests -p 'test_*.py'`: 640 tests (baseline
    636 + 4 new), 0 failures, 6 errors, 10 skipped. All 6 errors are
    `setUpClass` failing on missing `managed_components/espressif__cjson`
    (`test_link_discovery`, `test_link_reterminal_sht4x`,
    `test_link_sensecap_sensors`, `test_muse_chat_link_errors`,
    `test_muse_chat_link_note`, `test_muse_chat_session`). That's the known
    baseline.
- **Not verified here:** no ESP-IDF on this VM, so no board build. The
  `muse_chat_session.cpp` change (one `#define`, one argument) was not
  host-compiled, because `test_muse_chat_session` is one of the cJSON errors.
  `muse_chat_text.c` and the harness compile under `-Wall -Wextra -Werror`.
  Next gate: the Mac build, then listening on a board to see whether drift
  improves and how much the longer gap before the second piece costs.
