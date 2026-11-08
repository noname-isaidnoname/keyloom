# Keyloom

**Keyloom** is a Linux-first desktop automation language with its own readable syntax, static checker, CLI and runtime. It is a native C++20 binary using X11, XCB and libpng libraries for desktop integration. Input comes from physical keyboards through evdev ioctls on `/dev/input/event*`. It is designed for Xorg, Xwayland and native Wayland sessions.

- Global physical keyboard input: evdev ioctls on `/dev/input/event*`.
- Native remapping, AHK-style `send` syntax, active-window queries, modal GUI dialogs and runtime hotkey suspension.
- Physical mouse buttons (`left`, `right`, `middle`, `side`, `extra`) can be observed with `when mouse ...`, in coexistence mode: the click still reaches applications.
- Preferred keyboard/button output: `ydotool` + `ydotoold` (uinput-based and Wayland-friendly).
- Pointer motion uses Keyloom's own absolute `uinput` device, because ydotool 1.0.x's `mousemove --absolute` jumps to the top-left corner first.
- Fallback output: a direct absolute/relative `uinput` device, written with plain `input_event` structures.
- No code execution is embedded in `.loom` scripts, and the language never spawns a shell.
- Named mouse holds are reference-counted, so multiple hotkeys can safely hold one mouse button.
- Optional key consumption: `consume key A` suppresses a key from reaching applications while keeping the rest of the keyboard working.

The runtime drives devices, timers and dispatch from a single epoll-based event loop; output uinput reports are batched into one write per report. There is no interpreter in the hot path.

## Quick start

You need a C++20 compiler, CMake ≥ 3.25, Ninja, pkg-config, X11, XCB and libpng development headers. Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config libx11-dev libxcb1-dev libpng-dev
```

Arch:

```bash
sudo pacman -S cmake ninja pkgconf libx11 libxcb libpng
```

Build and install:

```bash
./scripts/install.sh                     # installs into /usr/local
PREFIX="$HOME/.local" ./scripts/install.sh   # or into your own prefix
keyloom --init my-first-script
keyloom --check my-first-script.loom
keyloom --docs
```

`scripts/install.sh` checks the build dependencies, builds with the `release` preset and runs `cmake --install`. It never uses sudo; pass `PREFIX` to install somewhere writable.

On Arch-based distributions you can build and install a native package instead:

```bash
cd packaging/arch
makepkg -si          # builds, runs the test suite, and installs keyloom
```

`make` at the repository root is a thin wrapper around the same CMake build, and `make install` / `make package` cover both paths above.

To build without installing:

```bash
cmake --preset release
cmake --build build/release
./build/release/keyloom --check examples/two_spots.loom
```

To run a script:

```bash
keyloom --run examples/two_spots.loom
```

## The requested A/D hold script

`examples/two_spots.loom` is ready to edit and run. It implements:

- Press/hold **A**: warp to `(640, 420)` and hold left mouse button.
- Release **A**: release A's left-button hold.
- Press/hold **D**: warp to `(1080, 420)` and hold left mouse button.
- Release **D**: release D's left-button hold.

Change the four coordinates to your own desktop. If A and D overlap, the left mouse button remains held until **both** independent holds have been released. This avoids an early mouse-up when one key is released before the other. The example consumes A and D, so holding either key does not type into the focused application; remove those `consume key ...` lines if you want the keys to pass through. The panic key is never consumed.

## Wayland and Xwayland setup

Keyloom reads physical keyboard events through evdev rather than trying to install a global X11 hotkey listener. This is important on Wayland, where applications normally cannot globally observe arbitrary keyboard input through Xwayland.

If the evdev event queue overflows, Keyloom discards events through the next synchronization report and reconciles pressed keys against the kernel's current state. Individual lost key transitions and relative motion cannot be reconstructed; if the key-state query fails, the runtime stops instead of continuing with unreliable state.

### 1. Install system packages

Debian/Ubuntu example:

```bash
sudo apt install ydotool
```

Build and install Keyloom:

```bash
./scripts/install.sh
```

On distributions where `ydotool` is not packaged, install it from your distribution or the upstream project. Both output backends need write access to `/dev/uinput`: ydotoold injects buttons/keys through it, and Keyloom creates its own absolute pointer device there for `cursor warp`/`move`/`scroll`. If that node is missing and your kernel supports it, load the module with `sudo modprobe uinput`.

### 2. Start ydotoold

Start `ydotoold` in your logged-in user session using your distribution's service/unit if provided, or launch it manually in a terminal:

```bash
ydotoold
```

Leave it running while Keyloom scripts are active. `ydotool` is a client of this daemon; installing only the client binary is not sufficient for injection. The default socket can be overridden with `YDOTOOL_SOCKET`.

### 3. Grant input access

Inspect devices:

```bash
keyloom --devices
```

If the keyboard event nodes report no permission, configure your distribution's input-device permissions. Some distributions use an `input` group; a typical setup is:

```bash
sudo usermod -aG input "$USER"
```

Log out completely and back in for group membership to take effect. Group membership grants broad access to input devices, so a narrowly scoped udev rule for a specific keyboard is preferable on multi-user systems. Do **not** run the automation program as root merely to bypass device permissions.

### 4. Confirm coordinate space

```bash
keyloom --screen
```

The script uses desktop absolute coordinates. Wayland compositors, multiple monitors, negative monitor origins and fractional scaling may affect the mapping. Set `option screen = "WIDTHxHEIGHT"` in the script or pass `--screen-size WIDTHxHEIGHT` if auto-detection is wrong. The example coordinates are placeholders, not a screen calibration.

To pick coordinates from the desktop instead of guessing, run:

```bash
keyloom --get-mouse-pos
```

A tooltip follows the pointer and shows the current `x, y`. Left-click consumes the click, prints the position (for example `640, 420`) and exits. Right-click cancels; `Esc` also works while the overlay has keyboard focus. The picker first captures the desktop with `spectacle` (KDE), `grim`, `gnome-screenshot`, `scrot`, or ImageMagick's `import`, then shows that still image full-screen in an XWayland overlay. The desktop stays visible and the click does not reach the window underneath; the real cursor and live tooltip are drawn on top. The picker needs X11 (with XWayland on Wayland sessions) and a screenshot tool, but not evdev or `/dev/uinput` permissions.

## CLI reference

```text
keyloom --init <script-name> [--force]
keyloom --check <existing-script-path>
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

