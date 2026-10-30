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