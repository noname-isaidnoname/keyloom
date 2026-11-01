# AGENTS.md

Keyloom is a Linux-first desktop automation language. A `.loom` file is parsed, statically checked, and run by an asyncio runtime. Input comes from physical keyboards through `python-evdev`; output goes through `ydotool`/`ydotoold` or a direct `uinput` device. The language never evaluates Python or shell strings. `launch` is opt-in and uses an argv vector with no shell.

User docs: `README.md`. Language reference: `keyloom --docs` (`src/keyloom/docs.py`). Examples: `examples/*.loom`.

## Layout

| path | owns |
| --- | --- |
| `src/keyloom/lexer.py` | tokens, strings, durations, comments |
| `src/keyloom/parser.py` | `loom` block, options, `when` events, statements, expressions |
| `src/keyloom/checker.py` | option values, key names, hold-token balance, `/dev/uinput` warnings |
| `src/keyloom/runtime.py` | event loop, key tracking, dispatch, flow, expressions, hold refcounts |
| `src/keyloom/backends.py` | `OutputBackend` protocol, ydotoold, uinput, dry run, backend selection |
| `src/keyloom/cli.py` | `--init`, `--check`, `--run`, `--docs`, `--devices`, `--screen`, `--doctor`, `--list-api`, `--version` |
| `src/keyloom/docs.py` | user-facing language guide and `API_REFERENCE` |
| `examples/two_spots.loom` | A/D hold regression script used by tests |
| `tests/test_language.py` | parser, checker, hold refcounts, two-spots dispatch, backend regression tests |

## Environment and install

Python 3.11+. This machine is CachyOS with KDE Wayland, `/dev/uinput` writable, and `ydotoold` running.

`./scripts/install.sh` installs editable into an existing environment. It never creates a venv. Interpreter selection order:

1. `PYTHON`
2. `VIRTUAL_ENV`
3. `CONDA_PREFIX`
4. `UV_PROJECT_ENVIRONMENT`
5. `./.venv`
6. a `python3` already inside a venv

It uses `uv pip install --python ... -e .` when `uv` exists because uv-created environments normally have no `pip`. Otherwise it uses `pip`, with `ensurepip` as a fallback. It refuses the distro Python unless `KEYLOOM_ALLOW_SYSTEM_PYTHON=1`.

```bash
source /home/fesuoy/Desktop/Stuff/Python/.venv/bin/activate   # or uv run
./scripts/install.sh
```

Live input needs read access to `/dev/input/event*` (`input` group or a udev rule). Pointer motion and scroll need write access to `/dev/uinput`. Buttons, keys, and text via ydotoold need the daemon running.

## Verify

```bash
python -m unittest discover -s tests -v
keyloom --check examples/two_spots.loom
keyloom --run examples/two_spots.loom --dry-run
keyloom --doctor
keyloom --screen
```

The suite is small; run all of it after touching the lexer, parser, checker, runtime, or backends. For pointer work, a real check beats a fake backend: run the script, press A and D, and confirm the pointer lands at `640,420` and `1080,420`. On KWin, a temporary scripting file can log `workspace.cursorPos` to the journal:

```js
function main() { console.log("KEYLOOM_POS " + workspace.cursorPos.x + "," + workspace.cursorPos.y); }
main();
```

Load it with `qdbus org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript FILE` and `...Scripting.start`. Do not test real mouse holds over important windows; they inject `BTN_LEFT`.

## Backend traps

- `ydotool mousemove --absolute` is not a real absolute warp in ydotool 1.0.4. It sends `REL_X/REL_Y = INT32_MIN` first (top-left), then applies the target as relative deltas. Pointer acceleration turns that into a corner jump or a wrong position. Measured on KWin: `--absolute 640 420` landed at `0,0`. Never use it for `cursor warp`.
- `YdotoolBackend` therefore uses its own `UInputBackend` absolute pointer lazily for `warp`, `move`, and `scroll`, and keeps ydotoold for buttons, keys, and text. If `/dev/uinput` is not writable, pointer motion fails with a clear error instead of falling back to the top-left hack.
- `evdev.AbsInfo` argument order is `(value, min, max, fuzz, flat, resolution)`. Passing `(0, width - 1, 0, ...)` puts max before min; the kernel rejects device creation with `EINVAL`. Use `absolute_axis_info(width - 1)`.
- `output = "auto"` uses ydotoold when the client and its socket exist, otherwise `uinput` when `/dev/uinput` is writable, otherwise ydotoold again so its error message can explain the missing daemon.
- `ydotool_socket_path()` checks `YDOTOOL_SOCKET`, then `$XDG_RUNTIME_DIR/.ydotool_socket`, then `/tmp/.ydotool_socket`.
- Buttons use ydotool's encoded values: `button | 0x40` for down, `button | 0x80` for up. Keys use `code:1` and `code:0`.
- `type_text` through ydotool passes `-e 0 --`, because Keyloom already decoded the `.loom` string. Letting ydotool escape again turns a literal backslash before `n` into a newline.
- The uinput pointer writes `ABS_X`/`ABS_Y` for warps and `REL_X`/`REL_Y` for moves, then `syn()`. Coordinates are clamped to `0..width-1` and `0..height-1`.
- The uinput text mapper covers US layout only. Add new shifted symbols to both `shifted` and `shifted_map`, and new plain symbols to `plain`.
- `normalize_key_event()` reads `ecodes.KEY`; some codes map to lists of aliases, so take the first entry.
- Avoid reacting to injected devices. `_open_keyboards()` drops names containing `keyloom`, `ydotool`, or `virtual pointer`.