A script path without an extension receives `.loom`. `--check` does not need input-device permissions and never starts the runtime. `--dry-run` logs output actions rather than injecting them; live key listening still needs evdev permissions.

Stop a running script with the default panic key **PAUSE**, or press `Ctrl+C` in the terminal. Set `option panic = "none"` to disable the panic key (not recommended).

## Language overview

```loom
loom "sample" {
    option output = "auto"
    option panic = "PAUSE"

    remap key F10 to ENTER

    when key F8 goes down {
        cursor center
        mouse left tap
        log "triggered"
    }

    when key F9 stays held every 100ms {
        log "F9 is still held"
    }

    when mouse left goes down {
        log "left button pressed"
    }
}
```

Run `keyloom --docs` for the full syntax and API guide. Topics for `--docs` include `events`, `pointer`, `mouse`, `keyboard`, `input`, `desktop`, `flow`, `functions`, and `ahk`.

## Consuming keys

`consume key A` keeps a key from reaching applications. It is a script-wide declaration, so place it in any event block; it takes effect at startup and swallows the key-down, autorepeat, and key-up. Keyloom grabs the physical keyboard exclusively and replays every other event through a virtual `uinput` device, so `/dev/uinput` must be writable and the panic key always passes through.

Release all keys and buttons before starting a script that consumes or remaps keys, and avoid typing during startup. Keyloom checks for held inputs before each grab and aborts if it detects queued input during the handoff. This reduces, but cannot eliminate, races: a press delivered to the desktop just before the grab may have its release intercepted before rollback. When it observes intercepted releases, the error identifies the affected inputs; it does not inject replacement releases. If a key appears stuck after a failed grab, physically tap and release that key. For devices that are only observed, already-held keys initialize `key_down()` state without firing bindings; their first release also does not trigger an up binding.

