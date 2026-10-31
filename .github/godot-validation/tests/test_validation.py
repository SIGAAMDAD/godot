"""Negative controls, historical incident fixtures, and exact-SHA acceptance."""
from __future__ import annotations
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / "scripts"))
from godot_validation_core import validate_snapshot, triage, diagnose_job, EvidenceError
from godot_build_impact import impact
from godot_acceptance import evaluate
from godot_regression_ledger import ledger
from godot_api_sync import audit
from godot_ci_collect import select_log_excerpt, GHCollector
from godot_ci_workflow_audit import audit_runner
from godot_build_manifest import manifest
from godot_preflight import perform

HEAD = "7240975b71be44cdc49330848ff977ef10b4a194"
OLD = "528a9282120f31e7009c3d5af07c8a0081da4dec"
BASE = "a41e230fd359c84944a8d58ef78dcfb5975cba07"
STATIC = "📊 Static checks / Code style, file formatting, and docs"
LINUX_MINIMAL = "🐧 Linux / Minimal template (target=template_release, everything disabled)"
LINUX_MONO = "🐧 Linux / Editor w/ Mono (target=editor)"
ANDROID = "🤖 Android / Template arm64 (target=template_debug, arch=arm64)"


def j(id, name, conclusion="success", log="", step=None, status="completed"):
    steps = [{"name": step, "status": "completed", "conclusion": "failure"}] if step else []
    return {"id": id, "name": name, "status": status, "conclusion": conclusion, "steps": steps, "log": log}


def snapshot(files=None, runs=None):
    return {"schema": "godot-ci-snapshot/v1", "repo": "SIGAAMDAD/godot", "branch": "feature/memory-profiler-editor-render-thread", "head_sha": HEAD, "base_sha": BASE, "changed_files": files or [], "runs": runs or []}


def run(id, sha, jobs, status="completed", conclusion="success", complete=True):
    return {"id": id, "head_sha": sha, "status": status, "conclusion": conclusion, "jobs_complete": complete, "total_job_count": len(jobs) if complete else None, "jobs": jobs}


def preflight(sha=HEAD, status="passed"):
    return {"schema": "godot-preflight/v1", "repo": "SIGAAMDAD/godot", "head_sha": sha, "status": status, "stages": [{"name": "source-checkout", "status": "passed"}, {"name": "exact-commit-clang-format", "status": "passed"}, {"name": "godot-prek", "status": "passed"}, {"name": "formatter-worktree-clean", "status": "passed"}]}


