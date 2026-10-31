"""Fail-closed Git-content clang-format pre-push contract tests."""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from godot_clang_format_gate import GateError, verify

POLICY = r"""exclude: |
  (?x)^(
    .*thirdparty/.*|
    .*-(dll|dylib|so)_wrap\.[ch]|
    platform/android/java/editor/src/main/java/com/android/.*|
    platform/android/java/lib/src/main/java/com/google/.*
  )$
repos:
  - repo: https://github.com/pre-commit/mirrors-clang-format
    rev: v22.1.5
    hooks:
      - id: clang-format
        files: \.(c|h|cpp|hpp|cc|hh|cxx|hxx|m|mm|inc|java)$
      - id: clang-format
        name: clang-format-glsl
        files: \.glsl$
        args: [-style=file:misc/utility/clang_format_glsl.yml]
"""
FAKE_CLANG = r'''#!/usr/bin/env python3
import pathlib
import sys
if "--version" in sys.argv:
    print("clang-format version 22.1.5")
    sys.exit(0)
path = pathlib.Path(sys.argv[-1])
content = path.read_text()
if "REQUIRE_NESTED" in content and not (path.parent / ".clang-format").exists():
    print("nested style missing", file=sys.stderr)
    sys.exit(1)
if "HARD_ERROR" in content:
    print("formatter could not parse", file=sys.stderr)
    sys.exit(1)
if "UNFORMATTED" in content:
    print('<?xml version="1.0"?><replacements xml:space="preserve" incomplete_format="false"><replacement offset="0" length="1">x</replacement></replacements>')
else:
    print('<?xml version="1.0"?><replacements xml:space="preserve" incomplete_format="false"></replacements>')
'''


def git(repo: Path, *cmd: str) -> str:
    p = subprocess.run(["git", "-C", str(repo), *cmd], text=True, capture_output=True)
    if p.returncode:
        raise AssertionError(p.stderr)
    return p.stdout.strip()


