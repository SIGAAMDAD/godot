#!/usr/bin/env python3
"""Fail-closed Godot pre-push validation against immutable Git commit bytes.

Performs the pinned per-commit C++/GLSL clang-format gate, then Godot's full
prek changed-files hook suite in a disposable Git worktree at candidate HEAD.
No source/ref modifications in the developer's original working directory.
"""
from __future__ import annotations
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from godot_validation_core import write_json


def _call(args, cwd=None, timeout=1200):
    try:
        p = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=False)
        return p.returncode, (p.stdout or "")[-12000:], (p.stderr or "")[-12000:]
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError) as exc:
        return 127, "", str(exc)


def perform(root: Path, repo: str, base: str, head: str, formatter: str, prek: str, timeout: int = 1200) -> dict:
    root = root.resolve()
    report = {"schema": "godot-preflight/v1", "repo": repo, "head_sha": head, "status": "blocked", "stages": [], "source_root": str(root)}
    if not (root / ".git").exists():
        report["stages"].append({"name": "source-checkout", "status": "blocked", "detail": "No complete local Git checkout available"})
        return report
    check, stdout, stderr = _call(["git", "rev-parse", "--verify", f"{head}^{{commit}}"], cwd=root)
    if check != 0 or stdout.strip() != head:
        report["stages"].append({"name": "source-checkout", "status": "blocked", "detail": "Exact candidate Git commit absent: " + stderr})
        return report
    check, stdout, stderr = _call(["git", "merge-base", "--is-ancestor", base, head], cwd=root)
    if check != 0:
        report["stages"].append({"name": "source-checkout", "status": "blocked", "detail": "Base is absent or not an ancestor of HEAD: " + stderr})
        return report
    report["stages"].append({"name": "source-checkout", "status": "passed", "detail": f"Git candidate {head} and base {base} are present"})

    checker = Path(__file__).parent / "godot_clang_format_gate.py"
    command = [sys.executable, str(checker), "--repo", str(root), "--base", base, "--head", head, "--clang-format", formatter]
    code, out, err = _call(command, cwd=root, timeout=timeout)
    report["stages"].append({"name": "exact-commit-clang-format", "status": "passed" if code == 0 else "failed", "exit_code": code, "command": command, "stdout_tail": out, "stderr_tail": err})

    # The independent prek validation is run even if formatting fails, so one
    # push attempt can reveal all pre-commit failures in the same candidate.
    if not shutil.which(prek):
        report["stages"].append({"name": "godot-prek", "status": "blocked", "detail": f"Required prek binary unavailable: {prek}"})
        return report
    with tempfile.TemporaryDirectory(prefix="godot-validate-") as d:
        wt = Path(d) / "source"
        code, out, err = _call(["git", "worktree", "add", "--detach", str(wt), head], cwd=root, timeout=timeout)
        if code != 0:
            report["stages"].append({"name": "godot-prek", "status": "blocked", "detail": "Disposable Git worktree failed: " + err})
            return report
        try:
            command = [prek, "run", "--show-diff-on-failure", "--from-ref", base]
            code, out, err = _call(command, cwd=wt, timeout=timeout)
            report["stages"].append({"name": "godot-prek", "status": "passed" if code == 0 else "failed", "exit_code": code, "command": command, "stdout_tail": out, "stderr_tail": err})
            if code == 0:
                dirty, dirtyout, dirtyerr = _call(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=wt)
                if dirty != 0 or dirtyout.strip():
                    report["stages"].append({"name": "formatter-worktree-clean", "status": "failed", "detail": dirtyout or dirtyerr})
                else:
                    report["stages"].append({"name": "formatter-worktree-clean", "status": "passed", "detail": "Hooks left disposable worktree unchanged"})
        finally:
            _call(["git", "worktree", "remove", "--force", str(wt)], cwd=root, timeout=timeout)
    report["status"] = "passed" if all(s["status"] == "passed" for s in report["stages"]) else "failed"
    return report


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo-root", required=True)
    p.add_argument("--repo", required=True, help="GitHub owner/repo; provenance label")
    p.add_argument("--base", required=True, help="Full base SHA")
    p.add_argument("--head", required=True, help="Full candidate SHA")
    p.add_argument("--clang-format", default="clang-format")
    p.add_argument("--prek", default="prek")
    p.add_argument("--timeout", type=int, default=1200)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    report = perform(Path(a.repo_root), a.repo, a.base, a.head, a.clang_format, a.prek, a.timeout)
    write_json(a.output, report)
    print("PREFLIGHT:", report["status"], "->", a.output)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