## Runtime traps

- Mouse holds are reference-counted by owner token. The default token is `default:<event_key>:<button>`. Use explicit `as TOKEN` when two keys can hold the same button. Releasing one owner must not release the physical button until the last owner frees it.
- Key presses are observed, not suppressed. `pressed_keys` is global across devices; `device_down` tracks per-device state so the up edge fires only after the last device releases the key.
- Held timers live in `held_tasks`, keyed by `(key, line)`, and are cancelled on key-up and device disconnect.
- Dispatch runs in asyncio tasks from `_spawn_dispatch()`. `event_key` and `event_edge` are context variables, not globals.
- `repeat` and `while` cap at `MAX_LOOP = 10000`. A panic key or Ctrl+C sets `stop_event`.
- `run()` releases every held button and key and closes the backend in `finally`. Keep `close()` defensive and idempotent.
- `cursor glide` starts from Keyloom's last known cursor position, or the screen center. It does not query the real pointer.
- The panic key is checked before regular bindings, so a script binding to the panic key is unreachable. The checker warns about that.

## Language rules

- Top level accepts only `option name = value` and `when ... { ... }`. A file needs at least one event block.
- Options: `input` (`auto`/`evdev`, currently informational), `output` (`auto`/`ydotool`/`uinput`), `panic` (key name or `"none"`), `screen` (`auto` or `WIDTHxHEIGHT`), `allow_launch` (boolean).
- Events: `when key NAME goes down/up`, `when key NAME stays held every DURATION`, `when timer every DURATION`, `when app starts`.
- Key names accept identifiers and numeric tokens, so `when key 1 goes down` and `key CTRL+1 tap` work. Key validation lives in `validate_key_name()`.
- Statements: `cursor`, `mouse`, `key`, `type`, `wait`, `scroll`, `log`/`say`, `notify`, `clip write`/`clip read into`, `launch`, `set`, `if`/`else`, `repeat`, `while`, `assert`, `call`.
- Expressions: literals, variables, function calls, unary `-`/`+`/`not`/`!`, arithmetic, comparisons, `and`/`or` (also `&&`/`||`), parentheses.
- Strings decode `\\`, `\n`, `\r`, `\t`, `\'`, and `\"` in one pass. Keep it single-pass; chained `str.replace` broke `"\\n"`.
- Add a statement in three places: a `Node` in `parser.py`, a handler in `Runtime._execute()`, and docs plus `API_REFERENCE` in `docs.py`. Add a checker rule when the statement has static constraints.
- Add a built-in in `Runtime._eval()` and list it in `docs.py`.
- Add a backend by implementing `warp`, `move`, `button`, `key`, `type_text`, `scroll`, `close`, then register it in `choose_backend()`.

## CLI behavior

- `--check` must not open input devices or require permissions. It prints diagnostics and returns 2 on errors.
- `--run` refuses scripts with checker errors. `--dry-run` logs actions but still listens for physical keys.
- Bad paths and parse errors print `keyloom: ...` and return 2, not a traceback.
- `--init` refuses to overwrite unless `--force` is passed.

## Style

- Python 4-space indentation, small helpers, explicit exceptions (`BackendError`, `RuntimeErrorKeyloom`, `ParseError`).
- Keep the DSL small. Do not add arbitrary code execution, shell strings, or file/network APIs without a deliberate design change.
- Update `README.md`, `docs.py`, and `API_REFERENCE` together when user-visible behavior changes.
- Use `rg` for searching. Keep new code warning-clean and covered by a focused test.
