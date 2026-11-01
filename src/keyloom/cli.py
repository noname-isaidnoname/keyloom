from __future__ import annotations

import argparse
import asyncio
import glob
import os
import shutil
import sys
from pathlib import Path

from . import __version__
from .backends import BackendError
from .checker import check_script
from .docs import render_docs, API_REFERENCE
from .parser import ParseError, parse
from .runtime import Runtime, RuntimeErrorKeyloom, detect_screen_size


TEMPLATE = '''loom "{name}" {{
    option input = "evdev"
    option output = "auto"
    option panic = "PAUSE"

    when key F8 goes down {{
        log "F8 pressed"
    }}

    when key F8 goes up {{
        log "F8 released"
    }}
}}
'''


def _parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="keyloom",
        description="Build and run Linux desktop automation scripts in the Keyloom language.",
        epilog="Examples: keyloom --init demo | keyloom --check demo.loom | keyloom --run demo.loom | keyloom --get-mouse-pos",
    )
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--init", metavar="SCRIPT", help="create a starter .loom script")
    mode.add_argument("--check", metavar="PATH", help="parse and statically check a script")
    mode.add_argument("--run", metavar="PATH", help="run a script")
    mode.add_argument("--docs", nargs="?", const="all", metavar="TOPIC", help="show language documentation")
    mode.add_argument("--devices", action="store_true", help="list visible input devices and permissions")
    mode.add_argument("--screen", action="store_true", help="print the detected desktop size")
    mode.add_argument("--get-mouse-pos", action="store_true", help="pick a pointer position with an on-screen tooltip")
    mode.add_argument("--list-api", action="store_true", help="list built-in API groups")
    mode.add_argument("--doctor", action="store_true", help="inspect input/output prerequisites")
    p.add_argument("script", nargs="?", help="script path (shorthand for --run PATH)")
    p.add_argument("--force", action="store_true", help="overwrite an existing file with --init")
    p.add_argument("--dry-run", action="store_true", help="log output actions instead of injecting them")
    p.add_argument("--input-device", metavar="EVENT_PATH", help="listen to one /dev/input/eventN keyboard")
    p.add_argument("--output-backend", choices=("auto", "ydotool", "uinput"), help="override the script's output backend")
    p.add_argument("--screen-size", metavar="WIDTHxHEIGHT", help="override detected pointer coordinate bounds")
    p.add_argument("--verbose", action="store_true", help="print extra runtime details")
    p.add_argument("--version", action="version", version=f"keyloom {__version__}")
    return p


def _resolve_script_path(name: str) -> Path:
    path = Path(name).expanduser()
    if path.suffix == "": path = path.with_suffix(".loom")
    return path


def _load_script(path: Path):
    try:
        source = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeErrorKeyloom(f"Cannot read script {path}: {exc}") from exc
    try:
        return parse(source, str(path))
    except ParseError as exc:
        raise RuntimeErrorKeyloom(f"{path}: {exc}") from exc


def _check(path: Path) -> int:
    try:
        script = _load_script(path)
    except RuntimeErrorKeyloom as exc:
        print(f"keyloom: {exc}", file=sys.stderr)
        return 2
    diagnostics = check_script(script)
    has_error = False
    for diag in diagnostics:
        print(f"{path}: {diag}")
        if diag.level == "error": has_error = True
    if not has_error:
        print(f"OK: {path} ({script.name})")
        return 0
    return 2


def _init(name: str, force: bool) -> int:
    path = _resolve_script_path(name)
    clean_name = path.stem
    # Escape quotes and backslashes because the stem becomes a DSL string.
    clean_name = clean_name.replace("\\", "\\\\").replace('"', '\\"')
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"keyloom: cannot create {path.parent}: {exc}", file=sys.stderr)
        return 2
    if path.exists() and not force:
        print(f"Refusing to overwrite {path}. Use --force to replace it.", file=sys.stderr)
        return 2
    try:
        path.write_text(TEMPLATE.format(name=clean_name), encoding="utf-8")
    except OSError as exc:
        print(f"keyloom: cannot write {path}: {exc}", file=sys.stderr)
        return 2
    print(f"Created {path}")
    print(f"Next: keyloom --check {path} && keyloom --run {path}")
    return 0


