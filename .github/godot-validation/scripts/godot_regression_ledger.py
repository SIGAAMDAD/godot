#!/usr/bin/env python3
"""Reconcile historical failures against checks on the *exact* current HEAD.

A passing job is a retested gate, not proof that a defect's precise source has
been repaired. Unobserved and cancelled jobs remain open.
"""
import argparse
from godot_validation_core import read_json, write_json, triage, latest_head_run, validate_snapshot


def ledger(snapshot: dict, report: dict | None = None) -> dict:
    validate_snapshot(snapshot)
    report = report or triage(snapshot)
    if report.get("head_sha") != snapshot["head_sha"]:
        raise ValueError("Triage belongs to another commit")
    run = latest_head_run(snapshot)
    current = {j["name"]: j for j in run["jobs"]} if run else {}
    rows = []
    for incident in report["incidents"]:
        affected = sorted(set(o["job_name"] for o in incident["observations"]))
        checked = []
        for name in affected:
            j = current.get(name)
            if j is None:
                state = "missing"
            elif j.get("conclusion") == "success" and j.get("status") == "completed":
                state = "passed"
            else:
                state = j.get("conclusion") or j.get("status") or "unverified"
            checked.append({"job": name, "current_status": state, "job_id": j.get("id") if j else None})
        passed_all = bool(run and run.get("status") == "completed" and run.get("conclusion") == "success" and checked and all(j["current_status"] == "passed" for j in checked))
        any_failure = any(j["current_status"] in ("failure", "timed_out", "action_required") for j in checked)
        state = "retested-gate-passed" if passed_all else ("active-failure" if any_failure else "not-closed")
        rows.append({"signature": incident["signature"], "category": incident["category"], "path": incident["path"], "historical_observations": len(incident["observations"]), "current_status": state, "checked_jobs": checked, "evidence_note": "Passing an affected CI job does not independently prove a historical source defect's behavior"})
    return {"schema": "godot-regression-ledger/v1", "repo": snapshot["repo"], "head_sha": snapshot["head_sha"], "current_run": run["id"] if run else None, "rows": rows, "closed_count": sum(r["current_status"] == "retested-gate-passed" for r in rows), "open_count": sum(r["current_status"] != "retested-gate-passed" for r in rows)}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--triage")
    p.add_argument("--output", required=True)
    a = p.parse_args()
    obj = ledger(read_json(a.snapshot), read_json(a.triage) if a.triage else None)
    write_json(a.output, obj)
    print(f"REGRESSION LEDGER: {obj['closed_count']} retested gates passed; {obj['open_count']} not closed -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
