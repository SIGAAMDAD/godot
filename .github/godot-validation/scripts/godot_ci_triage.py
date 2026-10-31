#!/usr/bin/env python3
"""Group GitHub Actions failures by evidenced source-level root cause."""
import argparse
from godot_validation_core import read_json, triage, write_json


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--output", required=True)
    a = p.parse_args()
    report = triage(read_json(a.snapshot))
    write_json(a.output, report)
    print(f"CI TRIAGE: {report['runs_analyzed']} runs, {len(report['incidents'])} distinct failure signatures -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
