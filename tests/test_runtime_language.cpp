#include "harness.hpp"
#include "runtime_fixtures.hpp"

#include <memory>
#include <string>
#include <vector>

#include "parser.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"

using namespace keyloom;
using keyloom_test::eval_expression;
using keyloom_test::RecordingBackend;


TEST_CASE(evaluates_language_expressions_like_python) {
    CHECK_EQ(eval_expression("5-2").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("1 / 2").to_string(), std::string("0.5"));
    CHECK_EQ(eval_expression("7 % 3").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("-7 % 3").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("\"ab\" + \"cd\"").to_string(), std::string("abcd"));
    CHECK_EQ(eval_expression("clamp(12, 0, 10)").to_string(), std::string("10"));
    CHECK_EQ(eval_expression("round(2.5)").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("round(3.5)").to_string(), std::string("4"));
    CHECK_EQ(eval_expression("str(1.0)").to_string(), std::string("1.0"));
    CHECK_EQ(eval_expression("min(3, 1, 2)").to_string(), std::string("1"));
    CHECK_EQ(eval_expression("max(3, 1, 2)").to_string(), std::string("3"));
    CHECK_EQ(eval_expression("not true").to_string(), std::string("False"));
    CHECK_EQ(eval_expression("len(\"hello\")").to_string(), std::string("5"));
    CHECK_EQ(eval_expression("1 == 1.0").to_string(), std::string("True"));
    CHECK_EQ(eval_expression("true and false").to_string(), std::string("False"));
    CHECK_EQ(eval_expression("floor(2.9)").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("ceil(2.1)").to_string(), std::string("3"));
}

