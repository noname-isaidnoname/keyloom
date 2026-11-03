# AGENTS.md

Keyloom is a Linux-first desktop automation language. A `.loom` file is parsed, statically checked, and run by a native C++20 runtime built on one epoll-based event loop (`src/scheduler.cpp`). Input comes from physical keyboards through evdev ioctls on `/dev/input/event*`; output goes through `ydotool`/`ydotoold` or a direct `uinput` device written with plain `input_event` structures. The language never evaluates code or shell strings. `launch` is opt-in and uses an argv vector with no shell.

This repo is the C++ port. The original Python implementation lives at `/home/fesuoy/Desktop/Stuff/Python/keyloom` and is the behavioral reference; the two should produce identical diagnostics and runtime behavior.

User docs: `README.md`. Language reference: `keyloom --docs` (`src/docs.cpp`). Examples: `examples/*.loom`.

## Layout

| path | owns |
| --- | --- |
| `src/lexer.cpp` | tokens, strings, durations, comments |
| `src/parser.cpp`, `src/ast.hpp` | `loom` block, options, `when` events, statements, expressions |
| `src/checker.cpp` | option values, key names, hold-token balance, `/dev/uinput` warnings |
| `src/runtime.cpp`, `src/runtime.hpp` | event loop glue, key tracking, dispatch, flow, expressions, hold refcounts |
| `src/scheduler.cpp`, `src/scheduler.hpp` | epoll event loop, task/timer/co-routine plumbing, `Task`, `Scheduler` |
| `src/backends.cpp`, `src/backends.hpp` | `OutputBackend` interface, ydotoold, uinput, dry run, backend selection |
| `src/uinput.cpp`, `src/uinput.hpp` | `UInputDevice`: creates the virtual device, batched `input_event` writes |
| `src/input.cpp`, `src/input.hpp` | `InputDevice`: open/list `/dev/input/event*`, evdev caps, grab/ungrab, passthrough replay |
| `src/subprocess.cpp`, `src/subprocess.hpp` | `run_command`/`spawn_detached`, `which`, `split_shell_words`, `ydotool_socket_path` |
| `src/keys.cpp`, `src/keys.inc` | key-name tables, `validate_key_name`, `canonical_key_name`, `mouse_buttons` |
| `src/value.cpp`, `src/value.hpp` | `Value` (int/double/bool/string), Python-style `repr`/`str`, truthiness |
| `src/mousepos.cpp`, `src/mousepos.hpp` | screenshot-overlay coordinate picker (`--get-mouse-pos`) |
| `src/cli.cpp`, `src/main.cpp` | `--init`, `--check`, `--run`, `--docs`, `--devices`, `--screen`, `--get-mouse-pos`, `--doctor`, `--list-api`, `--version` |
| `src/docs.cpp` | user-facing language guide (`kGuide`) and `api_reference()` |
| `examples/two_spots.loom` | A/D hold regression script used by tests |
| `tests/*.cpp` | parser, checker, runtime, scheduler, picker and CLI tests (`tests/harness.hpp` mini test runner) |
| `Makefile` | thin one-word targets (`make`, `make test`, `make install`, `make package`) |
| `packaging/arch/PKGBUILD` | Arch package built from a local checkout; `check()` runs the test suite |

## Environment and install

