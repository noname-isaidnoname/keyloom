# Keyloom hardening, codegen and build/test speedup plan

Goal (from the objective): improve memory safety, improve codegen (code
generation quality / build code generation), speed up the test suite
(especially the picker integration tests) and speed up cold builds. Work in
passes, with tests and a commit after each pass. Bump the version at the end.

## Baseline (measured on this machine, 4 cores)

| Measurement | Value |
| --- | --- |
| Cold configure + build (`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`, ccache cold) | ~69 s wall (4 jobs) |
| `runtime.cpp` compile | 24.3 s (21 s in "opt and generate") |
| `parser.cpp` / `checker.cpp` compile | 6.2 s / 6.1 s |
| `tests/test_runtime.cpp` compile | 19.0 s |
| `tests/test_language.cpp` compile | 6.8 s |
| `keyloom_tests` (unit suite) | 2.1 s, 154 tests |
| `ctest` total | 39.6 s |
| `picker_integration` | 15.6 s |
| `picker_integration_16` | 15.0 s |
| `gui_integration` | 6.7 s |

## Passes

### Pass 1 — memory safety (done)
* Add a sanitizer build (ASan + UBSan, plus LeakSanitizer) as a CMake option
  and a `sanitize` preset; wire the whole test suite to run under it.
* Fix whatever the sanitizers report in the runtime, scheduler, backends and
  the picker.
* Add hardening compile options for release builds
  (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-fPIE`) behind a
  `KEYLOOM_HARDEN` option (on by default).
* **Fixed**: the coroutine continuation was resumed inline, so the native stack
  grew by one frame per await and a runaway loop overflowed it (reproduced
  under ASan/UBSan and at `-O0`). The scheduler now hands a finished frame's
  awaiter back to its own loop (`Scheduler::defer_resume`), which keeps the
  stack flat at every optimisation level.
* **Fixed**: `_FORTIFY_SOURCE=3` surfaced a `warn_unused_result` on `write()`
  in the scheduler test; the call site now checks the result.
* Commit: "Flatten the coroutine resume chain and run the suite under ASan/UBSan".

### Pass 2 — codegen / build code generation (done)
* Split the two dominant translation units so the build parallelises and
  incremental rebuilds are cheap.
  * `src/runtime.cpp` (2550 lines) → `runtime.cpp` (core), `runtime_input.cpp`,
    `runtime_output.cpp`, `runtime_gui.cpp`. The shared text helpers moved to
    `src/runtime_internal.hpp`.
  * `tests/test_runtime.cpp` (1784 lines) → `test_runtime.cpp` (runtime
    behaviour) and `test_runtime_language.cpp` (language semantics). The
    shared fixtures moved to `tests/runtime_fixtures.hpp`.
* Compile time of the slowest unit: `runtime.cpp` 24.3 s → 11.8 s,
  `test_runtime.cpp` 19.0 s → 8.6 s. The other units compile in parallel.
* Commit: "Split the runtime and test translation units".

### Pass 3 — test speed
* Cut the fixed sleeps and slow polling in `tests/picker_integration.sh` and
  `tests/gui_integration.sh`; replace polling with tight event waits.
* Make the two picker variants run concurrently instead of behind a resource
  lock.
* Commit: "Speed up the picker and GUI integration tests".

### Pass 4 — cold build speed
* Unity build for the small core sources, precompiled header if it pays off,
  `-O2` where `-O3` only costs build time, ccache/mold already in place.
* Measure a cold build before/after and record the numbers.
* Commit: "Speed up cold builds".

### Pass 5 — wrap up
* Documentation updates (README / docs.cpp where user-visible), version bump,
  remove this plan file and any temporary files.
