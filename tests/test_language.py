import unittest

from keyloom.parser import parse, ParseError
from keyloom.checker import check_script


class LanguageTests(unittest.TestCase):
    def test_parse_a_d_example(self):
        source = '''
loom "two-spots" {
    option input = "evdev"
    option output = "auto"
    option panic = "PAUSE"
    when key A goes down {
        cursor warp 640, 420
        mouse left hold as spot_a
    }
    when key A goes up {
        mouse left free as spot_a
    }
    when key D goes down {
        cursor warp 1080, 420
        mouse left hold as spot_d
    }
    when key D goes up {
        mouse left free as spot_d
    }
}
'''
        script = parse(source)
        self.assertEqual(script.name, "two-spots")
        self.assertEqual(len(script.events), 4)
        self.assertEqual(script.events[0].args[:3], ("key", "A", "down"))

    def test_nested_flow_and_expressions(self):
        script = parse('''
loom "flow" {
    when app starts {
        set count = 3
        if count > 1 {
            repeat count {
                log "loop"
            }
        } else {
            log "single"
        }
    }
}
''')
        self.assertEqual(script.events[0].body[0].kind, "set")
        self.assertEqual(script.events[0].body[1].kind, "if")
        self.assertEqual(script.events[0].body[1].body[0].kind, "repeat")

    def test_held_timer_event(self):
        script = parse('''
loom "held" {
    when key F8 stays held every 50ms {
        log "held"
    }
}
''')
        self.assertEqual(script.events[0].args, ("key", "F8", "held", 0.05))

    def test_rejects_unknown_top_level(self):
        with self.assertRaises(ParseError):
            parse('loom "bad" { cursor warp 1, 2 }')

    def test_missing_event_rejected(self):
        with self.assertRaises(ParseError):
            parse('loom "empty" {}')

    def test_static_check_accepts_spot_sample(self):
        from pathlib import Path
        source = Path(__file__).parents[1].joinpath("examples", "two_spots.loom").read_text()
        diagnostics = check_script(parse(source, "two_spots.loom"))
        self.assertFalse([d for d in diagnostics if d.level == "error"])

    def test_else_on_next_line(self):
        script = parse('''
loom "else-line" {
    when app starts {
        if true {
            log "yes"
        }
        else {
            log "no"
        }
    }
}
''')
        self.assertEqual(len(script.events[0].body[0].otherwise), 1)

    def test_shared_mouse_holds_only_release_after_last_owner(self):
        from keyloom.runtime import Runtime
        script = parse('loom "holds" { when app starts { log "ready" } }')

        class FakeBackend:
            def __init__(self): self.buttons = []
            def button(self, name, down): self.buttons.append((name, down))
            def key(self, name, down): pass
            def warp(self, x, y): pass
            def move(self, x, y): pass
            def type_text(self, text): pass
            def scroll(self, x, y): pass
            def close(self): pass

        runtime = Runtime(script, dry_run=True)
        runtime.backend = FakeBackend()
        runtime._hold_button("A", "left")
        runtime._hold_button("D", "left")
        runtime._free_button("A")
        self.assertEqual(runtime.backend.buttons, [("left", True)])
        runtime._free_button("D")
        self.assertEqual(runtime.backend.buttons, [("left", True), ("left", False)])

    def test_key_holds_are_reference_counted(self):
        from keyloom.runtime import Runtime
        script = parse('loom "keys" { when app starts { log "ready" } }')

        class FakeBackend:
            def __init__(self): self.keys = []
            def key(self, name, down): self.keys.append((name, down))
            def button(self, name, down): pass
            def warp(self, x, y): pass
            def move(self, x, y): pass
            def type_text(self, text): pass
            def scroll(self, x, y): pass
            def close(self): pass

        runtime = Runtime(script, dry_run=True)
        runtime.backend = FakeBackend()
        runtime._hold_key("LEFTCTRL")
        runtime._hold_key("LEFTCTRL")
        runtime._free_key("LEFTCTRL")
        self.assertEqual(runtime.backend.keys, [("LEFTCTRL", True)])
        runtime._free_key("LEFTCTRL")
        self.assertEqual(runtime.backend.keys, [("LEFTCTRL", True), ("LEFTCTRL", False)])

    def test_arithmetic_subtraction_without_spaces(self):
        script = parse('''
loom "math" {
    when app starts {
        set count = 5-2
        assert count == 3
    }
}
''')
        self.assertEqual(script.events[0].body[0].args[1][0], "binary")

    def test_string_escapes_are_decoded_once(self):
        from keyloom.lexer import tokenize
        tokens = tokenize(r'loom "x" { when app starts { log "a\nb" log "c\\nd" } }')
        strings = [t.value for t in tokens if t.kind == "STRING"]
        self.assertEqual(strings, ["x", "a\nb", "c\\nd"])

    def test_numeric_keys_and_chords(self):
        script = parse('''
loom "numeric-keys" {
    when key 1 goes down {
        key CTRL+1 tap for 50ms
    }
}
''')
        self.assertEqual(script.events[0].args[:3], ("key", "1", "down"))
        self.assertEqual(script.events[0].body[0].args[0], "CTRL+1")

    def test_absolute_axis_info_orders_min_before_max(self):
        from keyloom.backends import absolute_axis_info
        info = absolute_axis_info(1367)
        self.assertEqual(info.min, 0)
        self.assertEqual(info.max, 1367)
        self.assertEqual(info.value, 0)

    def test_ydotool_pointer_motion_uses_real_absolute_backend(self):
        import keyloom.backends as backends

        calls = []

        class FakePointer:
            def __init__(self, width, height, *, keyboard=True):
                calls.append(("init", width, height, keyboard))
            def warp(self, x, y): calls.append(("warp", x, y))
            def move(self, dx, dy): calls.append(("move", dx, dy))
            def scroll(self, x, y): calls.append(("scroll", x, y))
            def close(self): calls.append(("close",))

        original_pointer = backends.UInputBackend
        original_which = backends.shutil.which
        original_call = backends.YdotoolBackend._call
        backends.UInputBackend = FakePointer
        backends.shutil.which = lambda name: "/usr/bin/ydotool"
        backends.YdotoolBackend._call = lambda self, args, tolerate=False: ""
        try:
            backend = backends.YdotoolBackend(width=1368, height=768)
            backend.warp(640, 420)
            backend.move(10, -5)
            backend.scroll(0, -3)
            backend.close()
        finally:
            backends.UInputBackend = original_pointer
            backends.shutil.which = original_which
            backends.YdotoolBackend._call = original_call

        # The old implementation called `ydotool mousemove --absolute`, which
        # jumps to the top-left first. Motion must go through the uinput pointer.
        self.assertEqual(calls, [
            ("init", 1368, 768, False),
            ("warp", 640, 420),
            ("move", 10, -5),
            ("scroll", 0, -3),
            ("close",),
        ])

    def test_two_spots_press_and_release_logic(self):
        import asyncio
        from pathlib import Path
        from keyloom.runtime import Runtime
        source = Path(__file__).parents[1].joinpath("examples", "two_spots.loom").read_text()
        runtime = Runtime(parse(source), dry_run=True)

        class FakeBackend:
            def __init__(self): self.actions = []
            def button(self, name, down): self.actions.append(("button", name, down))
            def key(self, name, down): self.actions.append(("key", name, down))
            def warp(self, x, y): self.actions.append(("warp", x, y))
            def move(self, x, y): self.actions.append(("move", x, y))
            def type_text(self, text): self.actions.append(("type", text))
            def scroll(self, x, y): self.actions.append(("scroll", x, y))
            def close(self): pass

        async def exercise():
            runtime.backend = FakeBackend()
            runtime.panic_key = None
            runtime.device_down["fake-keyboard"] = set()
            await runtime._physical_key("fake-keyboard", "A", True)
            await asyncio.sleep(0.01)
            await runtime._physical_key("fake-keyboard", "D", True)
            await asyncio.sleep(0.01)
            await runtime._physical_key("fake-keyboard", "A", False)
            await asyncio.sleep(0.01)
            self.assertIn(("button", "left", True), runtime.backend.actions)
            self.assertNotIn(("button", "left", False), runtime.backend.actions)
            await runtime._physical_key("fake-keyboard", "D", False)
            await asyncio.sleep(0.01)

        asyncio.run(exercise())
        self.assertEqual(
            [x for x in runtime.backend.actions if x[0] == "warp"],
            [("warp", 640, 420), ("warp", 1080, 420)],
        )
        self.assertEqual(
            [x for x in runtime.backend.actions if x[0] == "button"],
            [("button", "left", True), ("button", "left", False)],
        )


