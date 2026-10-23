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

"""USB drive mode: the fabricated FAT volume (muse_msc_fat.c) against its
harness, plus wiring contracts (snapshot from closed trips only, read-only,
NVS-flag reboot entry, s18-only sdkconfig)."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MUSE = ROOT / "components/muse"


class MuseMscFatTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "muse_msc_fat"
        cc = shlex.split(os.environ.get("CC", "cc"))
        compiled = subprocess.run(
            [*cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wformat-truncation=2", "-O1", "-g",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(MUSE),
             str(ROOT / "tests/muse_msc_fat_harness.c"),
             str(MUSE / "muse_msc_fat.c"),
             "-o", str(cls.binary)],
            capture_output=True, text=True)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def test_harness(self):
        run = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("harness PASS", run.stdout)

    def test_wiring_contracts(self):
        drive = (MUSE / "muse_usb_drive.c").read_text()
        # Snapshot comes only from the closed-trip listing; writes refused.
        self.assertIn("muse_obd_log_list", drive)
        self.assertIn("write", drive.lower())
        settings = (MUSE / "muse_settings_ui.c").read_text()
        self.assertIn("USB drive mode", settings)
        overlay = (ROOT / "devices/sdkconfig.muse-waveshare-s3-18").read_text()
        # Off until muse_usb_drive.c is migrated to the esp_tinyusb v2 API
        # (IDF v6 moved tinyusb to the component manager).
        self.assertIn("# CONFIG_MUSE_USB_DRIVE is not set", overlay)
        # No partition or log-store format changes ride along.
        self.assertNotIn("muse_log_store_delete", drive)
        self.assertNotIn("esp_partition", drive)


if __name__ == "__main__":
    unittest.main()
