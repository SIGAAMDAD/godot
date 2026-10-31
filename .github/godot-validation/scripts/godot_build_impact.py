#!/usr/bin/env python3
"""Select relevant Godot CI build gates from the real changed-file set."""
from __future__ import annotations
import argparse
from godot_validation_core import read_json, write_json, validate_snapshot

# Regexes match actual Godot job names without depending on display emojis.
JOBS = {
    "static": r"Static checks / Code style, file formatting, and docs$",
    "linux-minimal": r"Linux / Minimal template \(target=template_release, everything disabled\)$",
    "linux-mono": r"Linux / Editor w/ Mono \(target=editor\)$",
    "linux-double-scu": r"Linux / Editor with doubles and GCC sanitizers ",
    "linux-clang-sanitizers": r"Linux / Editor with clang sanitizers ",
    "linux-tsan": r"Linux / Editor with ThreadSanitizer ",
    "linux-mono-release": r"Linux / Template w/ Mono, release ",
    "linux-mono-debug": r"Linux / Template w/ Mono, debug ",
    "windows-msvc": r"Windows / Editor \(target=editor\)$",
    "windows-clang-cl": r"Windows / Editor w/ clang-cl ",
    "windows-template": r"Windows / Template \(target=template_release\)$",
    "windows-mingw": r"Windows / Template w/ GCC ",
    "android-arm32": r"Android / Template arm32 ",
    "android-arm64": r"Android / Template arm64 ",
    "android-editor": r"Android / Editor \(target=editor\)$",
    "macos-editor": r"macOS / Editor \(target=editor\)$",
    "macos-template": r"macOS / Template \(target=template_release\)$",
    "ios-template": r"iOS / Template \(target=template_release\)$",
    "web-threads": r"Web / Template w/ threads ",
    "web-no-threads": r"Web / Template w/o threads ",
}

ALL_PLATFORMS = tuple(k for k in JOBS if k != "static")
MEMORY = ("linux-minimal", "linux-mono", "linux-double-scu", "linux-clang-sanitizers", "linux-tsan", "windows-msvc", "windows-clang-cl", "windows-mingw", "windows-template", "web-no-threads", "web-threads", "android-arm32", "android-arm64", "macos-template", "ios-template")
RENDERING = ("linux-minimal", "linux-mono", "linux-double-scu", "linux-clang-sanitizers", "linux-tsan", "windows-msvc", "windows-clang-cl", "windows-template", "windows-mingw", "macos-editor", "macos-template", "android-arm64", "android-editor", "ios-template", "web-no-threads")
EDITOR = ("linux-mono", "linux-double-scu", "linux-clang-sanitizers", "linux-tsan", "windows-msvc", "windows-clang-cl", "macos-editor", "android-editor")


