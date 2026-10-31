# Godot CI snapshot contract

## Contents

- Required JSON schema
- Connector collection procedure
- Completeness and provenance
- Exact-SHA acceptance

## Required JSON schema

`godot-ci-snapshot/v1` is a portable, read-only evidence snapshot. Example below is **illustrative**, not a record of a real workflow:

```json
{
  "schema": "godot-ci-snapshot/v1",
  "repo": "OWNER/godot",
  "branch": "feature/name",
  "head_sha": "0123456789abcdef0123456789abcdef01234567",
  "base_sha": "abcdefabcdefabcdefabcdefabcdefabcdefabcd",
  "changed_files": ["core/os/memory.cpp"],
  "runs": [
    {
      "id": 1,
      "head_sha": "0123456789abcdef0123456789abcdef01234567",
      "status": "completed",
      "conclusion": "failure",
      "jobs_complete": true,
      "total_job_count": 1,
      "jobs": [
        {
          "id": 2,
          "name": "🐧 Linux / Minimal template (target=template_release, everything disabled)",
          "status": "completed",
          "conclusion": "failure",
          "steps": [{"name": "Compilation", "conclusion": "failure", "status": "completed"}],
          "log": "core/os/memory.cpp:186:52: error: pointer 'p_memory' may be used after 'realloc' [-Werror=use-after-free]"
        }
      ]
    }
  ],
  "external_checks": {}
}
```

All SHAs must be full 40-character lowercase hexadecimal strings. `changed_files` is a complete canonical compare response for the chosen base and HEAD. `jobs_complete` is `true` only if every job in the run was retrieved, pagination confirmed, and `total_job_count` is the API-reported count equal to the number of supplied jobs. Canceled jobs and incomplete job listings are never accepted. `steps` must be from the job's actual GitHub API response, not inferred from expected workflow YAML. `log` is an error-context excerpt from the real job log; an unavailable log is omitted, not guessed. A `log_completeness` field may be used to note `excerpt` or `missing`.

## Connected GitHub tool acquisition

When local `gh` or cloning is not available, use the connected GitHub tools:

1. Read `GET /repos/OWNER/REPO/branches/BRANCH` to record exact SHA and current branch.
2. Read `GET /repos/OWNER/REPO/compare/BASE...HEAD`, retaining complete changed filenames. Check compare limits and truncation.
3. Read `GET /repos/OWNER/REPO/actions/runs?branch=BRANCH&per_page=100`. Retain run IDs, SHA, status, conclusion.
4. Read `GET /repos/OWNER/REPO/actions/runs/RUN_ID/jobs?per_page=100`, paginate if `total_count` exceeds returned length. Set `jobs_complete=true` only when all jobs are present, recording the API `total_count` as `total_job_count`.
5. Read failed job logs (`fetch_workflow_job_logs`) and preserve actionable compiler, formatter, doctool, and Gradle diagnostics. Include the actual failing `steps` metadata. Fetch logs for historical runs when reconstructing regression patterns.
6. Export the observed data to the schema. Run `godot_validate.py`. **Never create a fake checkout, toolchain result or synthetic source SHA to fill gaps.**

The bundled `godot_ci_collect.py` performs the equivalent collection through an authenticated `gh api`, not through unauthenticated public network requests. It does not publish, amend, merge or push.

## Sources and honest scope

- The `compare` endpoint is the source for changed paths; CI logs alone do not prove file scope.
- The GitHub Actions jobs endpoint is authoritative for job completion; green individual jobs do not prove the run completed.
- Source compilation requires an actual checkout and compiler; code inspection and CI log parsing are distinct forms of evidence.
- `external_checks` may contain native Windows/Vulkan runtime evidence with exact `head_sha`, `status=passed`, nonempty `evidence` reference, a 64-hex-digit `artifact_sha256`, `native_windows=true`, `backend=Vulkan`, `requested_mode=separate`, `effective_mode=separate`, at least 60 `verified_frames`, `exit_code=0`, and `validation_error_count=0`. These fields must come from a real native GUI run. Never invent an external check to satisfy acceptance.
- `godot_preflight.py` produces source-bound local preflight evidence independently. A GitHub snapshot does not substitute for that requirement.
