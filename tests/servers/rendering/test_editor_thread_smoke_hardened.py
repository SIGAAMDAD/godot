"""Offline test oracle for T11 GUI capture checking."""

import csv
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from editor_thread_smoke import run, validate_capture


class CaptureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name)
        self.project = self.path / "project"
        self.project.mkdir()
        (self.project / "project.godot").write_text("config_version=5\n", encoding="utf-8")

    def capture(self, path, mode="separate", effective=None, count=120, backend="vulkan", errors=""):
        with path.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(
                output, fieldnames=["requested_mode", "effective_mode", "frame_id", "backend", "errors"]
            )
            writer.writeheader()
            for index in range(count):
                writer.writerow({
                    "requested_mode": mode,
                    "effective_mode": effective or mode,
                    "frame_id": str(index),
                    "backend": backend,
                    "errors": errors,
                })

    def fake_run(self, mode, *, effective=None, count=120, stderr="", emit_capture=True, exit_code=0):
        def process_run(command, **kwargs):
            assert kwargs["env"]["GODOT_BENCH_WORKLOAD_ID"]
            assert ("--editor-render-thread-experimental" in command) == (mode == "separate")
            if emit_capture:
                capture = pathlib.Path(command[command.index("--editor-render-thread-benchmark") + 1])
                self.capture(capture, mode=mode, effective=effective, count=count)
            return subprocess.CompletedProcess(command, exit_code, "", stderr)

        with patch("editor_thread_smoke.subprocess.run", side_effect=process_run):
            return run("godot.exe", self.project, mode, 10, self.path)

    def test_control_modes(self):
        for mode in ("safe", "separate"):
            with self.subTest(mode=mode):
                result = self.fake_run(mode)
                self.assertEqual(result["status"], "EXITED")
                self.assertEqual(result["verified_frames"], 120)

    def test_fallback_rejected(self):
        result = self.fake_run("separate", effective="safe")
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("fell back", result["reason"])

    def test_short_capture_rejected(self):
        result = self.fake_run("separate", count=1)
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("only 1", result["reason"])

    def test_missing_capture_rejected(self):
        result = self.fake_run("separate", emit_capture=False)
        self.assertEqual(result["status"], "FAILED")

    def test_vulkan_error_rejected(self):
        result = self.fake_run("separate", stderr="Validation Error: [ VUID-vkQueueSubmit ]")
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("Vulkan validation", result["reason"])

    def test_nonzero_exit_rejected(self):
        result = self.fake_run("separate", exit_code=1)
        self.assertEqual(result["status"], "FAILED")

    def test_malformed_frame_rejected(self):
        self.capture(self.path / "separate.csv")
        raw = (self.path / "separate.csv").read_text(encoding="utf-8")
        (self.path / "separate.csv").write_text(raw.replace(",vulkan,", ",,"), encoding="utf-8")
        self.assertIn("non-Vulkan", validate_capture(self.path / "separate.csv", "separate"))

    def test_non_monotonic_rejected(self):
        self.capture(self.path / "separate.csv")
        p = self.path / "separate.csv"
        rows = p.read_text(encoding="utf-8").splitlines()
        rows[2] = rows[1]
        p.write_text("\n".join(rows) + "\n", encoding="utf-8")
        self.assertIn("non-monotonic", validate_capture(p, "separate"))


if __name__ == "__main__":
    unittest.main()
