# Language internals

The user-facing language reference is `keyloom --docs` (`kGuide` and
`api_reference()` in `src/docs.cpp`). This document describes the internal
shape of the language and how to extend it. Keep the two in sync when the
language changes.

## Grammar

A `.loom` file contains exactly one `loom "name" { ... }` block. Top level
accepts only:

- `option name = value`
- `remap key SOURCE to TARGET`
- `gui "title" { ... }`
- `func NAME(params) { ... }`
- `when ... { ... }`

A file needs at least one event block. Blocks use braces, one action per line,
and `#` comments.

### Options

| Option | Values |
| --- | --- |
| `input` | `auto`, `evdev` (currently informational) |
| `output` | `auto`, `ydotool`, `uinput` |
| `compositor` | `auto` (default), `x11`, `wayland` — selects panel backend |
| `panic` | key name or `"none"` |
| `screen` | `auto` or `WIDTHxHEIGHT` |
| `allow_launch` | boolean |

### Events

- `when key NAME goes down/up`
- `when key NAME stays held every DURATION`
- `when mouse BUTTON goes down/up`
- `when mouse BUTTON stays held every DURATION`
- `when timer every DURATION`
- `when app starts`
- `when gui BUTTON clicked`
- `when gui window closes`

Chords use `+` (`CTRL+ALT+T`). The last key is the primary key; the preceding
keys must be physically held when it goes down. `event_key` inside a chord
handler is the primary key.

### Statements

`StmtKind` in `src/ast.hpp`:

`Cursor`, `Mouse`, `Key`, `Consume`, `Type`, `Wait`, `Scroll`, `Log`, `Notify`,
`ClipWrite`, `ClipRead`, `Launch`, `Set`, `If`, `Repeat`, `While`, `For`,
`Break`, `Continue`, `Assert`, `Call`, `Return`, `Send`.

### Expressions

`Expr::Kind` in `src/ast.hpp`: `Literal`, `Variable`, `Unary`, `Binary`,
`Call`, `List`, `Index`. Operators are `+ - * / %`, comparisons, `and`/`or`
(also `&&`/`||`), unary `-`/`+`/`not`/`!`, and parentheses.

## AST and data model

- `Script` — name, path, options, events, remaps, panels, functions.
- `Node` — one statement or event block. Flat fields mirror the original
  tuple-based nodes: event fields (`event_kind`, `event_key`, `chord_parts`,
  `edge`, `interval`) and statement fields (`a`, `b`, `c`, `exprs`,
  `has_duration`, `duration`, `body`, `otherwise`, `line`).
- `Expr` — expression tree; `ExprPtr` is a `shared_ptr`.
- `Function` — name, params, body, line.
- `GuiPanel` / `GuiWidget` — persistent panel declarations.
- `Remap` — source and target key/chord.
- `Value` — int/double/bool/string with Python-style `repr`/`str` and
  truthiness.

## Statement dispatch

`Runtime::execute()` (`src/runtime.cpp`) switches on `StmtKind` and handles one
statement. `execute_nodes()` runs a body and propagates `Break`, `Continue` and
`Return` signals. `eval()` is a coroutine because a user-defined function may
wait; it cannot return its result through a plain return value.

`call_function()` implements every builtin in the table below.
`call_user_function()` binds parameters into a fresh scope, pushes it, runs the
body, and pops it on the way out. A function can read the script's variables
but its own assignments stay local.

## Builtins

The table in `src/builtins.cpp` is the single source of truth for names and
arity. `Runtime::call_function` implements exactly these names; anything else
is an unknown function. Current groups:

- Numbers/general: `min`, `max`, `abs`, `round`, `floor`, `ceil`, `sqrt`,
  `clamp`, `int`, `float`, `is_number`, `str`, `bool`, `len`, `random`,
  `randint`, `elapsed`.
- Strings: `upper`, `lower`, `trim`, `starts_with`, `ends_with`, `contains`,
  `replace`, `substr`, `index_of`.
- Lists: `join`, `split`, `append`, `sort`.
- Runtime state: `key_down`, `mouse_held`, `suspend_hotkeys`,
  `hotkeys_suspended`, `set_hotkey_enabled`, `hotkey_enabled`, `screen_width`,
  `screen_height`.
- Windows/dialogs: `active_window_title`, `active_window_class`,
  `active_window_process`, `active_window_pid`, `gui_alert`, `gui_input`,
  `gui_confirm`.
- Panels: `gui_value`, `gui_set_text`, `gui_set_visible`.

## Key names

Key names accept identifiers and numeric tokens, so `when key 1 goes down` and
`key CTRL+1 tap` work. Validation lives in `validate_key_name()`
(`src/keys.cpp`). `canonical_key_name()` upper-cases and resolves aliases;
`canonical_key()` in `src/runtime.cpp` wraps it.

Mouse buttons are `left`, `right`, `middle`, `side`, `extra`
(`mouse_buttons()` in `src/keys.cpp`). Runtime event names carry a `MOUSE_`
prefix (`MOUSE_LEFT`, ...), and `normalize_key_event()` maps `BTN_*` codes to
them, so the left button can never fire a `LEFT` arrow-key binding.

## String decoding

Strings decode `\\`, `\n`, `\r`, `\t`, `\'`, and `\"` in **one pass**. Keep it
single-pass; a chained replace broke `"\\n"`.

## Extension points

### Add a statement

1. Add a `StmtKind` to `src/ast.hpp`.
2. Parse it in `src/parser.cpp`.
3. Handle it in `Runtime::execute()` (`src/runtime.cpp`).
4. Add a checker rule in `src/checker.cpp` when it has static constraints.
5. Document it in `src/docs.cpp` (`kGuide` and `api_reference()`).

### Add a builtin

1. Add the name and arity to the table in `src/builtins.cpp`.
2. Implement it in `Runtime::call_function()`.
3. Document it in `src/docs.cpp`.

### Add a backend

Implement `warp`, `move`, `button`, `key`, `type_text`, `scroll`, `close`, and
`name` in `OutputBackend` (`src/backends.hpp`), then register it in
`choose_backend()` (`src/backends.cpp`). Read
[implementation-traps.md](implementation-traps.md#backends) first.

## Checker

`src/checker.cpp` validates options against `option_rules()`, key names,
builtin arity, hold-token balance, `break`/`continue`/`return` placement,
remap conflicts, GUI IDs, and warns when `/dev/uinput` is not writable. It runs
before any device is opened. `--check` collects diagnostics and returns 2 on
errors; it never opens input devices or requires permissions.
