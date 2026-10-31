#!/usr/bin/env python3
"""Audit Godot --doctool class-reference evidence; fail closed when absent."""
import argparse
from godot_validation_core import DOC_DIFF, normalized_lines, read_json, write_json, validate_snapshot, latest_head_run


def audit(snapshot: dict) -> dict:
    validate_snapshot(snapshot)
    run = latest_head_run(snapshot)
    matches = []
    if run:
        for j in run["jobs"]:
            steps = [x for x in j.get("steps", []) if "class reference" in x.get("name", "").lower()]
            for step in steps:
                files = sorted(set(DOC_DIFF.findall("\n".join(normalized_lines(j.get("log", ""))))))
                status = "passed" if step.get("status") == "completed" and step.get("conclusion") == "success" else (step.get("conclusion") or step.get("status") or "unverified")
                matches.append({"job_id": j["id"], "step": step["name"], "status": status, "files": files})
    if len(matches) == 1:
        status = matches[0]["status"]
    elif not matches:
        status = "unverified"
    else:
        status = "invalid"
    return {"schema": "godot-api-sync/v1", "repo": snapshot["repo"], "head_sha": snapshot["head_sha"], "status": status, "checks": matches, "note": "Inspects actual --doctool CI evidence; no generated XML or API source is fabricated"}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    report = audit(read_json(a.snapshot))
    write_json(a.output, report)
    print("API SYNC:", report["status"], report["checks"][:1], "->", a.output)
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
