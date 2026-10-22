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

"""Stops a commit or push that would publish the build's secrets.

    tools/muse/check_secrets.py --staged          what's staged (pre-commit)
    tools/muse/check_secrets.py --commits A..B    commits in a range (pre-push)

.githooks/ runs it; turn the hooks on once per clone with
`git config core.hooksPath .githooks`. It looks at added lines for:

  - the real SDK token and ElevenLabs key from secrets/ (or the environment,
    as tools/muse/secrets.py reads them), whole or any 12-byte piece of them;
  - anything shaped like an SDK token (mgst_...) or an ElevenLabs key (sk_...),
    except the fake token in the pairing handshake test and upstream's own
    made-up test tokens (UPSTREAM_FAKES);
  - files that hold secrets: secrets/ other than its README and templates,
    and build-*/ (each build's sdkconfig carries them).

Found values are never printed, only where they are. Exit status 1 stops git.
"""

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SECRETS = (
    # (environment variable, file in secrets/): the ones that must stay private
    ("MUSE_SDK_TOKEN", "muse_sdk_token"),
    ("ELEVENLABS_API_KEY", "elevenlabs_api_key"),
)
PIECE = 12
SHAPES = re.compile(r"mgst_[A-Za-z0-9]{12,}|sk_[a-f0-9]{24,}")
FAKE = ("esp32/tests/link_pairing_handshake_harness.c", re.compile(r"mgst_A+\b"))
# Made-up tokens in upstream's tests, by file: the SHA-256 of each exact value,
# so this file holds nothing token-shaped. Any other token in them still counts.
UPSTREAM_FAKES = {
    "esp32/tests/link_identity_harness.c": {"11a790b00d395395a8f1d05d460ce5fed17c10816b3e02df5180dd8f996043cc"},
    "linux/tests/test_pair.py": {"582cddc6be3ee45420d084f1c56be858d83e50f616efdeda95bdd9021b18665d"},
    "linux/tests/test_service.py": {
        "8c21c6f50d835c2355763f65002be1d9f7f44a2865f6c50f45ecc4aac8f65f60",
        "a516de27e61566e7396410e5e30673e941757f194638f1166b15bd558695aeed",
    },
}
SECRET_PATH = re.compile(r"(^|/)secrets/(?!README\.md$)(?!.*\.example$)[^/]+$|(^|/)build-[^/]*/")


def real_values():
    folder = Path(os.environ.get("MUSE_SECRETS_DIR", ROOT / "secrets"))
    out = []
    for env, name in SECRETS:
        v = os.environ.get(env, "").strip()
        if not v and (folder / name).is_file():
            v = (folder / name).read_text().strip()
        if len(v) >= PIECE:
            out.append((name, v))
    return out


def pieces(value):
    return {value[i:i + PIECE] for i in range(len(value) - PIECE + 1)}


def added_lines(diff):
    """(path, line number, text) for each added line of a unified diff."""
    path, line = None, 0
    for raw in diff.splitlines():
        if raw.startswith("+++ "):
            path = raw[6:] if raw.startswith("+++ b/") else None
        elif raw.startswith("@@"):
            m = re.search(r"\+(\d+)", raw)
            line = int(m.group(1)) if m else 0
        elif raw.startswith("+") and path:
            yield path, line, raw[1:]
            line += 1
        elif not raw.startswith("-") and path:
            line += 1


def findings(diff, paths):
    values = [(name, pieces(v)) for name, v in real_values()]
    out = [f"{p}: holds secrets (secrets/ or a build folder)" for p in paths if SECRET_PATH.search(p)]
    for path, line, text in added_lines(diff):
        for name, ps in values:
            if any(p in text for p in ps):
                out.append(f"{path}:{line}: contains your {name} (or part of it)")
        for m in SHAPES.finditer(text):
            if path == FAKE[0] and FAKE[1].fullmatch(m.group(0)):
                continue
            if hashlib.sha256(m.group(0).encode()).hexdigest() in UPSTREAM_FAKES.get(path, ()):
                continue
            out.append(f"{path}:{line}: looks like {'an SDK token' if m.group(0).startswith('mgst_') else 'an API key'}")
    return out


def git(*args):
    r = subprocess.run(["git", *args], capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit(f"check_secrets: git {' '.join(args)} failed: {r.stderr.strip()}")
    return r.stdout


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--staged":
        diff = git("diff", "--cached", "--no-color", "-U0", "--no-ext-diff")
        paths = git("diff", "--cached", "--name-only", "--diff-filter=ACMR").split("\n")
        what = "this commit"
    elif len(sys.argv) == 3 and sys.argv[1] == "--commits":
        rng = sys.argv[2]
        diff = git("log", "-p", "--no-color", "-U0", "--no-ext-diff", "--format=", *rng.split())
        paths = git("log", "--name-only", "--diff-filter=ACMR", "--format=", *rng.split()).split("\n")
        what = "these commits"
    else:
        sys.exit(__doc__)
    found = sorted(set(findings(diff, [p for p in paths if p])))
    if found:
        print(f"check_secrets: {what} would publish secrets:", file=sys.stderr)
        for f in found:
            print("  " + f, file=sys.stderr)
        print("Remove them (and rotate any that were real). To override a false alarm: --no-verify.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
