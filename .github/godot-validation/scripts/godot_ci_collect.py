#!/usr/bin/env python3
"""Collect real, SHA-identified Godot workflow evidence through authenticated `gh api`.

Uses read-only Github REST endpoints. The complete workflow run job list is
paginated and checked against total_count, while log excerpts are bounded.
If gh or network is unavailable, DO NOT synthesize a snapshot; supply a
snapshot from the connected GitHub tool with the same documented schema.
"""
from __future__ import annotations
import argparse
import json
import re
import shutil
import subprocess
from urllib.parse import quote
from godot_validation_core import SCHEMA, write_json, validate_snapshot

ERROR_LINE = re.compile(r"(?:error(?:\s+[A-Z]\d+|:)|-Werror|fatal|#\[error\]|failed|failure|hook id:|diff --git a/doc/classes/|Could not resolve|Could not find)", re.I)


def select_log_excerpt(text: str, max_lines: int = 160) -> str:
    lines = text.splitlines()
    indices = []
    for i, line in enumerate(lines):
        if ERROR_LINE.search(line):
            indices.extend(range(max(0, i - 1), min(len(lines), i + 2)))
    chosen = sorted(set(indices))[-max_lines:]
    if not chosen:
        chosen = list(range(max(0, len(lines) - 40), len(lines)))
    return "\n".join(lines[i][:1200] for i in chosen)


class GHCollector:
    def __init__(self, gh="gh", timeout=90):
        self.gh, self.timeout = gh, timeout
        if not shutil.which(gh):
            raise RuntimeError("Authenticated gh executable unavailable: " + gh)

    def request(self, path: str, as_json=True):
        proc = subprocess.run([self.gh, "api", "--method", "GET", path], capture_output=True, text=True, timeout=self.timeout, check=False)
        if proc.returncode:
            raise RuntimeError(f"GitHub API {path} failed: {proc.stderr[:650]}")
        return json.loads(proc.stdout) if as_json else proc.stdout

    def collect(self, repo: str, branch: str, base: str, max_runs: int = 8, max_logs: int = 40):
        if not re.fullmatch(r"[\w.-]+/[\w.-]+", repo):
            raise ValueError("Expected owner/name repo")
        branch_obj = self.request(f"repos/{repo}/branches/{quote(branch, safe='')}")
        head = branch_obj["commit"]["sha"]
        compare = self.request(f"repos/{repo}/compare/{base}...{head}")
        if compare.get("too_large") or (compare.get("total_commits", 0) > 250):
            # Commit comparison may be paginated/truncated; do not issue a confident report.
            raise RuntimeError("Comparison is too large to prove complete changed-file coverage")
        if compare.get("files") is not None and len(compare["files"]) >= 300:
            raise RuntimeError("Compare file list reaches GitHub 300-file cap; complete path coverage is unverified")
        if compare.get("files") is None:
            raise RuntimeError("GitHub comparison did not return changed files")
        files = [f["filename"] for f in compare["files"]]
        runs_page = self.request(f"repos/{repo}/actions/runs?branch={quote(branch, safe='')}&per_page=100")
        runs = []
        log_count = 0
        for r in runs_page.get("workflow_runs", []):
            if r.get("event") not in ("push", "pull_request", "workflow_dispatch", "merge_group"):
                continue
            if len(runs) >= max_runs:
                break
            job_resp = self.request(f"repos/{repo}/actions/runs/{r['id']}/jobs?per_page=100")
            jobs = list(job_resp.get("jobs", []))
            expected = job_resp.get("total_count", len(jobs))
            page = 2
            while len(jobs) < expected:
                part = self.request(f"repos/{repo}/actions/runs/{r['id']}/jobs?per_page=100&page={page}")
                new = part.get("jobs", [])
                if not new:
                    break
                jobs.extend(new)
                page += 1
            row = {"id": r["id"], "head_sha": r["head_sha"], "status": r["status"], "conclusion": r.get("conclusion"), "created_at": r.get("created_at"), "jobs_complete": len(jobs) == expected, "total_job_count": expected, "jobs": []}
            for j in jobs:
                rec = {"id": j["id"], "name": j["name"], "status": j["status"], "conclusion": j.get("conclusion"), "steps": [{"name": st.get("name", ""), "status": st.get("status"), "conclusion": st.get("conclusion")} for st in j.get("steps", [])]}
                if rec["conclusion"] in ("failure", "timed_out", "action_required") and log_count < max_logs:
                    try:
                        rec["log"] = select_log_excerpt(self.request(f"repos/{repo}/actions/jobs/{j['id']}/logs", as_json=False))
                        rec["log_completeness"] = "excerpt"
                    except RuntimeError as exc:
                        rec["log_error"] = str(exc)
                        rec["log_completeness"] = "missing"
                    log_count += 1
                row["jobs"].append(rec)
            runs.append(row)
        snapshot = {"schema": SCHEMA, "repo": repo, "branch": branch, "head_sha": head, "base_sha": base, "changed_files": files, "runs": runs, "evidence_source": "authenticated-gh-read-only-api", "external_checks": {}}
        return validate_snapshot(snapshot)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--repo", required=True)
    p.add_argument("--branch", required=True)
    p.add_argument("--base", required=True)
    p.add_argument("--max-runs", type=int, default=8)
    p.add_argument("--max-logs", type=int, default=40)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    if a.max_runs <= 0 or a.max_logs < 0:
        p.error("max-runs must be positive and max-logs non-negative")
    s = GHCollector().collect(a.repo, a.branch, a.base, a.max_runs, a.max_logs)
    write_json(a.output, s)
    print(f"COLLECTED: {len(s['runs'])} runs at {s['head_sha']} -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
