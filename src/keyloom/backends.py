from __future__ import annotations

import os
import shutil
import subprocess
from typing import Protocol


class BackendError(RuntimeError):
    pass


class OutputBackend(Protocol):
    def warp(self, x: int, y: int) -> None: ...
    def move(self, dx: int, dy: int) -> None: ...
    def button(self, name: str, down: bool) -> None: ...
    def key(self, name: str, down: bool) -> None: ...
    def type_text(self, text: str) -> None: ...
    def scroll(self, x: int, y: int) -> None: ...
    def close(self) -> None: ...


BUTTONS = {"left": 0, "right": 1, "middle": 2, "side": 3, "extra": 4}


def absolute_axis_info(maximum: int):
    """Return evdev.AbsInfo in the correct (value, min, max, ...) order.

    A previous version passed the maximum as the second positional argument
    (the ``min`` field), so ``/dev/uinput`` rejected the device with EINVAL.
    """
    from evdev import AbsInfo
    return AbsInfo(0, 0, max(1, int(maximum)), 0, 0, 0)


def ydotool_socket_path() -> str | None:
    """Best-effort discovery of the socket ydotoold is listening on."""
    explicit = os.environ.get("YDOTOOL_SOCKET")
    if explicit:
        return explicit
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    candidates = []
    if runtime_dir:
        candidates.append(os.path.join(runtime_dir, ".ydotool_socket"))
    candidates.append("/tmp/.ydotool_socket")
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate
    return None


KEY_ALIASES = {
    "CTRL": "LEFTCTRL", "CONTROL": "LEFTCTRL", "SHIFT": "LEFTSHIFT",
    "ALT": "LEFTALT", "SUPER": "LEFTMETA", "WIN": "LEFTMETA",
    "CMD": "LEFTMETA", "ESC": "ESC", "ENTER": "ENTER",
    "RETURN": "ENTER", "BACKSPACE": "BACKSPACE", "DEL": "DELETE",
    "INS": "INSERT", "PGUP": "PAGEUP", "PGDN": "PAGEDOWN",
    "SPACE": "SPACE", "TAB": "TAB", "CAPS": "CAPSLOCK",
    "PRTSC": "SYSRQ", "PRINTSCREEN": "SYSRQ", "PAUSE": "PAUSE",
}


def _ecodes():
    try:
        from evdev import ecodes
        return ecodes
    except ImportError as exc:
        raise BackendError("The 'evdev' Python package is required. Run: python -m pip install -r requirements.txt") from exc


COMMON_KEYS = {
    "ESC", "ENTER", "BACKSPACE", "TAB", "SPACE", "MINUS", "EQUAL",
    "LEFTBRACE", "RIGHTBRACE", "BACKSLASH", "SEMICOLON", "APOSTROPHE",
    "GRAVE", "COMMA", "DOT", "SLASH", "CAPSLOCK", "SYSRQ", "SCROLLLOCK",
    "PAUSE", "INSERT", "HOME", "PAGEUP", "DELETE", "END", "PAGEDOWN",
    "RIGHT", "LEFT", "DOWN", "UP", "NUMLOCK", "LEFTCTRL", "LEFTSHIFT",
    "LEFTALT", "LEFTMETA", "RIGHTCTRL", "RIGHTSHIFT", "RIGHTALT", "RIGHTMETA",
    "MENU", "COMPOSE", "KPENTER", "KPDOT", "KPSLASH", "KPASTERISK",
    "KPMINUS", "KPPLUS", "KP0", "KP1", "KP2", "KP3", "KP4", "KP5",
    "KP6", "KP7", "KP8", "KP9", "MUTE", "VOLUMEDOWN", "VOLUMEUP",
    "PLAYPAUSE", "STOPCD", "PREVIOUSSONG", "NEXTSONG",
}


def canonical_key_name(name: str) -> str:
    return KEY_ALIASES.get(name.upper(), name.upper())


