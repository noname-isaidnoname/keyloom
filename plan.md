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

### Pass 3 — test speed (done)
* The picker integration test spent 7.3 s of its 11.6 s waiting on fixed
  sleeps. The `sleep 1` after the overlay appeared is now a wait for the real
  readiness signal: the picker takes the input focus once the overlay is
  viewable and its event masks are selected.
* Poll loops now use a 20 ms interval instead of 100 ms, and the loop bounds
  were rescaled to keep the same overall timeout.
* The GUI mapping probe ran for a fixed three seconds; it now runs for as long
  as the harness needs to switch the keyboard layout twice.
* The two picker variants use disjoint X display ranges and the test preset
  runs ctest with four jobs, so the independent integration tests overlap.
  The variants keep their resource lock: the host's screenshot tool
  (spectacle) is single-instance, so two pickers must not capture the desktop
  at the same time.
* `ctest -j4`: 31.5 s → 9.7 s. `picker_integration` 11.6 s → 5.1 s,
  `gui_integration` 5.8 s → 4.0 s.
* Commit: "Speed up the picker and GUI integration tests".

### Pass 4 — cold build speed (done)
* Measured the cold build (empty ccache, `-j4`, Release):
  69 s → 48 s after the split in pass 2 → **43 s**.
* The build is CPU bound: 195 s of compile edges over four cores, and the
  schedule is already at its lower bound (195/4 = 48.8 s ≈ the measured
  wall time). Linking is 0.2 s in total, and ccache only adds ~3 s on a cold
  cache.
* The test harness and the probes are built at `-O1`. The code they exercise
  lives in `keyloom_core`, which keeps the release optimisation level, so the
  behaviour under test does not change.
* Measured and rejected:
  * *Unity build*: the sources use anonymous namespaces for their file-local
    helpers, so the merged units redefine `atom()`, `is_space()` and
    `lower_ascii()`. Fixing that would mean renaming helpers across the tree
    for a small gain.
  * *Precompiled header*: only 4 % faster and the dependency tracking was not
    reliable — an object did not change when a header inside the PCH did.
    A stale build is much worse than a slow one.
  * *`-O2` instead of `-O3`*: 22.6 s versus 22.8 s over the heaviest units,
    and `-O3` even produced slightly smaller objects.
  * *`CCACHE_RUN_SECOND_CPP=false`*: no gain (50 s versus 49 s).
* Commit: "Speed up cold builds".

### Pass 5 — wrap up
* Documentation updates (README / docs.cpp where user-visible), version bump,
  remove this plan file and any temporary files.