def _devices() -> int:
    try:
        from evdev import InputDevice, list_devices, ecodes
    except ImportError:
        print("python-evdev is missing. Install it with: python -m pip install -r requirements.txt", file=sys.stderr)
        return 2
    visible = set(list_devices())
    paths = sorted(glob.glob("/dev/input/event*"))
    if not paths:
        print("No /dev/input/event* devices found.")
        return 0
    for path in paths:
        if path not in visible:
            print(f"{path}: no read permission (or device disappeared)")
            continue
        try:
            dev = InputDevice(path)
            caps = dev.capabilities()
            keys = caps.get(ecodes.EV_KEY, [])
            is_keyboard = ecodes.KEY_A in keys and ecodes.KEY_ENTER in keys
            print(f"{path}: {dev.name} — {'keyboard' if is_keyboard else 'other input'}")
            dev.close()
        except OSError as exc:
            print(f"{path}: unavailable ({exc})")
    return 0


def _doctor() -> int:
    print(f"Keyloom {__version__} — Linux desktop automation diagnostics")
    print(f"Session: {os.environ.get('XDG_SESSION_TYPE', 'unknown')} | DISPLAY={bool(os.environ.get('DISPLAY'))} | WAYLAND_DISPLAY={bool(os.environ.get('WAYLAND_DISPLAY'))}")
    width, height = detect_screen_size()
    print(f"Desktop bounds (best effort): {width}x{height}")
    try:
        import evdev  # noqa: F401
        from evdev import list_devices
        paths = list_devices()
        print(f"python-evdev: available | accessible event devices: {len(paths)}")
    except ImportError:
        print("python-evdev: MISSING — install with python -m pip install -r requirements.txt")
        paths = []
    except Exception as exc:
        print(f"python-evdev: present, but device enumeration failed: {exc}")
        paths = []
    print(f"ydotool: {shutil.which('ydotool') or 'not found'}")
    print(f"ydotoold: {shutil.which('ydotoold') or 'not found'}")
    if shutil.which("ydotool") and not shutil.which("ydotoold"):
        print("  note: the ydotool client expects a running ydotoold daemon (which may be provided by a system service).")
    uinput = Path("/dev/uinput")
    print(f"/dev/uinput: {'present and writable' if uinput.exists() and os.access(uinput, os.W_OK) else 'missing or not writable'}")
    try:
        import tkinter  # noqa: F401
        print("tkinter: available (keyloom --get-mouse-pos)")
    except ImportError:
        print("tkinter: MISSING — install python3-tk for keyloom --get-mouse-pos")
    print(f"wl-copy / wl-paste: {bool(shutil.which('wl-copy'))} / {bool(shutil.which('wl-paste'))}")
    if not paths:
        print("Input note: Keyloom needs permission to read a keyboard from /dev/input/event*. See README.md → Wayland and Xwayland setup.")
    return 0


def _get_mouse_pos(args) -> int:
    from .mousepos import PickerError, format_coordinates, pick_mouse_position

    try:
        position = pick_mouse_position(verbose=args.verbose)
    except PickerError as exc:
        print(f"keyloom: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\n[keyloom] Mouse position pick cancelled.", file=sys.stderr)
        return 130
    if position is None:
        print("keyloom: mouse position pick cancelled.", file=sys.stderr)
        return 130
    print(format_coordinates(*position))
    return 0


def _run(path: Path, args) -> int:
    try:
        script = _load_script(path)
    except RuntimeErrorKeyloom as exc:
        print(f"keyloom: {exc}", file=sys.stderr)
        return 2
    diagnostics = check_script(script)
    errors = [d for d in diagnostics if d.level == "error"]
    for diag in diagnostics:
        if diag.level != "ok": print(f"{path}: {diag}")
    if errors:
        print("Refusing to run: fix the errors above.", file=sys.stderr)
        return 2
    runtime = Runtime(
        script,
        dry_run=args.dry_run,
        input_device=args.input_device,
        output_backend=args.output_backend,
        screen_size=args.screen_size,
        verbose=args.verbose,
    )
    try:
        asyncio.run(runtime.run())
    except KeyboardInterrupt:
        print("\n[keyloom] Interrupted.")
    except (RuntimeErrorKeyloom, BackendError, OSError, ValueError, TypeError, KeyError, ZeroDivisionError) as exc:
        print(f"keyloom: {exc}", file=sys.stderr)
        return 1
    return 0


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if args.init is not None: return _init(args.init, args.force)
    if args.check is not None: return _check(_resolve_script_path(args.check))
    if args.run is not None: return _run(_resolve_script_path(args.run), args)
    if args.docs is not None:
        print(render_docs(args.docs)); return 0
    if args.devices: return _devices()
    if args.screen:
        w, h = detect_screen_size()
        print(f"{w}x{h}")
        return 0
    if args.get_mouse_pos: return _get_mouse_pos(args)
    if args.list_api:
        for group, entries in API_REFERENCE.items():
            print(f"{group}: {', '.join(entries)}")
        return 0
    if args.doctor: return _doctor()
    if args.script:
        return _run(_resolve_script_path(args.script), args)
    _parser().print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
