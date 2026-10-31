"""Platform-independent negative tests of native window lifecycle orchestration."""

import pathlib
import tempfile
import unittest
from unittest.mock import patch

from editor_thread_windows_stress import exercise_windows, run_mode


class Process:
    pid = 12345
    returncode = None

    def poll(self):
        return self.returncode


class Backend:
    def __init__(self):
        self.calls = []
        self.visible = [123]

    def windows_for_pid(self, pid):
        assert pid == 12345
        return self.visible

    def minimize(self, hwnd):
        self.calls.append(("minimize", hwnd))

    def restore(self, hwnd):
        self.calls.append(("restore", hwnd))

    def resize(self, hwnd, width, height):
        self.calls.append(("resize", hwnd, width, height))
        self.current_size = (width, height)

    def size(self, hwnd):
        return self.current_size

    def close(self, hwnd):
        self.calls.append(("close", hwnd))


class StressTests(unittest.TestCase):
    @patch("editor_thread_windows_stress.subprocess.Popen", side_effect=OSError("missing executable"))
    def test_launch_failure_is_recorded(self, popen):
        with tempfile.TemporaryDirectory() as dirname:
            project = pathlib.Path(dirname)
            (project / "project.godot").write_text("config_version=5\n", encoding="utf-8")
            result = run_mode(project / "nonexistent.exe", project, "safe", project, 1, 2, 2, Backend())
        self.assertEqual(result["status"], "FAIL")
        self.assertIn("cannot launch", result["reason"])
        popen.assert_called_once()

    @patch("editor_thread_windows_stress.time.sleep")
    def test_window_cycles_and_close(self, sleep):
        backend = Backend()
        self.assertEqual(exercise_windows(Process(), backend, 3, 2), 3)
        self.assertEqual([x[0] for x in backend.calls].count("minimize"), 3)
        self.assertEqual([x[0] for x in backend.calls].count("resize"), 3)
        self.assertEqual(backend.calls[-1], ("close", 123))
        self.assertEqual(sleep.call_count, 6)

    @patch("editor_thread_windows_stress.time.sleep")
    def test_resize_not_applied_fails(self, sleep):
        backend = Backend()
        backend.size = lambda hwnd: (320, 200)
        with self.assertRaisesRegex(RuntimeError, "resize did not apply"):
            exercise_windows(Process(), backend, 1, 2)

    @patch("editor_thread_windows_stress.time.sleep")
    def test_process_exit_before_window_fails(self, sleep):
        p = Process()
        p.returncode = 1
        with self.assertRaisesRegex(RuntimeError, "exited before"):
            exercise_windows(p, Backend(), 1, 2)

    @patch("editor_thread_windows_stress.time.sleep")
    def test_window_disappears_during_stress(self, sleep):
        backend = Backend()
        num_calls = 0

        def windows_for_pid(pid):
            nonlocal num_calls
            num_calls += 1
            return [123] if num_calls == 1 else []

        backend.windows_for_pid = windows_for_pid
        with self.assertRaisesRegex(RuntimeError, "windows disappeared"):
            exercise_windows(Process(), backend, 2, 2)


if __name__ == "__main__":
    unittest.main()
