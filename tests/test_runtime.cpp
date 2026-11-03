#include "harness.hpp"

#include <algorithm>
#include <fstream>
#include <linux/input.h>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "backends.hpp"
#include "keys.hpp"
#include "parser.hpp"
#include "runtime.hpp"
#include "scheduler.hpp"

using namespace keyloom;

namespace {

std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

struct RecordingBackend : OutputBackend {
    std::vector<std::string> log;

    void warp(int x, int y) override {
        log.push_back("warp " + std::to_string(x) + ", " + std::to_string(y));
    }
    void move(int dx, int dy) override {
        log.push_back("move " + std::to_string(dx) + ", " + std::to_string(dy));
    }
    void button(const std::string& name, bool down) override {
        log.push_back("button " + name + (down ? " down" : " up"));
    }
    void key(const std::string& name, bool down) override {
        log.push_back("key " + name + (down ? " down" : " up"));
    }
    void type_text(const std::string& text) override { log.push_back("type " + text); }
    void scroll(int x, int y) override {
        log.push_back("scroll " + std::to_string(x) + ", " + std::to_string(y));
    }
    void close() override { log.push_back("close"); }
    const char* name() const override { return "RecordingBackend"; }
};

struct TimedBackend : OutputBackend {
    std::vector<double> downs;
    void warp(int, int) override {}
    void move(int, int) override {}
    void button(const std::string&, bool down) override {
        if (down) downs.push_back(Scheduler::current().now());
    }
    void key(const std::string&, bool) override {}
    void type_text(const std::string&) override {}
    void scroll(int, int) override {}
    void close() override {}
    const char* name() const override { return "TimedBackend"; }
};

struct FakeSink : EventSink {
    std::vector<std::string> writes;
    void write_event(int type, int code, int value) override {
        writes.push_back(std::to_string(type) + ":" + std::to_string(code) + ":" + std::to_string(value));
    }
    void close() override {}
};

struct RecordedInput : VirtualInput {
    std::vector<std::string> writes;
    int closes = 0;

    void write(int type, int code, int value) override {
        writes.push_back(std::to_string(type) + ":" + std::to_string(code) + ":" + std::to_string(value));
    }
    void flush() override { writes.push_back("flush"); }
    void close() override { ++closes; }
};

/// Hands the recorded device to the passthrough while the test keeps the view.
struct ForwardingInput : VirtualInput {
    RecordedInput* target;
    explicit ForwardingInput(RecordedInput* recorder) : target(recorder) {}
    void write(int type, int code, int value) override { target->write(type, code, value); }
    void flush() override { target->flush(); }
    void close() override { target->close(); }
};

Value eval_expression(const std::string& expression) {
    Script script = parse("loom \"expr\" { when app starts { set result = " + expression + " } }");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    return runtime.variables().at("result");
}

Task drive_held_loop(Runtime& runtime, const Node& event) {
    runtime.pressed_keys().insert("X");
    runtime.scheduler().spawn(runtime.held_loop(event, "X", 0.01));
    co_await sleep_for(0.075);
    runtime.pressed_keys().erase("X");
}

/// Press a key, let a `stays held` body get partway through a tap, then release
/// the key so the runtime cancels the held task mid-tap.
Task drive_hold_then_release(Runtime& runtime, std::string key, double hold_seconds) {
    runtime.physical_key("fake-keyboard", key, true);
    co_await sleep_for(hold_seconds);
    runtime.physical_key("fake-keyboard", key, false);
    co_await sleep_for(0.3);
}

struct PointerRecorder : OutputBackend {
    std::vector<std::string>* log;
    explicit PointerRecorder(std::vector<std::string>* target) : log(target) {}
    void warp(int x, int y) override { log->push_back("warp " + std::to_string(x) + "," + std::to_string(y)); }
    void move(int dx, int dy) override {
        log->push_back("move " + std::to_string(dx) + "," + std::to_string(dy));
    }
    void button(const std::string&, bool) override {}
    void key(const std::string&, bool) override {}
    void type_text(const std::string&) override {}
    void scroll(int x, int y) override {
        log->push_back("scroll " + std::to_string(x) + "," + std::to_string(y));
    }
    void close() override { log->push_back("close"); }
    const char* name() const override { return "PointerRecorder"; }
};

}  // namespace

