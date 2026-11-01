from __future__ import annotations

import asyncio
import contextvars
import math
import os
import random
import shlex
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any

from .backends import (
    BackendError, DryRunBackend, KEY_ALIASES, PassthroughDevice, choose_backend,
    key_code, normalize_key_event,
)
from .parser import Node, Script


class RuntimeErrorKeyloom(RuntimeError):
    pass


def canonical_key(name: str) -> str:
    upper = name.upper()
    return KEY_ALIASES.get(upper, upper)


def detect_screen_size() -> tuple[int, int]:
    """Best-effort virtual desktop size discovery; users can override it."""
    env_w = os.environ.get("KEYLOOM_SCREEN_WIDTH")
    env_h = os.environ.get("KEYLOOM_SCREEN_HEIGHT")
    if env_w and env_h:
        try: return max(1, int(env_w)), max(1, int(env_h))
        except ValueError: pass

    # wlr-randr is useful on wlroots-based Wayland compositors.
    if shutil.which("wlr-randr"):
        try:
            text = subprocess.run(["wlr-randr"], capture_output=True, text=True, timeout=2).stdout
            sizes = []
            for line in text.splitlines():
                if " px" in line:
                    import re
                    m = re.search(r"(\d+)x(\d+)\s+px", line)
                    if m: sizes.append((int(m.group(1)), int(m.group(2))))
            if sizes:
                return max(w for w, _ in sizes), max(h for _, h in sizes)
        except Exception: pass

    if shutil.which("xrandr"):
        try:
            text = subprocess.run(["xrandr", "--current"], capture_output=True, text=True, timeout=2).stdout
            import re
            rects = []
            for line in text.splitlines():
                if " connected" not in line: continue
                m = re.search(r"(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", line)
                if m:
                    w, h, x, y = map(int, m.groups())
                    rects.append((w, h, x, y))
            if rects:
                min_x = min(r[2] for r in rects); min_y = min(r[3] for r in rects)
                max_x = max(r[2] + r[0] for r in rects); max_y = max(r[3] + r[1] for r in rects)
                return max(1, max_x - min(0, min_x)), max(1, max_y - min(0, min_y))
        except Exception: pass

    if shutil.which("xdpyinfo"):
        try:
            text = subprocess.run(["xdpyinfo"], capture_output=True, text=True, timeout=2).stdout
            import re
            m = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", text)
            if m: return int(m.group(1)), int(m.group(2))
        except Exception: pass
    return 1920, 1080


def parse_screen_option(value: Any, fallback: tuple[int, int]) -> tuple[int, int]:
    if value is None or value == "auto": return fallback
    if isinstance(value, str):
        try:
            w, h = value.lower().split("x", 1)
            return max(1, int(w)), max(1, int(h))
        except Exception as exc:
            raise RuntimeErrorKeyloom("option screen must look like '1920x1080' or 'auto'") from exc
    raise RuntimeErrorKeyloom("option screen must be a string such as '1920x1080'")


