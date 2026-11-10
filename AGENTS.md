# AGENTS.md

Keyloom is a Linux-first desktop automation language. A `.loom` file is parsed, statically checked, and run by a native C++20 runtime built on one epoll-based event loop (`src/scheduler.cpp`). Input comes from physical keyboards through evdev ioctls on `/dev/input/event*`; output goes through `ydotool`/`ydotoold` or a direct `uinput` device written with plain `input_event` structures. The language never evaluates code or shell strings. `launch` is opt-in and uses an argv vector with no shell.

This repo is the C++ port. The original Python implementation lives at `/home/fesuoy/Desktop/Stuff/Python/keyloom` and **was** the behavioral reference (not touched for a while; consider the python version deprecated); the C++ port has improvements/features over the original, so you no longer need it to produce identical diagnostics and runtime behavior.

## Documentation

User docs: `README.md`. Language reference: `keyloom --docs` (`src/docs.cpp`). Examples: `examples/*.loom`.

Internal docs live in `docs/`; start at [`docs/README.md`](docs/README.md).

| Document | What it covers |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | Repo layout, compilation units, lexer→parser→checker→runtime pipeline, scheduler/task model, input/output/GUI subsystems |
| [`docs/build-and-install.md`](docs/build-and-install.md) | Prerequisites, build presets, Makefile targets, CMake options, install layout, Arch packaging, runtime permissions |
| [`docs/testing.md`](docs/testing.md) | Test-suite layout, running the suite, unit and integration tests, sanitizers, verification commands |
| [`docs/benchmark.md`](docs/benchmark.md) | Measured build/test performance, the optimisations behind it, how to reproduce the numbers |
| [`docs/implementation-traps.md`](docs/implementation-traps.md) | Backend, input and runtime traps: ydotool absolute motion, uinput classification, coroutine stack growth, cancellation, holds, evdev recovery |
| [`docs/language-internals.md`](docs/language-internals.md) | Grammar, AST, statement dispatch, builtins, key names, string decoding, and the extension points for statements/builtins/backends |
| [`docs/cli.md`](docs/cli.md) | CLI surface, exit codes, `--check`/`--run` contracts, mouse-position picker architecture |
| [`docs/contributing.md`](docs/contributing.md) | Code style, docs-sync rules, versioning policy, commit policy |
| [`docs/state-machines.md`](docs/state-machines.md) | Working audit of the implicit state machines and their failure paths |

## Repository rules

- C++20, 4-space indentation, RAII, explicit exception types, and a warning-clean build. See [`docs/contributing.md`](docs/contributing.md).
- Keep the DSL small: no arbitrary code execution, shell strings, or file/network APIs without a deliberate design change.
- Update `README.md` and `src/docs.cpp` (`kGuide` and `api_reference()`) with user-visible changes, and keep the relevant `docs/*.md` in sync. See [`docs/contributing.md`](docs/contributing.md).
- Versioning: bump `project(keyloom VERSION ...)` in `CMakeLists.txt` (the source of truth) and keep `packaging/arch/PKGBUILD` in sync. See [`docs/contributing.md#versioning`](docs/contributing.md#versioning).
- Commit once verified: run `make` (build plus the full test suite) and, when the language changes, `keyloom --check` on every script in `examples/`. See [`docs/contributing.md#committing`](docs/contributing.md#committing).
- The current session is **real**: Wayland (with Xwayland), KDE Plasma, CachyOS. Do not assume the session type, compositor, distro or package manager anywhere — detect at runtime and keep docs generic. See [`docs/build-and-install.md#environment-assumptions`](docs/build-and-install.md#environment-assumptions).

## Gotchas

- Avoid using subagents. When they finish giving the report/summary of the task they were given, you do not know the context of places they searched, so you are better off directly reading/grepping files.