class ValidationTests(unittest.TestCase):
    def test_duplicate_run_and_invalid_sha_fail(self):
        r = run(1, HEAD, [j(1, STATIC)])
        with self.assertRaises(EvidenceError):
            validate_snapshot(snapshot(runs=[r, r]))
        broken = snapshot(runs=[r]); broken["head_sha"] = "abc"
        with self.assertRaises(EvidenceError):
            validate_snapshot(broken)

    def test_aggregate_android_linux_same_compiler_error(self):
        a = j(5, ANDROID, "failure", "tests/servers/rendering/test_editor_render_thread_contracts.cpp:63:11: error: variable 'acknowledged' set but not used [-Werror,-Wunused-but-set-variable]", "Compilation")
        b = j(6, LINUX_MINIMAL, "failure", "tests/servers/rendering/test_editor_render_thread_contracts.cpp:72:18: error: variable 'completed' set but not used [-Werror=unused-but-set-variable]", "Compilation")
        s = snapshot(runs=[run(2, OLD, [a,b], conclusion="failure")]); t = triage(s)
        defects = [it for it in t["incidents"] if it["category"] == "unused-variable"]
        self.assertEqual(len(defects), 1)
        self.assertEqual(len(defects[0]["observations"]), 2)

    def test_different_error_kinds_not_merged(self):
        x = j(3, LINUX_MINIMAL, "failure", "core/os/memory.cpp:186:52: error: pointer 'p_memory' may be used after 'realloc' [-Werror=use-after-free]\neditor/debugger/script_editor_debugger.cpp:333:41: error: declaration of 'data' shadows a member [-Werror=shadow]", "Compilation")
        issues = diagnose_job(x)
        self.assertEqual({i["category"] for i in issues}, {"memory-use-after-free", "shadow-warning"})

    def test_msvc_c2220_wrapper_prefers_warning(self):
        x = j(4, "🏁 Windows / Template (target=template_release)", "failure", "platform\\windows\\display_server_windows.cpp(1990): error C2220: the following warning is treated as an error\nplatform\\windows\\display_server_windows.cpp(1990): warning C4456: declaration of 'result' hides previous local declaration", "Compilation")
        issues = diagnose_job(x)
        self.assertEqual(len(issues), 1)
        self.assertEqual(issues[0]["category"], "shadow-warning")

    def test_precise_prek_hook_triage(self):
        log = "- hook id: clang-format\n- files were modified by this hook\n- hook id: copyright-headers\n- files were modified by this hook\n- hook id: file-format"
        x = j(1, STATIC, "failure", log, "Style checks via prek")
        names = {i["signature"] for i in diagnose_job(x)}
        self.assertEqual(names, {"style-hook|clang-format", "style-hook|copyright-headers", "style-hook|file-format"})

    def test_gradle_separate_from_native_compiler(self):
        x = j(1, ANDROID, "failure", "* What went wrong:\nCould not resolve org.jetbrains.kotlin:kotlin-gradle-plugin:2.1.21.\nCould not find org.jetbrains.kotlin:kotlin-gradle-plugins-bom:2.1.21.", "Generate Godot templates")
        problems = diagnose_job(x)
        self.assertEqual(len(problems), 1)
        self.assertEqual(problems[0]["category"], "dependency-resolution")

    def test_docs_xml_paths_and_failure(self):
        log = "\n".join(f"diff --git a/doc/classes/{name}.xml b/doc/classes/{name}.xml" for name in ("@GlobalScope", "Performance", "RenderingDevice", "RenderingServer", "Viewport"))
        x = j(1, LINUX_MONO, "failure", log, "Check for class reference updates")
        s = snapshot(["servers/rendering/rendering_device.cpp"], [run(1, HEAD, [x], conclusion="failure")]);
        docs = audit(s)
        self.assertEqual(len(docs["checks"][0]["files"]), 5)
        self.assertEqual(docs["status"], "failure")
        self.assertEqual([v["category"] for v in triage(s)["incidents"]], ["api-doc-sync"])
        im = impact(s)
        result = evaluate(s, im, preflight())
        self.assertFalse(result["accepted"])
        self.assertIn("api-doctool", {f["gate"] for f in result["missing_or_failed"]})

    def test_green_minimal_snapshot_can_be_accepted(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        a = evaluate(s, impact(s), preflight())
        self.assertTrue(a["accepted"], a["missing_or_failed"])

    def test_weak_self_certified_preflight_is_rejected(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        false_report = {"schema": "godot-preflight/v1", "repo": s["repo"], "head_sha": HEAD, "status": "passed", "stages": [{"name": "unchecked", "status": "passed"}, {"name": "claim", "status": "passed"}]}
        self.assertFalse(evaluate(s, impact(s), false_report)["accepted"])

    def test_compiled_test_sources_trigger_full_platform_matrix(self):
        s = snapshot(["tests/servers/rendering/test_editor_render_thread_contracts.cpp"])
        result = impact(s)
        ids = {g["id"] for g in result["gates"]}
        self.assertTrue({"windows-msvc", "linux-minimal", "android-arm32", "web-threads", "macos-template", "ios-template"}.issubset(ids))

    def test_build_system_file_triggers_all_platforms(self):
        ids = {g["id"] for g in impact(snapshot(["core/SCsub"]))["gates"]}
        self.assertIn("android-arm32", ids)
        self.assertIn("web-threads", ids)

    def test_no_preflight_rejected(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        a = evaluate(s, impact(s))
        self.assertFalse(a["accepted"])
        self.assertEqual(a["missing_or_failed"][0]["gate"], "local-preflight")

    def test_blocked_preflight_has_distinct_status(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        blocked = {"schema": "godot-preflight/v1", "repo": s["repo"], "head_sha": HEAD, "status": "blocked", "stages": [{"name": "source-checkout", "status": "blocked"}]}
        a = evaluate(s, impact(s), blocked)
        self.assertEqual(a["missing_or_failed"][0]["status"], "blocked")

    def test_stale_preflight_rejected(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        a = evaluate(s, impact(s), preflight(OLD))
        self.assertEqual(a["missing_or_failed"][0]["status"], "stale")

    def test_missing_older_sha_does_not_count(self):
        s = snapshot(runs=[run(1, OLD, [j(2, STATIC)])])
        a = evaluate(s, impact(s), preflight())
        self.assertFalse(a["accepted"])
        self.assertEqual(a["missing_or_failed"][0]["gate"], "ci-run")

    def test_cancelled_run_never_passes(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)], conclusion="cancelled")])
        a = evaluate(s, impact(s), preflight())
        self.assertFalse(a["accepted"])

    def test_forged_complete_job_count_rejected(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)])])
        s["runs"][0]["total_job_count"] = 20
        with self.assertRaisesRegex(EvidenceError, "total_job_count"):
            validate_snapshot(s)

    def test_incomplete_job_pages_block(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)], complete=False)])
        a = evaluate(s, impact(s), preflight())
        self.assertFalse(a["accepted"])
        self.assertEqual(a["missing_or_failed"][0]["status"], "incomplete-job-list")

    def test_pending_job_blocks(self):
        s = snapshot(runs=[run(1, HEAD, [j(2, STATIC)], status="in_progress", conclusion=None)])
        a = evaluate(s, impact(s), preflight())
        self.assertFalse(a["accepted"])

    def test_render_thread_requires_native_external_evidence(self):
        s = snapshot(["platform/windows/display_server_windows.cpp"], [run(1, HEAD, [j(2, STATIC)])])
        normal = impact(s)
        self.assertIn("windows-native-render-thread-runtime", {x["id"] for x in normal["gates"]})
        source = impact(s, require_native_runtime=False)
        self.assertNotIn("windows-native-render-thread-runtime", {x["id"] for x in source["gates"]})

    def test_fake_external_runtime_evidence_cannot_pass(self):
        s = snapshot(["platform/windows/display_server_windows.cpp"], [run(1, HEAD, [j(2, STATIC)])])
        s["external_checks"] = {"windows-native-render-thread-runtime": {"head_sha": HEAD, "status": "passed", "evidence": "I tested it"}}
        a = evaluate(s, impact(s), preflight())
        self.assertEqual(next(g["status"] for g in a["gates"] if g["gate"] == "windows-native-render-thread-runtime"), "unverified")

    def test_native_gui_runtime_evidence_requires_specific_protocol(self):
        s = snapshot(["platform/windows/display_server_windows.cpp"], [run(1, HEAD, [j(2, STATIC)])])
        record = {"head_sha": HEAD, "status": "passed", "evidence": "win-native-run-321", "artifact_sha256": "a"*64, "native_windows": True, "backend": "Vulkan", "requested_mode": "separate", "effective_mode": "separate", "verified_frames": 60, "exit_code": 0, "validation_error_count": 0}
        s["external_checks"] = {"windows-native-render-thread-runtime": record}
        a = evaluate(s, impact(s), preflight())
        self.assertEqual(next(g["status"] for g in a["gates"] if g["gate"] == "windows-native-render-thread-runtime"), "passed")
        record["verified_frames"] = 59
        a = evaluate(s, impact(s), preflight())
        self.assertEqual(next(g["status"] for g in a["gates"] if g["gate"] == "windows-native-render-thread-runtime"), "unverified")

    def test_allocator_impact_is_cross_platform(self):
        result = impact(snapshot(["core/os/memory.cpp"]))
        ids = {g["id"] for g in result["gates"]}
        self.assertTrue({"linux-minimal", "windows-mingw", "windows-msvc", "web-no-threads", "android-arm64", "macos-template", "linux-double-scu"}.issubset(ids))

    def test_regression_history_remains_open_on_incomplete_run(self):
        failed = j(1, LINUX_MINIMAL, "failure", "core/os/memory.cpp:186:52: error: pointer may be used after realloc [-Werror=use-after-free]", "Compilation")
        current = j(2, LINUX_MINIMAL, "success")
        s = snapshot(runs=[run(3, OLD, [failed], conclusion="failure"), run(4, HEAD, [current], status="in_progress", conclusion=None)])
        report = ledger(s)
        self.assertEqual(report["open_count"], 1)
        self.assertEqual(report["rows"][0]["current_status"], "not-closed")
        s["runs"][-1]["status"] = "completed";s["runs"][-1]["conclusion"] = "success"
        self.assertEqual(ledger(s)["closed_count"], 1)

    def test_manifest_rejects_stale_sources(self):
        s = snapshot()
        with tempfile.TemporaryDirectory() as d:
            x = Path(d)/"report.json"
            x.write_text(json.dumps({"schema": "godot-ci-triage/v1", "repo": s["repo"], "head_sha": OLD}))
            with self.assertRaisesRegex(ValueError, "not bound"):
                manifest(s, {"triage": str(x)})

    def test_workflow_source_recovers_all_six_platform_dependencies(self):
        # This test uses an actual root workflow shape, without relying on external files.
        source = """concurrency:
  group: ${{ github.workflow }}|${{ github.ref_name }}
  cancel-in-progress: true
jobs:
  static-checks:
    uses: ./.github/workflows/static_checks.yml
  android-build:
    needs: static-checks
    uses: ./.github/workflows/android_builds.yml
  windows-build:
    needs: static-checks
    uses: ./.github/workflows/windows_builds.yml
"""
        report = audit_runner(source, branch_protected=False)
        self.assertEqual(report["platform_build_job_count"], 2)
        self.assertEqual({r["id"] for r in report["risks"]}, {"same-branch-cancel", "static-blocks-all-builds", "no-required-branch-checks"})

    def test_workflow_unrecognized_format_is_rejected(self):
        with self.assertRaises(ValueError):
            audit_runner("name: workflow\njobs: {}")

    def test_collector_paginates_jobs_and_preserves_exact_head(self):
        repo, branch = "SIGAAMDAD/godot", "feature/test"
        ci = GHCollector.__new__(GHCollector)
        response = {
            f"repos/{repo}/branches/feature%2Ftest": {"commit": {"sha": HEAD}},
            f"repos/{repo}/compare/{BASE}...{HEAD}": {"status": "ahead", "total_commits": 2, "files": [{"filename": "core/os/memory.cpp"}]},
            f"repos/{repo}/actions/runs?branch=feature%2Ftest&per_page=100": {"workflow_runs": [{"id": 8, "event": "push", "head_sha": HEAD, "status": "completed", "conclusion": "failure"}]},
            f"repos/{repo}/actions/runs/8/jobs?per_page=100": {"total_count": 2, "jobs": [{"id": 20, "name": STATIC, "status": "completed", "conclusion": "success", "steps": []}]},
            f"repos/{repo}/actions/runs/8/jobs?per_page=100&page=2": {"total_count": 2, "jobs": [{"id": 21, "name": LINUX_MINIMAL, "status": "completed", "conclusion": "failure", "steps": [{"name": "Compilation", "status": "completed", "conclusion": "failure"}]}]},
            f"repos/{repo}/actions/jobs/21/logs": "core/os/memory.cpp:186:52: error: pointer may be used after realloc [-Werror=use-after-free]",
        }
        ci.request = lambda path, as_json=True: response[path]
        actual = ci.collect(repo, branch, BASE, max_runs=5, max_logs=2)
        self.assertTrue(actual["runs"][0]["jobs_complete"])
        self.assertEqual(len(actual["runs"][0]["jobs"]), 2)
        self.assertEqual(actual["head_sha"], HEAD)
        self.assertEqual(len(triage(actual)["incidents"]), 1)

    def test_collector_300_file_cap_is_blocked(self):
        ci = GHCollector.__new__(GHCollector)
        def fetch(path, as_json=True):
            if "/branches/" in path: return {"commit": {"sha": HEAD}}
            return {"total_commits": 50, "files": [{"filename": f"core/file_{i}.cpp"} for i in range(300)]}
        ci.request = fetch
        with self.assertRaisesRegex(RuntimeError, "300-file"):
            ci.collect("SIGAAMDAD/godot", "feature/test", BASE)

    def test_collector_truncated_compare_is_blocked(self):
        ci = GHCollector.__new__(GHCollector)
        ci.request = lambda path, as_json=True: ({"commit": {"sha": HEAD}} if "/branches/" in path else {"too_large": True, "total_commits": 251, "files": [{"filename": "core/x.cpp"}]})
        with self.assertRaises(RuntimeError):
            ci.collect("SIGAAMDAD/godot", "feature/test", BASE)

    def test_log_excerpt_is_bounded(self):
        logs = "\n".join(["noise"] * 300 + ["core/os/memory.cpp:9:1: error: failure"] + ["noise"] * 300)
        excerpt = select_log_excerpt(logs)
        self.assertIn("core/os/memory.cpp", excerpt)
        self.assertLessEqual(len(excerpt.splitlines()), 160)


