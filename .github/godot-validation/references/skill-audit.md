# Independent skill audit requirements

## Regression matrix

1. Same compiler warning in Android and Linux must consolidate to one path/category incident.
2. Distinct compiler conditions (`unused-variable`, `shadow-warning`, `use-after-free`) must never be flattened to one generic incident.
3. `C2220` alone cannot be mislabeled as the substantive compiler cause; preserve adjoining warning diagnostics.
4. Style and copyright hooks on the same run must be separate incidents.
5. Kotlin/Gradle dependency-resolution failure must not be labeled a native compiler failure.
6. `--doctool` XML diffs must be enumerated without generating fictional documentation edits.
7. A missing or skipped step must never satisfy API synchronization.
8. Run-level green jobs with one failure, cancellation, or pending job must not satisfy acceptance.
9. A run with missing job pages must fail acceptance even if visible jobs are green.
10. Historical run success on a different SHA cannot satisfy current acceptance.
11. Stale local preflight or unavailable formatter must fail closed.
12. A complete valid local preflight plus a complete passing run for exact SHA should be accepted for a minimal test scope.
13. A native Windows/Vulkan editor render-thread source change must require native GUI runtime evidence unless the owner explicitly scoped validation to source only.
14. Unit tests must exercise real temporary Git objects for local preflight and verified formatter behavior; fake formatter behavior tests are unit tests, not real Godot formatting proof.
15. The skill must remain read-only on the remote unless a separate authorized write request and mandatory preflight are satisfied.
16. Bogus two-step `passed` preflight cannot satisfy acceptance; all required named stages and a clean worktree are mandatory.
17. Claimed `jobs_complete=true` with missing or mismatched `total_job_count` must be rejected.
18. `SCsub` modifications and compiled engine tests require full platform matrix selection.
19. GitHub 300-file compare truncation must block a complete-source claim.
20. A bare user assertion of native Windows/Vulkan editor validation must fail; accepted external evidence requires a SHA256 artifact and measured frames under the actual separate worker mode.

## Audit procedure

1. Run `python -m unittest discover -s tests -v` from the skill directory.
2. Run `python -m compileall -q scripts tests` and inspect any warnings.
3. Run `python /home/oai/skills/skill-creator/scripts/quick_validate.py <skill-directory>` or equivalent validator.
4. Collect actual Godot Actions jobs/logs via the connected GitHub tool and record source SHA. Run the skill on that snapshot and independently compare diagnostics and job outcomes to the actual GitHub UI/API results.
5. Record false positives, false negatives and failed or unavailable acceptance evidence. Patch the skill, rerun the tests, and rerun the real snapshot before packaging.
6. Package the complete skill as `skill.zip` with the official skill packager. Inspect archive contents and ensure no CI credentials, raw private logs or unused sample assets were included.
