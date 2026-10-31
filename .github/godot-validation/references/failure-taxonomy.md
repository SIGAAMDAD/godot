# Godot-specific failure taxonomy and impact coverage

## Error classification

| Observed signature | Category | Expected impact |
|---|---|---|
| `-Werror=use-after-free` near realloc | `memory-use-after-free` | GCC release, MinGW, minimal; allocator lifetime audit |
| `unused variable`/`set but not used` | `unused-variable` | Debug/release, GCC/Clang, test build registration |
| `-Werror=shadow` | `shadow-warning` | GCC SCU and strict sanitizers |
| `C2220` | MSVC wrapper for a warning | Inspect adjacent C4xxx warning; do not report C2220 alone as root cause |
| `- hook id: clang-format` | `style-hook` | Pinned pre-commit policy; *all* other failing hook IDs must also be retained |
| `diff --git a/doc/classes/...` after `--doctool` | `api-doc-sync` | Generated class-reference/registrations out of sync |
| Kotlin Gradle `Could not resolve` | `dependency-resolution` | Android packaging/dependency repositories; verify native C++ build separately |
| Run `cancelled`, job `skipped`, unknown page | Incomplete verification | Preserve untested checks; no root-cause claim |

The triage groups source diagnostics by path + compiler diagnostic category across platforms, and consolidates all generated class-reference XML changes into one documentation incident. It does **not** infer a source fix from a passing unrelated configuration.

## Configuration selection

`godot_build_impact.py` uses changed-file classes to select relevant established matrix job names. It is conservative and additive: unknown native source defaults to Linux minimal/editor plus Windows compiler jobs; allocator and renderer changes select most of the platform/compiler matrix.

- `SCsub`, `SConstruct`, GitHub build actions, or compiled C++ engine tests: full platform build matrix due to build registration/target contamination risks.
- Core memory/profiler: GCC and Clang, debug/release, minimal modules, Windows MinGW, Web wasm32, Android, macOS/iOS, and sanitizer jobs.
- RenderingDevice/Forward+/driver changes: renderer backend and API checks across Linux/Windows/Android/macOS/iOS/Web.
- Editor debugger/thread changes: Mono editor, Linux GCC/Clang/TSAN/SCU, Windows MSVC/clang-cl, native Windows GUI verification, macOS/Android editor builds.
- Windows display server: Windows MSVC, clang-cl, MinGW and template targets.
- API registration, rendering device and performance monitors: Linux Mono docs (`--doctool`) plus applicable GDExtension build/checks.
- Workflow orchestration changes: full matrix.

To demand the complete matrix, pass `--full-matrix`. A job outside the changed-file impact matrix can still be a necessary acceptance gate if explicitly specified by the owner or the repository policy. The rules only select known risk configurations; they are not evidence that an unselected platform is compatible.

## Regression ledger semantics

For each historical incident, look up the **same affected named jobs** in the newest exact-HEAD run. A passing job is reported as retested only when the current workflow has fully completed successfully. An incomplete run, different SHA, missing job, or cancellation keeps the incident open. A retested successful job is not proof of runtime behavior outside what that job exercised.