class MousePositionPickerTests(unittest.TestCase):
    def test_coordinate_format_matches_loom_syntax(self):
        from keyloom.mousepos import format_coordinates
        self.assertEqual(format_coordinates(640, 420), "640, 420")

    def test_tooltip_prefers_below_and_right(self):
        from keyloom.mousepos import tooltip_position
        self.assertEqual(tooltip_position(100, 100, 1368, 768, 80, 30), (118, 118))

    def test_tooltip_flips_when_it_would_leave_the_screen(self):
        from keyloom.mousepos import tooltip_position
        # Near the right edge: move the tooltip to the left of the cursor.
        self.assertEqual(tooltip_position(1360, 100, 1368, 768, 80, 30), (1262, 118))
        # Near the bottom edge: move the tooltip above the cursor.
        self.assertEqual(tooltip_position(100, 760, 1368, 768, 80, 30), (118, 712))

    def test_tooltip_clamps_to_screen(self):
        from keyloom.mousepos import clamp_to_screen, tooltip_position
        self.assertEqual(clamp_to_screen(-5, 9999, 1368, 768), (0, 767))
        tx, ty = tooltip_position(0, 0, 60, 40, 80, 30)
        self.assertEqual((tx, ty), (0, 0))

    def test_screenshot_commands_prefer_spectacle(self):
        from unittest.mock import patch
        from keyloom import mousepos

        with patch("keyloom.mousepos.shutil.which", return_value="/usr/bin/tool"):
            commands = mousepos.screenshot_commands("/tmp/shot.png")
        self.assertEqual(
            [argv[0] for argv in commands],
            ["spectacle", "grim", "gnome-screenshot", "scrot", "import"],
        )
        self.assertEqual(
            commands[0],
            ["spectacle", "-b", "-n", "-f", "--scaled", "-o", "/tmp/shot.png"],
        )

    def test_screenshot_commands_empty_without_tools(self):
        from unittest.mock import patch
        from keyloom import mousepos

        with patch("keyloom.mousepos.shutil.which", return_value=None):
            self.assertEqual(mousepos.screenshot_commands("/tmp/shot.png"), [])

    def test_capture_desktop_reports_missing_tools(self):
        from unittest.mock import patch
        from keyloom import mousepos

        with patch("keyloom.mousepos.screenshot_commands", return_value=[]):
            with self.assertRaises(mousepos.PickerError) as raised:
                mousepos.capture_desktop()
        self.assertIn("screenshot tool", str(raised.exception))

    def test_cli_flag_is_parsed(self):
        from keyloom.cli import _parser
        args = _parser().parse_args(["--get-mouse-pos"])
        self.assertTrue(args.get_mouse_pos)

    def test_cli_prints_picked_position(self):
        import contextlib
        import io
        from unittest.mock import patch
        from keyloom import cli

        with patch("keyloom.mousepos.pick_mouse_position", return_value=(12, 34)):
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = cli.main(["--get-mouse-pos"])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue().strip(), "12, 34")

    def test_cli_cancel_returns_130(self):
        import contextlib
        import io
        from unittest.mock import patch
        from keyloom import cli

        with patch("keyloom.mousepos.pick_mouse_position", return_value=None):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = cli.main(["--get-mouse-pos"])
        self.assertEqual(code, 130)
        self.assertIn("cancelled", err.getvalue())

    def test_cli_reports_picker_startup_errors(self):
        import contextlib
        import io
        from unittest.mock import patch
        from keyloom import cli
        from keyloom.mousepos import PickerError

        with patch("keyloom.mousepos.pick_mouse_position", side_effect=PickerError("no display")):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code = cli.main(["--get-mouse-pos"])
        self.assertEqual(code, 2)
        self.assertIn("no display", err.getvalue())


if __name__ == "__main__":
    unittest.main()
