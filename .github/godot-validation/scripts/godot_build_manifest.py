#!/usr/bin/env python3
"""Create a source- and evidence-bound reproducibility manifest, without invented toolchain data."""
import argparse
import hashlib
import platform
from pathlib import Path
from godot_validation_core import read_json, write_json, validate_snapshot, snapshot_checksum


def manifest(snapshot: dict, inputs: dict[str, str], artifact_files: list[str] | None = None) -> dict:
    validate_snapshot(snapshot)
    evidence = {}
    for label, path in inputs.items():
        doc = read_json(path)
        if doc.get("head_sha") != snapshot["head_sha"] or doc.get("repo") != snapshot["repo"]:
            raise ValueError(f"Evidence {label} is not bound to exact candidate commit")
        evidence[label] = {"file": path, "sha256": snapshot_checksum(path), "status": doc.get("status", "documented"), "schema": doc.get("schema")}
    artifacts = []
    for item in artifact_files or []:
        p = Path(item)
        if not p.is_file():
            raise ValueError("Artifact unavailable: " + str(p))
        h = hashlib.sha256()
        with p.open("rb") as f:
            for block in iter(lambda: f.read(1024 * 1024), b""):
                h.update(block)
        artifacts.append({"name": p.name, "size": p.stat().st_size, "sha256": h.hexdigest()})
    return {"schema": "godot-build-manifest/v1", "repo": snapshot["repo"], "branch": snapshot["branch"], "head_sha": snapshot["head_sha"], "source_commit": snapshot["head_sha"], "source_checkout_verified": any(doc["schema"] == "godot-preflight/v1" and doc["status"] == "passed" for doc in evidence.values()), "toolchains": snapshot.get("toolchains", {}), "toolchain_discovery": "from snapshot only; absent means unverified", "host_python": platform.python_version(), "evidence": evidence, "artifacts": artifacts}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--input", action="append", default=[], help="label=/path/to/evidence.json")
    p.add_argument("--artifact", action="append", default=[])
    p.add_argument("--output", required=True)
    a = p.parse_args()
    pairs = {}
    for v in a.input:
        if "=" not in v:
            p.error("--input must be label=path")
        label, path = v.split("=", 1)
        if not label or not path or label in pairs:
            p.error("Invalid or duplicate input label")
        pairs[label] = path
    result = manifest(read_json(a.snapshot), pairs, a.artifact)
    write_json(a.output, result)
    print("MANIFEST:", result["head_sha"][:12], "evidence files:", len(result["evidence"]), "->", a.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
