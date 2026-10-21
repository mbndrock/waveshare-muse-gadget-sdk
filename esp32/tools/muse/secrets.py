#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Puts the build's secrets into a generated sdkconfig, never into git.

    tools/muse/secrets.py BUILD_DIR/sdkconfig

Each secret comes from an environment variable or, failing that, a one-line
file in the repository's secrets/ directory (gitignored; see
secrets/README.md). A missing secret is left as it is. Values are never
printed, only which ones were set.

secrets/elevenlabs_voices is the exception: one "Name=voice_id" per line
(blank lines and # comments skipped), joined with ";" for the device. Its
environment variable takes the joined form, and without either the list is
emptied rather than left as it was.
"""

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SECRETS = (
    # (Kconfig option, environment variable, file in secrets/)
    ("CONFIG_GADGET_SDK_TOKEN", "MUSE_SDK_TOKEN", "muse_sdk_token"),
    ("CONFIG_MUSE_ELEVENLABS_API_KEY", "ELEVENLABS_API_KEY", "elevenlabs_api_key"),
    ("CONFIG_MUSE_ELEVENLABS_VOICE_ID", "ELEVENLABS_VOICE_ID", "elevenlabs_voice_id"),
    ("CONFIG_MUSE_ELEVENLABS_VOICES", "ELEVENLABS_VOICES", "elevenlabs_voices"),
)
VOICES = "elevenlabs_voices"
VOICES_MAX = 8      # MUSE_VOICES_MAX
VOICE_NAME = re.compile(r"[^;=\"\\]{1,23}")
VOICE_ID = re.compile(r"[A-Za-z0-9]{1,39}")


def voices(text):
    """The "Name=ID" lines of secrets/elevenlabs_voices as "Name=ID;Name=ID"."""
    out = []
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        name, eq, vid = (p.strip() for p in line.partition("="))
        if not eq or not VOICE_NAME.fullmatch(name) or len(name.encode()) > 23 or not VOICE_ID.fullmatch(vid):
            sys.exit(f"secrets: {VOICES} line {n} must be Name=voice_id "
                     "(name up to 23 bytes without ; = or quotes, ID letters and digits)")
        out.append(f"{name}={vid}")
    if len(out) > VOICES_MAX:
        sys.exit(f"secrets: {VOICES} has {len(out)} voices, at most {VOICES_MAX} fit")
    return ";".join(out)


def value(env, name):
    v = os.environ.get(env, "").strip()
    if not v:
        f = ROOT / "secrets" / name
        v = f.read_text().strip() if f.is_file() else ""
        if name == VOICES:
            v = voices(v)
    if '"' in v or "\n" in v:
        sys.exit(f"secrets: {name} must be one line without quotes")
    return v


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    cfg = Path(sys.argv[1])
    cfg.parent.mkdir(parents=True, exist_ok=True)
    text = cfg.read_text() if cfg.exists() else ""
    done = []
    changed = False
    for opt, env, name in SECRETS:
        v = value(env, name)
        if not v and name != VOICES:   # deleting the voices file clears the list
            continue
        line = f'{opt}="{v}"'
        pattern = re.compile(rf"^(# )?{opt}[= ].*$", re.M)
        text = pattern.sub(line, text, count=1) if pattern.search(text) else text + line + "\n"
        changed = True
        if v:
            done.append(opt)
    if changed:
        cfg.write_text(text)
        cfg.chmod(0o600)
    print("secrets: " + (", ".join(done) if done else "none found (see secrets/README.md)"))


if __name__ == "__main__":
    main()