def validate_key_name(name: str) -> str:
    canonical = canonical_key_name(name)
    valid = (len(canonical) == 1 and canonical.isalpha()) or (len(canonical) == 1 and canonical.isdigit())
    valid = valid or (canonical.startswith("F") and canonical[1:].isdigit() and 1 <= int(canonical[1:]) <= 24)
    valid = valid or canonical in COMMON_KEYS
    if not valid:
        raise BackendError(f"Unknown key {name!r}. Use names such as A, F8, CTRL, ESC, or LEFT.")
    return canonical


def key_code(name: str) -> int:
    canonical = validate_key_name(name)
    ec = _ecodes()
    candidates = [f"KEY_{canonical}"]
    for candidate in candidates:
        if candidate in ec.ecodes:
            return int(ec.ecodes[candidate])
    raise BackendError(f"Key {name!r} is not available in this kernel's evdev key table")


def normalize_key_event(code: int) -> str | None:
    ec = _ecodes()
    # Reverse map only canonical KEY_* codes; ignore aliases and non-key buttons.
    name = ec.KEY.get(code)
    if not name:
        return None
    if isinstance(name, (list, tuple)):
        name = name[0]
    if not str(name).startswith("KEY_"):
        return None
    return str(name)[4:]


class YdotoolBackend:
    """Use ydotoold for keys/buttons and a real uinput pointer for motion.

    ydotool 1.0.x implements ``mousemove --absolute`` by first sending
    REL_X/REL_Y = INT32_MIN (the top-left corner) and then applying the target
    as a *relative* delta. With pointer acceleration that lands at the corner
    or somewhere unexpected, so pointer motion uses an absolute uinput device
    lazily while ydotoold still handles buttons, keys and text.
    """

    def __init__(self, executable: str = "ydotool", *, width: int = 1920, height: int = 1080):
        self.executable = shutil.which(executable)
        if not self.executable:
            raise BackendError("ydotool is not installed. Install ydotool and start ydotoold, or use --output-backend uinput.")
        self.width = max(1, int(width))
        self.height = max(1, int(height))
        self.down_buttons: set[str] = set()
        self.down_keys: set[str] = set()
        self._pointer: UInputBackend | None = None
        self._call(["--help"], tolerate=True)

    def _pointer_device(self) -> "UInputBackend":
        if self._pointer is None:
            try:
                self._pointer = UInputBackend(self.width, self.height, keyboard=False)
            except BackendError as exc:
                raise BackendError(
                    "ydotool cannot warp the pointer reliably by itself (its --absolute "
                    "mode jumps to the top-left first). Keyloom needs write access to "
                    "/dev/uinput for cursor motion; add your user to the 'input' group, "
                    "install a udev rule, or use a backend/environment where /dev/uinput "
                    f"is writable. Details: {exc}"
                ) from exc
        return self._pointer

    def _call(self, args: list[str], tolerate: bool = False) -> str:
        try:
            proc = subprocess.run([self.executable, *args], check=False, text=True,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        except (OSError, subprocess.TimeoutExpired) as exc:
            if tolerate:
                return ""
            raise BackendError(f"Could not talk to ydotool/ydotoold: {exc}. Is ydotoold running?") from exc
        if proc.returncode and not tolerate:
            detail = (proc.stderr or proc.stdout).strip()
            raise BackendError(f"ydotool failed ({proc.returncode}): {detail or 'no diagnostic'}; ensure ydotoold is running")
        return proc.stdout.strip()

    def warp(self, x: int, y: int) -> None:
        self._pointer_device().warp(x, y)

    def move(self, dx: int, dy: int) -> None:
        self._pointer_device().move(dx, dy)

    def button(self, name: str, down: bool) -> None:
        if name not in BUTTONS:
            raise BackendError(f"Unsupported mouse button {name!r}")
        code = BUTTONS[name] | (0x40 if down else 0x80)
        self._call(["click", hex(code)])
        if down:
            self.down_buttons.add(name)
        else:
            self.down_buttons.discard(name)

    def key(self, name: str, down: bool) -> None:
        code = key_code(name)
        self._call(["key", f"{code}:{1 if down else 0}"])
        if down:
            self.down_keys.add(name)
        else:
            self.down_keys.discard(name)

    def type_text(self, text: str) -> None:
        # Disable ydotool's own backslash escaping: Keyloom already decoded the
        # .loom string, and a literal backslash must stay a backslash.
        self._call(["type", "-e", "0", "--", text])

    def scroll(self, x: int, y: int) -> None:
        # Wheel events need the absolute uinput pointer device as well.
        self._pointer_device().scroll(x, y)

    def close(self) -> None:
        for button in list(self.down_buttons):
            try: self.button(button, False)
            except Exception: pass
        for key in list(self.down_keys):
            try: self.key(key, False)
            except Exception: pass
        if self._pointer is not None:
            try: self._pointer.close()
            except Exception: pass
            self._pointer = None


class UInputBackend:
    """Direct Linux uinput output. ABS_X/ABS_Y are used for absolute pointer warps."""

    def __init__(self, width: int, height: int, *, keyboard: bool = True):
        try:
            from evdev import UInput, ecodes
        except ImportError as exc:
            raise BackendError("The 'evdev' Python package is required for uinput output.") from exc
        if not os.path.exists("/dev/uinput"):
            raise BackendError("/dev/uinput is unavailable. Load the uinput kernel module and grant access, or use ydotool.")
        if not os.access("/dev/uinput", os.W_OK):
            raise BackendError("/dev/uinput is not writable. Add your user to the 'input' group or install a udev rule, then log out and back in.")
        button_codes = [ecodes.BTN_LEFT, ecodes.BTN_RIGHT, ecodes.BTN_MIDDLE,
                        ecodes.BTN_SIDE, ecodes.BTN_EXTRA]
        events = {
            ecodes.EV_KEY: sorted(set(button_codes)),
            ecodes.EV_REL: [ecodes.REL_X, ecodes.REL_Y, ecodes.REL_WHEEL, ecodes.REL_HWHEEL],
            ecodes.EV_ABS: [
                (ecodes.ABS_X, absolute_axis_info(width - 1)),
                (ecodes.ABS_Y, absolute_axis_info(height - 1)),
            ],
        }
        if keyboard:
            key_max = int(getattr(ecodes, "KEY_MAX", 0x2FF))
            key_codes = sorted({int(v) for k, v in ecodes.ecodes.items()
                                if k.startswith("KEY_") and not k.endswith(("_MAX", "_CNT"))
                                and isinstance(v, int) and 0 <= int(v) <= key_max})
            events[ecodes.EV_KEY] = sorted(set(key_codes + button_codes))
        try:
            self.ui = UInput(events=events, name="Keyloom virtual pointer", phys="keyloom/uinput")
        except OSError as exc:
            raise BackendError(f"Unable to create /dev/uinput device: {exc}") from exc
        self.ec = ecodes
        self.down_buttons: set[str] = set()
        self.down_keys: set[str] = set()
        self.width, self.height = width, height

    def _write(self, event_type: int, code: int, value: int) -> None:
        self.ui.write(event_type, code, value)
        self.ui.syn()

    def warp(self, x: int, y: int) -> None:
        self.ui.write(self.ec.EV_ABS, self.ec.ABS_X, max(0, min(self.width - 1, x)))
        self.ui.write(self.ec.EV_ABS, self.ec.ABS_Y, max(0, min(self.height - 1, y)))
        self.ui.syn()

    def move(self, dx: int, dy: int) -> None:
        self.ui.write(self.ec.EV_REL, self.ec.REL_X, dx)
        self.ui.write(self.ec.EV_REL, self.ec.REL_Y, dy)
        self.ui.syn()

    def button(self, name: str, down: bool) -> None:
        code = {"left": self.ec.BTN_LEFT, "right": self.ec.BTN_RIGHT,
                "middle": self.ec.BTN_MIDDLE, "side": self.ec.BTN_SIDE,
                "extra": self.ec.BTN_EXTRA}.get(name)
        if code is None:
            raise BackendError(f"Unsupported mouse button {name!r}")
        self._write(self.ec.EV_KEY, code, 1 if down else 0)
        if down: self.down_buttons.add(name)
        else: self.down_buttons.discard(name)

    def key(self, name: str, down: bool) -> None:
        self._write(self.ec.EV_KEY, key_code(name), 1 if down else 0)
        if down: self.down_keys.add(name)
        else: self.down_keys.discard(name)

    def type_text(self, text: str) -> None:
        # US keyboard layout mapping. For arbitrary layouts/text, use ydotool.
        shifted = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ~!@#$%^&*()_+{}|:"<>?')
        plain = {
            " ": "SPACE", "\n": "ENTER", "\t": "TAB", "-": "MINUS", "=": "EQUAL",
            "[": "LEFTBRACE", "]": "RIGHTBRACE", "\\": "BACKSLASH", ";": "SEMICOLON",
            "'": "APOSTROPHE", "`": "GRAVE", ",": "COMMA", ".": "DOT", "/": "SLASH",
        }
        shifted_map = dict(zip('~!@#$%^&*()_+{}|:"<>?',
                               ['GRAVE','1','2','3','4','5','6','7','8','9','0','MINUS','EQUAL','LEFTBRACE','RIGHTBRACE','BACKSLASH','SEMICOLON','APOSTROPHE','COMMA','DOT','SLASH']))
        for char in text:
            upper = char in shifted
            base = char.lower() if char.isalpha() else (shifted_map.get(char) if upper else plain.get(char, char))
            if isinstance(base, str) and len(base) == 1:
                base = base.upper() if base.isalpha() else base
            keyname = str(base)
            if upper:
                self.key("LEFTSHIFT", True)
            try:
                self.key(keyname, True)
                self.key(keyname, False)
            finally:
                if upper: self.key("LEFTSHIFT", False)

    def scroll(self, x: int, y: int) -> None:
        if x:
            self.ui.write(self.ec.EV_REL, self.ec.REL_HWHEEL, x)
        if y:
            self.ui.write(self.ec.EV_REL, self.ec.REL_WHEEL, y)
        self.ui.syn()

    def close(self) -> None:
        for button in list(self.down_buttons):
            try: self.button(button, False)
            except Exception: pass
        for key in list(self.down_keys):
            try: self.key(key, False)
            except Exception: pass
        self.ui.close()


class DryRunBackend:
    def _log(self, action: str) -> None:
        print(f"[dry-run] {action}")
    def warp(self, x: int, y: int) -> None: self._log(f"cursor warp {x}, {y}")
    def move(self, dx: int, dy: int) -> None: self._log(f"cursor move {dx}, {dy}")
    def button(self, name: str, down: bool) -> None: self._log(f"mouse {name} {'hold' if down else 'free'}")
    def key(self, name: str, down: bool) -> None: self._log(f"key {name} {'press' if down else 'free'}")
    def type_text(self, text: str) -> None: self._log(f"type {text!r}")
    def scroll(self, x: int, y: int) -> None: self._log(f"scroll {x}, {y}")
    def close(self) -> None: pass


def choose_backend(name: str, width: int, height: int) -> OutputBackend:
    if name == "ydotool":
        return YdotoolBackend(width=width, height=height)
    if name == "uinput":
        return UInputBackend(width, height)
    if name == "auto":
        # Prefer ydotoold when it is actually running (better text support on
        # non-US layouts). Otherwise use a direct uinput device if possible.
        if shutil.which("ydotool") and ydotool_socket_path():
            return YdotoolBackend(width=width, height=height)
        if os.access("/dev/uinput", os.W_OK):
            return UInputBackend(width, height)
        if shutil.which("ydotool"):
            return YdotoolBackend(width=width, height=height)
        return UInputBackend(width, height)
    raise BackendError(f"Unknown output backend {name!r}")
