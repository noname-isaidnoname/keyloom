# CLI

`src/cli.cpp` parses arguments, dispatches one command, and maps failures to
exit codes. `src/main.cpp` is a thin wrapper around `run_cli()`.

## Surface

```text
keyloom [-h] [--init SCRIPT | --check PATH | --run PATH | --docs [TOPIC] | --devices |
               --screen | --get-mouse-pos | --list-api | --doctor]
               [--force] [--dry-run] [--input-device EVENT_PATH]
               [--output-backend {auto,ydotool,uinput}] [--screen-size WIDTHxHEIGHT] [--verbose]
               [--version] [script]
```

A positional `script` is shorthand for `--run PATH`. Only one mode flag is
allowed per invocation; `--force`, `--dry-run`, `--verbose`,
`--input-device`, `--output-backend` and `--screen-size` modify the mode.
`--docs` takes an optional topic and treats the next non-flag token as the
topic.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success (including `--run` interrupted by the panic key/Ctrl+C) |
| 1 | Runtime failure while running a script |
| 2 | Usage error, unreadable path, parse error, checker error, `--init` failure, picker startup failure |
| 130 | Mouse-position pick cancelled |

Bad paths and parse errors print `keyloom: ...` and return 2, not a traceback or
exit 1. Runtime failures print `keyloom: ...` and return 1.

## Commands

### `--init SCRIPT`

Creates a starter `.loom` file (a `loom` block with `F8` down/up handlers).
Refuses to overwrite an existing file unless `--force` is passed. Returns 2 on
a write failure or when refusing to overwrite.

### `--check PATH`

Parses and statically checks the script. It must not open input devices or
require permissions. It prints diagnostics and returns 2 on errors, 0 on
success.

### `--run PATH`

Parses, checks, and refuses to run when the checker reports errors. Otherwise
it builds a `Runtime`, spawns `runtime.run()` onto the scheduler, and drives the
loop. After the loop it rethrows `runtime.failure()` if one was recorded.
`--dry-run` logs output actions instead of injecting them, but still listens
for physical keys. `--input-device`, `--output-backend`, `--screen-size` and
`--verbose` override the script/CLI options.

### `--docs [TOPIC]`, `--list-api`

`--docs` renders `kGuide` from `src/docs.cpp`, or one topic. `--list-api`
prints the `api_reference()` groups.

### `--devices`, `--screen`, `--doctor`

`--devices` opens each `/dev/input/event*` it can read and prints its name and
kind (keyboard/mouse/other). `--screen` prints the detected desktop bounds.
`--doctor` prints the detected session type, desktop bounds, accessible evdev
devices, ydotool/ydotoold, `/dev/uinput`, X11, screenshot tools and clipboard
tools.

## Mouse-position picker

`--get-mouse-pos` is an X11 client (XWayland on Wayland) and never needs evdev.
It is implemented in `src/mousepos.cpp`, `src/picker_xcb.cpp` and
`src/picker_image.cpp`.

Flow:

1. Capture the desktop with spectacle, grim, gnome-screenshot, scrot, or
   ImageMagick `import`.
2. Show that still image in a full-screen **managed** splash window.
3. Follow the pointer with a separate tooltip.
4. Left-click prints `x, y` and returns 0. Right-click (or Esc when focused)
   returns 130.

The opaque, managed splash window is what makes clicks over native Wayland
windows land on the picker; an override-redirect or transparent window does not
capture them.

Keep the pure helpers testable without opening a display:
`format_coordinates`, `clamp_to_screen`, `tooltip_position`,
`available_screenshot_tools` and `screenshot_commands` all take injectable
finders or none. `--get-mouse-pos` itself is tested through
`run_cli(args, picker_fn)`; pass a stub picker, never the real overlay.

## Testing the CLI

`run_cli(const std::vector<std::string>&, const PickerFunction&)` is the seam
used by `tests/test_language.cpp`. `PickerFunction` returns a position,
`std::nullopt` for a cancel, or throws `PickerError` for a startup failure. An
empty function calls the real picker. `parse_cli`, `usage_text`, `help_text`
and `version_string` are directly testable.