For a straight replacement such as Q -> Space, use `remap key Q to SPACE` instead of a trigger block plus `consume`. The remap source is script-wide, consumed on the same terms, and cannot be the panic key.

```loom
when key A goes down {
    consume key A
    cursor warp 304, 395
    mouse left hold as spot_a
}
```

## AutoHotkey-style macros

Keyloom supports a practical AHK v2 subset while keeping its checked, declarative syntax.

### Native remapping

```loom
remap key Q to SPACE
remap key A to CTRL+B
```

Sources are consumed and do not type. Targets may be a single key or a `+`-separated chord. Target keys are pressed when the source goes down and released in reverse order when it goes up.

### AHK-style send

```loom
send "hello{enter}"
send "^c"                         # Ctrl+C
send "!{tab}"                    # Alt+Tab
send "{TAB 3}"                   # three Tab taps
send "{CTRL down}a{CTRL up}"    # hold Ctrl, send A, release Ctrl
```

Prefixes are `^` Ctrl, `!` Alt, `+` Shift and `#` Super. Special tokens include `{ENTER}`, `{TAB}`, `{SPACE}`, `{BS}`, `{DELETE}`, arrows and `{F1}`–`{F24}`. Use `{CTRL down}`/`{CTRL up}` for explicit holds and `{A 3}` for repeats. `{{}` sends a literal brace; a bare `}` can be sent as ordinary text.

### Active-window context

```loom
when key F8 goes down {
    set window = active_window_process()
    if window == "firefox" {
        log "Firefox is active"
    } else {
        log "F8 ignored outside Firefox"
    }
}
```

`active_window_title()`, `active_window_class()`, `active_window_process()` and `active_window_pid()` are read-only. They use `xdotool` or `kdotool` where available; without one, string values are empty and the PID is 0. This works on X11 and XWayland, and is best-effort on native Wayland.

### Modal GUI dialogs

```loom
when app starts {
    set minutes = gui_input("Minutes between presses", "5", "antiafk")
    set start_now = gui_confirm("Start now?", "antiafk")
    call gui_alert("Minutes: " + minutes, "antiafk")
}
```

`gui_alert`, `gui_input` and `gui_confirm` use the first available tool from `zenity`, `kdialog` and `yad`. They are modal MsgBox/InputBox equivalents, not a modeless AHK `Gui` object model. Closing an input returns `""`; declining or cancelling a confirmation returns `false`.

### Persistent GUI panels

For controls that stay open while automation runs, declare a panel and handle button clicks:

```loom
loom "panel-demo" {
    gui "Settings" {
        label "Interval in seconds:"
        input interval = "1.0"
        button apply = "Apply"
    }
    when gui apply clicked {
        log "Interval: " + gui_value("interval")
        call gui_set_text("apply", "Applied")
    }
}
```

`gui_value("interval")` reads an input; `gui_set_text(id, text)` updates an input or button, and `gui_set_visible(title, bool)` shows or hides a panel. Widget IDs are unique across panels. Closing a panel stops the script unless a `when gui window closes` handler is declared. Panels use X11 or Xwayland; their events share Keyloom's scheduler, so timers keep running while the panel is open. See `examples/antiafk.loom` for a persistent Start/Stop control and editable timing fields. This is a small panel API, not full AHK GUI compatibility.

### Runtime hotkey control

```loom
when key F11 goes down {
    call set_hotkey_enabled("A", false)
    call set_hotkey_enabled("D", false)
    notify "A/D bindings disabled"
}

when key F12 goes down {
    call set_hotkey_enabled("A", true)
    call set_hotkey_enabled("D", true)
    notify "A/D bindings enabled"
}
```

`suspend_hotkeys(true)` suspends all regular key/mouse bindings until `suspend_hotkeys(false)` is called. The binding that calls `suspend_hotkeys(true)` remains exempt so it can re-enable the others. Key-up bindings for a key that was accepted before suspension still run, which prevents virtual keys and buttons from being left held. Timers, `when app starts`, the panic key and `remap` declarations are not suspended. `set_hotkey_enabled("F8", false)` or `"MOUSE_LEFT"` controls one binding; `hotkey_enabled("F8")` reports its effective state.

