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

Fill in this section in the implementation commit or a following docs
commit:

- Pending.
