# Godot memory-tracking diagnostics

These tests are deliberately outside `tests/`: Godot's `tests/SCsub` recursively compiles
`tests/**/*.cpp` and a standalone `main()` there would conflict with the engine test runner.

Native allocation ledger smoke test (from repository root):

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -pthread -I. \
  core/memory_tracking/memory_tracker.cpp \
  misc/memory_tracking/memory_tracker_standalone.cpp -o memory_tracker_test
./memory_tracker_test
```

Reference benchmark data-contract check:

```sh
python misc/memory_tracking/test_editor_thread_benchmark.py
```

The benchmark comparator checks reported modes, captures, frame order, build/hardware/
viewport controls, and computes A/B timing statistics. The bundled CSV files are synthetic
**format fixtures**, not actual measurements from a Windows editor. Actual editor A/B
results require instrumenting and running the editor on Windows, with a distinct render
worker confirmed during the separate-thread run. These tests do not verify Godot runtime
integration, GPU features, or UI behavior.
## Windows editor A/B capture (experimental)

**Do not compare the bundled synthetic fixtures as performance data.** Rebuild the same
`target=editor` executable from this exact branch, then launch the same project,
same viewport size/scene, driver settings, resolution, and input workload in both
modes. This fork currently restricts experimental threaded editor mode to Windows
Vulkan and still requires hardware runtime validation before treating it as stable.

In PowerShell, for both runs set:

```powershell
$env:GODOT_BENCH_HOST_ID = "Arc140V_test_machine"
$env:GODOT_BENCH_WORKLOAD_ID = "editor_terrain_city_camera_01"
$env:GODOT_BENCH_WORKLOAD_SHA256 = "<sha256-hex-of-fixed-workload-packet>"
```

Use a real 64-digit SHA-256 string for the workload packet. Choose *distinct* output
paths, close the editor cleanly to flush the logs, and repeat both runs with the same
editor executable and project. Example (replace executable and project path):

```powershell
godot.windows.editor.exe --path C:\\Project --editor --render-thread safe --editor-render-thread-benchmark C:\\bench\\safe.csv
godot.windows.editor.exe --path C:\\Project --editor --render-thread separate --editor-render-thread-experimental --editor-render-thread-benchmark C:\\bench\\separate.csv
python misc/memory_tracking/editor_thread_benchmark_compare.py --safe C:\\bench\\safe.csv --separate C:\\bench\\separate.csv
```

The capture records actual editor-main-loop duration and time spent synchronizing the
RenderingServer; effective mode is determined from the live server-thread identity.
GPU frame times, worker-thread execution and input-to-display latency are currently
unavailable in the trace and are left empty. A valid frame-time comparison is **not**
proof that separate-thread startup, render lifecycle or GPU work is correct.
