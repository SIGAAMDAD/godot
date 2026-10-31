#!/usr/bin/env python3
"""Interactive Windows/Vulkan editor lifecycle stress test.

Use a real Windows desktop and a native Windows editor build. Each mode runs
against the same project with benchmark capture enabled. No third-party modules.
"""

import argparse
import ctypes
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time
from ctypes import wintypes

from editor_thread_smoke import VALIDATION_ERROR, validate_capture

SW_MINIMIZE = 6
SW_RESTORE = 9
WM_CLOSE = 0x0010


class Win32Windows:
    def __init__(self):
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        self.user32.EnumWindows.argtypes = [self.callback_type, wintypes.LPARAM]
        self.user32.EnumWindows.restype = wintypes.BOOL
        self.user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user32.IsWindowVisible.argtypes = [wintypes.HWND]
        self.user32.IsWindowVisible.restype = wintypes.BOOL
        self.user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        self.user32.ShowWindow.restype = wintypes.BOOL
        self.user32.MoveWindow.argtypes = [
            wintypes.HWND,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            wintypes.BOOL,
        ]
        self.user32.MoveWindow.restype = wintypes.BOOL
        self.user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        self.user32.GetWindowRect.restype = wintypes.BOOL
        self.user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        self.user32.PostMessageW.restype = wintypes.BOOL

    def windows_for_pid(self, pid):
        handles = []

        def visit(hwnd, ignored):
            owner = wintypes.DWORD()
            self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
            if owner.value == pid and self.user32.IsWindowVisible(hwnd):
                handles.append(hwnd)
            return True

        callback = self.callback_type(visit)
        if not self.user32.EnumWindows(callback, 0):
            raise OSError(ctypes.get_last_error(), "EnumWindows failed")
        return handles

    def minimize(self, hwnd):
        self.user32.ShowWindow(hwnd, SW_MINIMIZE)

    def restore(self, hwnd):
        self.user32.ShowWindow(hwnd, SW_RESTORE)

    def resize(self, hwnd, width, height):
        if not self.user32.MoveWindow(hwnd, 40, 40, width, height, True):
            raise OSError(ctypes.get_last_error(), "MoveWindow failed")

    def size(self, hwnd):
        rect = wintypes.RECT()
        if not self.user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            raise OSError(ctypes.get_last_error(), "GetWindowRect failed")
        return rect.right - rect.left, rect.bottom - rect.top

    def close(self, hwnd):
        if not self.user32.PostMessageW(hwnd, WM_CLOSE, 0, 0):
            raise OSError(ctypes.get_last_error(), "PostMessageW(WM_CLOSE) failed")


def exercise_windows(process, backend, cycles, startup_timeout, pause=0.15):
    deadline = time.monotonic() + startup_timeout
    windows = []
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"editor exited before a native window appeared ({process.returncode})")
        windows = backend.windows_for_pid(process.pid)
        if windows:
            break
        time.sleep(pause)
    if not windows:
        raise RuntimeError("no visible native editor window appeared")
    for cycle in range(cycles):
        if process.poll() is not None:
            raise RuntimeError(f"editor exited during window stress cycle {cycle}")
        windows = backend.windows_for_pid(process.pid)
        if not windows:
            raise RuntimeError(f"editor windows disappeared during cycle {cycle}")
        hwnd = windows[0]
        backend.minimize(hwnd)
        time.sleep(pause)
        backend.restore(hwnd)
        expected_size = (960, 540) if cycle % 2 else (1280, 720)
        backend.resize(hwnd, *expected_size)
        time.sleep(pause)
        actual_size = backend.size(hwnd)
        if actual_size != expected_size:
            raise RuntimeError(f"editor resize did not apply: expected {expected_size}, got {actual_size}")
    for hwnd in backend.windows_for_pid(process.pid):
        backend.close(hwnd)
    return cycles


def run_mode(editor, project, mode, output, cycles, startup_timeout, shutdown_timeout, backend):
    capture = output / f"stress_{mode}.csv"
    capture.unlink(missing_ok=True)
    command = [
        str(editor),
        "--path",
        str(project),
        "--editor",
        "--rendering-driver",
        "vulkan",
        "--render-thread",
        mode,
        "--editor-render-thread-benchmark",
        str(capture),
        "--verbose",
    ]
    if mode == "separate":
        command.append("--editor-render-thread-experimental")
    env = os.environ.copy()
    env.update({
        "GODOT_BENCH_HOST_ID": "win32_editor_window_stress",
        "GODOT_BENCH_WORKLOAD_ID": "gui_window_lifecycle_stress",
        "GODOT_BENCH_WORKLOAD_SHA256": hashlib.sha256(
            (project / "project.godot").read_bytes() + b":gui_window_lifecycle_stress"
        ).hexdigest(),
    })
    result = {"mode": mode, "cycles_requested": cycles, "command": command}
    stdout_path = output / f"stress_{mode}.stdout.log"
    stderr_path = output / f"stress_{mode}.stderr.log"
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            process = subprocess.Popen(command, env=env, stdout=stdout, stderr=stderr)
        except OSError as exc:
            result.update({"status": "FAIL", "reason": f"cannot launch editor: {exc}"})
            return result
        try:
            result["cycles_completed"] = exercise_windows(process, backend, cycles, startup_timeout)
            result["exit_code"] = process.wait(timeout=shutdown_timeout)
            defect = validate_capture(capture, mode)
            stdout.flush()
            stderr.flush()
            validation_log = stderr_path.read_text(encoding="utf-8", errors="replace")
            validation_log += stdout_path.read_text(encoding="utf-8", errors="replace")
            if VALIDATION_ERROR.search(validation_log):
                defect = "engine or Vulkan validation error in process log"
            if result["exit_code"] != 0:
                defect = f"editor exited with code {result['exit_code']}"
            result["status"] = "PASS" if defect is None else "FAIL"
            if defect is not None:
                result["reason"] = defect
        except (RuntimeError, OSError, subprocess.TimeoutExpired) as exc:
            result.update({"status": "FAIL", "reason": str(exc)})
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=pathlib.Path, required=True)
    parser.add_argument("--project", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--cycles", type=int, default=10)
    parser.add_argument("--startup-timeout", type=int, default=60)
    parser.add_argument("--shutdown-timeout", type=int, default=45)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("native Win32 GUI desktop required")
    if args.cycles < 1:
        parser.error("--cycles must be at least one")
    if not args.editor.is_file() or not (args.project / "project.godot").is_file():
        parser.error("valid editor binary and project.godot required")
    args.output.mkdir(parents=True, exist_ok=True)
    backend = Win32Windows()
    results = [
        run_mode(
            args.editor,
            args.project,
            mode,
            args.output,
            args.cycles,
            args.startup_timeout,
            args.shutdown_timeout,
            backend,
        )
        for mode in ("safe", "separate")
    ]
    (args.output / "stress_results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0 if all(item["status"] == "PASS" for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
