# Architecture

Keyloom is a Linux-first desktop automation language and its native C++20
runtime. A `.loom` file is parsed, statically checked, and then executed on a
single epoll-based event loop. There is no interpreter in the hot path: the
runtime is a set of coroutines (`Task`) driven by one `Scheduler`.
Native Wayland panels run GTK in isolated helper processes; their messages are
handled by the same scheduler so a GTK/display failure cannot terminate the
automation process before its output cleanup.

```
loom file → lexer → parser → checker → runtime
          src/lexer.cpp → src/parser.cpp → src/checker.cpp → src/runtime*.cpp
```

## Repository layout

| Path | Owns |
| --- | --- |
| `src/lexer.cpp` | tokens, strings, durations, comments |
| `src/parser.cpp`, `src/parser.hpp`, `src/ast.hpp` | `loom` block, options, `when` events, statements, expressions |
| `src/checker.cpp` | option values, key names, hold-token balance, `/dev/uinput` warnings |
| `src/errors.hpp` | exception types: `BackendError`, `RuntimeErrorKeyloom`, `LexError`, `TaskCancelled` |
| `src/runtime.cpp`, `src/runtime.hpp` | event loop glue, key tracking, dispatch, flow, expressions, hold refcounts |
| `src/runtime_input.cpp` | device setup, evdev event decoding, passthrough replay, remaps |
| `src/runtime_output.cpp` | key and text output, mouse and key holds, clipboard, notifications, launching |
| `src/runtime_gui.cpp` | GUI panels and dialogs, active window queries |
| `src/runtime_internal.hpp` | text helpers shared by the runtime's translation units |
| `src/scheduler.cpp`, `src/scheduler.hpp` | epoll event loop, task/timer/co-routine plumbing, `Task`, `Scheduler` |
| `src/backends.cpp`, `src/backends.hpp` | `OutputBackend` interface, ydotoold, uinput, dry run, backend selection |
| `src/uinput.cpp`, `src/uinput.hpp` | `UInputDevice`: creates the virtual device, batched `input_event` writes |
| `src/input.cpp`, `src/input.hpp` | `InputDevice`: open/list `/dev/input/event*`, evdev caps, grab/ungrab, passthrough replay |
| `src/subprocess.cpp`, `src/subprocess.hpp` | `run_command`/`spawn_detached`, `which`, `split_shell_words`, `ydotool_socket_path` |
| `src/keys.cpp`, `src/keys.inc` | key-name tables, `validate_key_name`, `canonical_key_name`, `mouse_buttons` |
| `src/value.cpp`, `src/value.hpp` | `Value` (int/double/bool/string), Python-style `repr`/`str`, truthiness |
| `src/builtins.cpp`, `src/builtins.hpp` | the builtin-function table and arity lookup |
| `src/mousepos.cpp`, `src/mousepos.hpp` | screenshot-overlay coordinate picker (`--get-mouse-pos`) |
| `src/picker_xcb.cpp`, `src/picker_image.cpp` | the picker's XCB transport and screenshot pixel handling |
| `src/gui.cpp`, `src/gui.hpp` | `GuiHost`: X11/Xwayland panels, keyboard mapping, event dispatch |
| `src/gui_wayland.cpp` | Isolated GTK3 Wayland panel helpers and compositor selection |
| `src/xcb_connection.cpp`, `src/xcb_connection.hpp` | RAII XCB transport with explicit error inspection |
| `src/unique_fd.hpp` | RAII wrapper for raw file descriptors |
| `src/cli.cpp`, `src/main.cpp` | `--init`, `--check`, `--run`, `--docs`, `--devices`, `--screen`, `--get-mouse-pos`, `--doctor`, `--list-api`, `--version` |
| `src/docs.cpp` | user-facing language guide (`kGuide`) and `api_reference()` |
| `examples/two_spots.loom` | A/D hold regression script used by tests |
| `tests/*.cpp` | parser, checker, runtime, scheduler, picker and CLI tests (`tests/harness.hpp` mini test runner) |
| `Makefile` | thin one-word targets (`make`, `make test`, `make install`, `make package`) |
| `packaging/arch/PKGBUILD` | Arch package built from a local checkout; `check()` runs the test suite only with `KEYLOOM_BUILD_TESTS=ON` |
| `packaging/common/` | Shared desktop entry, scalable icon, MIME type, terminal launcher and package post-install scripts installed by CMake |

