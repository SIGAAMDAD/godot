#!/usr/bin/env python3
"""Fail-closed, exact-commit clang-format gate for Godot fork pushes.

Checks the changed source files as they exist in EACH candidate Git commit, using
that commit's .clang-format and Godot's pinned clang-format version. Does not
modify files or refs. Also checks .glsl files with the repository's GLSL style.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

CPP_RE = re.compile(r"\.(?:c|h|cpp|hpp|cc|hh|cxx|hxx|m|mm|inc|java)$")
GLSL_RE = re.compile(r"\.glsl$")
PIN_RE = re.compile(r"repo:\s*https://github\.com/pre-commit/mirrors-clang-format\s*\n\s*rev:\s*v?(\d+\.\d+\.\d+)")
STYLE_CPP = ".clang-format"
STYLE_GLSL = "misc/utility/clang_format_glsl.yml"
CONFIG = ".pre-commit-config.yaml"


class GateError(RuntimeError):
    pass


def git(repo: Path, *args: str) -> bytes:
    command = ["git", "-C", str(repo), *args]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        raise GateError(f"git {' '.join(args[:3])}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def required_blob(repo: Path, sha: str, path: str) -> bytes:
    if path.startswith("/") or ".." in Path(path).parts:
        raise GateError(f"unsafe source path: {path}")
    return git(repo, "show", f"{sha}:{path}")


def changed_paths(repo: Path, left: str, right: str) -> list[str]:
    raw = git(repo, "diff", "--name-only", "--diff-filter=ACMRT", "-z", left, right)
    return [os.fsdecode(p) for p in raw.split(b"\0") if p]


def paths_in_tree(repo: Path, sha: str) -> list[str]:
    raw = git(repo, "ls-tree", "-r", "--name-only", "-z", sha)
    return [os.fsdecode(p) for p in raw.split(b"\0") if p]


def type_of_path(path: str) -> str | None:
    if CPP_RE.search(path):
        return "cpp"
    if GLSL_RE.search(path):
        return "glsl"
    return None


def parse_policy(config: str) -> tuple[str, re.Pattern[str]]:
    match = PIN_RE.search(config)
    if not match:
        raise GateError("Godot's pinned mirrors-clang-format revision is unavailable")
    for rule in (r"files: \.(c|h|cpp|hpp|cc|hh|cxx|hxx|m|mm|inc|java)$", r"files: \.glsl$"):
        if rule not in config:
            raise GateError(f"Godot's clang-format hook selection changed: {rule}")
    if "args: [-style=file:misc/utility/clang_format_glsl.yml]" not in config:
        raise GateError("Godot's GLSL clang-format style argument changed")
    lines = config.splitlines(keepends=True)
    start = next((i for i, line in enumerate(lines) if line.startswith("exclude: |")), None)
    if start is None:
        raise GateError("Godot pre-commit global exclusions unavailable")
    body = []
    for line in lines[start + 1 :]:
        if line.strip() and not line.startswith((" ", "\t")):
            break
        body.append(line[2:] if line.startswith("  ") else line)
    try:
        return match.group(1), re.compile("".join(body), re.VERBOSE)
    except re.error as exc:
        raise GateError(f"Invalid pre-commit exclusion pattern: {exc}") from exc


def formatter_version(binary: str) -> str:
    try:
        result = subprocess.run([binary, "--version"], capture_output=True, text=True, check=False)
    except OSError as exc:
        raise GateError(f"clang-format unavailable ({binary}): {exc}") from exc
    if result.returncode:
        raise GateError(f"clang-format version check failed: {result.stderr.strip()}")
    found = re.search(r"\b(\d+\.\d+\.\d+)\b", result.stdout)
    if not found:
        raise GateError(f"Could not parse clang-format version: {result.stdout.strip()}")
    return found.group(1)


def check_blob(binary: str, root: Path, path: str, kind: str, content: bytes) -> int:
    target = root / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(content)
    style = "-style=file" if kind == "cpp" else f"-style=file:{STYLE_GLSL}"
    command = [binary, style, "-output-replacements-xml", str(target)]
    try:
        output = subprocess.run(command, cwd=root, capture_output=True, check=False)
    except OSError as exc:
        raise GateError(f"clang-format execution failed for {path}: {exc}") from exc
    if output.returncode:
        raise GateError(f"clang-format failed for {path}: {output.stderr.decode(errors='replace').strip()}")
    try:
        parsed = ET.fromstring(output.stdout)
    except ET.ParseError as exc:
        raise GateError(f"clang-format returned invalid replacement XML for {path}: {exc}") from exc
    if parsed.tag != "replacements":
        raise GateError(f"Unexpected clang-format XML root for {path}: {parsed.tag}")
    return len(parsed.findall("replacement"))


def get_git_mode(repo: Path, sha: str, path: str) -> str:
    line = git(repo, "ls-tree", sha, "--", path).split(b"\n")[0]
    if not line or b" " not in line:
        raise GateError(f"Source path is missing from commit {sha[:12]}: {path}")
    return line.split(b" ", 1)[0].decode("ascii")


def verify(repo: Path, base: str, head: str, binary: str = "clang-format", check_each: bool = True) -> dict:
    repo = repo.resolve()
    resolved_base = git(repo, "rev-parse", "--verify", f"{base}^{{commit}}").decode().strip()
    resolved_head = git(repo, "rev-parse", "--verify", f"{head}^{{commit}}").decode().strip()
    if resolved_base == resolved_head:
        raise GateError("No commits to check between base and head")
    ancestry = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", resolved_base, resolved_head])
    if ancestry.returncode != 0:
        raise GateError("Base is not an ancestor of candidate HEAD")
    candidates = git(repo, "rev-list", "--reverse", "--topo-order", f"{resolved_base}..{resolved_head}").decode().splitlines()
    if not candidates:
        raise GateError("Candidate range is empty")
    if not check_each:
        candidates = [resolved_head]
    installed = formatter_version(binary)
    checked = 0
    failures: list[str] = []
    seen: set[tuple[str, str, str, str, str]] = set()
    for commit in candidates:
        parents = git(repo, "rev-list", "--parents", "-n", "1", commit).decode().split()
        previous = parents[1] if len(parents) >= 2 else resolved_base
        config = required_blob(repo, commit, CONFIG).decode("utf-8")
        version, excluded = parse_policy(config)
        if version != installed:
            raise GateError(f"{commit[:12]} pins clang-format {version}, installed is {installed}")
        godot_format = required_blob(repo, commit, STYLE_CPP)
        if not godot_format:
            raise GateError(f"{commit[:12]} is missing Godot .clang-format")
        git(repo, "cat-file", "-e", f"{commit}:SConstruct")
        selection = changed_paths(repo, previous, commit)
        if STYLE_CPP in selection or CONFIG in selection or STYLE_GLSL in selection:
            selection = paths_in_tree(repo, commit)  # style policy changes require full-tree validation
        with tempfile.TemporaryDirectory(prefix="godot-format-gate-") as tmp:
            root = Path(tmp)
            (root / STYLE_CPP).write_bytes(godot_format)
            glsl_style = None
            for path in selection:
                kind = type_of_path(path)
                if not kind or excluded.search(path):
                    continue
                mode = get_git_mode(repo, commit, path)
                if mode not in ("100644", "100755"):
                    raise GateError(f"Uncheckable source mode {mode}: {path}")
                if kind == "glsl" and glsl_style is None:
                    glsl_style = required_blob(repo, commit, STYLE_GLSL)
                    (root / STYLE_GLSL).parent.mkdir(parents=True, exist_ok=True)
                    (root / STYLE_GLSL).write_bytes(glsl_style)
                nested_styles = []
                ancestor = Path(path).parent
                # Respect any more-specific .clang-format in the actual commit tree.
                for directory in reversed(ancestor.parents):
                    if str(directory) not in (".", ""):
                        nested_styles.append(directory / STYLE_CPP)
                if str(ancestor) not in (".", ""):
                    nested_styles.append(ancestor / STYLE_CPP)
                nested_data = []
                for style_path in nested_styles:
                    path_style = style_path.as_posix()
                    exists = subprocess.run(
                        ["git", "-C", str(repo), "cat-file", "-e", f"{commit}:{path_style}"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                    if exists.returncode == 0:
                        data = required_blob(repo, commit, path_style)
                        target_style = root / path_style
                        target_style.parent.mkdir(parents=True, exist_ok=True)
                        target_style.write_bytes(data)
                        nested_data.append((path_style, data))
                blob = required_blob(repo, commit, path)
                signature = (
                    git(repo, "rev-parse", f"{commit}:{path}").decode().strip(),
                    path, kind, version,
                    repr(nested_data) + repr(glsl_style if kind == "glsl" else godot_format),
                )
                if signature in seen:
                    continue
                seen.add(signature)
                changes = check_blob(binary, root, path, kind, blob)
                checked += 1
                if changes:
                    failures.append(f"{commit[:12]} {path}: clang-format requires {changes} replacement(s)")
    return {"passed": not failures, "base": resolved_base, "head": resolved_head, "commits": len(candidates), "formatted_files_checked": checked, "failures": failures}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--base", required=True, help="exact base SHA of candidate commits")
    parser.add_argument("--head", required=True, help="exact candidate commit SHA (not moving HEAD)")
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    try:
        result = verify(args.repo, args.base, args.head, args.clang_format, True)
    except GateError as exc:
        print(f"GODOT CLANG-FORMAT GATE BLOCKED: {exc}", file=sys.stderr)
        return 2
    if result["failures"]:
        for err in result["failures"]:
            print(f"GODOT CLANG-FORMAT GATE FAILED: {err}", file=sys.stderr)
        return 1
    print(f"GODOT CLANG-FORMAT PASS: {result['commits']} commit(s), {result['formatted_files_checked']} unique source blob(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