class GodotClangGateTests(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory()
        self.addCleanup(self.work.cleanup)
        self.repo = Path(self.work.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.name", "QA")
        git(self.repo, "config", "user.email", "qa@example.test")
        self.write("SConstruct", "# source fixture\n")
        self.write(".clang-format", "BasedOnStyle: LLVM\n")
        self.write(".pre-commit-config.yaml", POLICY)
        self.write("misc/utility/clang_format_glsl.yml", "BasedOnStyle: LLVM\n")
        self.commit("chore(build): baseline Godot formatting policy")
        self.base = git(self.repo, "rev-parse", "HEAD")
        self.formatter = Path(self.work.name) / "clang-format"
        self.formatter.write_text(FAKE_CLANG)
        self.formatter.chmod(0o755)

    def write(self, path, content):
        p = self.repo / path
        p.parent.mkdir(exist_ok=True, parents=True)
        p.write_text(content)

    def commit(self, title):
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", title)
        return git(self.repo, "rev-parse", "HEAD")

    def check(self, base=None, head="HEAD"):
        return verify(self.repo, base or self.base, head, str(self.formatter))

    def test_clean_cpp_and_glsl_pass(self):
        self.write("servers/rendering/render.cpp", "int work() {return 1;}\n")
        self.write("servers/shaders/sky.glsl", "void main() {}\n")
        self.commit("fix(render): update code")
        report = self.check()
        self.assertTrue(report["passed"])
        self.assertEqual(report["formatted_files_checked"], 2)
        self.assertEqual(report["commits"], 1)

    def test_bad_cpp_blocks_push(self):
        self.write("platform/windows/window.cpp", "UNFORMATTED\n")
        self.commit("fix(windows): update source")
        report = self.check()
        self.assertFalse(report["passed"])
        self.assertIn("window.cpp", report["failures"][0])

    def test_intermediate_bad_commit_blocks_even_when_tip_fixed(self):
        self.write("core/x.cpp", "UNFORMATTED\n")
        self.commit("fix(core): add rough source")
        self.write("core/x.cpp", "clean source\n")
        self.commit("style(core): apply formatter")
        self.assertFalse(self.check()["passed"])
        self.assertTrue(verify(self.repo, self.base, "HEAD", str(self.formatter), check_each=False)["passed"])

    def test_missing_binary_blocks_push(self):
        self.write("core/x.cpp", "clean\n")
        self.commit("fix(core): improve code")
        with self.assertRaisesRegex(GateError, "unavailable"):
            verify(self.repo, self.base, "HEAD", "/nonexistent/clang-format")

    def test_reject_different_formatter_version(self):
        self.write("core/x.cpp", "clean\n")
        self.commit("fix(core): improve code")
        self.write(".pre-commit-config.yaml", POLICY.replace("v22.1.5", "v19.1.0"))
        self.commit("build(core): change formatter")
        with self.assertRaisesRegex(GateError, "pins clang-format 19.1.0"):
            self.check()

    def test_skip_excluded_thirdparty(self):
        self.write("thirdparty/fake/bad.cpp", "UNFORMATTED\n")
        self.write("core/good.cpp", "good code\n")
        self.commit("fix(core): update third-party dependency")
        result = self.check()
        self.assertTrue(result["passed"], result["failures"])
        self.assertEqual(result["formatted_files_checked"], 1)

    def test_policy_change_requires_full_tree(self):
        self.write("core/untouched.cpp", "UNFORMATTED\n")
        self.commit("feat(core): add code")
        base = git(self.repo, "rev-parse", "HEAD")
        self.write(".clang-format", "BasedOnStyle: Google\n")
        self.commit("build(core): update code format policy")
        result = self.check(base=base)
        self.assertFalse(result["passed"])
        self.assertIn("untouched.cpp", result["failures"][0])

    def test_nested_commit_style_is_respected(self):
        self.write("modules/custom/.clang-format", "BasedOnStyle: Google\n")
        self.write("modules/custom/module.cpp", "REQUIRE_NESTED\n")
        self.commit("fix(module): update module formatting")
        report = self.check()
        self.assertTrue(report["passed"], report["failures"])

    def test_local_pre_push_hook_rejects_bad_commit(self):
        self.write(".github/scripts/godot_clang_format_gate.py", (ROOT / "scripts/godot_clang_format_gate.py").read_text())
        self.commit("build(godot): install pre-push gate")
        base = git(self.repo, "rev-parse", "HEAD")
        hook = self.repo / ".git/hooks/pre-push"
        hook.write_bytes((ROOT / "assets/godot-pre-push-hook").read_bytes())
        hook.chmod(0o755)
        self.write("core/crash.cpp", "UNFORMATTED\n")
        head = self.commit("fix(core): add bad format")
        env = os.environ.copy()
        env["GODOT_CLANG_FORMAT"] = str(self.formatter)
        result = subprocess.run(
            ["sh", str(hook)], cwd=self.repo,
            input=f"refs/heads/feature {head} refs/heads/feature {base}\n",
            text=True, capture_output=True, env=env,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GODOT CLANG-FORMAT GATE FAILED", result.stderr)

    def test_local_pre_push_hook_accepts_good_commit(self):
        self.write(".github/scripts/godot_clang_format_gate.py", (ROOT / "scripts/godot_clang_format_gate.py").read_text())
        self.commit("build(godot): install pre-push gate")
        base = git(self.repo, "rev-parse", "HEAD")
        hook = self.repo / ".git/hooks/pre-push"
        hook.write_bytes((ROOT / "assets/godot-pre-push-hook").read_bytes())
        hook.chmod(0o755)
        self.write("core/good.cpp", "int clean;\n")
        head = self.commit("feat(core): implement clean code")
        env = os.environ.copy()
        env["GODOT_CLANG_FORMAT"] = str(self.formatter)
        result = subprocess.run(
            ["sh", str(hook)], cwd=self.repo,
            input=f"refs/heads/feature {head} refs/heads/feature {base}\n",
            text=True, capture_output=True, env=env,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("GODOT CLANG-FORMAT PASS", result.stdout)

    def test_no_matching_source_still_requires_formatter(self):
        self.write("docs/README.md", "A change\n")
        self.commit("docs(core): update docs")
        self.assertTrue(self.check()["passed"])
        with self.assertRaisesRegex(GateError, "unavailable"):
            verify(self.repo, self.base, "HEAD", "/missing/formatter")

    def test_errored_formatter_blocks_push(self):
        self.write("core/crash.cpp", "HARD_ERROR\n")
        self.commit("fix(core): update source")
        with self.assertRaisesRegex(GateError, "formatter could not parse"):
            self.check()

    def test_style_change_requires_repository_config(self):
        self.write("core/x.cpp", "normal\n")
        self.commit("fix(core): initial")
        self.write(".pre-commit-config.yaml", "invalid config\n")
        self.commit("build(core): delete format policy")
        with self.assertRaisesRegex(GateError, "pinned mirrors-clang-format"):
            self.check()


if __name__ == "__main__":
    unittest.main()
