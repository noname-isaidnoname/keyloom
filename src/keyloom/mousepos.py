"""Interactive mouse position picker.

Runs an XWayland overlay and shows the pointer coordinates next to the cursor.
A left click is consumed by the overlay, printed to stdout, and ends the
picker. This module imports tkinter lazily so ``keyloom --check`` and the
runtime never need a display.
"""

from __future__ import annotations

import sys


class PickerError(RuntimeError):
    """The picker cannot start (no display, missing tkinter, Tk failure)."""


TOOLTIP_OFFSET = 18
OVERLAY_OPACITY = 0.01
RETICLE_SIZE = 128
RETICLE_POLL_MS = 8
OVERLAY_POLL_MS = 16


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


def _load_x11():
    """Return (ctypes, libX11, display) or None when X11 is unavailable."""
    try:
        import ctypes
        import ctypes.util
    except ImportError:
        return None
    library = ctypes.util.find_library("X11")
    if not library:
        return None
    try:
        x11 = ctypes.CDLL(library)
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XOpenDisplay.restype = ctypes.c_void_p
        display = x11.XOpenDisplay(None)
    except (AttributeError, OSError):
        return None
    if not display:
        return None
    return ctypes, x11, display


def _opacity_supported() -> bool:
    """Check the EWMH _NET_SUPPORTED list for window opacity."""
    loaded = _load_x11()
    if loaded is None:
        return False
    ctypes, x11, display = loaded
    try:
        c_ulong = ctypes.c_ulong
        c_int = ctypes.c_int
        c_uint = ctypes.c_uint
        c_void_p = ctypes.c_void_p
        c_char_p = ctypes.c_char_p

        x11.XInternAtom.argtypes = [c_void_p, c_char_p, c_int]
        x11.XInternAtom.restype = c_ulong
        x11.XDefaultRootWindow.argtypes = [c_void_p]
        x11.XDefaultRootWindow.restype = c_ulong
        x11.XGetWindowProperty.argtypes = [
            c_void_p, c_ulong, c_ulong, ctypes.c_long, ctypes.c_long, c_int,
            c_ulong, ctypes.POINTER(c_ulong), ctypes.POINTER(c_int),
            ctypes.POINTER(c_ulong), ctypes.POINTER(c_ulong),
            ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),
        ]
        x11.XGetWindowProperty.restype = c_int
        x11.XFree.argtypes = [c_void_p]
        x11.XCloseDisplay.argtypes = [c_void_p]

        supported_atom = x11.XInternAtom(display, b"_NET_SUPPORTED", False)
        atom_type = x11.XInternAtom(display, b"ATOM", False)
        opacity_atom = x11.XInternAtom(display, b"_NET_WM_WINDOW_OPACITY", False)
        actual_type = c_ulong()
        actual_format = c_int()
        nitems = c_ulong()
        bytes_after = c_ulong()
        prop = ctypes.POINTER(ctypes.c_ubyte)()
        status = x11.XGetWindowProperty(
            display, x11.XDefaultRootWindow(display), supported_atom,
            0, 4096, False, atom_type, ctypes.byref(actual_type),
            ctypes.byref(actual_format), ctypes.byref(nitems),
            ctypes.byref(bytes_after), ctypes.byref(prop),
        )
        if status != 0 or not prop:
            return False
        try:
            values = ctypes.cast(prop, ctypes.POINTER(c_ulong))
            return any(values[i] == opacity_atom for i in range(nitems.value))
        finally:
            x11.XFree(prop)
    except (AttributeError, OSError, ValueError):
        return False
    finally:
        try:
            x11.XCloseDisplay(display)
        except Exception:
            pass


