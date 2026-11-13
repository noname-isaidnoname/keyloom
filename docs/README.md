# Loomwright documentation

This folder is the home for Loomwright's internal and agent-facing documentation:
how the code is organised, how it is built and tested, the traps that are easy
to fall into, and the rules for changing the language and shipping a release.

User-facing documentation lives outside this folder:

| Audience | Document |
| --- | --- |
| Users | `README.md` (setup, CLI, language overview, examples) |
| Users | `loomwright --docs [topic]` — the language guide in `src/docs.cpp` |
| Users | `examples/*.loom` — runnable scripts |
| Agents and contributors | `AGENTS.md` — the entry point and repository rules |
| Agents and contributors | `docs/gather-context.md` — using `scripts/gather_context.py` to search AGENTS.md, docs, and the codebase |
| Agents and contributors | `docs/` — this folder |

## Internal documentation

| Document | What it covers |
| --- | --- |
| [architecture.md](architecture.md) | Repo layout, compilation units, the lexer→parser→checker→runtime pipeline, the scheduler and task model, input/output/GUI subsystems |
| [build-and-install.md](build-and-install.md) | Prerequisites, build presets, Makefile targets, CMake options, install layout, Arch packaging, runtime permissions |
| [gather-context.md](gather-context.md) | Using `scripts/gather_context.py` to search AGENTS.md, the docs/ directory, and the codebase |
| [testing.md](testing.md) | Test-suite layout, how to run it, unit tests, integration tests, sanitizers, verification commands |
| [releases.md](releases.md) | Release artifacts, publishing, and efficient Podman-based CI verification |
| [benchmark.md](benchmark.md) | Measured build/test performance, the optimisations that produced it, how to reproduce the numbers |
| [implementation-traps.md](implementation-traps.md) | Backend and runtime traps: ydotool absolute motion, uinput device classification, coroutine stack growth, cancellation, holds, evdev recovery |
| [language-internals.md](language-internals.md) | Grammar and AST, statement dispatch, builtins, key names, string decoding, and the extension points for statements, builtins and backends |
| [cli.md](cli.md) | CLI surface, exit codes, `--check`/`--run` contracts, the mouse-position picker architecture |
| [contributing.md](contributing.md) | Code style, versioning policy, commit policy, and the docs-sync rules |
| [state-machines.md](state-machines.md) | Working audit of the implicit state machines and their failure paths |

## Conventions

- The docs describe the code as it is. When behaviour changes, update the
  relevant doc in the same commit as the code.
- User-visible behaviour changes also update `README.md` and `src/docs.cpp`
  (`kGuide` and `api_reference()`). See [contributing.md](contributing.md).
- Keep session-, distro- and compositor-specific steps clearly labelled. The
  code must detect the environment at runtime; the docs must not assume one.