class LiveGitObjectPreflightTests(unittest.TestCase):
    def _git(self, root, *args):
        result = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True)
        if result.returncode:
            self.fail(result.stderr)
        return result.stdout.strip()

    def test_real_temp_worktree_preflight_pass_and_missing_prek_fails(self):
        from test_godot_format_gate import POLICY, FAKE_CLANG
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)/"godot";root.mkdir()
            self._git(root, "init", "-q")
            self._git(root, "config", "user.name", "QA")
            self._git(root, "config", "user.email", "qa@example.invalid")
            (root/"SConstruct").write_text("# fixture\n")
            (root/".clang-format").write_text("BasedOnStyle: LLVM\n")
            (root/".pre-commit-config.yaml").write_text(POLICY)
            (root/"misc/utility").mkdir(parents=True)
            (root/"misc/utility/clang_format_glsl.yml").write_text("BasedOnStyle: LLVM\n")
            self._git(root, "add", ".")
            self._git(root, "commit", "-q", "-m", "build: fixture")
            base = self._git(root, "rev-parse", "HEAD")
            (root/"core").mkdir()
            (root/"core/good.cpp").write_text("int result = 2;\n")
            self._git(root, "add", ".")
            self._git(root, "commit", "-q", "-m", "fix(core): fixture")
            head = self._git(root, "rev-parse", "HEAD")
            formatter = Path(d)/"clang-format"
            formatter.write_text(FAKE_CLANG);formatter.chmod(0o755)
            prek = Path(d)/"prek"
            prek.write_text("#!/bin/sh\n[ \"$1\" = run ] && exit 0\nexit 7\n");prek.chmod(0o755)
            result = perform(root, "SIGAAMDAD/godot", base, head, str(formatter), str(prek))
            self.assertEqual(result["status"], "passed", result["stages"])
            missing = perform(root, "SIGAAMDAD/godot", base, head, str(formatter), str(Path(d)/"not-exists"))
            self.assertNotEqual(missing["status"], "passed")


if __name__ == "__main__":
    unittest.main()
