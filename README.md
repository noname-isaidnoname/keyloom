# Keyloom

Keyloom is a Linux-first desktop automation language and native C++20 runtime. Scripts use readable `.loom` syntax, are statically checked before running, and execute on a single epoll-based event loop. The language never evaluates code or shell strings.

- **Input:** physical keyboards (and optional mouse buttons) via evdev ioctls on `/dev/input/event*`
- **Output:** `ydotool`/`ydotoold` (preferred for keys/buttons/text) plus a direct `uinput` device for absolute pointer motion, scroll, and fallback output
- **Platforms:** designed for Xorg, XWayland, and native Wayland sessions
- **Safety model:** no embedded code execution, no shell evaluation; `launch` is opt-in and uses an argv vector

## Setup

You need a C++20 compiler, CMake ≥ 3.25, Ninja, pkg-config, and X11/XCB/libpng development headers.

Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config libx11-dev libxcb1-dev libpng-dev
```

Arch:

```bash
sudo pacman -S cmake ninja pkgconf libx11 libxcb libpng
```

Build and install:

```bash
./scripts/install.sh                              # installs into /usr/local
PREFIX="$HOME/.local" ./scripts/install.sh        # install into a writable prefix
```

`scripts/install.sh` checks dependencies, builds with the `release` preset, and runs `cmake --install`. It never uses `sudo`.

On Arch-based distributions, you can build and install a native package:

```bash
cd packaging/arch
makepkg -si   # builds and installs keyloom (add KEYLOOM_BUILD_TESTS=ON to run the test suite)
```

## Quick start

```bash
keyloom --init my-first-script
keyloom --check my-first-script.loom
keyloom --docs
keyloom --run my-first-script.loom
```

A script path without an extension receives `.loom`.

## CLI

```text
keyloom --init <script-name> [--force]
keyloom --check <script-path>
keyloom --run <script-path> [--dry-run] [--input-device /dev/input/eventN]
                [--output-backend auto|ydotool|uinput] [--screen-size WxH]
keyloom --get-mouse-pos
keyloom --docs [topic]
keyloom --list-api
keyloom --devices
keyloom --screen
keyloom --doctor
keyloom --version
```

- `--check` does static checking only; it does not open input devices or start the runtime. It rejects unknown function names, wrong argument counts, `break` outside a loop, and `return` outside a function.
- `--run` refuses scripts with checker errors. Use `--dry-run` to log output actions without injecting them (live key listening still needs evdev permissions).
- Stop a running script with the panic key (**PAUSE** by default) or `Ctrl+C`. Set `option panic = "none"` to disable the panic key (not recommended).

## Example

`examples/two_spots.loom` shows independent A/D mouse holds with coordinate warps:

```loom
loom "two-spots" {
    option output = "auto"
    option panic = "PAUSE"

    consume key A
    consume key D

    when key A goes down {
        cursor warp 640, 420
        mouse left hold as spot_a
    }
    when key A goes up {
        mouse left free as spot_a
    }

    when key D goes down {
        cursor warp 1080, 420
        mouse left hold as spot_d
    }
    when key D goes up {
        mouse left free as spot_d
    }
}
```

Named mouse holds are reference-counted: if A and D overlap, the left button stays held until both holds are released.

## Language overview

```loom
loom "sample" {
    option output = "auto"
    option panic = "PAUSE"

    remap key F10 to ENTER

    func double(x) {
        return x * 2
    }

    when key F8 goes down {
        cursor center
        mouse left tap
        log "triggered"
    }

    when key F9 stays held every 100ms {
        log "F9 is still held"
    }

    when key CTRL+ALT+T goes down {
        set value = double(21)
        log "chord: " + str(value)
    }

    when mouse left goes down {
        key CTRL press
    }
    when mouse left goes up {
        key CTRL free
    }
}
```

Run `keyloom --docs` for the full guide. Useful topics: `events`, `pointer`, `mouse`, `keyboard`, `input`, `desktop`, `flow`, `lists`, `strings`, `functions`, `ahk`.

| Area | Highlights |
|---|---|
| Events | key/mouse down/up/held, chords, timer, app start |
| Pointer | absolute warp, relative move, glide, center |
| Mouse | hold/free/tap with independent tokens |
| Keyboard | press/free/tap chords, `type`, AHK-style `send`, native `remap`, `consume key` |
| Desktop | scroll, clipboard, notifications, opt-in launch, active-window queries, GUI dialogs/panels |
| Flow | variables, expressions, if/else if, for/repeat/while, break/continue, wait, assert, log |
| Data | lists with literals, indexing, `split`/`join`/`sort`/`append`, string helpers |
| Functions | user-defined `func` with parameters and `return` |

### Lists

```loom
set items = ["alpha", "beta"]
set first = items[0]          # negative indexes count from the end
set more = items + ["gamma"]  # a new list
call append(items, "delta")   # changes the list in place
set joined = join(items, ", ")
```

### Loops and functions

```loom
for i = 1 to 10 {
    if i == 5 {
        continue
    }
    log str(i)
}