def _apply_opacity(window_id: int, opacity: float) -> bool:
    """Set _NET_WM_WINDOW_OPACITY on a window and its WM frame.

    Tk's ``-alpha`` attribute targets the client window, but KWin composites
    the WM frame. Setting the EWMH property on both keeps the overlay nearly
    transparent without affecting the opaque tooltip.
    """
    loaded = _load_x11()
    if loaded is None:
        return False
    ctypes, x11, display = loaded
    try:
        c_ulong = ctypes.c_ulong
        c_int = ctypes.c_int
        c_uint = ctypes.c_uint
        c_void_p = ctypes.c_void_p
        c_char_p = ctypes.c_char_p

        x11.XInternAtom.argtypes = [c_void_p, c_char_p, c_int]
        x11.XInternAtom.restype = c_ulong
        x11.XQueryTree.argtypes = [
            c_void_p, c_ulong, ctypes.POINTER(c_ulong), ctypes.POINTER(c_ulong),
            ctypes.POINTER(ctypes.POINTER(c_ulong)), ctypes.POINTER(c_uint),
        ]
        x11.XQueryTree.restype = c_int
        x11.XChangeProperty.argtypes = [
            c_void_p, c_ulong, c_ulong, c_ulong, c_int, c_int, c_void_p, c_int,
        ]
        x11.XChangeProperty.restype = c_int
        x11.XFlush.argtypes = [c_void_p]
        x11.XFree.argtypes = [c_void_p]
        x11.XCloseDisplay.argtypes = [c_void_p]

        atom = x11.XInternAtom(display, b"_NET_WM_WINDOW_OPACITY", False)
        if not atom:
            return False
        value = c_ulong(max(0, min(0xFFFFFFFF, int(opacity * 0xFFFFFFFF))))

        targets = {int(window_id)}
        root_return = c_ulong()
        parent = c_ulong()
        children = ctypes.POINTER(c_ulong)()
        count = c_uint()
        if x11.XQueryTree(display, c_ulong(window_id), ctypes.byref(root_return),
                          ctypes.byref(parent), ctypes.byref(children), ctypes.byref(count)):
            if parent.value:
                targets.add(int(parent.value))
            if children:
                x11.XFree(children)

        for target in targets:
            x11.XChangeProperty(
                display, c_ulong(target), c_ulong(atom), c_ulong(6), 32, 0,
                ctypes.byref(value), 1,
            )
        x11.XFlush(display)
        return True
    except (AttributeError, OSError, ValueError):
        return False
    finally:
        try:
            x11.XCloseDisplay(display)
        except Exception:
            pass