TEST_CASE(shared_mouse_holds_release_after_the_last_owner) {
    const Script script = parse("loom \"holds\" { when app starts { log \"ready\" } }");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));

    runtime.hold_button("A", "left");
    runtime.hold_button("D", "left");
    runtime.free_button("A");
    CHECK_EQ(raw->log, std::vector<std::string>({"button left down"}));
    runtime.free_button("D");
    CHECK_EQ(raw->log, std::vector<std::string>({"button left down", "button left up"}));
}

TEST_CASE(key_holds_are_reference_counted) {
    const Script script = parse("loom \"keys\" { when app starts { log \"ready\" } }");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));

    runtime.hold_key("LEFTCTRL");
    runtime.hold_key("LEFTCTRL");
    runtime.free_key("LEFTCTRL");
    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down"}));
    runtime.free_key("LEFTCTRL");
    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key LEFTCTRL up"}));
}

TEST_CASE(held_loop_ticks_on_a_fixed_cadence) {
    const Script script = parse(R"LOOM(
loom "cadence" {
    when key X stays held every 10ms {
        mouse left tap for 5ms
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<TimedBackend>();
    TimedBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.scheduler().spawn(drive_held_loop(runtime, script.events.at(0)));
    runtime.scheduler().run();

    const auto& downs = raw->downs;
    CHECK_TRUE(downs.size() >= 5);
    std::vector<double> gaps;
    for (size_t index = 1; index < downs.size(); ++index) gaps.push_back(downs[index] - downs[index - 1]);
    std::sort(gaps.begin(), gaps.end());
    // A 5ms tap inside a 10ms tick: sleeping the tap on top of the interval would
    // make the period ~15ms instead of 10ms.
    const double median = gaps.at(gaps.size() / 2);
    CHECK_TRUE(median < 0.013);
}

TEST_CASE(cancelling_a_held_tap_unwinds_and_releases_the_key) {
    const Script script = parse(R"LOOM(
loom "cancel-tap" {
    when key X stays held every 10ms {
        key E tap for 200ms
    }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.scheduler().spawn(drive_hold_then_release(runtime, "X", 0.03));
    runtime.scheduler().run();

    // The tap is cancelled while its sleep is still pending: the key must be
    // released exactly once, and no orphaned frame may keep tapping.
    CHECK_EQ(raw->log, std::vector<std::string>({"key E down", "key E up"}));
}

TEST_CASE(two_spots_press_and_release_logic) {
    const std::string path = std::string(KEYLOOM_SOURCE_DIR) + "/examples/two_spots.loom";
    const Script script = parse(read_file(path), path);
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-keyboard", "A", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "D", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-keyboard", "A", false);
    runtime.scheduler().run();
    CHECK_TRUE(std::find(raw->log.begin(), raw->log.end(), "button left up") == raw->log.end());
    runtime.physical_key("fake-keyboard", "D", false);
    runtime.scheduler().run();

    CHECK_EQ(raw->log, std::vector<std::string>({"warp 304, 395", "button left down", "warp 940, 412",
                                                 "button left up"}));
}

TEST_CASE(consumed_keys_are_script_wide_and_panic_always_passes) {
    const Script script = parse(R"LOOM(
loom "consume" {
    option panic = "A"
    when key A goes down { consume key A }
    when key D goes down { consume key D }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_panic_key("A");
    const auto consumed = runtime.collect_consumed_keys();
    CHECK_EQ(consumed.size(), static_cast<size_t>(1));
    CHECK_TRUE(consumed.count("D") == 1);
}

std::vector<std::string> run_send_script(const std::string& statement) {
    const Script script = parse("loom \"send-test\" { when app starts { " + statement + " } }");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    return raw->log;
}

TEST_CASE(send_types_text_and_special_tokens) {
    CHECK_EQ(run_send_script("send \"a{TAB}{ENTER}\""),
             std::vector<std::string>({"type a", "key TAB down", "key TAB up", "key ENTER down",
                                       "key ENTER up"}));
}

TEST_CASE(send_prefixes_chords_for_modifiers) {
    CHECK_EQ(run_send_script("send \"^c!{TAB}\""),
             std::vector<std::string>({"key LEFTCTRL down", "key C down", "key C up", "key LEFTCTRL up",
                                       "key LEFTALT down", "key TAB down", "key TAB up", "key LEFTALT up"}));
}

TEST_CASE(send_tokens_keep_explicit_modifier_holds) {
    CHECK_EQ(run_send_script("send \"{CTRL down}a{TAB}{CTRL up}\""),
             std::vector<std::string>({"key LEFTCTRL down", "key A down", "key A up", "key TAB down",
                                       "key TAB up", "key LEFTCTRL up"}));
}

TEST_CASE(send_repeat_tokens_and_escaped_braces) {
    CHECK_EQ(run_send_script("send \"{TAB 2}\""),
             std::vector<std::string>({"key TAB down", "key TAB up", "key TAB down", "key TAB up"}));
    CHECK_EQ(run_send_script("send \"{{}\""),
             std::vector<std::string>({"key LEFTSHIFT down", "key LEFTBRACE down", "key LEFTBRACE up",
                                       "key LEFTSHIFT up"}));
}

TEST_CASE(window_query_functions_return_a_value_without_window_tools) {
    CHECK_TRUE(eval_expression("active_window_title()").type == Value::Type::Str);
    CHECK_TRUE(eval_expression("active_window_class()").type == Value::Type::Str);
    CHECK_TRUE(eval_expression("active_window_process()").type == Value::Type::Str);
    CHECK_TRUE(eval_expression("active_window_pid()").type == Value::Type::Int);
}

Value eval_expression_or_fails(const std::string& expression) {
    const Script script = parse("loom \"expr\" { when app starts { set result = " + expression + " } }");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_backend(std::make_unique<RecordingBackend>());
    runtime.scheduler().spawn(runtime.run_event(script.events.at(0)));
    runtime.scheduler().run();
    if (!runtime.runtime_error().empty()) {
        throw RuntimeErrorKeyloom(runtime.runtime_error());
    }
    return runtime.variables().at("result");
}

TEST_CASE(arbitrary_gui_calls_are_registered_functions) {
    CHECK_THROWS(eval_expression_or_fails("gui_input()"), RuntimeErrorKeyloom);
    CHECK_THROWS(eval_expression_or_fails("gui_confirm(1, 2, 3)"), RuntimeErrorKeyloom);
}

TEST_CASE(remap_key_holds_and_releases_target) {
    const Script script = parse(R"LOOM(
loom "remap" {
    remap key Q to SPACE
    when app starts { log "ready" }
}
)LOOM");
    CHECK_EQ(script.remaps.size(), static_cast<size_t>(1));
    CHECK_EQ(script.remaps.at(0).target, std::string("SPACE"));

    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-keyboard", "Q", true);
    runtime.physical_key("fake-keyboard", "Q", true);
    runtime.physical_key("fake-keyboard", "Q", false);

    CHECK_EQ(raw->log, std::vector<std::string>({"key SPACE down", "key SPACE up"}));
}

TEST_CASE(remap_source_is_script_wide_consumed) {
    const Script script = parse(R"LOOM(
loom "remap-passthrough" {
    remap key Q to SPACE
    when timer every 1s { log "tick" }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");
    runtime.set_consumed_keys(runtime.collect_consumed_keys());

    const std::vector<RawEvent> events = {
        {EV_KEY, KEY_Q, 1}, {EV_KEY, KEY_A, 1}, {EV_KEY, KEY_Q, 0}, {EV_KEY, KEY_A, 0},
    };
    FakeSink sink;
    runtime.process_events(events, &sink, "/dev/input/event99");

    CHECK_EQ(sink.writes.size(), static_cast<size_t>(2));
    CHECK_EQ(sink.writes.at(0), std::string(std::to_string(EV_KEY) + ":" + std::to_string(KEY_A) + ":1"));
    CHECK_EQ(sink.writes.at(1), std::string(std::to_string(EV_KEY) + ":" + std::to_string(KEY_A) + ":0"));
}

TEST_CASE(consumed_key_events_are_not_replayed) {
    const Script script = parse("loom \"passthrough\" { when timer every 1s { log \"tick\" } }");
    Runtime runtime(script, Runtime::Options{});
    runtime.set_panic_key("");
    runtime.set_consumed_keys({"A"});

    const std::vector<RawEvent> events = {
        {EV_KEY, KEY_A, 1},     {EV_KEY, KEY_A, 2}, {EV_KEY, KEY_B, 1}, {EV_SYN, SYN_REPORT, 0},
        {EV_KEY, KEY_A, 0},     {EV_KEY, KEY_B, 0}, {EV_SYN, SYN_REPORT, 0},
    };
    FakeSink sink;
    runtime.process_events(events, &sink, "/dev/input/event99");

    CHECK_EQ(sink.writes.size(), static_cast<size_t>(4));
    CHECK_EQ(sink.writes.at(0), std::string(std::to_string(EV_KEY) + ":" + std::to_string(KEY_B) + ":1"));
    CHECK_EQ(sink.writes.at(1), std::string(std::to_string(EV_SYN) + ":" + std::to_string(SYN_REPORT) + ":0"));
    CHECK_EQ(sink.writes.at(2), std::string(std::to_string(EV_KEY) + ":" + std::to_string(KEY_B) + ":0"));
}

TEST_CASE(passthrough_releases_held_keys_on_close) {
    RecordedInput recorder;
    DeviceCapabilities capabilities;
    capabilities.keys = {KEY_A};
    PassthroughDevice device(capabilities, [&recorder](const std::string&, const std::string&,
                                                       const DeviceCapabilities&) -> std::unique_ptr<VirtualInput> {
        return std::make_unique<ForwardingInput>(&recorder);
    });

    device.write_event(EV_KEY, KEY_A, 1);
    device.write_event(EV_KEY, KEY_A, 0);
    device.write_event(EV_KEY, KEY_A, 1);
    device.close();

    CHECK_EQ(recorder.writes.at(0), std::string("1:" + std::to_string(KEY_A) + ":1"));
    CHECK_TRUE(std::find(recorder.writes.begin(), recorder.writes.end(),
                         "1:" + std::to_string(KEY_A) + ":0") != recorder.writes.end());
    CHECK_EQ(recorder.writes.back(), std::string("flush"));
    CHECK_EQ(recorder.closes, 1);

    // A second close must be harmless.
    device.close();
    CHECK_EQ(recorder.closes, 1);
}

TEST_CASE(ydotool_pointer_motion_uses_a_real_absolute_backend) {
    std::vector<std::string> pointer_log;
    std::vector<std::string> calls;

    PointerFactory factory = [&pointer_log](int width, int height, bool keyboard) -> std::unique_ptr<OutputBackend> {
        pointer_log.push_back("init " + std::to_string(width) + "x" + std::to_string(height) +
                              (keyboard ? " keyboard" : " pointer"));
        return std::make_unique<PointerRecorder>(&pointer_log);
    };
    YdotoolBackend::Runner runner = [&calls](const std::vector<std::string>& argv, bool) {
        std::string joined;
        for (const auto& argument : argv) {
            if (!joined.empty()) joined += " ";
            joined += argument;
        }
        calls.push_back(joined);
        CommandResult result;
        result.returncode = 0;
        return result;
    };

    YdotoolBackend backend(1368, 768, factory, runner, "/usr/bin/ydotool");
    backend.warp(640, 420);
    backend.move(10, -5);
    backend.scroll(0, -3);
    backend.close();

    CHECK_EQ(pointer_log,
             std::vector<std::string>({"init 1368x768 pointer", "warp 640,420", "move 10,-5", "scroll 0,-3",
                                       "close"}));
    // Motion must never go through `ydotool mousemove --absolute`, which jumps to
    // the top-left corner first.
    CHECK_EQ(calls, std::vector<std::string>({"/usr/bin/ydotool --help"}));
}

TEST_CASE(ydotool_buttons_and_keys_use_encoded_values) {
    std::vector<std::string> calls;
    YdotoolBackend::Runner runner = [&calls](const std::vector<std::string>& argv, bool) {
        std::string joined;
        for (const auto& argument : argv) {
            if (!joined.empty()) joined += " ";
            joined += argument;
        }
        calls.push_back(joined);
        CommandResult result;
        result.returncode = 0;
        return result;
    };
    YdotoolBackend backend(1920, 1080, [](int, int, bool) { return std::unique_ptr<OutputBackend>(); }, runner,
                           "/usr/bin/ydotool");
    backend.button("left", true);
    backend.button("left", false);
    backend.key("A", true);
    backend.type_text("a\\nb");

    // ydotoold encodes clicks as its own button index (0 = left) plus 0x40/0x80.
    CHECK_EQ(calls, std::vector<std::string>({"/usr/bin/ydotool --help", "/usr/bin/ydotool click 0x40",
                                              "/usr/bin/ydotool click 0x80", "/usr/bin/ydotool key 30:1",
                                              "/usr/bin/ydotool type -e 0 -- a\\nb"}));
}

TEST_CASE(mouse_buttons_map_to_distinct_runtime_names) {
    const std::string left = normalize_key_event(BTN_LEFT).value();
    const std::string right = normalize_key_event(BTN_RIGHT).value();
    const std::string middle = normalize_key_event(BTN_MIDDLE).value();
    const std::string letter = normalize_key_event(KEY_A).value();
    CHECK_EQ(left, std::string("MOUSE_LEFT"));
    CHECK_EQ(right, std::string("MOUSE_RIGHT"));
    CHECK_EQ(middle, std::string("MOUSE_MIDDLE"));
    CHECK_EQ(letter, std::string("A"));
    CHECK_EQ(canonical_mouse_event_key("Left"), std::string("MOUSE_LEFT"));
    CHECK_EQ(canonical_mouse_event_key("mouse_right"), std::string("MOUSE_RIGHT"));
    // The left mouse button must never alias the LEFT arrow key.
    CHECK_TRUE(canonical_mouse_event_key("left") != canonical_key("LEFT"));
}

TEST_CASE(mouse_button_edges_press_and_release_the_modifier) {
    const Script script = parse(R"LOOM(
loom "ctrl-click" {
    when mouse left goes down { key CTRL press }
    when mouse left goes up { key CTRL free }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-mouse", "MOUSE_LEFT", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-mouse", "MOUSE_LEFT", false);
    runtime.scheduler().run();

    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key LEFTCTRL up"}));
}

TEST_CASE(mouse_held_repeats_until_the_button_is_released) {
    const Script script = parse(R"LOOM(
loom "mouse-held" {
    when mouse left stays held every 10ms { key E tap for 5ms }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.scheduler().spawn(drive_hold_then_release(runtime, "MOUSE_LEFT", 0.055));
    runtime.scheduler().run();

    const auto downs = std::count(raw->log.begin(), raw->log.end(), std::string("key E down"));
    const auto ups = std::count(raw->log.begin(), raw->log.end(), std::string("key E up"));
    CHECK_TRUE(downs >= 2);
    CHECK_EQ(downs, ups);
}

TEST_CASE(mouse_button_and_arrow_key_bindings_do_not_collide) {
    const Script script = parse(R"LOOM(
loom "collision" {
    when key LEFT goes down { key A press }
    when mouse left goes down { key B press }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    runtime.physical_key("fake-keyboard", "LEFT", true);
    runtime.scheduler().run();
    runtime.physical_key("fake-mouse", "MOUSE_LEFT", true);
    runtime.scheduler().run();

    CHECK_EQ(raw->log, std::vector<std::string>({"key A down", "key B down"}));
}

TEST_CASE(evdev_mouse_button_events_reach_mouse_bindings) {
    const Script script = parse(R"LOOM(
loom "evdev-mouse" {
    when mouse left goes down { key CTRL press }
    when mouse left goes up { key CTRL free }
}
)LOOM");
    Runtime runtime(script, Runtime::Options{});
    auto backend = std::make_unique<RecordingBackend>();
    RecordingBackend* raw = backend.get();
    runtime.set_backend(std::move(backend));
    runtime.set_panic_key("");

    const std::vector<RawEvent> events = {{EV_KEY, BTN_LEFT, 1}, {EV_SYN, SYN_REPORT, 0},
                                          {EV_KEY, BTN_LEFT, 0}, {EV_SYN, SYN_REPORT, 0}};
    runtime.process_events(events, nullptr, "/dev/input/event99");
    runtime.scheduler().run();

    CHECK_EQ(raw->log, std::vector<std::string>({"key LEFTCTRL down", "key LEFTCTRL up"}));
}

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
