# Benchmark

Measured build and test performance for Loomwright 0.4.0 on the maintainer's
machine (CachyOS, KDE Plasma, 4 jobs). Numbers are a snapshot, not a promise:
always measure before and after a change and keep the evidence with the commit.

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
