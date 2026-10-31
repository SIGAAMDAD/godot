---
name: godot-fork-validation
description: Validate, diagnose, and verify C++ Godot engine forks, including SIGAAMDAD/godot; use for Godot compile errors, CI failures, formatting failures, Godot engine patching, pre-push checks, CI matrix triage, GDExtension/RenderingDevice ABI changes, editor separate render thread, memory tracking, and release acceptance. Analyze actual source SHAs, aggregate independent GitHub Actions failures, select compiler configurations, run Godot's pinned formatting hooks, check generated class references, and reject incomplete or stale evidence. Use before publishing Godot source changes.
---

# Godot Fork Validation

Operate on **actual Godot source and CI evidence**, not presumed errors. This skill covers the Godot engine fork, distinct from the Terrain3D GDExtension repository. Never report an unexecuted check as passed or declare a candidate accepted with skipped, stale, incomplete, canceled or unverified required checks.

## Canonical workflow

1. **Identify the immutable baseline:** exact owner/repo, branch, full 40-digit HEAD, base SHA, changed-file list, GitHub run IDs and all job statuses. Read repository `.pre-commit-config.yaml`, SCons flags, `.github/workflows` and source if available. Do not substitute a different repo.
2. **Acquire evidence:** with an authenticated `gh` CLI, run `python scripts/godot_ci_collect.py --repo OWNER/REPO --branch BRANCH --base BASE_SHA --output snapshot.json`. With the connected GitHub tool, fetch the same branch, compare, Actions runs/jobs/failed-job logs and export the equivalent schema from [Snapshot contract](references/snapshot-contract.md). If no source checkout is available, report that as unavailable rather than pretending to compile.
3. **Triage all failures:** run `python scripts/godot_validate.py --snapshot snapshot.json --output-dir reports/ --runner-workflow runner.yml` when the exact-SHA root workflow is available. This performs impact selection, log deduplication, historical-regression reconciliation, generated-documentation assessment, immutable acceptance and evidence manifest generation. Its nonzero exit indicates required evidence is incomplete or failing, **not** that report generation failed.
4. **Apply source fixes:** inspect root-cause paths, dependencies, declarations, callers, test registration and all configurations affected by the change. For native allocator changes, cover `DEBUG_ENABLED`, release/minimal configurations, GCC, Clang, MSVC, and zero-size realloc semantics. For editor threading, cover worker lifetime, Vulkan window paths and shutdown. For ClassDB/API additions, synchronize `doc/classes` and GDExtension-facing interfaces. Integrate into the existing codebase without unrelated rewrites.
5. **Mandatory pre-push preflight:** on a complete local Git checkout with the candidate and base commits, execute `python scripts/godot_preflight.py --repo-root /path/to/godot --repo OWNER/REPO --base BASE_SHA --head CANDIDATE_SHA --output preflight.json`. It executes the exact pinned C++/GLSL formatter **for each new commit** and the full repository `prek` hooks in a disposable worktree. **No ordinary Git push or GitHub connector ref update is allowed when source/tooling is unavailable, or when this gate fails.** The GitHub connector bypasses local hooks.
6. **Configuration-aware local checks:** run targeted SCons build configurations selected by `reports/impact.json`, maintaining exact compiler commands and results. Use `--full-matrix` when the task requires full platform acceptance. Source-only validation can be requested with `--source-only`; never silently relabel runtime-gated work as accepted.
7. **Publish only an authorized, preflight-passing candidate.** Verify branch ownership, remote expected SHA and changed-file scope. No force pushes or history rewrites without explicit authorization. Group coherent fixes into semantic commits; do not push a formatting-fix commit as an alternative to preflight.
8. **Complete CI acceptance:** collect **every** required job and downstream step for the *exact pushed SHA*. For an unsuccessful run, classify all visible failures; for canceled or still-running runs keep gates open. Always record the latest run's `jobs_complete` result. Read the historical regression ledger before the next patch. Do not infer success from green jobs in an overall incomplete run.
9. **Report exact evidence:** source SHA, CI run ID, independent incidents, current failed/pending gates, named toolchain/SDK versions (only if observed), artifacts and omitted tests. The source/acceptance/manifest files must agree on SHA. A successful local validation is not a substitute for CI; a green CI build does not prove an unrun native Windows/Vulkan GUI test.