class Runtime:
    MAX_LOOP = 10000

    def __init__(self, script: Script, *, dry_run: bool = False,
                 input_device: str | None = None, output_backend: str | None = None,
                 screen_size: str | None = None, verbose: bool = False):
        self.script = script
        self.dry_run = dry_run
        self.input_device_override = input_device
        self.output_override = output_backend
        self.verbose = verbose
        detected = detect_screen_size()
        screen_opt = screen_size if screen_size else script.options.get("screen", "auto")
        self.width, self.height = parse_screen_option(screen_opt, detected)
        self.backend = DryRunBackend() if dry_run else None
        self.variables: dict[str, Any] = {
            "screen_width": self.width,
            "screen_height": self.height,
            "pi": math.pi,
            "e": math.e,
        }
        self.stop_event = asyncio.Event()
        self.device_down: dict[str, set[str]] = {}
        self.pressed_keys: set[str] = set()
        self.consumed_keys: set[str] = set()
        self.devices: list = []
        self.held_tokens: dict[str, str] = {}
        self.button_refs: dict[str, int] = {}
        self.key_refs: dict[str, int] = {}
        self._tap_counter = 0
        self.held_tasks: dict[tuple[str, int], asyncio.Task] = {}
        self._event_key_var = contextvars.ContextVar("keyloom_event_key", default="")
        self._event_edge_var = contextvars.ContextVar("keyloom_event_edge", default="")
        self.background_tasks: set[asyncio.Task] = set()
        self.started_at = time.monotonic()
        self._running = False
        self.runtime_error: str | None = None

    def _say(self, message: str) -> None:
        print(f"[keyloom] {message}")

    def _select_backend(self):
        backend_name = self.output_override or self.script.options.get("output", "auto")
        if self.dry_run: return DryRunBackend()
        return choose_backend(str(backend_name), self.width, self.height)

    async def run(self) -> None:
        if os.name != "posix" or not Path("/proc").exists():
            raise RuntimeErrorKeyloom("Keyloom's live runtime is Linux-only.")
        self.backend = self._select_backend()
        self._running = True
        panic = self.script.options.get("panic", "PAUSE")
        self.panic_key = canonical_key(str(panic)) if str(panic).lower() != "none" else None
        self.consumed_keys = self._collect_consumed_keys()
        try:
            for event in self.script.events:
                if event.args[0] == "app":
                    await self._safe_dispatch(event, None, "starts")
                elif event.args[0] == "timer":
                    task = asyncio.create_task(self._timer_loop(event, float(event.args[3])))
                    self.background_tasks.add(task)
            if not any(e.args[0] == "key" for e in self.script.events):
                self._say("No key bindings found; timer/app events are active. Press the panic key or Ctrl+C to stop.")
            devices = self._open_keyboards()
            self.devices = devices
            if not devices and any(e.args[0] == "key" for e in self.script.events):
                raise RuntimeErrorKeyloom(self._input_permission_help())
            if devices:
                passthroughs: dict[str, PassthroughDevice] = {}
                if self.consumed_keys:
                    keys = ", ".join(sorted(self.consumed_keys))
                    if self.dry_run:
                        self._say(f"Dry run: would consume key(s) {keys}; devices are not grabbed.")
                    else:
                        passthroughs = self._prepare_consumption(devices)
                        panic_note = f"; panic key {self.panic_key} still passes through" if self.panic_key else ""
                        self._say(f"Consuming key(s) {keys}{panic_note}.")
                for device in devices:
                    self.device_down[device.path] = set()
                    task = asyncio.create_task(self._read_device(device, passthroughs.get(device.path)))
                    self.background_tasks.add(task)
                names = ", ".join(f"{d.name} ({d.path})" for d in devices)
                self._say(f"Listening on: {names}")
            self._say(f"Running '{self.script.name}' — panic key: {panic if self.panic_key else 'disabled'}")
            if self.verbose:
                self._say(f"Output backend: {type(self.backend).__name__}; screen bounds: {self.width}x{self.height}")
            await self.stop_event.wait()
            if self.runtime_error:
                raise RuntimeErrorKeyloom(self.runtime_error)
        except asyncio.CancelledError:
            raise
        finally:
            self._running = False
            for task in list(self.background_tasks) + list(self.held_tasks.values()):
                task.cancel()
            all_tasks = list(self.background_tasks) + list(self.held_tasks.values())
            if all_tasks:
                await asyncio.gather(*all_tasks, return_exceptions=True)
            self.held_tasks.clear()
            self._release_all_holds()
            if self.backend:
                self.backend.close()
            for device in self.devices:
                try: device.close()
                except Exception: pass
            self.devices = []
            self._say("Stopped; held buttons and keys released.")

    def _open_keyboards(self):
        try:
            from evdev import InputDevice, list_devices, ecodes
        except ImportError as exc:
            raise RuntimeErrorKeyloom("Missing dependency 'evdev'. Install with: python -m pip install -r requirements.txt") from exc
        paths = [self.input_device_override] if self.input_device_override else list_devices()
        devices = []
        for path in paths:
            try:
                device = InputDevice(path)
                name = (device.name or "").lower()
                if any(marker in name for marker in ("keyloom", "ydotool", "virtual pointer")):
                    device.close(); continue
                caps = device.capabilities()
                keys = caps.get(ecodes.EV_KEY, [])
                if ecodes.KEY_A in keys and (ecodes.KEY_ENTER in keys or ecodes.KEY_SPACE in keys):
                    devices.append(device)
                else:
                    device.close()
            except (OSError, PermissionError):
                continue
        return devices

    def _input_permission_help(self) -> str:
        return (
            "No accessible keyboard devices were found. Keyloom reads /dev/input/event* through evdev. "
            "On most distributions, add your user to the 'input' group (then log out/in), or create a "
            "narrow udev rule for your keyboard. Do not run the whole automation as root. "
            "Use `keyloom --devices` to inspect permissions."
        )

    def _collect_consumed_keys(self) -> set[str]:
        found: set[str] = set()

        def walk(nodes: list[Node]) -> None:
            for node in nodes:
                if node.kind == "consume":
                    found.add(canonical_key(str(node.args[0])))
                walk(node.body)
                walk(node.otherwise)

        for event in self.script.events:
            walk(event.body)
        panic_key = getattr(self, "panic_key", None)
        if panic_key:
            # The panic key must always reach the desktop so a script can be
            # stopped even if it accidentally tries to consume that key.
            found.discard(panic_key)
        return found

    def _prepare_consumption(self, devices) -> dict[str, PassthroughDevice]:
        by_path = {device.path: device for device in devices}
        prepared: dict[str, PassthroughDevice] = {}
        try:
            for device in devices:
                try:
                    passthrough = PassthroughDevice(device)
                except Exception as exc:
                    raise RuntimeErrorKeyloom(
                        "Cannot consume keys: Keyloom needs write access to /dev/uinput "
                        f"to replay non-consumed input. Details: {exc}"
                    ) from exc
                try:
                    device.grab()
                except OSError as exc:
                    passthrough.close()
                    raise RuntimeErrorKeyloom(
                        f"Cannot grab input device {device.path} ({device.name}) for key "
                        "consumption; another program may already be grabbing it or your "
                        f"user lacks device access. Details: {exc}"
                    ) from exc
                prepared[device.path] = passthrough
        except Exception:
            for path, passthrough in list(prepared.items()):
                try: by_path[path].ungrab()
                except (OSError, AttributeError): pass
                passthrough.close()
            raise
        return prepared

    def _is_consumed_key_event(self, event) -> bool:
        from evdev import ecodes
        if event.type != ecodes.EV_KEY:
            return False
        name = normalize_key_event(event.code)
        if not name:
            return False
        key = canonical_key(name)
        panic_key = getattr(self, "panic_key", None)
        if panic_key and key == panic_key:
            return False
        return key in self.consumed_keys

    async def _read_device(self, device, passthrough: PassthroughDevice | None = None) -> None:
        from evdev import ecodes
        try:
            async for event in device.async_read_loop():
                if passthrough is not None and not self._is_consumed_key_event(event):
                    try:
                        passthrough.write_event(event)
                    except OSError as exc:
                        self.runtime_error = f"input passthrough failed for {device.path}: {exc}"
                        self._say(self.runtime_error)
                        self.stop_event.set()
                        return
                if event.type != ecodes.EV_KEY or event.value == 2:
                    continue
                name = normalize_key_event(event.code)
                if not name:
                    continue
                key = canonical_key(name)
                if self.verbose:
                    self._say(f"input {key} {'down' if event.value == 1 else 'up'} from {device.path}")
                await self._physical_key(device.path, key, event.value == 1)
        except asyncio.CancelledError:
            raise
        except OSError as exc:
            self._say(f"Input device {device.path} disconnected: {exc}")
        finally:
            released = list(self.device_down.pop(device.path, set()))
            try: device.ungrab()
            except (OSError, AttributeError): pass
            if passthrough is not None:
                try: passthrough.close()
                except Exception: pass
            device.close()
            for key in released:
                still_down = any(key in keys for keys in self.device_down.values())
                if not still_down and key in self.pressed_keys:
                    self.pressed_keys.discard(key)
                    for task_key, held in list(self.held_tasks.items()):
                        if task_key[0] == key:
                            self.held_tasks.pop(task_key, None)
                            held.cancel()
                    for event in self.script.events:
                        if event.args[0] == "key" and canonical_key(event.args[1]) == key and event.args[2] == "up":
                            self._spawn_dispatch(event, key, "up")

    async def _physical_key(self, path: str, key: str, down: bool) -> None:
        per_device = self.device_down.setdefault(path, set())
        if down:
            already = key in self.pressed_keys
            per_device.add(key)
            self.pressed_keys.add(key)
            if already: return
            if self.panic_key and key == self.panic_key:
                self._say(f"Panic key {key} pressed.")
                self.stop_event.set()
                return
            for event in self.script.events:
                if event.args[0] == "key" and canonical_key(event.args[1]) == key and event.args[2] == "down":
                    self._spawn_dispatch(event, key, "down")
                elif event.args[0] == "key" and canonical_key(event.args[1]) == key and event.args[2] == "held":
                    interval = float(event.args[3])
                    task_key = (key, event.line)
                    task = asyncio.create_task(self._held_loop(event, key, interval))
                    self.held_tasks[task_key] = task
                    self.background_tasks.add(task)
        else:
            per_device.discard(key)
            still_down = any(key in keys for keys in self.device_down.values())
            if still_down or key not in self.pressed_keys: return
            self.pressed_keys.discard(key)
            for task_key, held in list(self.held_tasks.items()):
                if task_key[0] == key:
                    self.held_tasks.pop(task_key, None)
                    held.cancel()
            for event in self.script.events:
                if event.args[0] == "key" and canonical_key(event.args[1]) == key and event.args[2] == "up":
                    self._spawn_dispatch(event, key, "up")

    async def _timer_loop(self, event: Node, interval: float) -> None:
        if interval <= 0: return
        try:
            while not self.stop_event.is_set():
                await asyncio.sleep(interval)
                if not self.stop_event.is_set():
                    await self._safe_dispatch(event, None, "tick")
        except asyncio.CancelledError:
            raise

    async def _held_loop(self, event: Node, key: str, interval: float) -> None:
        if interval <= 0: return
        loop = asyncio.get_running_loop()
        # Tick on a fixed schedule. Sleeping the whole interval *after* the
        # handler would add the handler's own duration to the period, so
        # `every 10ms` with a 5ms tap would actually fire every ~15ms.
        deadline = loop.time() + interval
        try:
            while key in self.pressed_keys and not self.stop_event.is_set():
                delay = deadline - loop.time()
                if delay > 0:
                    await asyncio.sleep(delay)
                if key not in self.pressed_keys or self.stop_event.is_set():
                    return
                await self._safe_dispatch(event, key, "held")
                deadline += interval
                now = loop.time()
                if deadline <= now:
                    # The handler overran its tick. Skip whole ticks instead of
                    # firing a burst of catch-up ticks.
                    deadline += math.ceil((now - deadline) / interval) * interval
        except asyncio.CancelledError:
            raise

    def _spawn_dispatch(self, event: Node, key: str | None, edge: str) -> None:
        task = asyncio.create_task(self._safe_dispatch(event, key, edge))
        self.background_tasks.add(task)
        task.add_done_callback(self.background_tasks.discard)

    async def _safe_dispatch(self, event: Node, key: str | None, edge: str) -> None:
        try:
            await self._dispatch(event, key, edge)
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self.runtime_error = f"script error at line {event.line}: {exc}"
            self._say(self.runtime_error)
            self.stop_event.set()

    async def _dispatch(self, event: Node, key: str | None, edge: str) -> None:
        key_token = self._event_key_var.set(key or "")
        edge_token = self._event_edge_var.set(edge)
        try:
            await self._execute_nodes(event.body)
        finally:
            self._event_key_var.reset(key_token)
            self._event_edge_var.reset(edge_token)

    async def _execute_nodes(self, nodes: list[Node]) -> None:
        for node in nodes:
            if self.stop_event.is_set(): return
            await self._execute(node)

    async def _execute(self, node: Node) -> None:
        kind, args = node.kind, node.args
        if kind == "cursor":
            action = args[0]
            if action == "warp":
                x, y = int(self._eval(args[1])), int(self._eval(args[2]))
                self.backend.warp(x, y)
                self.variables["cursor_x"], self.variables["cursor_y"] = x, y
            elif action == "move":
                dx, dy = int(self._eval(args[1])), int(self._eval(args[2]))
                self.backend.move(dx, dy)
                self.variables["cursor_x"] = int(self.variables.get("cursor_x", self.width // 2)) + dx
                self.variables["cursor_y"] = int(self.variables.get("cursor_y", self.height // 2)) + dy
            elif action == "center":
                self.backend.warp(self.width // 2, self.height // 2)
                self.variables["cursor_x"], self.variables["cursor_y"] = self.width // 2, self.height // 2
            elif action == "glide":
                await self._glide(int(self._eval(args[1])), int(self._eval(args[2])), float(args[3]))
        elif kind == "mouse":
            button, action, token, duration = args
            owner = token or f"default:{self._event_key_var.get() or 'script'}:{button}"
            if action == "hold":
                self._hold_button(owner, button)
            elif action == "free":
                self._free_button(owner)
            else:
                self._tap_counter += 1
                tap_owner = f"__keyloom_tap_{self._tap_counter}"
                self._hold_button(tap_owner, button)
                try: await asyncio.sleep(float(duration) if duration is not None else 0.06)
                finally: self._free_button(tap_owner)
        elif kind == "key":
            chord, action, duration = args
            names = [canonical_key(part) for part in chord.split("+")]
            if action == "press":
                for name in names: self._hold_key(name)
            elif action == "free":
                for name in reversed(names): self._free_key(name)
            else:
                for name in names: self._hold_key(name)
                try: await asyncio.sleep(float(duration) if duration is not None else 0.06)
                finally:
                    for name in reversed(names): self._free_key(name)
        elif kind == "consume":
            # Consumption is collected before devices are grabbed, so the very
            # first key-down can be swallowed. The declaration itself has no
            # per-event effect here.
            pass
        elif kind == "type":
            self.backend.type_text(str(self._eval(args[0])))
        elif kind == "wait":
            await asyncio.sleep(float(args[0]))
        elif kind == "scroll":
            self.backend.scroll(int(self._eval(args[0])), int(self._eval(args[1])))
        elif kind == "log":
            print(str(self._eval(args[0])))
        elif kind == "notify":
            message = str(self._eval(args[0]))
            if shutil.which("notify-send"):
                subprocess.Popen(["notify-send", "Keyloom", message], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            else: self._say(f"notification: {message}")
        elif kind == "clip_write":
            self._clipboard("write", str(self._eval(args[0])))
        elif kind == "clip_read":
            self.variables[args[0]] = self._clipboard("read")
        elif kind == "launch":
            if not self.script.options.get("allow_launch", False):
                raise RuntimeErrorKeyloom("launch is disabled. Set `option allow_launch = true` to permit application launching.")
            command = shlex.split(str(self._eval(args[0])))
            if not command: raise RuntimeErrorKeyloom("launch command cannot be empty")
            subprocess.Popen(command, start_new_session=True)
        elif kind == "set":
            self.variables[args[0]] = self._eval(args[1])
        elif kind == "if":
            await self._execute_nodes(node.body if self._eval(args[0]) else node.otherwise)
        elif kind == "repeat":
            count = int(self._eval(args[0]))
            if count < 0 or count > self.MAX_LOOP:
                raise RuntimeErrorKeyloom(f"repeat count must be from 0 to {self.MAX_LOOP}")
            for _ in range(count):
                if self.stop_event.is_set(): break
                await self._execute_nodes(node.body)
        elif kind == "while":
            n = 0
            while self._eval(args[0]):
                if self.stop_event.is_set(): break
                n += 1
                if n > self.MAX_LOOP:
                    raise RuntimeErrorKeyloom(f"while loop exceeded safety limit ({self.MAX_LOOP} iterations)")
                await self._execute_nodes(node.body)
        elif kind == "assert":
            if not self._eval(args[0]):
                raise RuntimeErrorKeyloom(f"assertion failed at line {node.line}")
        elif kind == "call":
            self._eval(args[0])
        else:
            raise RuntimeErrorKeyloom(f"Unsupported statement {kind!r} at line {node.line}")

    async def _glide(self, x: int, y: int, duration: float) -> None:
        duration = max(0.0, min(duration, 30.0))
        # The last cursor target is tracked when Keyloom itself moved it;
        # otherwise the desktop centre is the best deterministic starting guess.
        start_x = int(self.variables.get("cursor_x", self.width // 2))
        start_y = int(self.variables.get("cursor_y", self.height // 2))
        steps = max(1, min(120, int(duration * 60)))
        for i in range(1, steps + 1):
            t = i / steps
            # Smoothstep easing makes motion less abrupt.
            eased = t * t * (3 - 2 * t)
            px = round(start_x + (x - start_x) * eased)
            py = round(start_y + (y - start_y) * eased)
            self.backend.warp(px, py)
            self.variables["cursor_x"], self.variables["cursor_y"] = px, py
            await asyncio.sleep(duration / steps if duration else 0)

    def _clipboard(self, action: str, value: str | None = None) -> str:
        if action == "write":
            candidates = []
            if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-copy"):
                candidates = [["wl-copy"]]
            elif shutil.which("xclip"):
                candidates = [["xclip", "-selection", "clipboard"]]
            elif shutil.which("xsel"):
                candidates = [["xsel", "--clipboard", "--input"]]
            if not candidates: raise RuntimeErrorKeyloom("Clipboard write needs wl-copy (Wayland), xclip, or xsel.")
            subprocess.run(candidates[0], input=value or "", text=True, check=True, timeout=3)
            return ""
        if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-paste"):
            cmd = ["wl-paste", "--no-newline"]
        elif shutil.which("xclip"):
            cmd = ["xclip", "-selection", "clipboard", "-o"]
        elif shutil.which("xsel"):
            cmd = ["xsel", "--clipboard", "--output"]
        else: raise RuntimeErrorKeyloom("Clipboard read needs wl-paste (Wayland), xclip, or xsel.")
        return subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=3).stdout

    def _hold_button(self, owner: str, button: str) -> None:
        if owner in self.held_tokens:
            if self.held_tokens[owner] != button:
                raise RuntimeErrorKeyloom(f"Hold token {owner!r} already belongs to {self.held_tokens[owner]}")
            return
        self.held_tokens[owner] = button
        count = self.button_refs.get(button, 0)
        self.button_refs[button] = count + 1
        if count == 0: self.backend.button(button, True)

    def _free_button(self, owner: str) -> None:
        actual = self.held_tokens.pop(owner, None)
        if not actual: return
        count = self.button_refs.get(actual, 1) - 1
        if count <= 0:
            self.button_refs.pop(actual, None)
            self.backend.button(actual, False)
        else:
            self.button_refs[actual] = count

    def _hold_key(self, name: str) -> None:
        count = self.key_refs.get(name, 0)
        self.key_refs[name] = count + 1
        if count == 0: self.backend.key(name, True)

    def _free_key(self, name: str) -> None:
        count = self.key_refs.get(name, 0) - 1
        if count <= 0:
            if name in self.key_refs:
                self.key_refs.pop(name, None)
                self.backend.key(name, False)
        else:
            self.key_refs[name] = count

    def _release_all_holds(self) -> None:
        if self.backend:
            for button in set(self.held_tokens.values()):
                try: self.backend.button(button, False)
                except Exception: pass
            for key in list(self.key_refs):
                try: self.backend.key(key, False)
                except Exception: pass
            self.held_tokens.clear(); self.button_refs.clear(); self.key_refs.clear()

    def _eval(self, expr):
        kind = expr[0]
        if kind == "literal": return expr[1]
        if kind == "variable":
            name = expr[1]
            if name == "event_key": return self._event_key_var.get()
            if name == "event_edge": return self._event_edge_var.get()
            if name not in self.variables:
                raise RuntimeErrorKeyloom(f"Unknown variable {name!r}")
            return self.variables[name]
        if kind == "unary":
            op, value = expr[1], self._eval(expr[2])
            if op == "-": return -value
            if op == "+": return +value
            return not bool(value)
        if kind == "binary":
            op, left_expr, right_expr = expr[1], expr[2], expr[3]
            left = self._eval(left_expr)
            if op in ("and", "&&") and not left: return False
            if op in ("or", "||") and left: return True
            right = self._eval(right_expr)
            operations = {
                "+": lambda: left + right, "-": lambda: left - right,
                "*": lambda: left * right, "/": lambda: left / right,
                "%": lambda: left % right, "==": lambda: left == right,
                "!=": lambda: left != right, "<": lambda: left < right,
                "<=": lambda: left <= right, ">": lambda: left > right,
                ">=": lambda: left >= right, "and": lambda: bool(left and right),
                "&&": lambda: bool(left and right), "or": lambda: bool(left or right),
                "||": lambda: bool(left or right),
            }
            try: return operations[op]()
            except ZeroDivisionError as exc: raise RuntimeErrorKeyloom("Division by zero") from exc
        if kind == "call":
            name, args_expr = expr[1], expr[2]
            args = [self._eval(a) for a in args_expr]
            functions = {
                "min": min, "max": max, "abs": abs, "round": round,
                "floor": math.floor, "ceil": math.ceil, "sqrt": math.sqrt,
                "clamp": lambda x, lo, hi: max(lo, min(hi, x)),
                "int": int, "float": float, "str": str, "bool": bool,
                "len": len, "random": random.random,
                "randint": random.randint,
                "elapsed": lambda: time.monotonic() - self.started_at,
                "key_down": lambda k: canonical_key(str(k)) in self.pressed_keys,
                "mouse_held": lambda b: self.button_refs.get(str(b).lower(), 0) > 0,
                "screen_width": lambda: self.width,
                "screen_height": lambda: self.height,
            }
            if name not in functions:
                raise RuntimeErrorKeyloom(f"Unknown function {name!r}")
            try: return functions[name](*args)
            except TypeError as exc: raise RuntimeErrorKeyloom(f"Invalid arguments for {name}(): {exc}") from exc
        raise RuntimeErrorKeyloom(f"Invalid expression node {kind!r}")
