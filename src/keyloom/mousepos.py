"""Interactive mouse position picker.

Captures the desktop, shows it full-screen in an XWayland window, and draws a
live coordinate tooltip over it. A left click is consumed by the overlay,
printed to stdout, and ends the picker. The screenshot keeps the desktop
visible even though the overlay itself is opaque, which is what makes clicks
over native Wayland windows reliable. This module imports tkinter lazily so
``keyloom --check`` and the runtime never need a display.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile


class PickerError(RuntimeError):
    """The picker cannot start (no display, missing tkinter, no screenshot)."""


TOOLTIP_OFFSET = 18
POLL_MS = 16
SCREENSHOT_TIMEOUT = 15

# Preference order. Spectacle covers KDE, grim covers wlroots, and the rest are
# X11 and GNOME fallbacks.
SCREENSHOT_TOOLS = ("spectacle", "grim", "gnome-screenshot", "scrot", "import")


def format_coordinates(x: int, y: int) -> str:
    """Render a position in the same shape the .loom language uses."""
    return f"{int(x)}, {int(y)}"


def clamp_to_screen(x: int, y: int, width: int, height: int) -> tuple[int, int]:
    width = max(1, int(width))
    height = max(1, int(height))
    return max(0, min(width - 1, int(x))), max(0, min(height - 1, int(y)))


def tooltip_position(
    x: int,
    y: int,
    screen_width: int,
    screen_height: int,
    tip_width: int,
    tip_height: int,
    offset: int = TOOLTIP_OFFSET,
) -> tuple[int, int]:
    """Place a tooltip next to the pointer without leaving the screen.

    The preferred spot is below and to the right of the cursor. It flips to the
    other side when there is no room, then clamps to the screen edge.
    """
    screen_width = max(1, int(screen_width))
    screen_height = max(1, int(screen_height))
    tip_width = max(1, int(tip_width))
    tip_height = max(1, int(tip_height))

    tx = int(x) + offset
    ty = int(y) + offset
    if tx + tip_width > screen_width:
        tx = int(x) - tip_width - offset
    if ty + tip_height > screen_height:
        ty = int(y) - tip_height - offset
    tx = max(0, min(screen_width - tip_width, tx))
    ty = max(0, min(screen_height - tip_height, ty))
    return tx, ty


def _tkinter():
    try:
        import tkinter as tk
    except ImportError as exc:
        raise PickerError(
            "The mouse position picker needs tkinter (the python3-tk package on "
            "Debian/Ubuntu). Install it and try again."
        ) from exc
    return tk


def available_screenshot_tools() -> list[str]:
    """Return the installed screenshot tools, in preference order."""
    return [name for name in SCREENSHOT_TOOLS if shutil.which(name)]


def screenshot_commands(path: str) -> list[list[str]]:
    """Return screenshot commands that can capture the whole desktop.

    Spectacle is preferred on KDE. grim covers wlroots, and the remaining
    commands are fallbacks for X11 and GNOME.
    """
    commands: list[list[str]] = []
    for name in available_screenshot_tools():
        if name == "spectacle":
            commands.append(["spectacle", "-b", "-n", "-f", "--scaled", "-o", path])
        elif name == "grim":
            commands.append(["grim", path])
        elif name == "gnome-screenshot":
            commands.append(["gnome-screenshot", "-f", path])
        elif name == "scrot":
            commands.append(["scrot", path])
        elif name == "import":
            commands.append(["import", "-window", "root", path])
    return commands


def capture_desktop() -> tuple[str, str]:
    """Capture the desktop to a temporary PNG.

    Returns ``(path, tool_name)``. The caller owns the file and should remove it.
    """
    handle, path = tempfile.mkstemp(prefix="keyloom-picker-", suffix=".png")
    os.close(handle)
    commands = screenshot_commands(path)
    if not commands:
        try:
            os.unlink(path)
        except OSError:
            pass
        raise PickerError(
            "The mouse position picker needs a screenshot tool so the desktop "
            "stays visible while you pick. Install spectacle (KDE), grim "
            "(wlroots), gnome-screenshot, scrot, or ImageMagick's import."
        )

    failures: list[str] = []
    for argv in commands:
        try:
            proc = subprocess.run(
                argv, capture_output=True, text=True, timeout=SCREENSHOT_TIMEOUT
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            failures.append(f"{argv[0]}: {exc}")
            continue
        if proc.returncode == 0 and os.path.exists(path) and os.path.getsize(path) > 0:
            return path, argv[0]
        detail = (proc.stderr or proc.stdout).strip()
        failures.append(f"{argv[0]}: {detail or f'exit {proc.returncode}'}")

    try:
        os.unlink(path)
    except OSError:
        pass
    raise PickerError(
        "Could not capture the desktop for the mouse position picker. "
        + "; ".join(failures)
    )


def _load_image(tk, path: str, width: int, height: int):
    """Load the screenshot, preferring Pillow when it is installed."""
    try:
        from PIL import Image, ImageTk
    except ImportError:
        pass
    else:
        try:
            image = Image.open(path)
            if image.size != (width, height):
                image = image.resize((width, height), Image.LANCZOS)
            return ImageTk.PhotoImage(image)
        except Exception:
            pass

    image = tk.PhotoImage(file=path)
    image_width, image_height = image.width(), image.height()
    # Tk can only downscale by integer factors. This covers 2x HiDPI captures;
    # fractional scaling may still need --screen calibration.
    factor = min(image_width // max(1, width), image_height // max(1, height))
    if factor > 1:
        image = image.subsample(factor, factor)
    return image


class MousePositionPicker:
    """A screenshot overlay with a coordinate tooltip and click capture."""

    def __init__(self, *, verbose: bool = False):
        self.verbose = verbose
        self.result: tuple[int, int] | None = None
        self.cancelled = False
        self.screen_width = 0
        self.screen_height = 0
        self._root = None
        self._capture = None
        self._canvas = None
        self._tip = None
        self._label = None
        self._image = None
        self._screenshot_path: str | None = None
        self._tick_id = None
        self._closed = False

    def run(self) -> tuple[int, int] | None:
        try:
            self._build()
            self._root.mainloop()
        except KeyboardInterrupt:
            self.cancelled = True
        finally:
            self._destroy()
        return self.result

    def _build(self) -> None:
        tk = _tkinter()
        try:
            root = tk.Tk()
        except tk.TclError as exc:
            raise PickerError(
                "Cannot open a display for the mouse position picker. On Wayland, "
                "run it in a session with XWayland and DISPLAY set, or use "
                "keyloom --screen to calibrate coordinates manually. "
                f"Tk said: {exc}"
            ) from exc

        self._root = root
        root.withdraw()
        width = max(1, int(root.winfo_screenwidth()))
        height = max(1, int(root.winfo_screenheight()))
        self.screen_width, self.screen_height = width, height

        self._screenshot_path, tool = capture_desktop()
        try:
            self._build_overlay(tk, width, height)
        except Exception:
            self._remove_screenshot()
            raise
        if self.verbose:
            print(
                f"[keyloom] picker screenshot: {tool}; screen bounds: {width}x{height}",
                file=sys.stderr,
            )

        self._build_tooltip(tk)
        self._canvas.bind("<ButtonPress>", self._on_button)
        self._canvas.bind("<ButtonRelease>", self._consume)
        self._canvas.bind("<Escape>", self._on_cancel)
        self._tip.bind("<ButtonPress>", self._on_button)
        self._tip.bind("<ButtonRelease>", self._consume)
        self._tip.bind("<Escape>", self._on_cancel)
        root.bind_all("<Escape>", self._on_cancel)
        self._tick()

    def _build_overlay(self, tk, width: int, height: int) -> None:
        capture = tk.Toplevel(self._root)
        self._capture = capture
        capture.title("Keyloom mouse position")
        try:
            # A managed splash window is undecorated but still receives input
            # above native Wayland windows. Override-redirect windows do not.
            capture.attributes("-type", "splash")
        except tk.TclError:
            capture.overrideredirect(True)
        capture.geometry(f"{width}x{height}+0+0")
        capture.configure(bg="black")
        capture.attributes("-topmost", True)
        capture.lift()
        try:
            capture.focus_force()
        except tk.TclError:
            pass

        canvas = tk.Canvas(
            capture, width=width, height=height, bg="black",
            highlightthickness=0,
        )
        canvas.pack(fill="both", expand=True)
        self._canvas = canvas
        self._image = _load_image(tk, self._screenshot_path, width, height)
        canvas.create_image(0, 0, anchor="nw", image=self._image)
        capture.update_idletasks()
        capture.update()

    def _build_tooltip(self, tk) -> None:
        tip = tk.Toplevel(self._root)
        self._tip = tip
        tip.withdraw()
        tip.overrideredirect(True)
        tip.attributes("-topmost", True)
        tip.configure(bg="#111111")
        label = tk.Label(
            tip,
            text=format_coordinates(0, 0),
            bg="#111111",
            fg="#f5f5f5",
            font=("DejaVu Sans", 15, "bold"),
            padx=12,
            pady=7,
            highlightthickness=1,
            highlightbackground="#f5f5f5",
        )
        label.pack()
        self._label = label
        tip.update_idletasks()

    def _tick(self) -> None:
        if self._closed or self._root is None:
            return
        try:
            px, py = self._root.winfo_pointerxy()
            px, py = clamp_to_screen(px, py, self.screen_width, self.screen_height)
            self._label.configure(text=format_coordinates(px, py))
            self._tip.update_idletasks()
            tw = max(1, self._tip.winfo_width())
            th = max(1, self._tip.winfo_height())
            tx, ty = tooltip_position(px, py, self.screen_width, self.screen_height, tw, th)
            self._tip.geometry(f"+{tx}+{ty}")
            if not self._tip.winfo_ismapped():
                self._tip.deiconify()
                self._tip.lift()
            self._tick_id = self._root.after(POLL_MS, self._tick)
        except Exception as exc:  # Tk can fail if the display disappears.
            if self.verbose:
                print(f"[keyloom] picker stopped: {exc}", file=sys.stderr)
            self.cancelled = True
            self._stop()

    def _on_button(self, event) -> str:
        if self._closed:
            return "break"
        if event.num == 1:
            self.result = clamp_to_screen(
                event.x_root, event.y_root, self.screen_width, self.screen_height
            )
        elif event.num == 3:
            self.cancelled = True
        # Consume every button, including middle/back/forward, so the click does
        # not pass through to the window underneath.
        self._stop()
        return "break"

    def _on_cancel(self, _event=None) -> str:
        self.cancelled = True
        self._stop()
        return "break"

    def _consume(self, _event=None) -> str:
        return "break"

    def _stop(self) -> None:
        if self._closed:
            return
        self._closed = True
        if self._tick_id is not None and self._root is not None:
            try:
                self._root.after_cancel(self._tick_id)
            except Exception:
                pass
            self._tick_id = None
        if self._root is not None:
            try:
                self._root.quit()
            except Exception:
                pass

    def _remove_screenshot(self) -> None:
        if self._screenshot_path:
            try:
                os.unlink(self._screenshot_path)
            except OSError:
                pass
            self._screenshot_path = None

    def _destroy(self) -> None:
        self._closed = True
        self._remove_screenshot()
        for widget in (self._tip, self._capture, self._root):
            if widget is None:
                continue
            try:
                widget.destroy()
            except Exception:
                pass
        self._tip = None
        self._capture = None
        self._canvas = None
        self._image = None
        self._root = None


def pick_mouse_position(*, verbose: bool = False) -> tuple[int, int] | None:
    """Show the screenshot overlay and return the picked position."""
    return MousePositionPicker(verbose=verbose).run()