for item in items {
    log item
}

func double(x) {
    return x * 2
}
set value = double(21)   # 42
```

`break` and `continue` work in `for`, `repeat`, and `while` loops. A `func`
declaration lives at the top level of the `loom` block. A function has its own
scope: its parameters and the variables it assigns stay inside the call. It
can read the script's variables but cannot change them, so return a value
instead.

### Chord bindings

```loom
when key CTRL+ALT+T goes down {
    log "Ctrl+Alt+T"
}
```

The last key of the chord is the primary key and the ones before it must be
physically held when it goes down. Use the whole chord to address the
binding, for example `call set_hotkey_enabled("CTRL+ALT+T", false)`.


## Runtime setup (evdev + output)

Keyloom reads global physical input through evdev. Output uses `ydotoold` for keys/buttons/text and Keyloom's own `uinput` device for absolute pointer motion.

1. **Install/start ydotoold** (required for `ydotool` output):
   ```bash
   sudo apt install ydotool   # or your distro's package
   ydotoold                   # run in your logged-in session; keep it running
   ```
   The socket can be overridden with `YDOTOOL_SOCKET`.

2. **Grant input access**:
   ```bash
   keyloom --devices
   ```
   If event nodes are not readable, configure input-device permissions (for example an `input` group or a narrow udev rule). Log out/in after group changes. Do not run Keyloom as root to bypass permissions.

3. **Check coordinates**:
   ```bash
   keyloom --screen
   keyloom --get-mouse-pos   # left-click prints x,y; right-click/Esc cancels
   ```
   Set `option screen = "WIDTHxHEIGHT"` or pass `--screen-size WIDTHxHEIGHT` if auto-detection is wrong.

Both output backends need `/dev/uinput` access. Pointer motion, scrolling, and key consumption all rely on it. If `/dev/uinput` is missing, `sudo modprobe uinput` may help when your kernel supports it.

## Backend notes

- `output = "auto"` prefers `ydotool` when the client and daemon socket exist; otherwise it uses `uinput` when writable.
- `ydotool` handles buttons/keys/text. Keyloom still uses its own absolute `uinput` pointer for `cursor warp`/`move`/`scroll` (ydotool 1.0.x absolute motion is unreliable).
- `uinput` creates an absolute pointer plus keys/buttons/relative axes. `ydotool` is preferred for non-US layouts and Unicode text.
- `consume key NAME` grabs the physical keyboard and replays non-consumed events through a virtual passthrough device; the panic key is never consumed.

## Development

```bash
make            # configure, build (Release), and run the test suite
make build      # build Release only
make test       # run tests (ctest --preset release)
make debug      # Debug preset
make clean      # delete the build directory
```

Or with CMake directly:

```bash
cmake --preset release
cmake --build build/release
ctest --preset release
```

The `sanitize` preset builds the whole project with the address and undefined
behaviour sanitizers and runs the suite under them:

```bash
cmake --preset sanitize
cmake --build build/sanitize
ctest --preset sanitize
```

Release builds enable `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3` and
`-fPIE`; turn them off with `-DKEYLOOM_HARDEN=OFF`.

Architecture: `src/lexer.cpp` → `src/parser.cpp` → `src/checker.cpp` → `src/runtime.cpp`. The runtime is split by concern into `src/runtime.cpp` (statements and expressions), `src/runtime_input.cpp` (devices), `src/runtime_output.cpp` (output and holds) and `src/runtime_gui.cpp` (panels). Output drivers implement `OutputBackend` (`src/backends.hpp`); devices/timers/dispatch run on one epoll scheduler (`src/scheduler.cpp`). See `docs/architecture.md` for the full module map.

## Contributing

- Keep changes consistent with surrounding style and patterns.
- Run the full test suite after touching lexer, parser, checker, runtime, scheduler, input, uinput, or backends.
- When user-visible behavior changes, update `README.md` and `src/docs.cpp` (`keyloom --docs` / `api_reference()`) together.
- See `AGENTS.md` for agent guidance, and `docs/` for architecture, build, testing, benchmark, implementation traps, and contributing documentation.

## License

MIT. See `LICENSE`.
