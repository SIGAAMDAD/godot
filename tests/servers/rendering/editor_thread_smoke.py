#!/usr/bin/env python3
"""Opt-in Windows/Vulkan GUI editor render-thread smoke runner.

A clean process exit and actual render-worker samples are both required.
"""

import argparse
import csv
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
import time

MINIMUM_VERIFIED_FRAMES = 60
VALIDATION_ERROR = re.compile(
    r"(?im)(?:\bVUID-[A-Za-z0-9-]+|\bVK_ERROR_[A-Z_]+|(?:^|\s)(?:VALIDATION\s+ERROR|ERROR:)\s*)"
)


def validate_capture(path, expected_mode, minimum_frames=MINIMUM_VERIFIED_FRAMES):
    """Return a useful failure message or None for genuine live mode samples."""
    if not path.is_file():
        return "benchmark CSV was not produced"
    try:
        with path.open(newline="", encoding="utf-8") as capture:
            reader = csv.DictReader(capture)
            required = {"requested_mode", "effective_mode", "frame_id", "backend", "errors"}
            if not reader.fieldnames or not required.issubset(reader.fieldnames):
                return "benchmark CSV is missing required columns"
            rows = list(reader)
    except (OSError, UnicodeError, csv.Error) as exc:
        return f"cannot read benchmark CSV: {exc}"
    if len(rows) < minimum_frames:
        return f"only {len(rows)} completed editor frames; expected at least {minimum_frames}"
    previous_frame = -1
    for row in rows:
        try:
            frame = int(row["frame_id"])
            backend = row["backend"] or ""
        except (TypeError, ValueError, KeyError) as exc:
            return f"malformed frame record: {exc}"
        if frame <= previous_frame:
            return f"duplicate or non-monotonic editor frame: {frame}"
        previous_frame = frame
        if row["requested_mode"] != expected_mode or row["effective_mode"] != expected_mode:
            return f"render thread fell back or changed mode at frame {frame}"
        if "vulkan" not in backend.lower():
            return f"non-Vulkan backend at frame {frame}"
        if (row["errors"] or "").strip():
            return f"editor reported errors at frame {frame}"
    return None


def run(editor, project, mode, timeout_seconds, output_dir):
    capture = output_dir / (mode + ".csv")
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
    ]
    if mode == "separate":
        command.append("--editor-render-thread-experimental")
    command += [
        "--editor-render-thread-benchmark",
        str(capture),
        "--verbose",
        "--quit-after",
        "120",
    ]
    env = os.environ.copy()
    env["GODOT_BENCH_HOST_ID"] = "editor_render_thread_smoke"
    env["GODOT_BENCH_WORKLOAD_ID"] = "gui_editor_smoke_120_frames"
    env["GODOT_BENCH_WORKLOAD_SHA256"] = hashlib.sha256(
        (project / "project.godot").read_bytes() + b":gui_editor_smoke_120_frames"
    ).hexdigest()
    started = time.monotonic()
    result = {"mode": mode, "command": command, "timeout_seconds": timeout_seconds}
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            errors="replace",
            env=env,
            timeout=timeout_seconds,
            check=False,
        )
        result.update({
            "status": "EXITED" if completed.returncode == 0 else "FAILED",
            "exit_code": completed.returncode,
        })
        stdout, stderr = completed.stdout, completed.stderr
        if completed.returncode == 0:
            validation_match = VALIDATION_ERROR.search(stdout + "\n" + stderr)
            defect = validate_capture(capture, mode)
            if validation_match:
                defect = "Vulkan validation or engine error in process log: " + validation_match.group(0).strip()
            if defect:
                result.update({"status": "FAILED", "reason": defect})
            else:
                with capture.open(encoding="utf-8") as completed_capture:
                    result["verified_frames"] = sum(1 for _ in completed_capture) - 1
    except subprocess.TimeoutExpired as exc:
        result.update({"status": "TIMED_OUT", "exit_code": None})
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="replace")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="replace")
    except OSError as exc:
        result.update({"status": "FAILED", "exit_code": None, "reason": str(exc)})
        stdout, stderr = "", str(exc)
    result["duration_seconds"] = round(time.monotonic() - started, 3)
    (output_dir / (mode + ".stdout.log")).write_text(stdout, encoding="utf-8")
    (output_dir / (mode + ".stderr.log")).write_text(stderr, encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=pathlib.Path, required=True)
    parser.add_argument("--project", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if not args.editor.is_file() or not (args.project / "project.godot").is_file():
        parser.error("A built Windows editor and a valid GUI test project are required.")
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run(args.editor, args.project, mode, args.timeout, args.output) for mode in ("safe", "separate")]
    (args.output / "results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0 if all(result["status"] == "EXITED" for result in results) else 1


if __name__ == "__main__":
    sys.exit(main())