## Compilation units

`CMakeLists.txt` builds one static library, `keyloom_core`, from every source
file except `src/main.cpp`, then links the `keyloom` executable against it.
The runtime was split by concern (`runtime.cpp`, `runtime_input.cpp`,
`runtime_output.cpp`, `runtime_gui.cpp`) so no single translation unit
dominates compile time; shared text helpers live in `runtime_internal.hpp`.

The test harness and probes are separate executables. They compile at `-O1`
(`keyloom_test_options`) because they are not performance critical, while
`keyloom_core` keeps the release optimisation level. See
[benchmark.md](benchmark.md) for the measured effect.

## Pipeline stages

### Lexer (`src/lexer.cpp`)

Produces tokens for identifiers, numbers, strings, durations, comments, and
punctuation. Strings decode `\\`, `\n`, `\r`, `\t`, `\'`, and `\"` in one
pass. Durations accept `25ms`, `0.5s`, `2m`, `1h`; a bare number in a duration
position means seconds.

### Parser (`src/parser.cpp`, `src/ast.hpp`)

Builds a `Script`: a name, options, event blocks, `remap` declarations, GUI
panels, and user functions. Each `when` block and each statement is a `Node`.
Expressions are an `Expr` tree (`Literal`, `Variable`, `Unary`, `Binary`,
`Call`, `List`, `Index`). `Node` is deliberately flat: one struct carries the
event fields and the statement fields so the checker and runtime dispatch stay
readable.

### Checker (`src/checker.cpp`)

Walks the script before anything opens a device. It validates option values
against the table in `option_rules()`, key names through
`validate_key_name()`, builtin arity through the table in `src/builtins.cpp`,
hold-token balance, `break`/`continue`/`return` placement, remap conflicts,
and warns when `/dev/uinput` is not writable. Diagnostics are collected and
printed; `--check` returns 2 when any are errors.

### Runtime (`src/runtime*.cpp`)

`Runtime` owns the parsed `Script`, the `Scheduler`, the selected
`OutputBackend`, the open `InputDevice`s, variables, hold refcounts, remaps,
GUI hosts, and the failure state. `Runtime::run()` is the root task: it opens
devices, installs signal handling, dispatches `when app starts`, and then
waits for input until the panic key, Ctrl+C, or a script error stops it.

## Scheduler and task model

`Scheduler` (`src/scheduler.hpp`, `src/scheduler.cpp`) is the single epoll loop.
It owns:

- `ready_`: frames that can run now.
- `timers_`: a deadline priority queue for `sleep_for`.
- `fd_waiters_`: epoll registrations for `readable`.
- `live_` / `live_order_`: spawned tasks, for shutdown and drain.
- `known_`: every resumable frame, including awaited children that are not
  spawned tasks.
- `stop_waiters_` / `drain_waiters_`: tasks suspended on `wait_stop`/`drain`.
- `deferred_`: frames handed back by a finished frame instead of resumed
  inline.

A `Task` is a coroutine. `promise_type` tracks the owning `scheduler`, the
`driver` that must resume it, the `continuation` (the frame awaiting it), and
the `parent`/`child` links that form the live await chain. Cancellation walks
to the deepest suspended frame, marks the whole chain, and wakes that frame so
every awaiter unwinds with `TaskCancelled` at its own await point.

