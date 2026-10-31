#!/usr/bin/env python3
"""Audit Godot's workflow DAG and cancellation policy without modifying source."""
from __future__ import annotations
import argparse
import re
from pathlib import Path
from godot_validation_core import read_json, write_json


def audit_runner(contents: str, branch_protected: bool | None = None) -> dict:
    if "jobs:" not in contents or "concurrency:" not in contents:
        raise ValueError("Missing Godot root workflow structure (cannot audit)")
    group = re.search(r"(?m)^  group:\s*(.+)$", contents)
    cancellation = re.search(r"(?m)^  cancel-in-progress:\s*(true|false)\s*$", contents)
    if not group or not cancellation:
        raise ValueError("Cannot establish root workflow concurrency semantics")
    jobs_start = re.search(r"(?m)^jobs:\s*$", contents)
    job_headers = list(re.finditer(r"(?m)^  ([\w-]+):\s*$", contents[jobs_start.end():]))
    parsed = {}
    section = contents[jobs_start.end():]
    for i, match in enumerate(job_headers):
        end = job_headers[i+1].start() if i+1 < len(job_headers) else len(section)
        body = section[match.end():end]
        match_needs = re.search(r"(?m)^    needs:\s*(.+)$", body)
        match_uses = re.search(r"(?m)^    uses:\s*(.+)$", body)
        parsed[match.group(1)] = {"needs": match_needs.group(1).strip() if match_needs else None, "uses": match_uses.group(1).strip() if match_uses else None}
    buildjobs = {name: val for name, val in parsed.items() if name.endswith("-build")}
    gated = [name for name, val in buildjobs.items() if val["needs"] and "static-checks" in val["needs"]]
    risks = []
    if cancellation.group(1) == "true" and "github.ref_name" in group.group(1) and "github.sha" not in group.group(1):
        risks.append({"id": "same-branch-cancel", "severity": "high", "detail": "Push to same branch cancels previous run before slow jobs may finish"})
    if buildjobs and len(gated) == len(buildjobs):
        risks.append({"id": "static-blocks-all-builds", "severity": "high", "detail": f"All {len(buildjobs)} platform builds depend on static-checks; any style failure skips the entire compile matrix"})
    if branch_protected is False:
        risks.append({"id": "no-required-branch-checks", "severity": "high", "detail": "Current branch was reported unprotected by GitHub API"})
    return {"schema": "godot-ci-workflow-audit/v1", "status": "risks-found" if risks else "no-listed-risks", "root_job_count": len(parsed), "platform_build_job_count": len(buildjobs), "builds_gated_on_static": gated, "cancel_in_progress": cancellation.group(1) == "true", "concurrency_group": group.group(1), "branch_protected": branch_protected, "risks": risks, "note": "Source-level policy audit only. Missing risk flags do not prove the workflow functions correctly."}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--workflow", required=True)
    p.add_argument("--snapshot")
    p.add_argument("--output", required=True)
    a = p.parse_args()
    info = read_json(a.snapshot) if a.snapshot else {}
    report = audit_runner(Path(a.workflow).read_text(encoding="utf-8"), info.get("branch_protected"))
    report["repo"] = info.get("repo", "unknown")
    report["head_sha"] = info.get("head_sha", "unknown")
    write_json(a.output, report)
    print(f"WORKFLOW AUDIT: {len(report['risks'])} structural risks -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
