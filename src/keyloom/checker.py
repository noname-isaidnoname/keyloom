from __future__ import annotations

from dataclasses import dataclass
import os

from .parser import Node, Script
from .backends import BUTTONS, validate_key_name, canonical_key_name, BackendError


@dataclass
class Diagnostic:
    level: str
    message: str
    line: int | None = None

    def __str__(self) -> str:
        where = f"line {self.line}: " if self.line else ""
        return f"{self.level}: {where}{self.message}"


VALID_OPTIONS = {
    "input": {"auto", "evdev"},
    "output": {"auto", "ydotool", "uinput"},
    "panic": None,
    "screen": None,
    "allow_launch": {True, False},
}


def _walk(nodes: list[Node]):
    for node in nodes:
        yield node
        yield from _walk(node.body)
        yield from _walk(node.otherwise)


def check_script(script: Script) -> list[Diagnostic]:
    out: list[Diagnostic] = []
    for key, value in script.options.items():
        if key not in VALID_OPTIONS:
            out.append(Diagnostic("error", f"Unknown option {key!r}") )
        elif VALID_OPTIONS[key] is not None and value not in VALID_OPTIONS[key]:
            out.append(Diagnostic("error", f"Invalid value {value!r} for option {key!r}"))
    if "panic" in script.options:
        panic = script.options["panic"]
        if not isinstance(panic, str):
            out.append(Diagnostic("error", "Option 'panic' must be a key name string or \"none\""))
        elif panic.lower() != "none":
            try: validate_key_name(panic)
            except BackendError as exc: out.append(Diagnostic("error", str(exc)))
    if "screen" in script.options and not isinstance(script.options["screen"], str):
        out.append(Diagnostic("error", "Option 'screen' must be 'auto' or a string such as '1920x1080'"))
    held: set[str] = set()
    freed: set[str] = set()
    panic_value = script.options.get("panic", "PAUSE")
    panic_key = canonical_key_name(panic_value) if isinstance(panic_value, str) and panic_value.lower() != "none" else None
    for event in script.events:
        kind, key, edge, interval = event.args
        if kind == "key":
            try: validate_key_name(key)
            except BackendError as exc: out.append(Diagnostic("error", str(exc), event.line))
            if panic_key and canonical_key_name(key) == panic_key:
                out.append(Diagnostic("warning", f"Binding {key} is unreachable while it is the panic key" , event.line))
        if kind == "timer" and interval <= 0:
            out.append(Diagnostic("error", "Timer interval must be greater than zero", event.line))
        if kind == "key" and edge == "held" and interval <= 0:
            out.append(Diagnostic("error", "Held-key interval must be greater than zero", event.line))
        for node in _walk(event.body):
            if node.kind == "mouse":
                button, action, token, _duration = node.args
                if button not in BUTTONS:
                    out.append(Diagnostic("error", f"Unknown mouse button {button!r}", node.line))
                event_owner = key if kind == "key" else "script"
                default_token = f"default:{event_owner}:{button}"
                if action == "hold": held.add(token or default_token)
                if action == "free": freed.add(token or default_token)
            elif node.kind == "key":
                chord, action, _duration = node.args
                try:
                    for part in chord.split("+"): validate_key_name(part)
                except BackendError as exc:
                    out.append(Diagnostic("error", str(exc), node.line))
    needs_uinput = any(
        node.kind in ("cursor", "scroll")
        for event in script.events for node in _walk(event.body)
    )
    if needs_uinput and not os.access("/dev/uinput", os.W_OK):
        out.append(Diagnostic("warning", "cursor motion and scrolling require write access to /dev/uinput on this machine"))
    for token in sorted(held - freed):
        out.append(Diagnostic("warning", f"Mouse hold token {token!r} is never freed; runtime safety cleanup will release it"))
    for token in sorted(freed - held):
        out.append(Diagnostic("warning", f"Mouse hold token {token!r} is freed but never held"))
    if not out:
        out.append(Diagnostic("ok", "Script syntax and static checks passed"))
    return out
