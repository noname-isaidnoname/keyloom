#include "docs.hpp"

#include <cctype>
#include <string>
#include <vector>

#include "value.hpp"

namespace keyloom {
namespace {

const char* const kGuide = R"DOC(KEYLOOM LANGUAGE GUIDE
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

A `stays held every DURATION` block ticks on a fixed schedule: time spent
inside the block is part of the interval, not added to it, so one 5ms tap in an
`every 10ms` block repeats every 10ms. A block that overruns its interval skips
whole ticks instead of firing a burst of catch-up ticks.

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
    consume key A                  # swallow A (including key-up and autorepeat)
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
* `consume key NAME` is a script-wide declaration, not a per-event action: it
  applies from startup no matter where it appears, so the first key-down can be
  swallowed. Keyloom grabs the physical keyboard and replays every other event
  through a virtual uinput device; this needs `/dev/uinput` write access and the
  same device permissions as reading. The panic key is never consumed.
* The runtime releases all held virtual buttons/keys on stop and on errors.
* Named mouse hold tokens are reference-counted. Use a different token for
  each independent physical key if several bindings can hold the same button.
* `repeat` and `while` loops are capped at 10,000 iterations per block.
* `launch` is disabled unless explicitly enabled; it never invokes a shell.
* `--check` parses and statically checks a script without opening input devices.
* `--dry-run` logs actions instead of injecting them, but still listens for keys.
* `--get-mouse-pos` captures the desktop with spectacle, grim,
  gnome-screenshot, scrot, or ImageMagick import, then shows that still image
  full-screen in an XWayland overlay with a live coordinate tooltip. Left-click
  consumes the click, prints `x, y`, and exits; right-click cancels (Esc works
  when the overlay has focus). It needs X11 and a screenshot tool, not evdev
  or /dev/uinput permissions.
* `--doctor` reports input devices, ydotool, uinput, X11/Wayland and screen
  prerequisites.

Linux backends
--------------

Input reads physical devices from /dev/input/event* with evdev ioctls. This
works under Xorg, Xwayland, and Wayland because it is below the compositor; the
user needs permission to read the intended input devices. Prefer a udev rule or
an `input` group membership over running the program as root.

For output, `ydotool` + `ydotoold` is the recommended Wayland/Xwayland
backend for keyboard output. ydotool 1.0.x's `mousemove --absolute` is not a
real absolute warp (it jumps to the top-left first), so Keyloom uses its own
absolute `uinput` pointer for `cursor warp`, `cursor move` and `scroll`.
Start `ydotoold` in your user session and make sure `/dev/uinput` is writable.
The `uinput` backend writes input_event structures straight to /dev/uinput for
everything; ABS_X/ABS_Y pointer mapping is compositor-dependent. Multi-monitor
layouts and fractional scaling can change coordinate interpretation.

API extension points
--------------------

The runtime is split into a lexer, parser, static checker, input listener,
and an OutputBackend interface. Add an output driver by implementing warp,
move, button, key, type_text, scroll, and close, then register it in
backends::choose_backend(). New language statements are represented as Node
kinds in src/parser.cpp and handled in Runtime::execute(). Expression built-ins
are registered in Runtime::eval(). This keeps integrations isolated from parsing.

Performance notes
-----------------

The C++20 runtime drives every device, timer and dispatch from one epoll-based
event loop, so a key press never waits for an interpreter or a thread hand-off.
uinput events are batched into one write per report instead of one syscall per
event, and pointer, button and key output goes straight to /dev/uinput instead
of spawning a helper process. The `uinput` output backend is therefore the
lowest-latency choice for tight `stays held every` loops; `ydotool` remains the
right choice for non-US layouts and Unicode text.)DOC";

const std::vector<std::pair<std::string, std::vector<std::string>>>& reference() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
        {"events",
         {"when key NAME goes down", "when key NAME goes up", "when key NAME stays held every DURATION",
          "when timer every DURATION", "when app starts"}},
        {"pointer",
         {"cursor warp X, Y", "cursor move DX, DY", "cursor glide X, Y over DURATION",
          "cursor center"}},
        {"mouse",
         {"mouse BUTTON hold as TOKEN", "mouse BUTTON free as TOKEN",
          "mouse BUTTON tap [for DURATION]"}},
        {"keyboard",
         {"key KEY_OR_CHORD press", "key KEY_OR_CHORD free", "key KEY_OR_CHORD tap [for DURATION]",
          "type STRING"}},
        {"input", {"consume key NAME"}},
        {"desktop",
         {"scroll X, Y", "clip write EXPR", "clip read into VARIABLE", "notify EXPR",
          "launch EXPR (allow_launch=true required)"}},
        {"flow",
         {"set NAME = EXPR", "if EXPR { ... } else { ... }", "repeat EXPR { ... }",
          "while EXPR { ... }", "wait DURATION", "assert EXPR", "log EXPR"}},
        {"functions",
         {"min", "max", "abs", "round", "floor", "ceil", "sqrt", "clamp", "int", "float", "str",
          "bool", "len", "random", "randint", "elapsed", "key_down", "mouse_held", "screen_width",
          "screen_height"}},
    };
    return table;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

const std::string& language_guide() {
    static const std::string guide = kGuide;
    return guide;
}

const std::vector<std::pair<std::string, std::vector<std::string>>>& api_reference() { return reference(); }

std::string render_docs(const std::string& topic) {
    const std::string key = lower_ascii(topic);
    if (key.empty() || key == "all" || key == "guide") return language_guide();
    for (const auto& group : reference()) {
        if (group.first != key) continue;
        std::string title = "KEYLOOM API: " + group.first;
        for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::vector<std::string> lines;
        lines.push_back(title);
        lines.push_back(std::string(12 + group.first.size(), '='));
        for (const auto& item : group.second) lines.push_back("  " + item);
        std::string out;
        for (size_t index = 0; index < lines.size(); ++index) {
            if (index > 0) out += "\n";
            out += lines[index];
        }
        return out;
    }
    std::string topics;
    for (const auto& group : reference()) {
        if (!topics.empty()) topics += ", ";
        topics += group.first;
    }
    return "Unknown docs topic " + py_repr_string(topic) + ". Topics: " + topics;
}

}  // namespace keyloom
