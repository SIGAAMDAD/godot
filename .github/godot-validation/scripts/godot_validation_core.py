#!/usr/bin/env python3
"""Pure, deterministic Godot CI snapshot normalization, triage, and gating.

Schema: godot-ci-snapshot/v1. This module NEVER fetches Github and never executes
builds. Evidence input must be produced by a connector or independent exporter.
"""
from __future__ import annotations

import hashlib
import json
import re
from collections import defaultdict
from pathlib import Path
from typing import Any

SCHEMA = "godot-ci-snapshot/v1"
HEX40 = re.compile(r"^[0-9a-f]{40}$")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP = re.compile(r"^20\d\d-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z\s+")
GCC_DIAG = re.compile(r"(?P<path>(?:[A-Za-z]:)?[\w.\\/\-]+\.(?:cpp|cc|c|cxx|h|hpp|mm)):(?P<line>\d+)(?::\d+)?:\s*(?:fatal\s+)?(?:error|warning):\s*(?P<msg>.+)")
MSVC_DIAG = re.compile(r"(?P<path>(?:[A-Za-z]:)?[\w.\\/\-]+\.(?:cpp|cc|c|cxx|h|hpp|mm))\((?P<line>\d+)\):\s*(?:fatal\s+)?(?:error|warning)\s+(?P<code>[A-Z]\d+):\s*(?P<msg>.+)")
DOC_DIFF = re.compile(r"diff --git a/(doc/classes/[^\s]+\.xml) b/\1")
HOOK = re.compile(r"- hook id:\s*([\w-]+)")


class EvidenceError(ValueError):
    pass


def read_json(path: str | Path) -> dict[str, Any]:
    with open(path, "r", encoding="utf-8") as stream:
        item = json.load(stream)
    if not isinstance(item, dict):
        raise EvidenceError("JSON evidence root must be an object")
    return item


