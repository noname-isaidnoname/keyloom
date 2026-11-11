# Testing

For release packaging and Podman CI testing, see [releases.md](releases.md).

## Layout

| Path | What it does |
| --- | --- |
| `tests/harness.hpp` | Mini test runner: `TEST_CASE`, `CHECK_TRUE`, `CHECK_EQ`, `CHECK_THROWS`, `capture_output`, `run_all` |
| `tests/test_main.cpp` | Unit-test entry point |
| `tests/test_language.cpp` | Lexer, parser, checker and `Value` formatting tests |
| `tests/test_runtime.cpp` | Runtime behaviour: statements, holds, dispatch, subprocess, evdev recovery |
| `tests/test_runtime_language.cpp` | Runtime language features (lists, functions, flow) |
| `tests/test_backends.cpp` | Output backends and `UInputDevice` setup/buffer behaviour |
| `tests/test_mousepos.cpp` | Picker helpers (coordinate formatting, tooltip placement, screenshot commands) and CLI argument/exit-code tests |
| `tests/test_scheduler.cpp` | Scheduler tasks, timers, fds, cancellation, shutdown, flat-stack invariant |
| `tests/runtime_fixtures.hpp` | Shared fixtures for the runtime tests |
| `tests/evdev_handoff.cpp` | Real evdev/uinput handoff test (skips when isolation cannot be confirmed) |
| `tests/picker_integration.sh` | End-to-end `--get-mouse-pos` test on a private X server |
| `tests/gui_integration.sh` | Real GUI/panel/timer/dry-run test on a private X server |
| `tests/uinput_fault_integration.sh` | Real `/dev/uinput` ioctl fault injection via `LD_PRELOAD` |
| `tests/uinput_ioctl_fault_shim.c` | The ioctl fault-injection shim |
| `tests/*_probe.cpp` | Deterministic fault and lifecycle probes used by the integration scripts |

The unit tests are one executable, `keyloom_tests`. The integration tests are
separate executables and shell scripts registered with CTest.

## Running

```bash
make                      # configure, build (Release) and run the whole suite
make test                 # same effect: configure, build Release and run ctest
ctest --preset release    # run CTest directly (4 jobs)
ctest --preset sanitize   # run under ASan/UBSan
```

Or build a preset and run the binary directly:

```bash
cmake --build build/release -j$(nproc)
./build/release/keyloom_tests
```

`keyloom_tests` prints one `ok   <name>` line per test and a final
`N/N tests passed` summary. `run_all(filter)` accepts a substring filter.
The release and sanitizer CTest presets print failing test output. GitHub CI
installs Xvfb, xdotool and ImageMagick to exercise the headless GUI and picker tests;
device-dependent tests can still skip on hosted runners without `/dev/uinput`.
Native Wayland integration starts an isolated headless Weston compositor when
Weston and the optional GTK3/wayland-client build dependencies are available.

### CTest tests

| Test | What it covers | Skip code |
| --- | --- | --- |
| `keyloom_tests` | All unit tests | — |
| `evdev_handoff` | Split evdev/uinput handoff against real kernel devices | 77 |
| `picker_integration` | `--get-mouse-pos` on a private X server, 24-bit | 77 |
| `picker_integration_16` | Same, 16-bit (only when Xvfb is found) | 77 |
| `gui_integration` | GUI panels, layout switches, close/destroy/server-loss | 77 |
| `wayland_integration` | Native panels and compositor selection on headless Weston | 77 |
| `uinput_fault_integration` | ioctl failure at every named `UInputDevice` setup step | 77 |

Exit code 77 means "skipped" (missing X server, xdotool, screenshot tool,
`/dev/uinput`, or an unconfirmed isolation precondition), not a failure.
The picker variants use disjoint display ranges but share screenshot tools.
They, the X11 GUI test and the Wayland test share the `graphical_integration`
resource lock to avoid compositor startup starvation on small CI runners.

## Sanitizers

```bash
cmake --preset sanitize
cmake --build build/sanitize -j$(nproc)
ctest --preset sanitize
```

The suite must pass cleanly under ASan/UBSan. The scheduler's flat-stack
regression test (`repeated_awaits_do_not_grow_the_native_stack`) is the one that
catches a reintroduced inline coroutine resume; it fails under ASan before it
fails in a release build.

## Verification checklist

```bash
make
ctest --preset release
keyloom --check examples/two_spots.loom
keyloom --run examples/two_spots.loom --dry-run
keyloom --doctor
keyloom --screen
```

Run the full suite after touching the lexer, parser, checker, runtime,
scheduler, input, uinput or backends. When the language changes, also run
`keyloom --check` on every script in `examples/`.

### Real pointer verification

For pointer work, a real check beats a fake backend: run the script, press A
and D, and confirm the pointer lands at `640,420` and `1080,420`. On KWin, a
temporary scripting file can log `workspace.cursorPos` to the journal:

```js
function main() { console.log("KEYLOOM_POS " + workspace.cursorPos.x + "," + workspace.cursorPos.y); }
main();
```

Load it with
`qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript FILE` and
`...Scripting.start`.

Do not test real mouse holds over important windows; they inject `BTN_LEFT`.

## Writing tests

- Add a focused test for every new behaviour, next to the code it covers.
- Keep pure helpers testable without a display, device or network. The picker
  helpers (`format_coordinates`, `tooltip_position`, `screenshot_commands`)
  and `run_cli(args, picker_fn)` exist so tests can inject a stub picker
  instead of opening the real overlay.
- Deterministic fault injection belongs in probes and integration scripts, not
  in the unit suite. See `tests/uinput_fault_integration.sh` and
  `tests/picker_upload_probe.cpp`.