A finished frame does **not** resume its awaiter inline. `FinalAwaiter` calls
`Scheduler::defer_resume(continuation)` and returns `std::noop_coroutine()`;
`Scheduler::drain_deferred()` then resumes the continuation from the scheduler's
own loop. This keeps the native stack flat: a script that awaits once per loop
iteration would otherwise grow one native frame per iteration and overflow the
stack at `-O0`/under sanitizers. See
[implementation-traps.md](implementation-traps.md#coroutine-stack-growth).

`Scheduler::spawn` makes a task live and associates it with the currently
running root for shutdown ordering. `add_shutdown_dependency` lets a task
declare that it must be destroyed before another live task. `shutdown` destroys
dependents before prerequisites and falls back to reverse spawn order.

## Runtime subsystems

### Input (`src/runtime_input.cpp`, `src/input.cpp`)

`Runtime::open_input_devices()` lists `/dev/input/event*`, opens the readable
ones, and filters out injected devices by name (`keyloom`, `ydotool`,
`virtual pointer`, `remap_clicks`). Keyboards are always kept; mice are kept
only when the script binds a mouse button. Consumed keys are grabbed with
`EVIOCGRAB`; non-consumed events are replayed through a `PassthroughDevice`.
`process_events` decodes `RawEvent`s, tracks per-device and global key state,
handles remaps, and synthesises releases on disconnect.

### Output (`src/runtime_output.cpp`, `src/backends.cpp`)

`OutputBackend` is the interface: `warp`, `move`, `button`, `key`, `type_text`,
`scroll`, `close`, `name`. Three implementations exist:

- `YdotoolBackend` — keys, buttons and text through `ydotoold`; pointer motion
  through a lazily created `UInputBackend`.
- `UInputBackend` — direct `/dev/uinput` device for pointer motion, scroll,
  keys, buttons and US-layout text.
- `DryRunBackend` — logs actions without injecting them.

`choose_backend()` implements `output = "auto"` selection. See
[implementation-traps.md](implementation-traps.md#backends) before changing any
of this.

### Dispatch and flow (`src/runtime.cpp`)

Physical input arrives on `read_device` tasks. `process_events` updates key
state and calls `dispatch` for each edge. `dispatch` spawns a background task
per matching event block (`Runtime::spawn_background`) and passes an
`EventContext` carrying `event_kind`, `event_key`, `event_edge`, and
`hotkey_id` into `execute()`. There are no globals or TLS for the current
event.

`execute_nodes` runs a block; `execute` switches on `StmtKind` and handles one
statement. `eval` evaluates expressions and is a coroutine because a
user-defined function may wait. `call_function` implements builtins;
`call_user_function` pushes a fresh scope, binds parameters, and caps recursion
at `kMaxCallDepth = 32`. Loops cap at `kMaxLoop = 10000`.

### GUI (`src/gui.cpp`, `src/runtime_gui.cpp`)

`GuiHost` owns an XCB connection and one or more panels. It caches the keyboard
mapping and refreshes it on `XCB_MAPPING_NOTIFY` so layout switches are picked
up. Panel events are dispatched back into the same `execute()` path through
`Runtime::dispatch_gui_event`.

### Mouse-position picker (`src/mousepos.cpp`, `src/picker_xcb.cpp`, `src/picker_image.cpp`)

`--get-mouse-pos` is a separate X11 client. It captures the desktop with an
installed screenshot tool, shows the still image in a full-screen managed
splash window, follows the pointer with a tooltip, and prints the clicked
coordinates. See [cli.md](cli.md#mouse-position-picker).

## Data model

| Type | File | Purpose |
| --- | --- | --- |
| `Value` | `src/value.hpp` | int/double/bool/string with Python-style `repr`/`str` and truthiness |
| `Expr` | `src/ast.hpp` | expression tree node |
| `Node` | `src/ast.hpp` | one statement or event block |
| `Script` | `src/ast.hpp` | options, events, remaps, panels, functions |
| `Function` | `src/ast.hpp` | user-defined function: params and body |
| `GuiPanel` / `GuiWidget` | `src/ast.hpp` | persistent panel declarations |
| `Remap` | `src/ast.hpp` | `remap key SOURCE to TARGET` |

## Build-time integration

`KEYLOOM_VERSION` is defined from `project(keyloom VERSION ...)` in
`CMakeLists.txt` and is the single source of truth for the version. The
`keyloom_core` target links X11, XCB and libpng through pkg-config and applies
the warning flags from `keyloom_options`. See
[build-and-install.md](build-and-install.md) and
[contributing.md](contributing.md#versioning).
