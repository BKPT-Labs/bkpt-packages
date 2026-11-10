# SPDX-License-Identifier: LicenseRef-BKPT-Proprietary
"""Release regressions for the public firmware component and its generator."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from tools import sync_recorder as sync


# Independent consumer requirements: especially the nested RTX exception port.
REQUIRED_FILES = (
    "core/ViewAlyzer.c", "core/ViewAlyzer.h", "core/ViewAlyzerConfig.h",
    "core/viewalyzer_cobs.c", "core/viewalyzer_cobs.h",
    "freertos/VA_Adapter_FreeRTOS.c", "freertos/ViewAlyzerFreeRTOSHook.h",
    "zephyr/VA_Adapter_Zephyr.c", "zephyr/CMakeLists.txt", "zephyr/Kconfig", "zephyr/module.yml",
    "threadx/VA_Adapter_ThreadX.c", "threadx/VA_Adapter_ThreadX.h",
    "threadx/VA_ThreadXConfig.h", "threadx/ViewAlyzerThreadXHook.h", "threadx/tx_api_extension.h",
    "rtx5/VA_Adapter_RTX5.c", "rtx5/VA_RTX5Config.h", "rtx5/ViewAlyzerRTX5Hook.h",
    "rtx5/ports/irq_armv7m.S",
)


def snapshot(root):
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob("*") if p.is_file()}


class RecorderSyncTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="bkpt-recorder-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "packages"
        self.package = self.repo / "viewalyzer-recorder"
        self.package.mkdir(parents=True)
        self.source = self.root / "source"
        self.recorder = self.source / "ViewAlyzerRecorder"
        for relative in REQUIRED_FILES:
            path = self.recorder / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"// source\r\n")
        (self.recorder / "core/ViewAlyzer.h").write_text(
            "#define VA_RECORDER_VERSION_MAJOR 1\n#define VA_RECORDER_VERSION_MINOR 4\n"
            "#define VA_RECORDER_VERSION_PATCH 0\n#define VA_WIRE_VERSION 2\n", encoding="utf-8")
        (self.recorder / "core/ViewAlyzerConfig.h").write_text(
            "#define VA_RTOS_NONE 0\n#define VA_RTOS_FREERTOS 1\n#define VA_RTOS_ZEPHYR 2\n"
            "#define VA_RTOS_THREADX 3\n#define VA_RTOS_RTX5 4\n", encoding="utf-8")
        (self.package / "template.h").write_bytes(b"preserved template\r\n")
        (self.package / "manifest.json").write_text(
            json.dumps({"package": {"files": ["template.h"]}}), encoding="utf-8")
        (self.repo / "index.json").write_text(json.dumps({"packages": [
            {"id": "other", "rev": "9"}, {"id": "viewalyzer-recorder", "rev": "17"}]}), encoding="utf-8")
        for name, value in (("REPO_ROOT", self.repo), ("PKG_DIR", self.package),
                            ("RECORDER_DEST", self.package / "recorder")):
            patch = mock.patch.object(sync, name, value)
            patch.start()
            self.addCleanup(patch.stop)
        patch = mock.patch.object(sync, "_git_rev", return_value="a" * 40)
        patch.start()
        self.addCleanup(patch.stop)

    def run_sync(self, *arguments):
        with mock.patch("sys.argv", ["sync_recorder.py", "--source", str(self.source), *arguments]), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return sync.main()

    def test_all_adapters_nested_assembly_and_templates_ship(self):
        self.assertEqual(self.run_sync(), 0)
        manifest = json.loads((self.package / "manifest.json").read_text())
        self.assertTrue({"recorder/" + p for p in REQUIRED_FILES} <= set(manifest["package"]["files"]))
        for relative in REQUIRED_FILES:
            self.assertEqual((self.package / "recorder" / relative).read_bytes(),
                             (self.recorder / relative).read_bytes().replace(b"\r\n", b"\n"))
        self.assertEqual((self.package / "template.h").read_bytes(), b"preserved template\r\n")
        self.assertEqual(manifest["rev"], "18")
        self.assertEqual(json.loads((self.repo / "index.json").read_text())["packages"][0],
                         {"id": "other", "rev": "9"})
        before = snapshot(self.repo)
        self.assertEqual(self.run_sync("--check"), 0)
        self.assertEqual(self.run_sync(), 0)
        self.assertEqual(snapshot(self.repo), before)

    def test_check_detects_missing_modified_extra_payload_and_stale_metadata_without_writes(self):
        for damage in ("missing", "modified", "extra", "manifest", "index"):
            with self.subTest(damage=damage):
                self.assertEqual(self.run_sync(), 0)
                path = self.package / "recorder/rtx5/ports/irq_armv7m.S"
                if damage == "missing":
                    path.unlink()
                elif damage == "modified":
                    path.write_bytes(b"broken assembly")
                elif damage == "extra":
                    (path.parent / "old.S").write_bytes(b"stale")
                elif damage == "manifest":
                    path = self.package / "manifest.json"
                    data = json.loads(path.read_text())
                    data["provenance"]["source_rev"] = "b" * 40
                    path.write_text(json.dumps(data), encoding="utf-8")
                else:
                    path = self.repo / "index.json"
                    data = json.loads(path.read_text())
                    data["packages"][1]["hash"] = "stale"
                    path.write_text(json.dumps(data), encoding="utf-8")
                before = snapshot(self.repo)
                self.assertNotEqual(self.run_sync("--check"), 0)
                self.assertEqual(snapshot(self.repo), before)

    def test_future_adapter_is_not_silently_omitted(self):
        self.assertEqual(self.run_sync(), 0)
        new_port = self.recorder / "future_rtos"
        new_port.mkdir()
        (new_port / "adapter.c").write_bytes(b"future source")
        before = snapshot(self.repo)
        self.assertNotEqual(self.run_sync(), 0)
        self.assertEqual(snapshot(self.repo), before)

    def test_missing_preserved_file_and_escaping_paths_fail_without_writes(self):
        self.assertEqual(self.run_sync(), 0)
        (self.package / "template.h").unlink()
        before = snapshot(self.repo)
        self.assertNotEqual(self.run_sync("--check"), 0)
        self.assertNotEqual(self.run_sync(), 0)
        self.assertEqual(snapshot(self.repo), before)
        for relative in ("../outside.h", "..\\outside.h", "/absolute.h", "C:/outside.h"):
            with self.subTest(relative=relative):
                manifest_path = self.package / "manifest.json"
                manifest = json.loads(manifest_path.read_text())
                manifest["package"]["files"] = [relative]
                manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
                before = snapshot(self.repo)
                self.assertNotEqual(self.run_sync("--check"), 0)
                self.assertNotEqual(self.run_sync(), 0)
                self.assertEqual(snapshot(self.repo), before)

    def test_missing_advertised_adapter_and_bad_header_leave_package_intact(self):
        self.assertEqual(self.run_sync(), 0)
        before = snapshot(self.repo)
        (self.recorder / "threadx/VA_Adapter_ThreadX.c").unlink()
        self.assertNotEqual(self.run_sync(), 0)
        self.assertEqual(snapshot(self.repo), before)
        (self.recorder / "core/ViewAlyzer.h").write_bytes(b"invalid")
        self.assertNotEqual(self.run_sync(), 0)
        self.assertEqual(snapshot(self.repo), before)


class PublishedRecorderTests(unittest.TestCase):
    def test_published_inventory_hash_and_registry_are_consistent(self):
        package = sync.PKG_DIR
        manifest = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        files = manifest["package"]["files"]
        payload = [name for name in files if name.startswith("recorder/")]
        self.assertEqual(len(files), len(set(files)))
        self.assertTrue({"recorder/" + p for p in REQUIRED_FILES} <= set(payload))
        self.assertEqual(payload, sorted(payload))
        actual = {"recorder/" + p.relative_to(package / "recorder").as_posix()
                  for p in (package / "recorder").rglob("*") if p.is_file()}
        self.assertEqual(actual, set(payload))
        for name in files:
            self.assertTrue((package / name).is_file(), name)
        digest = hashlib.sha256()
        for name in payload:
            digest.update(name.encode() + b"\x00" + (package / name).read_bytes())
        expected = "sha256:" + digest.hexdigest()
        rows = [r for r in json.loads((sync.REPO_ROOT / "index.json").read_text())["packages"]
                if r["id"] == "viewalyzer-recorder"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["hash"], expected)
        self.assertEqual(manifest["provenance"]["content_hash"], expected)
        self.assertEqual(rows[0]["rev"], manifest["rev"])
        self.assertRegex(manifest["provenance"]["source_rev"], r"^[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
