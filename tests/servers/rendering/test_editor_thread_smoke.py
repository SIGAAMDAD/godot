"""Offline negative controls for the GUI mode-verification smoke runner."""

import csv
import pathlib
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from editor_thread_smoke import run


class SmokeRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.project = self.root / "project"
        self.project.mkdir()
        (self.project / "project.godot").write_text("config_version=5\n", encoding="utf-8")

    def execute(self, expected_mode, actual_mode=None, create_capture=True, returncode=0):
        def fake_run(command, **kwargs):
            self.assertEqual(kwargs["env"]["GODOT_BENCH_HOST_ID"], "editor_render_thread_smoke")
            self.assertEqual(len(kwargs["env"]["GODOT_BENCH_WORKLOAD_SHA256"]), 64)
            self.assertEqual("--editor-render-thread-experimental" in command, expected_mode == "separate")
            if create_capture:
                path = pathlib.Path(command[command.index("--editor-render-thread-benchmark") + 1])
                with path.open("w", newline="", encoding="utf-8") as capture:
                    writer = csv.DictWriter(
                        capture,
                        fieldnames=["requested_mode", "effective_mode", "frame_id", "backend", "errors"],
                    )
                    writer.writeheader()
                    for frame in range(1, 121):
                        writer.writerow({
                            "requested_mode": expected_mode,
                            "effective_mode": actual_mode or expected_mode,
                            "frame_id": str(frame),
                            "backend": "vulkan",
                            "errors": "",
                        })
            return subprocess.CompletedProcess(command, returncode, "ok", "")

        with patch("editor_thread_smoke.subprocess.run", side_effect=fake_run):
            return run("godot.exe", self.project, expected_mode, 15, self.root)

    def test_safe_and_separate_modes_are_verified(self):
        for mode in ("safe", "separate"):
            with self.subTest(mode=mode):
                result = self.execute(mode)
                self.assertEqual(result["status"], "EXITED")
                self.assertEqual(result["verified_frames"], 120)

    def test_effective_safe_fallback_fails(self):
        result = self.execute("separate", actual_mode="safe")
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("fell back", result["reason"])

    def test_missing_capture_fails(self):
        result = self.execute("separate", create_capture=False)
        self.assertEqual(result["status"], "FAILED")
        self.assertIn("not produced", result["reason"])

    def test_nonzero_exit_fails(self):
        result = self.execute("safe", returncode=1)
        self.assertEqual(result["status"], "FAILED")

    def test_stale_capture_does_not_pass(self):
        (self.root / "safe.csv").write_text("old capture", encoding="utf-8")
        result = self.execute("safe", create_capture=False)
        self.assertEqual(result["status"], "FAILED")


if __name__ == "__main__":
    unittest.main()
