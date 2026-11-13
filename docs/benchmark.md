# Benchmark

Measured build and test performance for Loomwright 0.4.0 on the maintainer's
machine (CachyOS, KDE Plasma, 4 jobs). Numbers are a snapshot, not a promise:
always measure before and after a change and keep the evidence with the commit.

## Compiler comparison

The runtime benchmark (`tests/bench_runtime.cpp`, built as `loomwright_bench`)
parses a script and runs a loop of integer arithmetic. Measured on CachyOS
with GCC 16.2.1 and Clang 23.1.4, both at `-O3`, 20 repetitions of 100 000
inner iterations, minimum of eight runs pinned to one core:

| Compiler | Run time per repetition | Binary size |
| --- | --- | --- |
| GCC 16.2.1 | 33.9 ms | 1 145 536 bytes |
| Clang 23.1.4 | 23.5 ms | 1 214 048 bytes |

Clang is about 30 % faster on this workload, so it is the reference compiler
for the release artifacts. The two binaries are about the same size. GCC stays
supported and is built by the CI workflow.

Reproduce it with:

```bash
cmake --preset release-clang && cmake --build build/release-clang -j4
./build/release-clang/loomwright_bench 20
cmake --preset release-gcc && cmake --build build/release-gcc -j4
./build/release-gcc/loomwright_bench 20
```

## Runtime work (0.5.5)

Every expression used to be evaluated by a coroutine, and every statement by
another one. A coroutine frame is allocated on the heap, and the awaiting
frame is handed back to the scheduler and resumed from there, so a script
spent most of its time in the scheduler instead of in the script. The parser
now marks the expressions and the statements that may wait, and the runtime
evaluates everything else without a coroutine:

| Build | Run time per repetition | Allocations per run |
| --- | --- | --- |
| Before | 149 ms | 1 592 000 |
| After | 23.5 ms | 29 974 |

That is 6.3× faster with 53× fewer allocations. The rest of the change is
smaller: the operator is resolved by the parser instead of by comparing
strings while the script runs, the variables and the functions live in hash
maps, the key table is searched with views instead of with strings, and the
lists share their storage through a plain reference count instead of an
atomic one.

Link-time optimization is worth about 13 % of the same benchmark, because the
runtime is split over many translation units and the inliner only sees across
them when the linker gets the bitcode.

## Results at 0.4.0

| Measurement | Before | After |
| --- | --- | --- |
| Cold build (`-j4`, empty ccache) | ~69 s | ~43 s |
| Slowest compile unit (`runtime.cpp`) | 24.3 s | 11.8 s |
| `ctest` total | 39.6 s (serial) | 9.7 s (`-j4`) |
| `picker_integration` | 11.6 s | 5.1 s |
| `gui_integration` | 5.8 s | 4.0 s |

Baseline unit suite: 154 tests in 2.1 s. Current suite: 155 tests in ~2.1 s.
`loomwright_tests` itself was never the bottleneck; the integration scripts and
the cold build were.

## What produced the improvement

### Pass 2 — codegen

`src/runtime.cpp` (~2550 lines) was split into `runtime.cpp`,
`runtime_input.cpp`, `runtime_output.cpp` and `runtime_gui.cpp`, with shared
text helpers in `runtime_internal.hpp`. `tests/test_runtime.cpp` (~1784 lines)
was split into `test_runtime.cpp` and `test_runtime_language.cpp`, with shared
fixtures in `runtime_fixtures.hpp`. Smaller translation units compile faster
and invalidate less when a header changes.

### Pass 3 — test speed

The picker test used a fixed `sleep 1` settle, which approximated readiness.
The real signal is that the picker takes input focus once the overlay is
viewable and its masks are selected, so the poll interval dropped from 100 ms
to 20 ms. The GUI mapping probe dropped from a fixed 3 s to 1.5 s. The two
picker variants got disjoint display ranges (91–95 and 81–85) so CTest can run
four jobs without them fighting over an X server. They still share the
`picker_display` resource lock in this measurement because they use the same
screenshot tool, and spectacle is single-instance. The current suite uses the
broader `graphical_integration` lock; see [testing.md](testing.md).

### Pass 4 — cold build

The build is CPU-bound (195 s of compile edges ÷ 4 cores ≈ the measured wall
time). The test harness and probes now compile at `-O1`
(`loomwright_test_options`) while `loomwright_core` keeps `-O3`. The harness is not
performance critical, and the code it exercises is still optimized.

## Approaches measured and rejected

| Approach | Result |
| --- | --- |
| Unity builds | Rejected: anonymous-namespace collisions across translation units |
| Precompiled header | Rejected: only ~4 % faster, and its dependency tracking produced stale objects |
| `-O2` instead of `-O3` | No measurable difference |
| ccache single-preprocessing mode | No gain |

## How to reproduce

```bash
# Cold build with an empty ccache
ccache -C
cmake --preset release
time cmake --build build/release -j4

# Full test suite
ctest --preset release

# Individual integration tests
ctest --preset release -R picker_integration --output-on-failure
ctest --preset release -R gui_integration --output-on-failure
```

## Guidance

- Measure before and after. A claim without command output is not evidence.
- Keep `loomwright_core` at the release optimisation level; only the harness and
  probes are downgraded.
- Prefer splitting a translation unit over unity builds or PCH. Both of those
  hid breakage (collisions, stale objects) that was worse than the slow build.
- Test-speed changes should remove approximation, not add sleeps. Poll for the
  real readiness signal.
