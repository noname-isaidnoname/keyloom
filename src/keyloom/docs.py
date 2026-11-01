from __future__ import annotations

DOCS = r'''
KEYLOOM LANGUAGE GUIDE
======================

Keyloom is a Linux desktop automation language. A .loom file is parsed and
checked before it is run. It is deliberately not Python or AutoHotkey syntax.

A minimal file
--------------

    loom "hello" {
        option input = "evdev"
        option output = "auto"
        option panic = "PAUSE"

        when key F8 goes down {
            log "F8 pressed"
        }
        when key F8 goes up {
            log "F8 released"
        }
    }

Syntax rules
------------

* A file contains one `loom "name" { ... }` block.
* Top-level declarations are `option name = value` and `when ... { ... }`.
* Blocks use braces. Each action occupies one line; comments begin with #.
* Strings use single or double quotes. Escape sequences include \\, \n, \t.
* Durations: 25ms, 0.5s, 2m, 1h. A bare number in a duration position means seconds.
* Expressions support + - * / %, comparisons, and/or/not, parentheses, variables,
  literals, and function calls. Booleans are true and false.

Event triggers
--------------

    when key A goes down { ... }       # first physical key-down edge
    when key A goes up { ... }         # final physical key-up edge
    when key A stays held every 80ms { ... }  # repeating while held
    when timer every 2s { ... }        # periodic event
    when app starts { ... }            # one time at runtime start

Keyboard names are Linux input names: A-Z, 0-9, F1-F24, LEFT, RIGHT, UP,
DOWN, ENTER, SPACE, TAB, ESC, BACKSPACE, DELETE, CTRL, SHIFT, ALT, SUPER,
HOME, END, PAGEUP, PAGEDOWN, INSERT, PAUSE. Chords use +, e.g. CTRL+SHIFT+S.

Actions
-------

    cursor warp 420, 300          # absolute desktop coordinates
    cursor move 20, -10           # relative motion
    cursor glide 900, 500 over 400ms
    cursor center                 # center of configured screen bounds

    mouse left hold as drag_a     # acquire a named hold token
    mouse left free as drag_a     # release only that token
    mouse right tap               # quick click (60ms default)
    mouse middle tap for 150ms

    key CTRL press                 # hold a key down
    key CTRL free                  # release a key
    key CTRL+S tap for 80ms        # press chord, then release in reverse order
    type "Hello from Keyloom"      # type text via selected output backend
    scroll 0, -3                   # vertical/horizontal wheel (needs /dev/uinput)
    wait 150ms

    log "diagnostic message"
    notify "automation step complete"  # desktop notification if notify-send exists
    clip write "text"              # set clipboard
    clip read into clipboard_text  # read clipboard into variable
    launch "gnome-calculator"      # requires option allow_launch = true

    set count = 3
    if count > 1 {
        log "many"
    } else {
        log "one"
    }
    repeat count {
        log "again"
    }
    while count > 0 {
        set count = count - 1
    }
    assert count == 0

Expressions and built-in functions
----------------------------------

Variables: screen_width, screen_height, event_key, event_edge, plus names
created with `set` or `clip read into`.

Functions: min(a,b,...), max(a,b,...), abs(x), round(x), floor(x), ceil(x),
sqrt(x), clamp(value, low, high), int(x), float(x), str(x), bool(x), len(x),
random(), randint(low, high), elapsed(), key_down("A"), mouse_held("left"),
screen_width(), screen_height(). The screen_width/screen_height variables are
also available without parentheses.

Options
-------

    option input = "evdev"       # currently evdev; `auto` is an alias
    option output = "auto"        # auto, ydotool, or uinput
    option screen = "auto"        # or an explicit string, e.g. "2560x1440"
    option panic = "PAUSE"        # stop key; set "none" to disable
    option allow_launch = false   # launch command is denied unless true

Safety and runtime behavior
---------------------------

* The panic key defaults to PAUSE. Ctrl+C also stops the runtime.
* The runtime releases all held virtual buttons/keys on stop and on errors.
* Named mouse hold tokens are reference-counted. Use a different token for
  each independent physical key if several bindings can hold the same button.
* `repeat` and `while` loops are capped at 10,000 iterations per block.
* `launch` is disabled unless explicitly enabled; it never invokes a shell.
* `--check` parses and statically checks a script without opening input devices.
* `--dry-run` logs actions instead of injecting them, but still listens for keys.
* `--doctor` reports evdev, ydotool, uinput, Wayland/X11 and screen prerequisites.

Linux backends
--------------

Input uses python-evdev and reads physical devices from /dev/input/event*. This
works under Xorg, Xwayland, and Wayland because it is below the compositor; the
user needs permission to read the intended input devices. Prefer a udev rule or
an `input` group membership over running the program as root.

For output, `ydotool` + `ydotoold` is the recommended Wayland/Xwayland
backend for keyboard output. ydotool 1.0.x's `mousemove --absolute` is not a
real absolute warp (it jumps to the top-left first), so Keyloom uses its own
absolute `uinput` pointer for `cursor warp`, `cursor move` and `scroll`.
Start `ydotoold` in your user session and make sure `/dev/uinput` is writable.
The `uinput` backend is a direct python-evdev fallback for everything;
ABS_X/ABS_Y pointer mapping is compositor-dependent. Multi-monitor layouts and
fractional scaling can change coordinate interpretation.

API extension points
--------------------

The runtime is split into a lexer, parser, static checker, input listener,
and an OutputBackend protocol. Add an output driver by implementing warp,
move, button, key, type_text, scroll, and close, then register it in
backends.choose_backend(). New language statements are represented as Node
kinds in parser.py and handled in Runtime._execute(). Expression built-ins are
registered in Runtime._eval(). This keeps integrations isolated from parsing.
'''.strip()

API_REFERENCE = {
    "events": [
        "when key NAME goes down", "when key NAME goes up",
        "when key NAME stays held every DURATION", "when timer every DURATION",
        "when app starts",
    ],
    "pointer": ["cursor warp X, Y", "cursor move DX, DY", "cursor glide X, Y over DURATION", "cursor center"],
    "mouse": ["mouse BUTTON hold as TOKEN", "mouse BUTTON free as TOKEN", "mouse BUTTON tap [for DURATION]"],
    "keyboard": ["key KEY_OR_CHORD press", "key KEY_OR_CHORD free", "key KEY_OR_CHORD tap [for DURATION]", "type STRING"],
    "desktop": ["scroll X, Y", "clip write EXPR", "clip read into VARIABLE", "notify EXPR", "launch EXPR (allow_launch=true required)"],
    "flow": ["set NAME = EXPR", "if EXPR { ... } else { ... }", "repeat EXPR { ... }", "while EXPR { ... }", "wait DURATION", "assert EXPR", "log EXPR"],
    "functions": ["min", "max", "abs", "round", "floor", "ceil", "sqrt", "clamp", "int", "float", "str", "bool", "len", "random", "randint", "elapsed", "key_down", "mouse_held", "screen_width", "screen_height"],
}


def render_docs(topic: str | None = None) -> str:
    if not topic or topic.lower() in ("all", "guide"):
        return DOCS
    key = topic.lower()
    if key not in API_REFERENCE:
        return f"Unknown docs topic {topic!r}. Topics: {', '.join(API_REFERENCE)}"
    lines = [f"KEYLOOM API: {key.upper()}", "=" * (12 + len(key))]
    lines.extend(f"  {item}" for item in API_REFERENCE[key])
    return "\n".join(lines)
