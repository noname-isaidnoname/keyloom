# Keyloom

**Keyloom** is a Linux-first desktop automation language with its own readable syntax, static checker, CLI and runtime. It is designed for Xorg, Xwayland and native Wayland sessions.

- Global physical keyboard input: `python-evdev` (`/dev/input/event*`).
- Preferred keyboard/button output: `ydotool` + `ydotoold` (uinput-based and Wayland-friendly).
- Pointer motion uses Keyloom's own absolute `uinput` device, because ydotool 1.0.x's `mousemove --absolute` jumps to the top-left corner first.
- Fallback output: direct `python-evdev` uinput device.
- No Python code execution is embedded in `.loom` scripts.
- Named mouse holds are reference-counted, so multiple hotkeys can safely hold one mouse button.

## Quick start

Python 3.11+ is required. Activate the environment you want Keyloom in first; the installer never creates one.

```bash
python3 -m venv .venv          # or: uv venv
source .venv/bin/activate
./scripts/install.sh
keyloom --init my-first-script
keyloom --check my-first-script.loom
keyloom --docs
```

`scripts/install.sh` installs editable into `$VIRTUAL_ENV` (or `$CONDA_PREFIX`, `$UV_PROJECT_ENVIRONMENT`, an existing `./.venv`, or an interpreter passed with `PYTHON=/path/to/venv/bin/python`). It uses `uv pip` when `uv` is available, because uv-created environments normally have no `pip`. It refuses the system Python unless `KEYLOOM_ALLOW_SYSTEM_PYTHON=1` is set.

To run a script:

```bash
keyloom --run examples/two_spots.loom
```

The package can also be installed from the included pure-Python wheel with `python -m pip install dist/keyloom-0.1.0-py3-none-any.whl`, or installed editable in a development environment with `python -m pip install -e .`; afterward the `keyloom` command is available on `PATH`.

## The requested A/D hold script

`examples/two_spots.loom` is ready to edit and run. It implements:

- Press/hold **A**: warp to `(640, 420)` and hold left mouse button.
- Release **A**: release A's left-button hold.
- Press/hold **D**: warp to `(1080, 420)` and hold left mouse button.
- Release **D**: release D's left-button hold.

Change the four coordinates to your own desktop. If A and D overlap, the left mouse button remains held until **both** independent holds have been released. This avoids an early mouse-up when one key is released before the other. Physical A/D keystrokes are observed, not suppressed; they still reach the focused application.

## Wayland and Xwayland setup

Keyloom reads physical keyboard events through evdev rather than trying to install a global X11 hotkey listener. This is important on Wayland, where applications normally cannot globally observe arbitrary keyboard input through Xwayland.

### 1. Install system packages

Debian/Ubuntu example:

```bash
sudo apt install ydotool
```

Install the Python dependency through Keyloom's venv installer:

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

A tooltip follows the pointer and shows the current `x, y`. Left-click consumes the click, prints the position (for example `640, 420`) and exits. Right-click cancels; `Esc` also works while the overlay has keyboard focus. The picker first captures the desktop with `spectacle` (KDE), `grim`, `gnome-screenshot`, `scrot`, or ImageMagick's `import`, then shows that still image full-screen in an XWayland overlay. The desktop stays visible and the click does not reach the window underneath; the real cursor and live tooltip are drawn on top. It needs `tkinter` (usually `python3-tk`) and a screenshot tool, but not evdev or `/dev/uinput` permissions.

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

    when key F8 goes down {
        cursor center
        mouse left tap
        log "triggered"
    }

    when key F9 stays held every 100ms {
        log "F9 is still held"
    }
}
```

Run `keyloom --docs` for the full syntax and API guide. Topics for `--docs` include `events`, `pointer`, `mouse`, `keyboard`, `desktop`, `flow`, and `functions`.

## API summary

| Area | Available commands |
|---|---|
| Events | key down/up/held, timer, app start |
| Pointer | absolute warp, relative move, glide, center |
| Mouse | hold/free/tap with independent tokens |
| Keyboard | press/free/tap chords, text typing |
| Desktop | wheel scrolling, clipboard read/write, notifications, opt-in app launch |
| Flow | variables, expressions, if/else, repeat, while, wait, assert, log |
| Functions | math, conversions, random values, elapsed time, key/button state, screen dimensions |

The language is intentionally constrained: it does not evaluate Python, execute shell strings, or expose an arbitrary shell statement. `launch` is disabled unless `option allow_launch = true` is set; it uses `subprocess.Popen` with a tokenized argument vector and no shell.

## Backend notes

- `output = "auto"` prefers `ydotool` when its executable is installed and its daemon socket exists, otherwise it uses direct `uinput` when `/dev/uinput` is writable.
- `output = "ydotool"` uses ydotoold for buttons, keys and text. Pointer motion still uses a Keyloom absolute `uinput` device, because ydotool 1.0.x's `mousemove --absolute` first jumps to the top-left corner and only then applies relative deltas.
- `output = "uinput"` creates an absolute pointer plus relative axes for buttons, keys, text and scroll. Some Wayland compositors may treat absolute uinput pointers differently; `ydotool` is still preferred for non-US keyboard layouts and Unicode text.
- `scroll` injects wheel events through `/dev/uinput`, so both backends need that device to be writable.
- `type` via uinput uses a US keyboard mapping. ydotool is recommended for non-US layouts and Unicode text.
- Input is event-device based; a compositor may lock input devices while in secure sessions or with special device access policies.

## Development

```bash
python -m unittest discover -s tests -v
```

Architecture: `lexer.py` → `parser.py` → `checker.py` → `runtime.py`; output drivers implement the protocol in `backends.py`. Add parser node kinds and runtime handlers to extend the language, and register new drivers in `choose_backend()`.

## License

MIT. See `LICENSE`.