class MousePositionPicker:
    """A click-capturing overlay with a coordinate tooltip."""

    def __init__(self, *, verbose: bool = False):
        self.verbose = verbose
        self.result: tuple[int, int] | None = None
        self.cancelled = False
        self.screen_width = 0
        self.screen_height = 0
        self._root = None
        self._capture = None
        self._tip = None
        self._label = None
        self._tick_id = None
        self._closed = False
        self._fullscreen = False
        self._poll_ms = OVERLAY_POLL_MS
        self._tooltip_offset = TOOLTIP_OFFSET
        self._reticle_size = RETICLE_SIZE
        self._reticle_pos: tuple[int, int] | None = None

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

        if _opacity_supported():
            self._build_fullscreen(tk, width, height)
        else:
            self._build_reticle(tk, width, height)
        if self.verbose:
            mode = "fullscreen overlay" if self._fullscreen else "reticle (no compositor opacity)"
            print(f"[keyloom] picker mode: {mode}; screen bounds: {width}x{height}", file=sys.stderr)

        self._build_tooltip(tk)
        for widget in (self._capture, self._tip):
            widget.bind("<ButtonPress>", self._on_button)
            widget.bind("<ButtonRelease>", self._consume)
            widget.bind("<Escape>", self._on_cancel)
        root.bind_all("<Escape>", self._on_cancel)
        self._tick()

    def _build_fullscreen(self, tk, width: int, height: int) -> None:
        capture = tk.Toplevel(self._root)
        self._capture = capture
        self._fullscreen = True
        self._poll_ms = OVERLAY_POLL_MS
        self._tooltip_offset = TOOLTIP_OFFSET
        capture.title("Keyloom mouse position")
        try:
            # Splash windows are undecorated and stay out of the taskbar while
            # still being managed, so the compositor honors EWMH opacity.
            capture.attributes("-type", "splash")
        except tk.TclError:
            capture.overrideredirect(True)
        capture.geometry(f"{width}x{height}+0+0")
        capture.configure(bg="black", cursor="crosshair")
        capture.attributes("-topmost", True)
        capture.lift()
        try:
            capture.focus_force()
        except tk.TclError:
            pass
        capture.update_idletasks()
        capture.update()
        if not _apply_opacity(capture.winfo_id(), OVERLAY_OPACITY):
            try:
                capture.attributes("-alpha", OVERLAY_OPACITY)
            except tk.TclError:
                pass
            if self.verbose:
                print(
                    "[keyloom] could not set _NET_WM_WINDOW_OPACITY; the overlay "
                    "may cover the desktop.",
                    file=sys.stderr,
                )

    def _build_reticle(self, tk, width: int, height: int) -> None:
        size = max(24, min(RETICLE_SIZE, width, height))
        self._reticle_size = size
        self._fullscreen = False
        self._poll_ms = RETICLE_POLL_MS
        self._tooltip_offset = size // 2 + 10

        capture = tk.Toplevel(self._root)
        self._capture = capture
        capture.overrideredirect(True)
        capture.attributes("-topmost", True)
        capture.configure(bg="#111111", cursor="crosshair")
        canvas = tk.Canvas(
            capture, width=size, height=size, bg="#111111",
            highlightthickness=0, cursor="crosshair",
        )
        canvas.pack()
        center = size // 2
        canvas.create_line(center, 0, center, size, fill="#f5f5f5")
        canvas.create_line(0, center, size, center, fill="#f5f5f5")
        radius = max(6, size // 5)
        canvas.create_oval(
            center - radius, center - radius, center + radius, center + radius,
            outline="#f5f5f5",
        )
        canvas.create_oval(
            center - 2, center - 2, center + 2, center + 2,
            fill="#ff6666", outline="",
        )
        px, py = self._root.winfo_pointerxy()
        self._position_reticle(*clamp_to_screen(px, py, width, height))
        capture.update_idletasks()
        capture.update()

    def _build_tooltip(self, tk) -> None:
        tip = tk.Toplevel(self._root)
        self._tip = tip
        tip.withdraw()
        tip.overrideredirect(True)
        tip.attributes("-topmost", True)
        tip.configure(bg="#111111", cursor="crosshair")
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

    def _position_reticle(self, px: int, py: int) -> None:
        size = self._reticle_size
        x = max(0, min(self.screen_width - size, px - size // 2))
        y = max(0, min(self.screen_height - size, py - size // 2))
        if self._reticle_pos == (x, y):
            return
        self._reticle_pos = (x, y)
        self._capture.geometry(f"{size}x{size}+{x}+{y}")

    def _tick(self) -> None:
        if self._closed or self._root is None:
            return
        try:
            px, py = self._root.winfo_pointerxy()
            px, py = clamp_to_screen(px, py, self.screen_width, self.screen_height)
            self._label.configure(text=format_coordinates(px, py))
            if not self._fullscreen:
                self._position_reticle(px, py)
            self._tip.update_idletasks()
            tw = max(1, self._tip.winfo_width())
            th = max(1, self._tip.winfo_height())
            tx, ty = tooltip_position(
                px, py, self.screen_width, self.screen_height, tw, th,
                offset=self._tooltip_offset,
            )
            self._tip.geometry(f"+{tx}+{ty}")
            if not self._tip.winfo_ismapped():
                self._tip.deiconify()
                self._tip.lift()
            self._tick_id = self._root.after(self._poll_ms, self._tick)
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

    def _destroy(self) -> None:
        self._closed = True
        for widget in (self._tip, self._capture, self._root):
            if widget is None:
                continue
            try:
                widget.destroy()
            except Exception:
                pass
        self._tip = None
        self._capture = None
        self._root = None


def pick_mouse_position(*, verbose: bool = False) -> tuple[int, int] | None:
    """Show the overlay and return the picked position, or None if cancelled."""
    return MousePositionPicker(verbose=verbose).run()
