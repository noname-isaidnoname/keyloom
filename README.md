# Keyloom

Keyloom is a Linux-first desktop automation language and native C++20 runtime. Scripts use readable `.loom` syntax, are statically checked before running, and execute on a single epoll-based event loop. The language never evaluates code or shell strings.

- **Input:** physical keyboards (and optional mouse buttons) via evdev ioctls on `/dev/input/event*`
- **Output:** `ydotool`/`ydotoold` (preferred for keys/buttons/text) plus a direct `uinput` device for absolute pointer motion, scroll, and fallback output
- **Platforms:** designed for Xorg, XWayland, and native Wayland sessions
- **Safety model:** no embedded code execution, no shell evaluation; `launch` is opt-in and uses an argv vector

## Setup

### Release downloads

GitHub Releases provides x86-64 Linux `.deb`, `.rpm`, binary `.tar.gz`,
AppImage and source archives, plus `SHA256SUMS`. Nightly builds are prereleases
and may be untested. Verify downloads with `sha256sum -c SHA256SUMS` (download
all files listed first). Install the native package with your distribution's
package manager, or extract the binary tarball and run its `bin/keyloom`.
For the AppImage, `chmod +x keyloom-*.AppImage` and run it with the usual CLI
arguments, for example `./keyloom-*.AppImage --version`. If FUSE is unavailable,
use `APPIMAGE_EXTRACT_AND_RUN=1 ./keyloom-*.AppImage --version`.

Release binaries are built on Ubuntu 24.04 (glibc 2.39 baseline); older systems
may need a source build. Packages do not install input permissions or optional
desktop tools; see the setup and permissions instructions below.

### Build from source

You need a C++20 compiler, CMake ≥ 3.25, Ninja, pkg-config, and X11/XCB/libpng development headers. GTK3 and Wayland development headers enable native Wayland panels; without them, panels use X11/Xwayland.

Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config libx11-dev libxcb1-dev libpng-dev libgtk-3-dev libwayland-dev
```

Arch:

```bash
sudo pacman -S cmake ninja pkgconf libx11 libxcb libpng gtk3 wayland
```

Build and install:

```bash
./scripts/install.sh                              # installs into /usr/local
PREFIX="$HOME/.local" ./scripts/install.sh        # install into a writable prefix
```

`scripts/install.sh` builds and installs Keyloom. It never uses `sudo`.

On Arch-based distributions, you can build and install a native package:

```bash
cd packaging/arch
makepkg -si   # builds and installs keyloom
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

`option compositor = "auto"` (the default, also used when omitted) chooses
native Wayland panels when a Wayland session and native support are available,
otherwise X11/Xwayland. Set `"wayland"` to require a native Wayland session,
or `"x11"` to require an accessible X11/Xwayland display. These choices affect
persistent `gui` panels, not evdev input or uinput output. The separate
`--get-mouse-pos` picker still needs X11/Xwayland. Wayland does not provide a
portable global window-query API; window-query helpers may return empty values.
Compositor window placement and always-on-top behavior are controlled by the
Wayland compositor and may differ from X11.
When `WAYLAND_SOCKET` is supplied, native panels also require a named
`WAYLAND_DISPLAY`.
When screen detection cannot determine the size on Wayland, set
`option screen = "WIDTHxHEIGHT"` explicitly for coordinate-based actions.

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

## Contributing

See the [contributing guide](docs/contributing.md).

## License

MIT. See `LICENSE`.