## Mouse buttons

`when mouse left goes down`, `when mouse right goes up`, and
`when mouse middle stays held every 100ms` bind physical mouse buttons. The
available buttons are `left`, `right`, `middle`, `side`, and `extra`.

Mouse buttons are read through evdev like keyboards, but the device is **not
grabbed**: the original click still reaches the focused application. A mouse
binding can add output while a button is down (for example
`key CTRL press`), but it cannot replace or suppress the click. This is the
coexistence behavior; Keyloom has no exclusive mouse mode. Mouse events and key
events use separate names, so the `LEFT` arrow key and the left mouse button do
not collide.

```loom
loom "ctrl-click" {
    option output = "uinput"

    when mouse left goes down {
        key CTRL press
    }

    when mouse left goes up {
        key CTRL free
    }
}
```

## API summary

| Area | Available commands |
|---|---|
| Events | key down/up/held, mouse down/up/held, timer, app start |
| Pointer | absolute warp, relative move, glide, center |
| Mouse | hold/free/tap with independent tokens |
| Keyboard | press/free/tap chords, `type`, AHK-style `send`, native `remap` |
| Consume | swallow keys with `consume key NAME` |
| Desktop | wheel scrolling, clipboard read/write, notifications, opt-in app launch, active-window queries, modal GUI dialogs |
| Flow | variables, expressions, if/else, repeat, while, wait, assert, log |
| Functions | math, conversions, random values, elapsed time, key/button state, screen dimensions, hotkey suspension, active window, modal dialogs |

The language is intentionally constrained: it does not evaluate arbitrary code, execute shell strings, or expose an arbitrary shell statement. `launch` is disabled unless `option allow_launch = true` is set; it uses an argument vector and no shell. Active-window queries are read-only and modal dialogs use `zenity`, `kdialog` or `yad`; neither provides arbitrary system access.

## Backend notes

- `output = "auto"` prefers `ydotool` when its executable is installed and its daemon socket exists, otherwise it uses direct `uinput` when `/dev/uinput` is writable.
- `output = "ydotool"` uses ydotoold for buttons, keys and text. Pointer motion still uses a Keyloom absolute `uinput` device, because ydotool 1.0.x's `mousemove --absolute` first jumps to the top-left corner and only then applies relative deltas.
- `output = "uinput"` creates an absolute pointer plus relative axes for buttons, keys, text and scroll. Some Wayland compositors may treat absolute uinput pointers differently; `ydotool` is still preferred for non-US keyboard layouts and Unicode text.
- `scroll` injects wheel events through `/dev/uinput`, so both backends need that device to be writable.
- `consume key NAME` also needs `/dev/uinput`: Keyloom grabs the physical keyboard and replays all non-consumed events through a virtual passthrough device. Without that replay, grabbing would make the whole keyboard unusable. The panic key is never consumed.
- `type` via uinput uses a US keyboard mapping. ydotool is recommended for non-US layouts and Unicode text.
- `send` lowers its plain-text portions to the same typing path; braced special-key tokens are sent as key events, so use ydotool for non-US Unicode when modifiers are involved.
- Input is event-device based; a compositor may lock input devices while in secure sessions or with special device access policies.

## Development

```bash
make                      # configure, build (Release) and run the test suite
make build                # build Release only, no test suite
make test                 # or: ctest --preset release / ./build/release/keyloom_tests
make debug                # Debug preset
make package              # Arch: build the package with makepkg
make clean                # delete the build directory
```

Architecture: `src/lexer.cpp` → `src/parser.cpp` → `src/checker.cpp` → `src/runtime.cpp`; output drivers implement the `OutputBackend` interface in `src/backends.hpp`. All devices, timers and dispatches are driven by one epoll-based scheduler (`src/scheduler.cpp`). Add parser node kinds and runtime handlers to extend the language, and register new drivers in `choose_backend()`. The mouse-position picker (`--get-mouse-pos`) lives in `src/mousepos.cpp` and needs only X11 and a screenshot tool.

User-visible behavior changes should update this README and `src/docs.cpp` (the `keyloom --docs` guide and `API_REFERENCE`) together.

## License

MIT. See `LICENSE`.
