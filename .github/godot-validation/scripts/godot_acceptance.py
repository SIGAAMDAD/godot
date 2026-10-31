#!/usr/bin/env python3
"""Reject stale, incomplete, cancelled, missing, skipped, or failing Godot CI evidence."""
import argparse
import re
from godot_validation_core import read_json, write_json, validate_snapshot, latest_head_run


def evaluate(snapshot: dict, impact: dict, preflight: dict | None = None) -> dict:
    validate_snapshot(snapshot)
    if impact.get("schema") != "godot-build-impact/v1" or impact.get("head_sha") != snapshot["head_sha"] or impact.get("repo") != snapshot["repo"] or impact.get("branch") != snapshot["branch"]:
        raise ValueError("Impact analysis belongs to a different source or has invalid schema")
    statuses = []
    head = latest_head_run(snapshot)
    head_sha = snapshot["head_sha"]
    if head is None:
        statuses.append({"gate": "ci-run", "status": "missing", "detail": "No CI run for exact candidate HEAD"})
    else:
        done = head["status"] == "completed" and head.get("conclusion") == "success" and head.get("jobs_complete") is True
        if not done:
            statuses.append({"gate": "ci-run", "status": "incomplete-job-list" if head.get("jobs_complete") is not True else (head.get("conclusion") or head["status"]), "detail": f"Run {head['id']} has no complete successful result with a complete job listing for candidate SHA"})
        else:
            statuses.append({"gate": "ci-run", "status": "passed", "detail": f"Run {head['id']} finished successfully"})

    jobs = head["jobs"] if head else []
    external = snapshot.get("external_checks", {})
    for gate in impact["gates"]:
        gate_id = gate["id"]
        status = "missing"
        detail = "No matching evidence for exact candidate SHA"
        if gate["type"] == "github-job":
            matches = [j for j in jobs if re.search(gate["job_regex"], j["name"], re.IGNORECASE)]
            if len(matches) > 1:
                status, detail = "invalid", f"Ambiguous: {len(matches)} jobs matched"
            elif len(matches) == 1:
                item = matches[0]
                status = "passed" if item["status"] == "completed" and item["conclusion"] == "success" else (item.get("conclusion") or item["status"])
                detail = f"Run {head['id']}, job {item['id']}: {item['name']}"
        elif gate["type"] == "github-step":
            parent = next(g for g in impact["gates"] if g["id"] == gate["parent_gate"])
            parents = [j for j in jobs if re.search(parent["job_regex"], j["name"], re.IGNORECASE)]
            if len(parents) == 1:
                steps = [st for st in parents[0].get("steps", []) if re.search(gate["step_regex"], st.get("name", ""), re.IGNORECASE)]
                if len(steps) == 1:
                    step = steps[0]
                    status = "passed" if step.get("status") == "completed" and step.get("conclusion") == "success" else (step.get("conclusion") or step.get("status") or "unverified")
                    detail = f"Run {head['id']}, job {parents[0]['id']}, step: {step['name']}"
                elif len(steps) > 1:
                    status, detail = "invalid", "Duplicate API class-reference steps"
        elif gate["type"] == "external":
            record = external.get(gate_id)
            if (isinstance(record, dict) and record.get("head_sha") == head_sha and
                record.get("status") == "passed" and isinstance(record.get("evidence"), str) and record["evidence"].strip() and
                re.fullmatch(r"[0-9a-f]{64}", record.get("artifact_sha256", "")) and
                record.get("native_windows") is True and
                record.get("backend", "").lower() == "vulkan" and
                record.get("requested_mode") == "separate" and record.get("effective_mode") == "separate" and
                isinstance(record.get("verified_frames"), int) and record["verified_frames"] >= 60 and
                record.get("exit_code") == 0 and record.get("validation_error_count") == 0):
                status, detail = "passed", str(record["evidence"])
            elif record is not None:
                status, detail = "unverified", "Native Windows/Vulkan evidence lacks exact HEAD, sha256 artifact, 60 separate-thread frames or clean validation"
        else:
            status, detail = "invalid", "Unrecognized gate type"
        statuses.append({"gate": gate_id, "status": status, "detail": detail})

    if preflight is None:
        statuses.append({"gate": "local-preflight", "status": "missing", "detail": "No local complete pre-push report supplied; cannot certify publication"})
    elif preflight.get("schema") != "godot-preflight/v1" or preflight.get("head_sha") != head_sha or preflight.get("repo") != snapshot["repo"]:
        statuses.append({"gate": "local-preflight", "status": "stale", "detail": "Preflight SHA/schema/repository mismatch"})
    elif (preflight.get("status") != "passed" or
          not isinstance(preflight.get("stages"), list) or
          not {"source-checkout", "exact-commit-clang-format", "godot-prek", "formatter-worktree-clean"}.issubset({x.get("name") for x in preflight.get("stages", [])}) or
          len({x.get("name") for x in preflight.get("stages", [])}) != len(preflight.get("stages", [])) or
          not all(x.get("status") == "passed" for x in preflight.get("stages", []))):
        statuses.append({"gate": "local-preflight", "status": "blocked" if preflight.get("status") == "blocked" else "failed", "detail": "Preflight stages are missing, skipped, failed, or blocked"})
    else:
        statuses.append({"gate": "local-preflight", "status": "passed", "detail": "Exact source preflight completed"})

    accepted = bool(statuses) and all(x["status"] == "passed" for x in statuses)
    return {"schema": "godot-acceptance/v1", "repo": snapshot["repo"], "branch": snapshot["branch"], "head_sha": head_sha, "run_id": head["id"] if head else None, "accepted": accepted, "gates": statuses, "missing_or_failed": [x for x in statuses if x["status"] != "passed"]}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--impact", required=True)
    p.add_argument("--preflight")
    p.add_argument("--output", required=True)
    a = p.parse_args()
    result = evaluate(read_json(a.snapshot), read_json(a.impact), read_json(a.preflight) if a.preflight else None)
    write_json(a.output, result)
    print(f"ACCEPTANCE: {'PASS' if result['accepted'] else 'NOT VERIFIED'}; {len(result['missing_or_failed'])} missing/failed/pending gate(s) -> {a.output}")
    return 0 if result["accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