def write_json(path: str | Path, value: dict[str, Any]) -> None:
    dest = Path(path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def validate_snapshot(s: dict[str, Any]) -> dict[str, Any]:
    if s.get("schema") != SCHEMA:
        raise EvidenceError(f"Expected snapshot schema {SCHEMA}")
    if not isinstance(s.get("repo"), str) or "/" not in s["repo"]:
        raise EvidenceError("Missing owner/repo")
    if not isinstance(s.get("branch"), str) or not s["branch"].strip():
        raise EvidenceError("Missing branch")
    if not HEX40.fullmatch(s.get("head_sha", "")):
        raise EvidenceError("Missing full 40-digit immutable head_sha")
    if not isinstance(s.get("changed_files"), list) or any(not isinstance(p, str) for p in s["changed_files"]):
        raise EvidenceError("Missing changed_files list (empty list is valid)")
    if not isinstance(s.get("runs"), list):
        raise EvidenceError("Missing workflow runs list")
    ids = set()
    for r in s["runs"]:
        if not isinstance(r, dict) or not isinstance(r.get("id"), int):
            raise EvidenceError("Run must have numeric id")
        if r["id"] in ids:
            raise EvidenceError(f"Duplicate workflow run {r['id']}")
        ids.add(r["id"])
        if not HEX40.fullmatch(r.get("head_sha", "")):
            raise EvidenceError(f"Run {r['id']} missing full head_sha")
        if r.get("status") not in ("completed", "queued", "in_progress", "requested", "waiting", "pending"):
            raise EvidenceError(f"Run {r['id']} has unknown status")
        if r.get("conclusion") not in (None, "success", "failure", "cancelled", "skipped", "neutral", "timed_out", "action_required", "stale"):
            raise EvidenceError(f"Run {r['id']} has unknown conclusion")
        if not isinstance(r.get("jobs"), list):
            raise EvidenceError(f"Run {r['id']} missing jobs list")
        if r.get("jobs_complete") is True and (not isinstance(r.get("total_job_count"), int) or r["total_job_count"] != len(r["jobs"])):
            raise EvidenceError(f"Run {r['id']} claims a complete job listing without an independently recorded matching total_job_count")
        jobids = set()
        for j in r["jobs"]:
            if not isinstance(j.get("id"), int) or not isinstance(j.get("name"), str):
                raise EvidenceError(f"Run {r['id']} has invalid job")
            if j["id"] in jobids:
                raise EvidenceError(f"Duplicate job id in run {r['id']}")
            jobids.add(j["id"])
            if j.get("status") not in ("completed", "queued", "in_progress", "waiting", "pending", "requested"):
                raise EvidenceError(f"Job {j['id']} missing/invalid status")
            if j.get("conclusion") not in (None, "success", "failure", "cancelled", "skipped", "neutral", "timed_out", "action_required", "stale"):
                raise EvidenceError(f"Job {j['id']} invalid conclusion")
            if "log" in j and not isinstance(j["log"], str):
                raise EvidenceError(f"Job {j['id']} log must be text")
            if "steps" in j and not isinstance(j["steps"], list):
                raise EvidenceError(f"Job {j['id']} steps must be list")
    return s


def normalized_lines(log: str) -> list[str]:
    return [TIMESTAMP.sub("", ANSI.sub("", line)).strip() for line in log.splitlines()]


def norm_path(path: str) -> str:
    p = path.replace("\\", "/").lstrip("./")
    for root in ("D:/a/godot/godot/", "/home/runner/work/godot/godot/"):
        if p.startswith(root):
            p = p[len(root):]
    return p


def diagnosis_kind(message: str, symbol: str = "") -> str:
    lower = message.lower()
    if "use-after-free" in lower or "used after" in lower and "realloc" in lower:
        return "memory-use-after-free"
    if "unused" in lower or "set but not used" in lower:
        return "unused-variable"
    if "shadows" in lower or "hides previous" in lower or "-wshadow" in lower or symbol in ("C4456", "C4458", "C4459"):
        return "shadow-warning"
    if "undeclared" in lower or "not declared" in lower:
        return "undeclared-identifier"
    if "redefinition" in lower:
        return "redefinition"
    if "could not find" in lower or "no such file" in lower:
        return "missing-dependency"
    if "C2220" in symbol:
        return "warnings-as-errors"
    return "compiler-error"


def diagnose_job(job: dict[str, Any]) -> list[dict[str, Any]]:
    """Return evidence-backed distinct causes (never infer a success from missing logs)."""
    if job.get("conclusion") not in ("failure", "timed_out", "action_required"):
        return []
    lines = normalized_lines(job.get("log", ""))
    joined = "\n".join(lines)
    steps = [s.get("name", "") for s in job.get("steps", []) if s.get("conclusion") == "failure"]
    categories: dict[str, dict[str, Any]] = {}

    def add(cat: str, key: str, path: str, detail: str) -> None:
        signature = f"{cat}|{key}"
        categories.setdefault(signature, {"category": cat, "signature": signature, "path": path, "detail": detail[:360]})

    docs = sorted(set(DOC_DIFF.findall(joined)))
    if docs or any("class reference" in s.lower() for s in steps):
        add("api-doc-sync", "doctool", "doc/classes", ", ".join(docs) or "class-reference check failed; no diff captured")

    if "Could not resolve org.jetbrains.kotlin" in joined or "Could not resolve all artifacts" in joined or "Could not find org.jetbrains.kotlin" in joined:
        add("dependency-resolution", "gradle-kotlin", "platform/android/java", "Gradle could not resolve Kotlin dependencies")
    elif "Could not resolve all files" in joined:
        add("dependency-resolution", "gradle-general", "platform/android/java", "Gradle dependency resolution failed")

    if "Style checks via prek" in " ".join(steps) or "- hook id:" in joined:
        hooks = sorted(set(HOOK.findall(joined)))
        for hook in hooks:
            add("style-hook", hook, ".pre-commit-config.yaml", f"prek hook failed: {hook}")

    for line in lines:
        diagnostic = GCC_DIAG.search(line)
        msvc = None if diagnostic else MSVC_DIAG.search(line)
        match = diagnostic or msvc
        if not match:
            continue
        message = match["msg"]
        code = msvc["code"] if msvc else ""
        if code == "C2220":
            # C2220 is merely a wrapper. Prefer adjacent real warning diagnostics.
            continue
        path = norm_path(match["path"])
        category = diagnosis_kind(message, code)
        if msvc and code.startswith("C") and "warning" in line:
            category = diagnosis_kind(message, code)
        if category == "compiler-error":
            # Collapse generic same-file failures; keep actionable diagnostic in evidence.
            key = path
        else:
            key = path + ":" + category
        add(category, key, path, f"{path}:{match['line']}: {message}")

    if not categories:
        reason = ("; ".join(steps) if steps else "No failing step details available")
        add("unclassified-failure", job["name"], "", reason)
    return list(categories.values())


def triage(s: dict[str, Any]) -> dict[str, Any]:
    validate_snapshot(s)
    by_signature: dict[str, dict[str, Any]] = {}
    job_counters = defaultdict(int)
    runs = sorted(s["runs"], key=lambda r: r["id"])
    for r in runs:
        for j in r["jobs"]:
            status = j["conclusion"] or j["status"]
            job_counters[status] += 1
            for issue in diagnose_job(j):
                sig = issue["signature"]
                if sig not in by_signature:
                    by_signature[sig] = {**issue, "observations": []}
                by_signature[sig]["observations"].append({"run_id": r["id"], "sha": r["head_sha"], "job_id": j["id"], "job_name": j["name"], "detail": issue["detail"]})
    for entry in by_signature.values():
        entry["observations"] = sorted(entry["observations"], key=lambda o: (o["run_id"], o["job_id"]))
    return {"schema": "godot-ci-triage/v1", "repo": s["repo"], "branch": s["branch"], "head_sha": s["head_sha"], "runs_analyzed": len(runs), "incomplete_run_ids": [r["id"] for r in runs if r.get("jobs_complete") is not True], "jobs_by_outcome": dict(sorted(job_counters.items())), "incidents": sorted(by_signature.values(), key=lambda x: (x["category"], x["signature"]))}


def exact_head_runs(s: dict[str, Any]) -> list[dict[str, Any]]:
    return [r for r in s["runs"] if r["head_sha"] == s["head_sha"]]


def latest_head_run(s: dict[str, Any]) -> dict[str, Any] | None:
    seq = exact_head_runs(s)
    return max(seq, key=lambda r: r["id"]) if seq else None


def snapshot_checksum(path: str | Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