TEST_CASE(executes_statement_flow) {
    const Script script = parse(R"LOOM(
loom "flow" {
    when app starts {
        set total = 0
        repeat 4 {
            set total = total + 2
        }
        while total > 5 {
            set total = total - 4
        }
        assert total == 4
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("total").to_string(), std::string("4"));
}

TEST_CASE(stops_a_runaway_while_loop) {
    const Script script = parse(R"LOOM(
loom "runaway" {
    when app starts {
        set count = 0
        while true {
            set count = count + 1
        }
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.scheduler().stop_requested());
}

TEST_CASE(emits_cursor_and_tap_actions) {
    const Script script = parse(R"LOOM(
loom "taps" {
    when key A goes down {
        cursor center
        mouse left tap for 10ms
        key CTRL+S tap for 10ms
        type "hi"
        scroll 0, -3
    }
}
)LOOM");
    Runtime::Options options;
    options.screen_size = "1920x1080";
    Runtime runtime(script, options);
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake", "A", true);
    runtime.scheduler().run();

    CHECK_EQ(runtime.width(), 1920);
    CHECK_EQ(raw->log,
             std::vector<std::string>({"warp 960, 540", "button left down", "button left up", "key LEFTCTRL down",
                                       "key S down", "key S up", "key LEFTCTRL up", "type hi", "scroll 0, -3"}));
}

TEST_CASE(screen_option_is_parsed_and_validated) {
    const Value auto_value = Value::of_str("auto");
    const auto fallback = parse_screen_option(&auto_value, {1920, 1080});
    CHECK_EQ(fallback.first, 1920);
    CHECK_EQ(fallback.second, 1080);

    const Value custom = Value::of_str("2560x1440");
    const auto parsed = parse_screen_option(&custom, {1920, 1080});
    CHECK_EQ(parsed.first, 2560);
    CHECK_EQ(parsed.second, 1440);

    const Value invalid = Value::of_str("2560-1440");
    CHECK_THROWS(parse_screen_option(&invalid, {1920, 1080}), RuntimeErrorKeyloom);
    const Value not_a_string = Value::of_int(5);
    CHECK_THROWS(parse_screen_option(&not_a_string, {1920, 1080}), RuntimeErrorKeyloom);
}

TEST_CASE(split_shell_words_matches_shlex) {
    CHECK_EQ(split_shell_words("gnome-calculator"), std::vector<std::string>({"gnome-calculator"}));
    CHECK_EQ(split_shell_words("cmd -a 'two words' \"three four\""),
             std::vector<std::string>({"cmd", "-a", "two words", "three four"}));
    CHECK_EQ(split_shell_words(R"(printf "a\"b")"), std::vector<std::string>({"printf", "a\"b"}));
    CHECK_THROWS(split_shell_words("cmd 'unclosed"), BackendError);
}

TEST_CASE(run_command_captures_output_and_exit_status) {
    const CommandResult ok = run_command({"/bin/sh", "-c", "printf out; printf err >&2"}, "", 2000);
    CHECK_TRUE(!ok.spawn_failed);
    CHECK_TRUE(!ok.timed_out);
    CHECK_EQ(ok.returncode, 0);
    CHECK_EQ(ok.out, "out");
    CHECK_EQ(ok.err, "err");

    const CommandResult status = run_command({"/bin/sh", "-c", "exit 7"}, "", 2000);
    CHECK_TRUE(!status.spawn_failed);
    CHECK_EQ(status.returncode, 7);
}

TEST_CASE(run_command_writes_stdin) {
    const CommandResult result = run_command({"/bin/cat"}, "hello\nfrom stdin", 2000);
    CHECK_TRUE(!result.spawn_failed);
    CHECK_TRUE(!result.timed_out);
    CHECK_EQ(result.returncode, 0);
    CHECK_EQ(result.out, "hello\nfrom stdin");
}

TEST_CASE(run_command_enforces_timeout) {
    const CommandResult result = run_command({"/bin/sleep", "10"}, "", 50);
    CHECK_TRUE(!result.spawn_failed);
    CHECK_TRUE(result.timed_out);
}

TEST_CASE(spawn_detached_returns_without_waiting_and_reports_missing_command) {
    const auto start = std::chrono::steady_clock::now();
    std::string error;
    CHECK_TRUE(spawn_detached({"/bin/sleep", "2"}, error));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CHECK_TRUE(elapsed.count() < 1500);

    std::string missing;
    CHECK_TRUE(!spawn_detached({"/definitely/not/a/keyloom-command"}, missing));
    CHECK_TRUE(!missing.empty());
}

TEST_CASE(evaluates_list_and_string_functions) {
    CHECK_EQ(eval_expression("len([\"a\", \"b\"])").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("[\"a\", \"b\"][0]").to_string(), std::string("a"));
    CHECK_EQ(eval_expression("[\"a\", \"b\"][-1]").to_string(), std::string("b"));
    CHECK_EQ(eval_expression("join([\"a\", \"b\"], \"-\")").to_string(), std::string("a-b"));
    CHECK_EQ(eval_expression("split(\"a,b\", \",\")[1]").to_string(), std::string("b"));
    CHECK_EQ(eval_expression("upper(\"hi\")").to_string(), std::string("HI"));
    CHECK_EQ(eval_expression("lower(\"HI\")").to_string(), std::string("hi"));
    CHECK_EQ(eval_expression("trim(\"  hi  \")").to_string(), std::string("hi"));
    CHECK_EQ(eval_expression("starts_with(\"hello\", \"he\")").to_string(), std::string("True"));
    CHECK_EQ(eval_expression("ends_with(\"hello\", \"lo\")").to_string(), std::string("True"));
    CHECK_EQ(eval_expression("contains(\"hello\", \"ell\")").to_string(), std::string("True"));
    CHECK_EQ(eval_expression("contains([\"a\", \"b\"], \"b\")").to_string(), std::string("True"));
    CHECK_EQ(eval_expression("replace(\"a-b\", \"-\", \"+\")").to_string(), std::string("a+b"));
    CHECK_EQ(eval_expression("substr(\"hello\", 1, 3)").to_string(), std::string("ell"));
    CHECK_EQ(eval_expression("substr(\"hello\", -2)").to_string(), std::string("lo"));
    CHECK_EQ(eval_expression("index_of(\"hello\", \"l\")").to_string(), std::string("2"));
    CHECK_EQ(eval_expression("index_of(\"hello\", \"z\")").to_string(), std::string("-1"));
}

TEST_CASE(lists_are_mutable_and_compare_by_value) {
    const Script script = parse(R"LOOM(
loom "lists" {
    when app starts {
        set items = ["b", "a"]
        call append(items, "c")
        call sort(items)
        set joined = join(items, "")
        set same = items == ["a", "b", "c"]
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("joined").to_string(), std::string("abc"));
    CHECK_EQ(runtime.variables().at("same").to_string(), std::string("True"));
}

TEST_CASE(for_loops_iterate_ranges_and_lists) {
    const Script script = parse(R"LOOM(
loom "loops" {
    when app starts {
        set total = 0
        for i = 1 to 4 {
            set total = total + i
        }
        set down = ""
        for i = 3 downto 1 {
            set down = down + str(i)
        }
        set stepped = ""
        for i = 0 to 6 step 3 {
            set stepped = stepped + str(i)
        }
        set items = ["x", "y"]
        set seen = ""
        for item in items {
            set seen = seen + item
        }
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("total").to_string(), std::string("10"));
    CHECK_EQ(runtime.variables().at("down").to_string(), std::string("321"));
    CHECK_EQ(runtime.variables().at("stepped").to_string(), std::string("036"));
    CHECK_EQ(runtime.variables().at("seen").to_string(), std::string("xy"));
}

TEST_CASE(break_and_continue_control_loops) {
    const Script script = parse(R"LOOM(
loom "control" {
    when app starts {
        set visited = ""
        for i = 1 to 10 {
            if i == 2 {
                continue
            }
            if i > 4 {
                break
            }
            set visited = visited + str(i)
        }
        set count = 0
        while true {
            set count = count + 1
            if count == 3 {
                break
            }
        }
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("visited").to_string(), std::string("134"));
    CHECK_EQ(runtime.variables().at("count").to_string(), std::string("3"));
}

TEST_CASE(user_functions_return_values_and_restore_their_scope) {
    const Script script = parse(R"LOOM(
loom "functions" {
    func double(x) {
        return x * 2
    }
    func shout(text) {
        return upper(text)
    }
    when app starts {
        set value = double(21)
        set loud = shout("hey")
        set shadow = 5
        call double(shadow)
        set after = shadow
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("value").to_string(), std::string("42"));
    CHECK_EQ(runtime.variables().at("loud").to_string(), std::string("HEY"));
    CHECK_EQ(runtime.variables().at("after").to_string(), std::string("5"));
}

TEST_CASE(user_functions_can_call_each_other) {
    const Script script = parse(R"LOOM(
loom "recursion" {
    func countdown(n) {
        if n <= 0 {
            return 0
        }
        return countdown(n - 1) + 1
    }
    when app starts {
        set depth = countdown(4)
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("depth").to_string(), std::string("4"));
}

TEST_CASE(runaway_recursion_is_stopped) {
    const Script script = parse(R"LOOM(
loom "runaway" {
    func loop_forever(n) {
        return loop_forever(n + 1)
    }
    when app starts {
        set value = loop_forever(0)
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_TRUE(runtime.runtime_error().find("Maximum function call depth") != std::string::npos);
}

TEST_CASE(chord_events_need_their_modifiers) {
    const Script script = parse(R"LOOM(
loom "chords" {
    when key CTRL+ALT+T goes down {
        set fired = 1
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());

    // Without the modifiers the chord stays quiet.
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(0));

    // With both modifiers held the chord fires.
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));

    // Releasing the primary key does not fire the chord again.
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));
}

TEST_CASE(chord_bindings_can_be_disabled_by_name) {
    const Script script = parse(R"LOOM(
loom "chords" {
    when key CTRL+ALT+T goes down {
        set fired = 1
    }
    when key F9 goes down {
        call set_hotkey_enabled("CTRL+ALT+T", false)
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(0));
}

TEST_CASE(function_assignments_stay_inside_the_function) {
    const Script script = parse(R"LOOM(
loom "scope" {
    func helper(x) {
        set doubled = x * 2
        return doubled
    }
    func reader() {
        return counter * 2
    }
    when app starts {
        set counter = 5
        set value = helper(21)
        set doubled = reader()
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().at("value").to_string(), std::string("42"));
    CHECK_EQ(runtime.variables().at("doubled").to_string(), std::string("10"));
    // The helper's own variable never reaches the script scope.
    CHECK_EQ(runtime.variables().count("doubled"), static_cast<size_t>(1));
    CHECK_EQ(runtime.variables().count("x"), static_cast<size_t>(0));
}

TEST_CASE(suspending_hotkeys_keeps_the_chord_binding_enabled) {
    const Script script = parse(R"LOOM(
loom "chords" {
    when key CTRL+ALT+T goes down {
        call suspend_hotkeys(true)
        set fired = 1
    }
    when key F9 goes down {
        set other = 1
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));

    // Other bindings are suspended, the chord that suspended them is not.
    runtime.physical_key("fake-keyboard", "F9", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("other"), static_cast<size_t>(0));

    runtime.physical_key("fake-keyboard", "T", false);
    runtime.physical_key("fake-keyboard", "LEFTALT", false);
    runtime.physical_key("fake-keyboard", "LEFTCTRL", false);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("fired"), static_cast<size_t>(1));
}

TEST_CASE(chord_release_events_fire_when_the_primary_key_is_released) {
    const Script script = parse(R"LOOM(
loom "chords" {
    when key CTRL+ALT+T goes up {
        set released = 1
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.physical_key("fake-keyboard", "LEFTCTRL", true);
    runtime.physical_key("fake-keyboard", "LEFTALT", true);
    runtime.physical_key("fake-keyboard", "T", true);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("released"), static_cast<size_t>(0));

    // The release fires even when the modifiers went up first.
    runtime.physical_key("fake-keyboard", "LEFTALT", false);
    runtime.physical_key("fake-keyboard", "T", false);
    runtime.scheduler().run();
    CHECK_EQ(runtime.variables().count("released"), static_cast<size_t>(1));
}
