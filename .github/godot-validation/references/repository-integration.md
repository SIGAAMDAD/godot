# Godot repository-side CI integration

These are repository changes recommended to apply *with owner authorization*, independently of packaging this skill. Do not claim a skill archive automatically changes GitHub Actions.

## 1. Keep independent source and style findings visible

The current Godot fork's `.github/workflows/runner.yml` sets `needs: static-checks` on every build job. A formatting failure therefore skips the entire platform matrix. Use one independent fast compiler sentinel in parallel with static checks, or otherwise schedule core compile checks independently while retaining the source-change selection logic for pull requests. Do not simply remove `needs` references without updating expressions that consume its outputs.

## 2. Prevent premature CI cancellation for accepted candidates

Current fork:

```yaml
concurrency:
  group: ${{ github.workflow }}|${{ github.ref_name }}
  cancel-in-progress: true
```

A new push can cancel unfinished jobs on an older SHA. For immutable acceptance runs use SHA-level grouping and no cancellation:

```yaml
concurrency:
  group: ${{ github.workflow }}|${{ github.ref }}|${{ github.sha }}
  cancel-in-progress: false
```

Cost/queue tradeoff: running all pushed candidate SHAs concurrently can be expensive. Prefer a dedicated approval workflow triggered for a finalized candidate SHA, with inexpensive checks during editing. Keep status reports tied to that exact SHA.

## 3. Require useful status checks

Require the platform and static checks actually relevant to accepted source changes, including generated Godot XML documentation if public APIs changed. Configure branch protection/rulesets through the repository owner; skill files cannot enforce repository settings.

## 4. Pin toolchain and dependency inputs

Record SCons, Python, compiler, .NET, Android Gradle plugin/Kotlin, SDK versions and immutable action identifiers where practical. A Gradle dependency-resolution outage must be classified separately from native engine compilation.

## 5. Incorporate the validation tool

Place the skill's scripts in a reviewed tracked location (e.g. `.github/godot-validation/`) or invoke them from a pinned build artifact. A local hook is not enough for GitHub API-created commits; the validation must be run before `update_ref`. The script's `godot_preflight.py` is local-source-only and may require a compatible runner/toolchain.

## 6. Integration boundaries

- Skill packaging does not install local `.git/hooks` or GitHub branch rules.
- GitHub source writes require a complete candidate checkout and pinned formatter gate; if unavailable, provide the proposed patch and report the blocker instead of pushing unverified commits.
- Preserve the original upstream/Godot `prek` policy. Do not weaken style flags, remove compiler warnings, or skip failed jobs to obtain a green badge.