## Execution entrypoints

```bash
# Connected-GitHub snapshot (when already exported from the connector):
python scripts/godot_validate.py --snapshot snapshot.json --output-dir reports --runner-workflow runner.yml

# Full cross-platform validation matrix selection:
python scripts/godot_validate.py --snapshot snapshot.json --output-dir reports --full-matrix

# Source-level-only procedure (only when scope explicitly calls for it):
python scripts/godot_validate.py --snapshot snapshot.json --output-dir reports --source-only

# Candidate preflight (requires complete source history plus pinned clang-format and prek):
python scripts/godot_preflight.py --repo-root /src/godot --repo SIGAAMDAD/godot \
  --base FULL_BASE_SHA --head FULL_CANDIDATE_SHA --output preflight.json

# Reassess once source preflight and a complete CI run are available:
python scripts/godot_validate.py --snapshot snapshot.json --output-dir reports --preflight preflight.json
```

## Failure and acceptance rules

- `passed`: a required, exact-SHA check ran successfully with complete evidence.
- `failed`: an executed check failed. Preserve first actionable diagnostics and full source/job identity.
- `missing`: no required check or matching job is available.
- `pending`, `cancelled`, `skipped`, `timed_out`: never count as passed.
- `unverified`: external GUI checks, docs, source preflight or log details have no trustworthy attestation.
- `retested-gate-passed`: historical failure's affected job passed in a completed green current run. This is evidence of gate re-execution, not a claim that the original bug was behaviorally diagnosed.
- **Fail closed:** if required job pagination, source SHA, tool version, class-reference diff, external native runtime sample, or preflight is unavailable, the associated acceptance gate stays open.

## Script map

- `scripts/godot_ci_collect.py`: authenticated, read-only GitHub run/job/diagnostic collection, paginated, with fail-close metadata.
- `scripts/godot_ci_triage.py`: log normalization, common-root grouping, separated source/Gradle/style/docs failures.
- `scripts/godot_ci_workflow_audit.py`: read-only root workflow DAG, cancellation, and branch-protection risks.
- `scripts/godot_build_impact.py`: changed-path to required platform/compiler jobs and API/runtime checks.
- `scripts/godot_preflight.py`: immutable exact-commit clang-format and full Godot prek in a disposable worktree; no remote writes.
- `scripts/godot_clang_format_gate.py`: pinned exact-commit formatter checker, carried forward from git-history-curator.
- `scripts/godot_api_sync.py`: check the actual `--doctool` job step and XML delta from CI logs.
- `scripts/godot_regression_ledger.py`: cumulative failure ledger with bounded closure claims.
- `scripts/godot_acceptance.py`: reject stale, missing, incomplete and failing acceptance evidence.
- `scripts/godot_build_manifest.py`: SHA256 provenance of evidence files and actual available artifacts.
- `scripts/godot_validate.py`: read-only orchestration producing every report without manual command sequencing.

## Supporting documents

- [Snapshot contract and connector acquisition](references/snapshot-contract.md)
- [Root-cause classification and configuration matrix](references/failure-taxonomy.md)
- [Godot CI repository integration](references/repository-integration.md)
- [Independent audit and negative-test requirements](references/skill-audit.md)

Run `python -m unittest discover -s tests -v` after changing the skill. Review test output, run the package validator, and then exercise the skill on an actual Godot CI snapshot. **The skill itself has no access to CI until the connected GitHub tool or `gh` provides a source-grounded snapshot.**