def impact(s: dict, full_matrix: bool = False, require_native_runtime: bool = True) -> dict:
    validate_snapshot(s)
    triggers: dict[str, set[str]] = {"static": {"Repository-wide pre-commit checks"}}
    apis = []
    runtime_needed = False

    def add(gates: tuple[str, ...], why: str):
        for gate in gates:
            triggers.setdefault(gate, set()).add(why)

    for path in s["changed_files"]:
        p = path.replace("\\", "/").lower()
        if p == "sconstruct" or p.endswith("/scsub") or p.startswith(".github/actions/"):
            add(ALL_PLATFORMS, f"Engine build system or CI action changed: {path}")
        elif p.startswith("tests/") and p.endswith((".cpp", ".cc", ".h", ".hpp")):
            add(ALL_PLATFORMS, f"Compiled engine test source: {path}")
        elif p.startswith("core/os/memory.") or p.startswith("core/memory_tracking/"):
            add(MEMORY, f"Native allocator/profiler instrumentation: {path}")
        elif p.startswith("servers/rendering/") or p.startswith("drivers/vulkan/") or p.startswith("drivers/d3d12/"):
            add(RENDERING, f"RenderingDevice/backend engine changes: {path}")
        elif p.startswith("editor/") or "editor_render_thread" in p or "editor_thread" in p:
            add(EDITOR, f"Editor, debugger, or render-thread changes: {path}")
        elif p.startswith("platform/windows/"):
            add(("windows-msvc", "windows-clang-cl", "windows-mingw", "windows-template"), f"Windows platform change: {path}")
        elif p.startswith("platform/android/"):
            add(("android-arm32", "android-arm64", "android-editor"), f"Android-specific change: {path}")
        elif p.startswith("platform/"):
            add(("linux-minimal", "linux-mono", "windows-msvc", "android-arm64", "macos-editor", "ios-template", "web-no-threads"), f"Cross-platform system change: {path}")
        elif p.startswith("main/"):
            add(("linux-minimal", "linux-mono", "windows-msvc", "windows-mingw", "macos-editor", "android-arm64", "web-no-threads"), f"Main engine change: {path}")
        elif p.startswith("tests/") or p.startswith("misc/memory_tracking/"):
            add(("linux-minimal", "linux-mono", "windows-msvc", "android-arm64"), f"Engine test integration: {path}")
        elif p.startswith(".github/workflows/"):
            if "android" in p:
                add(("android-arm32", "android-arm64", "android-editor"), f"Android workflow changed: {path}")
            elif "runner" in p or "static" in p:
                add(ALL_PLATFORMS, f"Workflow orchestration changed: {path}")
            elif "windows" in p:
                add(("windows-msvc", "windows-mingw", "windows-clang-cl"), f"Windows workflow changed: {path}")
            elif "linux" in p:
                add(("linux-minimal", "linux-mono", "linux-double-scu"), f"Linux workflow changed: {path}")
        elif p.endswith((".cpp", ".h", ".c", ".mm", ".cc", ".hpp")):
            add(("linux-minimal", "linux-mono", "windows-msvc", "windows-mingw"), f"Unclassified native engine code: {path}")

        if (p.startswith("servers/register_server_types.") or
            p.startswith("servers/rendering/rendering_device.") or
            p.startswith("servers/rendering/rendering_server") or
            p.startswith("main/performance.") or
            "forward_plus_bridge" in p or
            p.startswith("doc/classes/")):
            apis.append(path)
        if ("editor_render_thread" in p or "editor_thread" in p or
            p.startswith("platform/windows/display_server_windows") or
            p.startswith("servers/rendering/rendering_server_default")):
            runtime_needed = True

    if apis:
        add(("linux-mono",), "Public API/class-reference/extension sync must execute through Linux Mono job")
    if full_matrix:
        add(ALL_PLATFORMS, "Explicit full-matrix acceptance requested")
    gates = [
        {"id": gate, "type": "github-job", "job_regex": JOBS[gate], "reasons": sorted(triggers[gate])}
        for gate in JOBS if gate in triggers
    ]
    if apis:
        # Existing CI docs check is a substep of linux-mono; require step-level evidence.
        gates.append({"id": "api-doctool", "type": "github-step", "parent_gate": "linux-mono", "step_regex": r"^Check for class reference updates$", "reasons": ["API-related source changes: " + ", ".join(apis)]})
    if runtime_needed and require_native_runtime:
        gates.append({"id": "windows-native-render-thread-runtime", "type": "external", "reasons": ["Native Windows/Vulkan editor lifetime and frame ownership must be exercised"]})

    return {"schema": "godot-build-impact/v1", "repo": s["repo"], "branch": s["branch"], "head_sha": s["head_sha"], "changed_files": len(s["changed_files"]), "gates": gates, "source": "rule-based from changed files; conservative, review for novel platform flags"}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--snapshot", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--full-matrix", action="store_true")
    p.add_argument("--source-only", action="store_true", help="Do not require native Windows runtime check; scope must permit this")
    a = p.parse_args()
    val = impact(read_json(a.snapshot), a.full_matrix, not a.source_only)
    write_json(a.output, val)
    print(f"IMPACT: {len(val['gates'])} required gates -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
