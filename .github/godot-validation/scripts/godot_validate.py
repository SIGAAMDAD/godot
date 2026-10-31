#!/usr/bin/env python3
"""Run read-only Godot fork impact, CI triage, ledger, API sync, and acceptance."""
import argparse
from pathlib import Path
from godot_validation_core import read_json, write_json, triage, validate_snapshot, snapshot_checksum
from godot_build_impact import impact
from godot_regression_ledger import ledger
from godot_api_sync import audit
from godot_acceptance import evaluate
from godot_build_manifest import manifest
from godot_ci_workflow_audit import audit_runner


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--output-dir", required=True)
    p.add_argument("--full-matrix", action="store_true")
    p.add_argument("--source-only", action="store_true")
    p.add_argument("--preflight", help="Exact commit report from godot_preflight.py")
    p.add_argument("--runner-workflow", help="Actual Godot .github/workflows/runner.yml source from exact commit")
    a = p.parse_args()
    root = Path(a.output_dir)
    root.mkdir(parents=True, exist_ok=True)
    s = validate_snapshot(read_json(a.snapshot))
    i = impact(s, a.full_matrix, not a.source_only)
    t = triage(s)
    l = ledger(s, t)
    d = audit(s)
    f = read_json(a.preflight) if a.preflight else None
    v = evaluate(s, i, f)
    paths = {"impact": root / "impact.json", "triage": root / "triage.json", "regressions": root / "regressions.json", "api": root / "api.json", "acceptance": root / "acceptance.json"}
    if a.runner_workflow:
        w = audit_runner(Path(a.runner_workflow).read_text(encoding="utf-8"), s.get("branch_protected"))
        w.update({"repo": s["repo"], "head_sha": s["head_sha"]})
        paths["workflow"] = root / "workflow.json"
        write_json(paths["workflow"], w)
    for name, data in (("impact", i), ("triage", t), ("regressions", l), ("api", d), ("acceptance", v)):
        write_json(paths[name], data)
    man = manifest(s, {n: str(path) for n, path in paths.items()})
    man["snapshot_sha256"] = snapshot_checksum(a.snapshot)
    write_json(root / "manifest.json", man)
    report = {"schema": "godot-validation-summary/v1", "repo": s["repo"], "branch": s["branch"], "head_sha": s["head_sha"], "runs_analyzed": t["runs_analyzed"], "historical_run_listings_incomplete": t["incomplete_run_ids"], "distinct_failure_signatures": len(t["incidents"]), "regressions_unclosed": l["open_count"], "api_status": d["status"], "workflow_risks": len(w["risks"]) if a.runner_workflow else None, "gate_count": len(v["gates"]), "gates_missing_or_failed": len(v["missing_or_failed"]), "accepted": v["accepted"], "reports": {**{k: str(path) for k, path in paths.items()}, "manifest": str(root / "manifest.json")}}
    write_json(root / "summary.json", report)
    print("GODOT VALIDATION:", s["repo"], s["branch"], s["head_sha"])
    print(f"  {len(t['incidents'])} historical failure signatures, {l['open_count']} not closed by current job evidence")
    print(f"  {len(v['missing_or_failed'])}/{len(v['gates'])} required gates absent/pending/failed; accepted={v['accepted']}")
    for fail in v["missing_or_failed"][:12]:
        print(f"   - {fail['gate']}: {fail['status']}")
    print("  reports:", root)
    return 0 if v["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