C++20 toolchain, CMake ≥ 3.25, Ninja, pkg-config, X11 and libpng dev headers.
This machine is Arch-based with KDE Plasma on Wayland — **do not assume the session type, compositor, distro or package manager anywhere**: detect at runtime (`--doctor`, `choose_backend()`, picker X11/XWayland), keep docs generic, and keep session/distro-specific steps clearly labelled (e.g. Arch's `makepkg`).

```bash
./scripts/install.sh                     # cmake --preset release + build + install (PREFIX, default /usr/local)
make                                     # configure, build (Release) and run the test suite
make build                               # build Release only, no test suite
make install                             # install into PREFIX (default /usr/local)
make package                             # Arch: build keyloom-0.2.0-1-x86_64.pkg.tar.zst via makepkg
make clean                               # delete the build directory

# Or with CMake directly
cmake --preset release && cmake --build build/release
```

`scripts/install.sh` checks for cmake/ninja/pkg-config and the x11/libpng dev packages, and refuses a non-writable default prefix with a `PREFIX="$HOME/.local"` hint. It never runs sudo. `packaging/arch/PKGBUILD` builds the package from a local checkout (no network) and runs the test suite in `check()`; `make install` and the package both honour the `cmake` install layout (`GNUInstallDirs`, binary into `CMAKE_INSTALL_BINDIR`).

Live input needs read access to `/dev/input/event*` (`input` group or a udev rule). Pointer motion and scroll need write access to `/dev/uinput`. Buttons, keys, and text via ydotoold need the daemon running.

## Verify

```bash
make                      # configure, build and run the test suite
ctest --preset release
keyloom --check examples/two_spots.loom
keyloom --run examples/two_spots.loom --dry-run
keyloom --doctor
keyloom --screen
```

The suite is small; run all of it after touching the lexer, parser, checker, runtime, scheduler, input, uinput or backends. For pointer work, a real check beats a fake backend: run the script, press A and D, and confirm the pointer lands at `640,420` and `1080,420`. On KWin, a temporary scripting file can log `workspace.cursorPos` to the journal:

```js
function main() { console.log("KEYLOOM_POS " + workspace.cursorPos.x + "," + workspace.cursorPos.y); }
main();
```

Load it with `qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript FILE` and `...Scripting.start`. Do not test real mouse holds over important windows; they inject `BTN_LEFT`.

## Backend traps

- `ydotool mousemove --absolute` is not a real absolute warp in ydotool 1.0.4. It sends `REL_X/REL_Y = INT32_MIN` first (top-left), then applies the target as relative deltas. Pointer acceleration turns that into a corner jump or a wrong position. Measured on KWin: `--absolute 640 420` landed at `0,0`. Never use it for `cursor warp`.
- `YdotoolBackend` therefore uses its own `UInputBackend` absolute pointer lazily for `warp`, `move`, and `scroll`, and keeps ydotoold for buttons, keys, and text. If `/dev/uinput` is not writable, pointer motion fails with a clear error instead of falling back to the top-left hack.
- `UInputBackend` declares `ABS_X`/`ABS_Y` with `min = 0`, `max = width - 1`. The kernel rejects inverted or inconsistent axis ranges with `EINVAL`; the axis info goes through `UInputDevice` in `src/uinput.cpp`.
- `output = "auto"` uses ydotoold when the client and its socket exist, otherwise `uinput` when `/dev/uinput` is writable, otherwise ydotoold again so its error message can explain the missing daemon.
- `ydotool_socket_path()` (in `src/subprocess.cpp`) checks `YDOTOOL_SOCKET`, then `$XDG_RUNTIME_DIR/.ydotool_socket`, then `/tmp/.ydotool_socket`.
- Buttons use ydotool's encoded values: ydotool's own button index (`left = 0`, `right = 1`, ...) `| 0x40` for down, `| 0x80` for up — not the kernel `BTN_*` code. Keys use `code:1` and `code:0`. `tests/test_runtime.cpp` pins `click 0x40` / `click 0x80` for left.
- `type_text` through ydotool passes `-e 0 --`, because Keyloom already decoded the `.loom` string. Letting ydotool escape again turns a literal backslash before `n` into a newline.
- The uinput pointer writes `ABS_X`/`ABS_Y` for warps and `REL_X`/`REL_Y` for moves, then a `SYN_REPORT`. Coordinates are clamped to `0..width-1` and `0..height-1`.
- The uinput text mapper covers US layout only. Add new shifted symbols to both the shifted set and the shifted map, and new plain symbols to the plain set (`src/backends.cpp`).
- Key code lookup uses the tables in `src/keys.inc` (generated by `scripts/gen-keytable.sh` from the kernel `input-event-codes.h`). Some kernel codes map to lists of aliases; `canonical_key_name` upper-cases and resolves aliases.
- Avoid reacting to injected devices. `Runtime::open_keyboards()` drops names containing `keyloom`, `ydotool`, or `virtual pointer`, and also drops devices without both `KEY_A` and `KEY_ENTER`/`KEY_SPACE`.
- `uinput_key_capabilities()` in `src/backends.cpp` builds the advertised key set. It deliberately skips the `BTN_MISC..KEY_OK-1` block and adds back only the pointer buttons. Declaring `BTN_TOOL_PEN`/`BTN_STYLUS` (which a naive `0..KEY_MAX` sweep does) makes udev tag the device `ID_INPUT_TABLET=1`, and libinput then ignores its pointer and keyboard events entirely — warps, clicks, scrolling and typing all silently do nothing. Check classification with `udevadm info -q property -p /sys/class/input/eventN | grep ID_INPUT` while a script runs.
- `UInputDevice` waits for udev to write `/run/udev/data/+input:<sysname>` and then for libinput to open the node before returning. Without that wait, the first `cursor warp` after startup is folded into the device's initial ABS state and never arrives as motion, and a replayed key can be swallowed. Keep the device open for the whole run (created once, not per action).

- Key consumption grabs the physical device with `EVIOCGRAB` (`src/input.cpp`) and replays non-consumed events through a passthrough uinput device. The passthrough mirrors `EV_KEY`, `EV_REL`, `EV_ABS`, and `EV_MSC` but deliberately not `EV_REP`, otherwise held keys would repeat twice. `consume` declarations are collected before devices are grabbed so the first key-down is swallowed, and the panic key is removed from the consumed set.

## Runtime traps

- Mouse holds are reference-counted by owner token. The default token is `default:<event_key>:<button>`. Use explicit `as TOKEN` when two keys can hold the same button. Releasing one owner must not release the physical button until the last owner frees it.
- Key presses are observed, not suppressed. `pressed_keys_` is global across devices; `device_down_` tracks per-device state so the up edge fires only after the last device releases the key.
- Held timers live in `held_tasks_`, keyed by `(key, line)`, favour a fixed cadence: each `stays held every` tick is scheduled from the previous deadline, not from when the handler returned, and overruns skip whole ticks instead of bursting (see `Runtime::held_loop`). Held tasks are cancelled on key-up and device disconnect.
- Dispatch runs as tasks spawned onto the scheduler (`Runtime::spawn_background`). `event_key`/`event_edge` are carried in an `EventContext` struct passed through `execute()`, not globals or TLS.
- `repeat` and `while` cap at `kMaxLoop = 10000`. A panic key or Ctrl+C sets the stop event; the CLI runs the scheduler and rethrows `runtime.failure()` after the loop ends.
- `Runtime` releases every held button and key and closes the backend on teardown (`~Runtime`/`release_all_holds`). Keep `close()` defensive and idempotent.
- `cursor glide` starts from Keyloom's last known cursor position, or the screen center. It does not query the real pointer.
- The panic key is checked before regular bindings, so a script binding to the panic key is unreachable. The checker warns about that.

## Language rules

- Top level accepts only `option name = value` and `when ... { ... }`. A file needs at least one event block.
- Options: `input` (`auto`/`evdev`, currently informational), `output` (`auto`/`ydotool`/`uinput`), `panic` (key name or `"none"`), `screen` (`auto` or `WIDTHxHEIGHT`), `allow_launch` (boolean).
- Events: `when key NAME goes down/up`, `when key NAME stays held every DURATION`, `when timer every DURATION`, `when app starts`.
- Key names accept identifiers and numeric tokens, so `when key 1 goes down` and `key CTRL+1 tap` work. Key validation lives in `validate_key_name()` (`src/keys.cpp`).
- Statements: `cursor`, `mouse`, `key`, `type`, `wait`, `scroll`, `log`/`say`, `notify`, `clip write`/`clip read into`, `launch`, `set`, `if`/`else`, `repeat`, `while`, `assert`, `call`.
- Expressions: literals, variables, function calls, unary `-`/`+`/`not`/`!`, arithmetic, comparisons, `and`/`or` (also `&&`/`||`), parentheses.
- Strings decode `\\`, `\n`, `\r`, `\t`, `\'`, and `\"` in one pass. Keep it single-pass; a chained replace broke `"\\n"`.
- Add a statement in three places: a `StmtKind` in `src/ast.hpp` (+ parser in `src/parser.cpp`), a handler in `Runtime::execute()` (`src/runtime.cpp`), and docs plus `api_reference()` in `src/docs.cpp`. Add a checker rule when the statement has static constraints.
- Add a built-in in `Runtime::call_function()` and list it in `src/docs.cpp`.
- Add a backend by implementing `warp`, `move`, `button`, `key`, `type_text`, `scroll`, `close` in `OutputBackend` (`src/backends.hpp`), then register it in `choose_backend()`.

## CLI behavior

- `--check` must not open input devices or require permissions. It prints diagnostics and returns 2 on errors.
- `--run` refuses scripts with checker errors. `--dry-run` logs actions but still listens for physical keys.
- Bad paths and parse errors print `keyloom: ...` and return 2, not a traceback/exit-1.
- `--init` refuses to overwrite unless `--force` is passed.
- `--get-mouse-pos` is an X11 client (XWayland on Wayland) and never needs evdev. It captures the desktop with spectacle, grim, gnome-screenshot, scrot, or ImageMagick import, then shows that still image in a full-screen managed splash window. The opaque splash window is what makes clicks over native Wayland windows land on the picker; an override-redirect or transparent window does not capture them. A separate tooltip follows the pointer. Left-click prints `x, y` and returns 0; right-click (or Esc when focused) returns 130. Keep pure helpers (`format_coordinates`, `tooltip_position`, `screenshot_commands`) testable without opening a display. `--get-mouse-pos` is tested through `run_cli(args, picker_fn)`; pass a stub picker, never the real overlay.

## Style

- C++20, 4-space indentation, small free functions in anonymous namespaces where possible, RAII ownership (`std::unique_ptr`, no raw `new`).
- Explicit exception types: `ParseError`, `BackendError`, `RuntimeErrorKeyloom`, `CliUsageError`, `PickerError` (`src/errors.hpp`, `src/cli.hpp`, `src/mousepos.hpp`).
- Keep the DSL small. Do not add arbitrary code execution, shell strings, or file/network APIs without a deliberate design change.
- Update `README.md`, `src/docs.cpp` (both `kGuide` and `api_reference()`) together when user-visible behavior changes.
- Build is warning-clean on `-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual`; keep it that way.
- Use `rg` for searching. Keep new code covered by a focused test in `tests/`.

## Gotchas
- Avoid using subagents. because when they finish giving the report/summary of the task they were given, you don't know the context of places they searched, so you're better off just directly reading/grepping files.
